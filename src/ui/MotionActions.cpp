// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/MotionActions.h"

#include "core/AssemblySolve.h"
#include "core/CollisionCache.h"
#include "core/Document.h"
#include "core/MeshCollide.h"
#include "core/io/GlbAnimation.h"
#include "core/io/Serialization.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJob.h"
#include "ui/BackgroundJobs.h"

#include <OSD.hxx>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace ui {
namespace {

std::string Revision(AppState& state, uint64_t studyId) {
  if (!state.assembly)
    return {};
  const auto* study = state.assembly->FindMotionStudyById(studyId);
  if (!study)
    return {};
  nlohmann::json revision{
      {"assembly", reinterpret_cast<uintptr_t>(state.assembly.get())},
      {"version", state.assembly->Version()},
      {"study", DocumentIO::MotionStudyToJson(*study)}};
  auto children = nlohmann::json::array();
  for (const auto& [id, version] :
       assembly::SubtreeVersions(*state.assembly, PartResolverFor(state),
                                 AssemblyResolverFor(state))) {
    auto row = nlohmann::json{{"id", id}, {"version", version}};
    if (auto* part = PartDocById(state, id))
      row["instance"] = part->InstanceId();
    children.push_back(std::move(row));
  }
  revision["children"] = std::move(children);
  return revision.dump();
}

// Everything a run reads, copied so the worker shares nothing with the tab.
struct MotionRun {
  uint64_t studyId = 0;
  const AssemblyDocument* ranAgainst = nullptr; // identity, never dereferenced
  MotionStudy study;
  AssemblyDocument asmCopy;
  // (part, configuration) -> a copy evaluated in it
  std::map<std::pair<uint64_t, uint64_t>, std::unique_ptr<Document>> parts;
  std::map<uint64_t, std::unique_ptr<AssemblyDocument>> assemblies;
  std::map<const void*, std::shared_ptr<const collide::Mesh>> meshes;
  motion::Result result;
  std::shared_ptr<collide::ExactContactCache> contacts;
  std::string revision;
  uint64_t inputHash = 0; // motion::InputKey of what this run reads
  std::chrono::steady_clock::time_point started;
};

// Copies of every document under the assembly, and a resolver over them.
void CopySubtree(AppState& state, MotionRun& run) {
  const assembly::PartResolver resolve = PartResolverFor(state);
  // One copy per (part, configuration), not per occurrence: a part placed
  // four times built four Documents to overwrite each other (16 copies for
  // 10 parts on the SO-101 gripper). CopyWithGeometry, not a plain copy: a
  // plain one ends in a Regenerate that re-executes every boolean in the
  // part -- 54% of a profile of this call -- and the run only READS it.
  for (const auto& key : assembly::SubtreePartConfigurations(
           *state.assembly, AssemblyResolverFor(state)))
    if (const Document* pd = resolve(key.first, key.second))
      run.parts[key] = Document::CopyWithGeometry(*pd);
  for (const auto& [slotId, version] : assembly::SubtreeVersions(
           *state.assembly, resolve, AssemblyResolverFor(state))) {
    (void)version;
    if (run.assemblies.count(slotId) == 0)
      if (const AssemblyDocument* ad = AssemblyDocById(state, slotId))
        run.assemblies[slotId] = std::make_unique<AssemblyDocument>(*ad);
  }
}

assembly::PartResolver PartsOf(MotionRun& run) {
  return [&run](uint64_t id, uint64_t configuration) -> const Document* {
    const auto it = run.parts.find({id, configuration});
    return it == run.parts.end() ? nullptr : it->second.get();
  };
}

assembly::AssemblyResolver AssembliesOf(MotionRun& run) {
  return [&run](uint64_t id) -> const AssemblyDocument* {
    const auto it = run.assemblies.find(id);
    return it == run.assemblies.end() ? nullptr : it->second.get();
  };
}

// The triangulation is the one thing a run needs that MUTATES a shape
// (BRepMesh attaches it), so it happens here, on the main thread, before any
// worker exists.
void PrebuildMeshes(MotionRun& run) {
  if (!run.study.checkClash || run.study.clashMode != "mesh")
    return;
  for (const auto& [id, doc] : run.parts)
    for (const Body& b : doc->GetBodies()) {
      if (b.reference || !b.shape.IsValid())
        continue;
      const void* key = b.shape.GeometryId();
      if (!run.meshes.count(key))
        run.meshes[key] =
            collide::Mesh::Build(b.shape, run.study.clashDeflectionMm);
    }
}

// The copies the worker reads, and the collision meshes it would otherwise
// build off the main thread.
void CopyForWorker(AppState& state, MotionRun& run) {
  CopySubtree(state, run);
  PrebuildMeshes(run);
}

bool Prepare(AppState& state, uint64_t studyId, MotionRun& run,
             std::string* error, bool copyDocs,
             const RunOverride* override = nullptr) {
  if (!state.assembly) {
    if (error)
      *error = "the active document is not an assembly";
    return false;
  }
  const MotionStudy* s = state.assembly->FindMotionStudyById(studyId);
  if (!s) {
    if (error)
      *error = "no motion study with id " + std::to_string(studyId);
    return false;
  }
  const std::string why =
      motion::WhyNot(*state.assembly, *s, AssemblyResolverFor(state));
  if (!why.empty()) {
    if (error)
      *error = why;
    return false;
  }
  if (!state.motionContacts)
    state.motionContacts = std::make_shared<collide::ExactContactCache>();
  run.contacts = state.motionContacts;
  run.revision = Revision(state, studyId);
  run.studyId = studyId;
  run.ranAgainst = state.assembly.get();
  run.study = *s;
  // What THIS run checks. Applied to the copy only, after the revision was
  // taken from the stored study — so the study is not edited and a preview
  // of an unchanged study is not stale. Before PrebuildMeshes, which reads it.
  if (override) {
    run.study.checkClash = override->checkClash;
    run.study.clashMode = override->clashMode;
  }
  run.asmCopy = *state.assembly;
  run.started = std::chrono::steady_clock::now();
  // What the answer depends on, hashed from the live documents the copies
  // are about to be taken from (docs/simulation/README.md#studies-as-jobs).
  run.inputHash = motion::InputKey(*state.assembly, PartResolverFor(state),
                                   AssemblyResolverFor(state), run.study);
  // The copies exist so the worker shares nothing with the tab, and are made
  // only for a run that leaves the main thread — a synchronous run reads the
  // live documents it is blocking anyway. They are cheap now
  // (docs/assemblies/README.md#background-motion-copies-without-regenerating);
  // they were not, and a plain Document copy still is not.
  if (copyDocs)
    CopyForWorker(state, run);
  return true;
}

// `result` as the study's own: what MotionResultFor answers, and what the
// assembly carries into the project when it is saved.
void Install(AppState& state, AssemblyDocument& asmDoc, uint64_t studyId,
             motion::Result result, std::string revision) {
  asmDoc.StoreMotionResult(studyId,
                           std::make_shared<const motion::Result>(result));
  state.motionResults[studyId] = std::move(result);
  state.motionResultRevisions[studyId] = std::move(revision);
}

// A result the session holds for exactly these inputs, installed as the
// study's own: the stored run answers, and nothing is computed.
bool ReuseStored(AppState& state, const MotionRun& run, JobStart* job) {
  if (job != nullptr && job->recompute)
    return false;
  const std::shared_ptr<const motion::Result> stored =
      state.motionResultsByInput.Find(run.inputHash);
  if (!stored)
    return false;
  if (state.motionPlaybackStudy == run.studyId)
    StopMotionPlayback(state);
  Install(state, *state.assembly, run.studyId, *stored, run.revision);
  if (job != nullptr)
    job->reused = true;
  return true;
}

// Keep a finished run by its input hash -- a whole one only: a cancelled or
// refused run is not the answer to its inputs.
void Remember(AppState& state, motion::Result& result, uint64_t inputHash) {
  result.inputHash = inputHash;
  if (!result.cancelled && result.error.empty())
    state.motionResultsByInput.Put(
        inputHash, std::make_shared<const motion::Result>(result));
}

} // namespace

