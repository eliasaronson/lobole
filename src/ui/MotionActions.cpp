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

#include <chrono>
#include <map>
#include <memory>
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
  std::map<uint64_t, std::unique_ptr<Document>> parts;
  std::map<uint64_t, std::unique_ptr<AssemblyDocument>> assemblies;
  std::map<const void*, std::shared_ptr<const collide::Mesh>> meshes;
  motion::Result result;
  std::shared_ptr<collide::ExactContactCache> contacts;
  std::string revision;
  std::chrono::steady_clock::time_point started;
};

// Copies of every document under the assembly, and a resolver over them.
void CopySubtree(AppState& state, MotionRun& run) {
  for (const auto& [slotId, version] :
       assembly::SubtreeVersions(*state.assembly, PartResolverFor(state),
                                 AssemblyResolverFor(state))) {
    (void)version;
    if (const Document* pd = PartDocById(state, slotId))
      run.parts[slotId] = std::make_unique<Document>(*pd);
    else if (const AssemblyDocument* ad = AssemblyDocById(state, slotId))
      run.assemblies[slotId] = std::make_unique<AssemblyDocument>(*ad);
  }
}

assembly::PartResolver PartsOf(MotionRun& run) {
  return [&run](uint64_t id) -> const Document* {
    const auto it = run.parts.find(id);
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
        run.meshes[key] = collide::Mesh::Build(b.shape);
    }
}

bool Prepare(AppState& state, uint64_t studyId, MotionRun& run,
             std::string* error, bool copyDocs) {
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
  const std::string why = motion::WhyNot(*state.assembly, *s);
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
  run.asmCopy = *state.assembly;
  run.started = std::chrono::steady_clock::now();
  // Copying a Document regenerates it (an Import re-reads its STEP), so the
  // copies are made only for a run that leaves the main thread; a
  // synchronous run reads the live documents it is blocking anyway.
  if (copyDocs) {
    CopySubtree(state, run);
    PrebuildMeshes(run);
  }
  return true;
}

} // namespace

bool StartMotionRun(AppState& state, uint64_t studyId, std::string* error) {
  if (BackgroundJobRunning(state, "motion")) {
    if (error)
      *error = "a motion study is already running (background_jobs lists "
               "it; cancel_background_job {kind: \"motion\"} stops it)";
    return false;
  }
  auto run = std::make_shared<MotionRun>();
  if (!Prepare(state, studyId, *run, error, /*copyDocs=*/true))
    return false;
  // Keep the previous result visible, with its revision, while running.

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "motion";
  bg->label = "Motion study '" + run->study.name + "'";
  bg->cancellable = true;
  BackgroundJob* raw = bg.get();
  bg->thread = std::thread([run, raw]() {
    OSD::SetSignal(Standard_False);
    run->result = motion::Run(
        run->asmCopy, PartsOf(*run), AssembliesOf(*run), run->study,
        [raw] { return raw->cancel.load(); },
        [raw](double f) { raw->fraction.store(f); }, &run->meshes,
        run->contacts.get());
    raw->done.store(true); // last
  });
  bg->finish = [run](AppState& s) {
    // Only for the document it was run against: a tab switched underneath
    // a run does not receive somebody else's collision table.
    if (s.assembly.get() == run->ranAgainst) {
      s.motionResults[run->studyId] = std::move(run->result);
      s.motionResultRevisions[run->studyId] = run->revision;
    }
    return true;
  };
  StartBackgroundJob(state, std::move(bg));
  return true;
}

motion::Result RunMotionNow(AppState& state, uint64_t studyId) {
  MotionRun run;
  std::string error;
  if (!Prepare(state, studyId, run, &error, /*copyDocs=*/false)) {
    motion::Result r;
    r.error = error;
    return r;
  }
  motion::Result r = motion::Run(run.asmCopy, PartResolverFor(state),
                                 AssemblyResolverFor(state), run.study, nullptr,
                                 nullptr, &run.meshes, run.contacts.get());
  state.motionResults[studyId] = r;
  state.motionResultRevisions[studyId] = run.revision;
  return r;
}

const motion::Result* MotionResultFor(const AppState& state, uint64_t studyId) {
  const auto it = state.motionResults.find(studyId);
  return it == state.motionResults.end() ? nullptr : &it->second;
}

bool MotionResultStale(AppState& state, uint64_t studyId) {
  const auto it = state.motionResultRevisions.find(studyId);
  return it == state.motionResultRevisions.end() ||
         it->second != Revision(state, studyId);
}

bool ApplyMotionPose(AppState& state, uint64_t studyId, double t,
                     std::string* error) {
  if (!state.assembly) {
    if (error)
      *error = "the active document is not an assembly";
    return false;
  }
  MotionStudy* s = state.assembly->FindMotionStudyById(studyId);
  if (!s) {
    if (error)
      *error = "no motion study with id " + std::to_string(studyId);
    return false;
  }
  const std::string why = motion::WhyNot(*state.assembly, *s);
  if (!why.empty()) {
    if (error)
      *error = why;
    return false;
  }
  Mate* driver = state.assembly->FindMateById(s->driverMate);
  const double target = motion::ValueAt(*s, t);

  // WALK to the pose, in the study's own steps, rather than jumping to it.
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
  const double current = assembly::MateDofValue(*driver, s->driverDof);
  const double span = s->to - s->from;
  const int steps = std::max(s->samples - 1, 1);
  const double stride = std::abs(span) / steps;
  int walk = 1;
  if (stride > 1e-12)
    walk = static_cast<int>(std::ceil(std::abs(target - current) / stride));
  walk = std::clamp(walk, 1, steps);
  for (int k = 1; k <= walk; k++) {
    const double v = current + (target - current) * (double(k) / walk);
    assembly::SetMateDof(*driver, s->driverDof, v);
    assembly::PropagateRelations(*state.assembly, driver->id);
    SolveActiveAssemblyDriven(state, driver->id, s->driverDof);
  }
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
  // the node's keyframes are the occurrence placements the run recorded.
  auto flat = assembly::Flatten(*state.assembly, PartResolverFor(state),
                                AssemblyResolverFor(state));
  if (!flat)
    return fail(flat.error());
  std::map<uint64_t, glb::Node> nodes;
  for (const Occurrence& o : state.assembly->GetOccurrences()) {
    if (!o.visible || state.assembly->OccurrenceSuppressed(o.id))
      continue;
    glb::Node n;
    n.name = o.name;
    const glm::mat4 rootInv = glm::inverse(OccurrenceMatrix(o));
    for (const assembly::FlatInstance& inst : *flat) {
      if (inst.rootOccurrenceId != o.id || !inst.visible || !inst.part)
        continue;
      for (const Body& b : inst.part->GetBodies()) {
        if (b.reference || !b.shape.IsValid())
          continue;
        glb::AppendShape(n.mesh, b.shape, rootInv * inst.world);
      }
    }
    if (n.mesh.positions.empty())
      continue;
    nodes[o.id] = std::move(n);
  }
  // Keyframes from the run. 24 frames per second, so a 49-sample study
  // plays in two seconds: derived from the sample count, not a duration
  // someone typed.
  std::vector<double> times;
  for (const motion::Sample& sample : r->samples)
    times.push_back(sample.t * (r->samples.size() - 1) / 24.0);
  std::vector<glb::Node> out;
  for (auto& [id, n] : nodes) {
    for (const motion::Sample& sample : r->samples) {
      glm::mat4 world(1.0f);
      for (const auto& [occId, m] : sample.worlds)
        if (occId == id)
          world = m;
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

} // namespace ui