bool StartMotionRun(AppState& state, uint64_t studyId, std::string* error,
                    const RunOverride* override, JobStart* jobStart) {
  auto run = std::make_shared<MotionRun>();
  if (!Prepare(state, studyId, *run, error, /*copyDocs=*/false, override))
    return false;
  // Answered from a stored run before asking whether one is running: a
  // reuse starts nothing, so it waits for nothing.
  if (ReuseStored(state, *run, jobStart))
    return true;
  if (BackgroundJobRunning(state, "motion")) {
    if (error)
      *error = "a motion study is already running (background_jobs lists "
               "it with its job id; cancel_study {job} stops it)";
    return false;
  }
  CopyForWorker(state, *run);
  // Keep the previous result visible, with its revision, while running.

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "motion";
  bg->label = "Motion study '" + run->study.name + "'";
  bg->cancellable = true;
  bg->studyId = studyId;
  bg->owner = jobStart ? jobStart->owner : 0;
  BackgroundJob* raw = bg.get();
  bg->thread = std::thread([run, raw]() {
    OSD::SetSignal(Standard_False);
    run->result = motion::Run(
        run->asmCopy, PartsOf(*run), AssembliesOf(*run), run->study,
        [raw] { return raw->cancel.load(); },
        [raw](double f) { raw->fraction.store(f); }, &run->meshes,
        run->contacts.get());
    raw->error = run->result.error;
    raw->done.store(true); // last
  });
  bg->finish = [run](AppState& s) {
    Remember(s, run->result, run->inputHash);
    // Only for the document it was run against: a tab switched underneath
    // a run does not receive somebody else's collision table.
    if (s.assembly.get() == run->ranAgainst) {
      // The poses being played are about to be replaced.
      if (s.motionPlaybackStudy == run->studyId)
        StopMotionPlayback(s);
      Install(s, *s.assembly, run->studyId, std::move(run->result),
              run->revision);
    }
    return true;
  };
  BackgroundJob& started = StartBackgroundJob(state, std::move(bg));
  if (jobStart)
    jobStart->jobId = started.id;
  return true;
}

motion::Result RunMotionNow(AppState& state, uint64_t studyId,
                            JobStart* jobStart) {
  MotionRun run;
  std::string error;
  if (!Prepare(state, studyId, run, &error, /*copyDocs=*/false)) {
    motion::Result r;
    r.error = error;
    return r;
  }
  if (ReuseStored(state, run, jobStart))
    return state.motionResults[studyId];
  // A caller that hangs up mid-run stops it: nobody is left to read it.
  const std::function<bool()> gone = state.rpcCallerGone;
  motion::Result r = motion::Run(run.asmCopy, PartResolverFor(state),
                                 AssemblyResolverFor(state), run.study, gone,
                                 nullptr, &run.meshes, run.contacts.get());
  Remember(state, r, run.inputHash);
  if (state.motionPlaybackStudy == studyId)
    StopMotionPlayback(state);
  Install(state, *state.assembly, studyId, r, run.revision);
  return r;
}

const motion::Result* MotionResultFor(const AppState& state, uint64_t studyId) {
  const auto it = state.motionResults.find(studyId);
  return it == state.motionResults.end() ? nullptr : &it->second;
}

void DropMotionResult(AppState& state, uint64_t studyId) {
  state.motionResults.erase(studyId);
  state.motionResultRevisions.erase(studyId);
  if (state.assembly)
    state.assembly->StoreMotionResult(studyId, nullptr);
}

void AdoptSavedMotionResults(AppState& state) {
  if (!state.assembly)
    return;
  for (const uint64_t id : state.assembly->MotionResultIds()) {
    const std::shared_ptr<const motion::Result> saved =
        state.assembly->MotionResult(id);
    const MotionStudy* study = state.assembly->FindMotionStudyById(id);
    if (!saved || study == nullptr)
      continue;
    // The session's own (a tab switched back to): it keeps the revision it
    // was run at, which says whether anything moved since.
    const auto have = state.motionResults.find(id);
    if (have != state.motionResults.end() &&
        have->second.inputHash == saved->inputHash)
      continue;
    // Current when the inputs hash as they did when it ran; stale (a
    // revision nothing produces) when they do not.
    const bool current =
        saved->inputHash != 0 &&
        motion::InputKey(*state.assembly, PartResolverFor(state),
                         AssemblyResolverFor(state),
                         *study) == saved->inputHash;
    state.motionResults[id] = *saved;
    state.motionResultRevisions[id] =
        current ? Revision(state, id) : std::string("saved, inputs moved");
    if (current && !saved->cancelled && saved->error.empty())
      state.motionResultsByInput.Put(saved->inputHash, saved);
  }
}

bool MotionResultStale(AppState& state, uint64_t studyId) {
  const auto it = state.motionResultRevisions.find(studyId);
  return it == state.motionResultRevisions.end() ||
         it->second != Revision(state, studyId);
}

namespace {

// The study, ready to be posed: validated, and under the configuration it
// names. A study that names its configuration is posed under it: a pose of
// the "full load" study with the rack at idle would be a pose the study
// never made. Switching is a document change, so it is an undo step.
MotionStudy* StudyToPose(AppState& state, uint64_t studyId,
                         std::string* error) {
  const auto fail = [&](const std::string& m) -> MotionStudy* {
    if (error)
      *error = m;
    return nullptr;
  };
  if (!state.assembly)
    return fail("the active document is not an assembly");
  MotionStudy* s = state.assembly->FindMotionStudyById(studyId);
  if (!s)
    return fail("no motion study with id " + std::to_string(studyId));
  const std::string why =
      motion::WhyNot(*state.assembly, *s, AssemblyResolverFor(state));
  if (!why.empty())
    return fail(why);
  if (s->configuration &&
      state.assembly->ActiveConfiguration() != *s->configuration) {
    SnapshotAssembly(state);
    state.assembly->SetActiveConfiguration(*s->configuration);
    SolveActiveAssembly(state);
  }
  return s;
}

// WALK the driver to `target`, in the study's own steps, rather than jumping
// to it.
//
// A mechanism's pose is not a function of its driver alone. A slider-crank
// at 180 degrees has the crank and rod collinear, and BOTH branches close
// the loop exactly — the piston at +107 and the piston at -193 are equally
// valid answers to "crank at 180". Which one the solver returns depends on
// where it started, and jumping 90 degrees into that singularity landed the
// engine on the mirrored branch: the piston out through the bottom of the
// crankcase, reported as a clean solve because it IS one.
//
// The run does not have this problem, because it walks its samples in
// order and each solve starts from the last. A scrub is the same path at
// the same spacing, so it costs what one run sample costs per step and is
// bounded by the study's own sample count.
void WalkDriver(AppState& state, const MotionStudy& s, double target) {
  // The driver may be inside a flexible placement (v129): read and set
  // through its path, which for this document's own mates is empty.
  const assembly::AssemblyResolver subs = AssemblyResolverFor(state);
  const double current = assembly::MateDofAt(*state.assembly, s.driverPath,
                                             s.driverMate, s.driverDof, subs)
                             .value_or(0.0);
  const double span = s.to - s.from;
  const int steps = std::max(s.samples - 1, 1);
  const double stride = std::abs(span) / steps;
  int walk = 1;
  if (stride > 1e-12)
    walk = static_cast<int>(std::ceil(std::abs(target - current) / stride));
  walk = std::clamp(walk, 1, steps);
  for (int k = 1; k <= walk; k++) {
    const double v = current + (target - current) * (double(k) / walk);
    assembly::SetMateDofAt(*state.assembly, s.driverPath, s.driverMate,
                           s.driverDof, v, subs);
    SolveActiveAssemblyDriven(state, s.driverMate, s.driverDof,
                              /*diagnose=*/false, // a motion sample
                              s.driverPath);
  }
  state.syncedDocumentVersion = UINT64_MAX; // the placements changed
}

} // namespace

bool ApplyMotionPose(AppState& state, uint64_t studyId, double t,
                     std::string* error) {
  MotionStudy* s = StudyToPose(state, studyId, error);
  if (!s)
    return false;
  WalkDriver(state, *s, motion::ValueAt(*s, t));
  return true;
}

bool ApplyMotionValue(AppState& state, uint64_t studyId, int sample,
                      double value, std::string* error) {
  MotionStudy* s = StudyToPose(state, studyId, error);
  if (!s)
    return false;
  const int steps = std::max(s->samples - 1, 1);
  const double t = static_cast<double>(std::clamp(sample, 0, steps)) / steps;
  WalkDriver(state, *s, motion::ValueAt(*s, t)); // onto the run's own path
  WalkDriver(state, *s, value); // under one spacing: a single step
  return true;
}

bool ApplyMotionSample(AppState& state, uint64_t studyId,
                       const motion::Result& result, size_t sample,
                       const double* value, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (sample >= result.samples.size())
    return fail("sample " + std::to_string(sample) + " of a run with " +
                std::to_string(result.samples.size()) + " samples");
  const motion::Sample& at = result.samples[sample];
  if (!at.solved)
    return fail("sample " + std::to_string(sample) +
                " did not solve (residual " + std::to_string(at.residualMm) +
                " mm), so the run has no pose there to leave the assembly in");
  MotionStudy* s = StudyToPose(state, studyId, error);
  if (!s)
    return false;
  AssemblyDocument& asmDoc = *state.assembly;
  const assembly::AssemblyResolver subs = AssemblyResolverFor(state);
  // Where the run had everything: each top-level placement, and each moving
  // mate's value (a flexible placement's inner ones through its path).
  for (const auto& [occId, world] : at.worlds)
    if (Occurrence* o = asmDoc.FindById(occId)) {
      o->position = glm::dvec3(world[3]);
      o->orientation = glm::normalize(glm::quat_cast(glm::dmat3(world)));
    }
  for (const motion::Sample::MateValue& v : at.mates) {
    if (v.turns)
      assembly::SetMateDofAt(asmDoc, v.path, v.mate, MateDof::Rotation,
                             v.rotation, subs);
    if (v.slides)
      assembly::SetMateDofAt(asmDoc, v.path, v.mate, MateDof::Offset, v.offset,
                             subs);
  }
  if (value != nullptr)
    assembly::SetMateDofAt(asmDoc, s->driverPath, s->driverMate, s->driverDof,
                           *value, subs);
  // The one solve: from the recorded pose it only settles, and to a value
  // between samples it is the single step the walk would have ended on.
  SolveActiveAssemblyDriven(state, s->driverMate, s->driverDof,
                            /*diagnose=*/false, s->driverPath);
  state.syncedDocumentVersion = UINT64_MAX; // the placements changed
  return true;
}

bool ExportMotionGlb(AppState& state, uint64_t studyId, const std::string& path,
                     std::string* error, size_t* nodesOut, size_t* framesOut) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (!state.assembly)
    return fail("the active document is not an assembly");
  const MotionStudy* s = state.assembly->FindMotionStudyById(studyId);
  if (!s)
    return fail("no motion study with id " + std::to_string(studyId));
  const motion::Result* r = MotionResultFor(state, studyId);
  if (!r || r->samples.empty())
    return fail("motion study '" + s->name +
                "' has not been run — run_motion_study first; the export is "
                "the run's poses, not a guess at them");
  for (size_t i = 0; i < r->samples.size(); i++)
    if (!r->samples[i].solved)
      return fail("sample " + std::to_string(i) + " of '" + s->name +
                  "' did not solve (residual " +
                  std::to_string(r->samples[i].residualMm) +
                  " mm) — an animation that held a frame there would be "
                  "a lie; fix the mechanism and run it again");

  // One node per top-level occurrence: its leaves' meshes in ITS frame, so
  // the node's keyframes are the occurrence placements the run recorded —
  // and, inside a FLEXIBLE placement (v129), one per occurrence of it that
  // moves, keyed {root, occurrence}; what does not move rides in the root's.
  auto flat = assembly::Flatten(*state.assembly, PartResolverFor(state),
                                AssemblyResolverFor(state));
  if (!flat)
    return fail(flat.error());
  using Key = std::pair<uint64_t, uint64_t>;
  std::set<Key> moving;
  for (const motion::Sample::LeafWorld& l : r->samples.front().leaves)
    moving.insert({l.root, l.occurrence});
  std::map<Key, glb::Node> nodes;
  for (const Occurrence& o : state.assembly->GetOccurrences()) {
    if (!o.visible || state.assembly->OccurrenceSuppressed(o.id))
      continue;
    const glm::dmat4 rootWorld = OccurrenceMatrix(o);
    for (const assembly::FlatInstance& inst : *flat) {
      if (inst.rootOccurrenceId != o.id || !inst.visible || !inst.part)
        continue;
      Key key{o.id, 0};
      glm::dmat4 frame = rootWorld;
      if (!inst.innerIds.empty() && moving.count({o.id, inst.innerIds[0]})) {
        key.second = inst.innerIds[0];
        frame = rootWorld *
                (inst.levels.size() >= 2 ? inst.levels[1].local : inst.local);
      }
      glb::Node& n = nodes[key];
      if (n.name.empty())
        n.name = key.second == 0
                     ? o.name
                     : o.name + "/" +
                           (inst.levels.size() >= 2 ? inst.levels[1].name
                                                    : inst.name);
      const glm::dmat4 inv = glm::inverse(frame);
      for (const Body& b : inst.part->GetBodies()) {
        if (b.reference || !b.shape.IsValid())
          continue;
        glb::AppendShape(n.mesh, b.shape, inv * inst.world);
      }
    }
  }
  std::erase_if(
      nodes, [](const auto& kv) { return kv.second.mesh.positions.empty(); });
  // Keyframes from the run. 24 frames per second, so a 49-sample study
  // plays in two seconds: derived from the sample count, not a duration
  // someone typed.
  std::vector<double> times;
  for (const motion::Sample& sample : r->samples)
    times.push_back(sample.t * (r->samples.size() - 1) / 24.0);
  std::vector<glb::Node> out;
  for (auto& [key, n] : nodes) {
    for (const motion::Sample& sample : r->samples) {
      glm::dmat4 world(1.0);
      if (key.second == 0) {
        for (const auto& [occId, m] : sample.worlds)
          if (occId == key.first)
            world = m;
      } else {
        for (const motion::Sample::LeafWorld& l : sample.leaves)
          if (l.root == key.first && l.occurrence == key.second)
            world = l.world;
      }
      n.keyframes.push_back(world);
    }
    out.push_back(std::move(n));
  }
  if (!glb::Write(path, out, times, error))
    return false;
  if (nodesOut)
    *nodesOut = out.size();
  if (framesOut)
    *framesOut = times.size();
  return true;
}

// --- playback (docs/ui/panels.md#assembly-tab-the-model-panel) ---------------

namespace {
// kPlaybackSamplesPerSecond (MotionActions.h) is the rate the GLB export
// plays at too, so the panel and the exported file show a study at the same
// speed.
constexpr double kMaxPlaybackStep = 0.25; // s: a stalled frame does not skip

bool FindWorld(const motion::Sample& s, uint64_t occId, glm::dmat4& out) {
  for (const auto& [id, m] : s.worlds)
    if (id == occId) {
      out = m;
      return true;
    }
  return false;
}

bool FindLeaf(const motion::Sample& s, uint64_t root, uint64_t occ,
              glm::dmat4& out) {
  for (const motion::Sample::LeafWorld& l : s.leaves)
    if (l.root == root && l.occurrence == occ) {
      out = l.world;
      return true;
    }
  return false;
}

// Rigid placements: position lerped, orientation slerped, between the two
// samples either side.
glm::dmat4 Blend(const glm::dmat4& a, const glm::dmat4& b, double f) {
  const glm::dquat qa = glm::normalize(glm::quat_cast(glm::dmat3(a)));
  const glm::dquat qb = glm::normalize(glm::quat_cast(glm::dmat3(b)));
  const glm::dvec3 p = glm::mix(glm::dvec3(a[3]), glm::dvec3(b[3]), f);
  return glm::translate(glm::dmat4(1.0), p) *
         glm::mat4_cast(glm::slerp(qa, qb, f));
}

// The two samples playback stands between, and how far along.
bool PlaybackSpan(const AppState& state, const motion::Result*& r, size_t& i,
                  double& f) {
  if (state.motionPlaybackStudy == 0)
    return false;
  r = state.motionPlaybackPinned
          ? state.motionPlaybackPinned.get()
          : MotionResultFor(state, state.motionPlaybackStudy);
  if (!r || r->samples.size() < 2)
    return false;
  const size_t n = r->samples.size();
  const double pos = std::clamp(state.motionPlaybackPos, 0.0, double(n - 1));
  i = std::min(static_cast<size_t>(pos), n - 2);
  f = pos - double(i);
  return true;
}

void PushAllLive(AppState& state) {
  if (!state.assembly)
    return;
  for (const Occurrence& o : state.assembly->GetOccurrences())
    PushOccurrenceLive(state, o.id);
}
} // namespace

std::string MotionPosesRefusal(const motion::Result& r) {
  if (!r.error.empty())
    return r.error;
  if (r.cancelled)
    return "the run was cancelled";
  if (r.samples.size() < 2)
    return "the run has fewer than two samples";
  for (size_t i = 0; i < r.samples.size(); i++)
    if (!r.samples[i].solved)
      return "sample " + std::to_string(i) +
             " did not solve, so there is no pose to show there";
  return {};
}

std::shared_ptr<MotionPoses> PosesToPlay(AppState& state, uint64_t studyId,
                                         std::string* error) {
  auto poses = std::make_shared<MotionPoses>();
  if (MotionPlaybackRefusal(state, studyId).empty()) {
    // The stored run, fresh and whole: the poses Play would show. A copy,
    // so a run finishing meanwhile cannot change them under the video.
    poses->result = std::make_shared<const motion::Result>(
        *MotionResultFor(state, studyId));
    poses->done.store(true);
    return poses;
  }
  // No run to play: make one, without the collision checks — only the poses
  // are wanted, and the exact checks can take minutes on an engine.
  auto run = std::make_shared<MotionRun>();
  RunOverride preview;
  preview.checkClash = false;
  if (!Prepare(state, studyId, *run, error, /*copyDocs=*/true, &preview))
    return nullptr;
  MotionPoses* raw = poses.get();
  poses->worker = std::thread([run, raw] {
    OSD::SetSignal(Standard_False);
    auto r = std::make_shared<motion::Result>(motion::Run(
        run->asmCopy, PartsOf(*run), AssembliesOf(*run), run->study,
        [raw] { return raw->cancel.load(); }, nullptr, &run->meshes,
        run->contacts.get()));
    raw->error = MotionPosesRefusal(*r);
    raw->result = std::move(r);
    raw->done.store(true); // last
  });
  return poses;
}

void ShowMotionPoses(AppState& state, uint64_t studyId,
                     const std::shared_ptr<const motion::Result>& poses,
                     double pos) {
  if (state.motionPlaybackStudy != studyId ||
      state.motionPlaybackPinned != poses) {
    state.motionPlaybackStudy = studyId;
    state.motionPlaybackPinned = poses;
    state.syncedDocumentVersion = UINT64_MAX; // a full resync reads them too
  }
  state.motionPlaying = false; // the caller moves it
  state.motionPlaybackPos = pos;
  PushAllLive(state); // matrices only: no solve, no re-tessellation
}

std::string MotionPlaybackRefusal(AppState& state, uint64_t studyId) {
  if (!state.assembly)
    return "the active document is not an assembly";
  if (!state.assembly->FindMotionStudyById(studyId))
    return "no motion study with id " + std::to_string(studyId);
  const motion::Result* r = MotionResultFor(state, studyId);
  if (!r || r->samples.size() < 2)
    return "run it first (Preview or Validate): playback shows the poses a "
           "run made";
  if (r->cancelled)
    return "the last run was cancelled; run it again";
  if (MotionResultStale(state, studyId))
    return "the result is stale (the model or the study changed); run it "
           "again";
  for (size_t i = 0; i < r->samples.size(); i++)
    if (!r->samples[i].solved)
      return "sample " + std::to_string(i) +
             " did not solve, so there is no pose to show there";
  return {};
}

bool StartMotionPlayback(AppState& state, uint64_t studyId, std::string* why) {
  const std::string no = MotionPlaybackRefusal(state, studyId);
  if (!no.empty()) {
    if (why)
      *why = no;
    return false;
  }
  if (state.motionPlaybackStudy != studyId)
    state.motionPlaybackPos = 0.0; // a new study starts at its beginning
  state.motionPlaybackStudy = studyId;
  state.motionPlaying = true;
  state.motionPlaybackLastTick = -1.0;      // the first tick sets the clock
  state.syncedDocumentVersion = UINT64_MAX; // pickers follow the pose
  return true;
}

void StopMotionPlayback(AppState& state) {
  if (state.motionPlaybackStudy == 0)
    return;
  state.motionPlaybackStudy = 0;
  state.motionPlaybackPinned.reset();
  state.motionPlaying = false;
  state.motionPlaybackPos = 0.0;
  // Back to the document's own placements, and picking with them.
  state.syncedDocumentVersion = UINT64_MAX;
}

void TickMotionPlayback(AppState& state, double now) {
  if (state.motionPlaybackStudy == 0)
    return;
  // Poses a video export is showing: it moves them itself, and they are not
  // the stored result, so that result's staleness is no reason to stop.
  if (state.motionPlaybackPinned)
    return;
  // Anything that invalidated the result — an edit, a new run, a tab
  // switch — ends playback rather than showing poses of something else.
  if (!MotionPlaybackRefusal(state, state.motionPlaybackStudy).empty()) {
    StopMotionPlayback(state);
    return;
  }
  if (!state.motionPlaying) {
    state.motionPlaybackLastTick = -1.0;
    return;
  }
  const motion::Result* r = MotionResultFor(state, state.motionPlaybackStudy);
  const double last = static_cast<double>(r->samples.size() - 1);
  if (state.motionPlaybackLastTick >= 0.0) {
    const double dt =
        std::clamp(now - state.motionPlaybackLastTick, 0.0, kMaxPlaybackStep);
    state.motionPlaybackPos += dt * kPlaybackSamplesPerSecond;
    if (state.motionPlaybackPos > last) // loop: a mechanism runs round
      state.motionPlaybackPos = std::fmod(state.motionPlaybackPos, last);
  }
  state.motionPlaybackLastTick = now;
  PushAllLive(state); // matrices only: no solve, no re-tessellation
}

bool MotionPlaybackWorld(const AppState& state, uint64_t occurrenceId,
                         glm::dmat4& world) {
  const motion::Result* r = nullptr;
  size_t i = 0;
  double f = 0.0;
  if (!PlaybackSpan(state, r, i, f))
    return false;
  glm::dmat4 a, b;
  if (!FindWorld(r->samples[i], occurrenceId, a) ||
      !FindWorld(r->samples[i + 1], occurrenceId, b))
    return false;
  world = Blend(a, b, f);
  return true;
}

bool MotionPlaybackLocal(AppState& state, const Occurrence& root,
                         const std::vector<uint64_t>& innerPath,
                         const glm::dmat4& docLocal, glm::dmat4& local) {
  if (!root.flexible || innerPath.empty())
    return false;
  const motion::Result* r = nullptr;
  size_t i = 0;
  double f = 0.0;
  if (!PlaybackSpan(state, r, i, f))
    return false;
  glm::dmat4 la, lb, ra, rb;
  if (!FindLeaf(r->samples[i], root.id, innerPath[0], la) ||
      !FindLeaf(r->samples[i + 1], root.id, innerPath[0], lb) ||
      !FindWorld(r->samples[i], root.id, ra) ||
      !FindWorld(r->samples[i + 1], root.id, rb))
    return false;
  const AssemblyDocument* sub = AssemblyDocById(state, root.partId);
  const Occurrence* inner = sub ? sub->FindById(innerPath[0]) : nullptr;
  if (inner == nullptr)
    return false;
  // docLocal = P * rest, with P where the document has that occurrence now;
  // the played one stands at the leaf's recorded pose instead.
  const glm::dmat4 rest =
      glm::inverse(assembly::InnerPlacement(root, *inner)) * docLocal;
  local = glm::inverse(Blend(ra, rb, f)) * Blend(la, lb, f) * rest;
  return true;
}

} // namespace ui
