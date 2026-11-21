// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SimRunActions.h"

#include "cad/CADKernel.h" // ArmOccSignalHandlers
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/fem/FemCheck.h"
#include "core/fem/SimDocument.h"
#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/DocumentSlots.h"
#include "ui/SimulationActions.h"
#include "ui/SimulationView.h"
#include "ui/SimulationViewInternal.h"
#include "ui/TopoJob.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ui {

namespace {

bool Fail(std::string* error, const std::string& why) {
  if (error)
    *error = why;
  return false;
}

double Seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since)
      .count();
}

// A study run is one at a time, whichever document it is on.
bool AStudyRunIsRunning(const AppState& state) {
  return BackgroundJobRunning(state, "topo") ||
         BackgroundJobRunning(state, "study") ||
         BackgroundJobRunning(state, "check");
}

std::string SimStudyLabel(const fem::SimDocument& sim, uint64_t studyId,
                          const char* verb) {
  const fem::Study* s = sim.FindStudy(studyId);
  return std::string(verb) + " '" +
         (s != nullptr && !s->name.empty()
              ? s->name
              : "study " + std::to_string(studyId)) +
         "'";
}

// Where a finished run lands: the simulation it was started from, even
// with another tab in front. Null (and said) when that tab is gone.
fem::SimDocument* LandingSim(AppState& state, uint64_t simSlotId,
                             const char* what) {
  fem::SimDocument* sim = SimBySlotId(state, simSlotId);
  if (sim == nullptr)
    LOG_WARN("{}: the simulation it ran on was closed before it finished; "
             "the result is dropped",
             what);
  return sim;
}

} // namespace

fem::SimDocument* SimBySlotId(AppState& state, uint64_t simSlotId) {
  const std::optional<size_t> at = SlotIndexById(state, simSlotId);
  if (!at || state.parts[*at].kind != DocKind::Simulation)
    return nullptr;
  return SlotSim(state, *at);
}

uint64_t ActiveSimSlotId(const AppState& state) {
  if (!state.sim || state.activePart < 0 ||
      static_cast<size_t>(state.activePart) >= state.parts.size())
    return 0;
  const PartSlot& slot = state.parts[state.activePart];
  return slot.kind == DocKind::Simulation ? slot.id : 0;
}

uint64_t SimStampOf(AppState& state, uint64_t docId) {
  if (const Document* part = PartDocById(state, docId))
    return PartStamp(*part);
  if (const AssemblyDocument* asmDoc = AssemblyDocById(state, docId))
    return asmDoc->Version();
  return 0;
}

fem::SimContext SimContextFor(AppState& state, const fem::SimDocument& sim) {
  fem::SimContext ctx;
  ctx.sim = &sim;
  ctx.symbols = &state.projectSymbols;
  const fem::SimSubject& subject = sim.Subject();
  if (subject.kind == fem::SubjectKind::Part) {
    // Configuration 0 is the part's active one: the live document.
    if (PartDocById(state, subject.docId) == nullptr)
      return ctx; // gone: the planner says the subject is missing
    ctx.part = PartResolverFor(state)(subject.docId, subject.configuration);
    if (ctx.part != nullptr)
      ctx.symbols = &ctx.part->Symbols();
    ctx.subjectStamps.push_back(
        {subject.docId, SimStampOf(state, subject.docId)});
    return ctx;
  }
  const AssemblyDocument* asmDoc = AssemblyDocById(state, subject.docId);
  if (asmDoc == nullptr)
    return ctx;
  ctx.assembly = asmDoc;
  // Walked once for the context: every participant's bodies and part read
  // the same flattening, placed in the subject (attachments are already in
  // the occurrences' poses; explode and motion offsets are display only).
  auto flat = std::make_shared<std::vector<assembly::FlatInstance>>();
  if (auto walked = assembly::Flatten(*asmDoc, PartResolverFor(state),
                                      AssemblyResolverFor(state)))
    *flat = std::move(*walked);
  else
    LOG_ERROR("Simulation subject '{}' cannot be walked: {}", subject.name,
              walked.error());
  ctx.placedBodies = [flat](uint64_t occurrenceId) {
    std::vector<Body> out;
    for (const assembly::FlatInstance& inst : *flat) {
      if (!inst.visible || inst.part == nullptr ||
          inst.rootOccurrenceId != occurrenceId)
        continue;
      for (const Body& b : inst.part->GetBodies())
        if (!b.reference) // a derived ghost is not the occurrence's matter
          out.push_back(PlacedBody(b, inst.world));
    }
    return out;
  };
  ctx.partOf = [flat](uint64_t occurrenceId) {
    // A part placed by the subject itself: the leaf whose own placement is
    // the top-level occurrence. A sub-assembly's leaves sit below it.
    for (const assembly::FlatInstance& inst : *flat)
      if (inst.rootOccurrenceId == occurrenceId && inst.levels.empty() &&
          inst.occurrenceId == occurrenceId)
        return fem::SimPart{inst.part, inst.world};
    return fem::SimPart{};
  };
  // The assembly, every sub-assembly it places and every part: each one's
  // edit moves what is solved.
  ctx.subjectStamps.push_back({subject.docId, asmDoc->Version()});
  const auto add = [&](uint64_t id) {
    for (const auto& [have, stamp] : ctx.subjectStamps)
      if (have == id)
        return;
    ctx.subjectStamps.push_back({id, SimStampOf(state, id)});
  };
  for (const assembly::FlatInstance& inst : *flat) {
    for (const assembly::FlatInstance::Level& level : inst.levels)
      add(level.assemblyId);
    add(inst.partId);
  }
  return ctx;
}

bool SimStudyStale(AppState& state, const fem::SimDocument& sim,
                   uint64_t studyId) {
  return sim.StudyResultsStale(
      studyId, [&state](uint64_t docId) { return SimStampOf(state, docId); });
}

uint64_t SimStudyInputHash(AppState& state, const fem::SimDocument& sim,
                           uint64_t studyId, const JobStart* job) {
  const fem::Study* s = sim.FindStudy(studyId);
  if (s == nullptr)
    return 0;
  const fem::ConvergeOptions o =
      ConvergeOptionsFor(state, sim.Effective(*s), job);
  if (o.mode == s->converge || job == nullptr || job->converge < 0)
    return fem::RunInputKey(
        fem::SimStudyInputKey(SimContextFor(state, sim), studyId), o);
  // The run's mode in place of the study's: an input like any other.
  fem::SimDocument withMode = sim; // shares the store; only the study moves
  withMode.StudyForRun(studyId)->converge = o.mode;
  return fem::RunInputKey(
      fem::SimStudyInputKey(SimContextFor(state, withMode), studyId), o);
}

bool ReuseSimStudyResult(AppState& state, uint64_t simSlotId, uint64_t studyId,
                         JobStart* job) {
  if (job != nullptr && job->recompute)
    return false;
  fem::SimDocument* sim = SimBySlotId(state, simSlotId);
  if (sim == nullptr || sim->FindStudy(studyId) == nullptr)
    return false;
  const uint64_t hash = SimStudyInputHash(state, *sim, studyId, job);
  if (hash == 0)
    return false;
  const fem::StudyRuntime* own = sim->StudyResults(studyId);
  std::shared_ptr<const fem::StudyRuntime> stored;
  if (own != nullptr && own->inputHash == hash && !own->cancelled)
    stored = std::make_shared<const fem::StudyRuntime>(*own);
  else
    stored = state.studyResultsByInput.Find(hash);
  if (!stored || stored->cancelled)
    return false;
  const fem::SimContext ctx = SimContextFor(state, *sim);
  fem::StoreSimStudyResult(*sim, studyId, *stored, hash, ctx.subjectStamps);
  if (job != nullptr)
    job->reused = true;
  return true;
}

bool RunSimStudyAndCache(AppState& state, uint64_t simSlotId, uint64_t studyId,
                         std::string* error, double timeoutSeconds,
                         const JobStart* job) {
  fem::SimDocument* sim = SimBySlotId(state, simSlotId);
  if (sim == nullptr)
    return Fail(error, "no such simulation");
  fem::Study* s = sim->StudyForRun(studyId);
  if (s == nullptr)
    return Fail(error, "no such study");
  const fem::Study effective = sim->Effective(*s);
  fem::SolveOptions options;
  if (!CalculixRunFor(state, effective, timeoutSeconds, options, error)) {
    s->error = error ? *error : "";
    return false;
  }
  const fem::SimContext ctx = SimContextFor(state, *sim);
  const auto started = std::chrono::steady_clock::now();
  if (!options.calculix && s->type == fem::AnalysisType::Topology) {
    fem::StudyRuntime rt;
    if (!fem::RunSimStudy(*sim, ctx, studyId, rt, error))
      return false;
    rt.solveSeconds = Seconds(started);
    RememberStudyResult(state, rt);
    return true;
  }
  if (!options.calculix)
    OfferGpuSolver(state, options);
  options.cancelled = state.rpcCallerGone;
  const fem::ConvergeOptions converge =
      ConvergeOptionsFor(state, effective, job);
  const uint64_t inputHash = SimStudyInputHash(state, *sim, studyId, job);
  fem::StudyRuntime rt;
  if (!fem::RunSimStudy(*sim, ctx, studyId, rt, converge, options, error))
    return false;
  // RunSimStudy stamped it with the study's own key; a run's override (or
  // the layer normal) is part of what it answers.
  rt.inputHash = inputHash;
  rt.solveSeconds = Seconds(started);
  rt.solver = SolverOf(rt, effective.type);
  RememberStudyResult(state, rt); // whole, before the store cuts it
  fem::StoreSimStudyResult(*sim, studyId, rt, inputHash, ctx.subjectStamps);
  return true;
}

bool StartSimStudyRun(AppState& state, uint64_t simSlotId, uint64_t studyId,
                      std::string* error, JobStart* jobStart) {
  fem::SimDocument* sim = SimBySlotId(state, simSlotId);
  if (sim == nullptr)
    return Fail(error, "no such simulation");
  fem::Study* s = sim->StudyForRun(studyId);
  if (s == nullptr)
    return Fail(error, "no such study");
  if (s->type == fem::AnalysisType::Topology)
    return Fail(error, "topology studies run through the topology path");
  if (ReuseSimStudyResult(state, simSlotId, studyId, jobStart))
    return true;
  if (AStudyRunIsRunning(state))
    return Fail(error, "a study run is already in progress");
  const fem::Study effective = sim->Effective(*s);
  fem::SolveOptions calculix;
  if (!CalculixRunFor(state, effective,
                      jobStart ? jobStart->timeoutSeconds : 0.0, calculix,
                      error)) {
    s->error = error ? *error : "";
    return false;
  }
  const fem::ConvergeOptions converge =
      ConvergeOptionsFor(state, effective, jobStart);
  const uint64_t inputHash = SimStudyInputHash(state, *sim, studyId, jobStart);
  // Planned here: the subject's documents are read on this thread only.
  const fem::SimContext ctx = SimContextFor(state, *sim);
  auto job = std::make_shared<StudyJob>();
  if (!fem::PlanSimStudy(ctx, studyId, job->plan, error)) {
    s->error = error ? *error : "";
    return false;
  }
  job->studyId = studyId;
  job->partVersions = ctx.subjectStamps;
  job->started = std::chrono::steady_clock::now();

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "study";
  bg->label = SimStudyLabel(*sim, studyId, "Solving");
  bg->cancellable = true; // at its boundaries, as StartStudyRun's
  bg->studyId = studyId;
  bg->owner = jobStart ? jobStart->owner : 0;
  BackgroundJob* raw = bg.get();
  fem::SolveOptions solveOptions = JobSolveOptions(state, raw, job.get());
  solveOptions.calculix = WithEstimateTo(calculix.calculix, job.get(), raw);
  bg->thread = std::thread([job, raw, solveOptions, converge]() {
    ArmOccSignalHandlers();
    std::string err;
    if (!fem::SolveConverged(job->plan, job->input, job->runtime, converge,
                             &err, solveOptions) &&
        !raw->cancel.load())
      job->error = err.empty() ? "solve failed" : err;
    raw->error = job->error;
    raw->done.store(true); // last
  });
  bg->finish = [job, raw, inputHash, simSlotId](AppState& st) {
    StudyJob& j = *job;
    if (fem::SimDocument* target = LandingSim(st, simSlotId, "study solve")) {
      if (raw->cancel.load()) {
        // Cancelled: whatever it reached is not the answer it was asked for.
      } else if (j.error.empty()) {
        fem::StoreSimMadeMeshes(*target, j.plan, j.input);
        j.runtime.solver = SolverOf(j.runtime, j.input.type);
        j.runtime.solveSeconds = Seconds(j.started);
        j.runtime.inputHash = inputHash;
        RememberStudyResult(st, j.runtime);
        fem::StoreSimStudyResult(*target, j.studyId, std::move(j.runtime),
                                 inputHash, j.partVersions);
      } else if (fem::Study* found = target->StudyForRun(j.studyId)) {
        found->error = j.error;
      }
    }
    if (st.studyJob == job)
      st.studyJob.reset();
    return true;
  };
  state.studyJob = job;
  BackgroundJob& started = StartBackgroundJob(state, std::move(bg));
  if (jobStart)
    jobStart->jobId = started.id;
  return true;
}

bool StartSimTopologyRun(AppState& state, uint64_t simSlotId, uint64_t studyId,
                         std::string* error, JobStart* jobStart) {
  fem::SimDocument* sim = SimBySlotId(state, simSlotId);
  if (sim == nullptr)
    return Fail(error, "no such simulation");
  fem::Study* s = sim->StudyForRun(studyId);
  if (s == nullptr)
    return Fail(error, "no such study");
  if (s->type != fem::AnalysisType::Topology)
    return Fail(error, "study is not a topology study");
  if (ReuseSimStudyResult(state, simSlotId, studyId, jobStart))
    return true;
  if (AStudyRunIsRunning(state))
    return Fail(error, "a study run is already in progress");
  const fem::SimContext ctx = SimContextFor(state, *sim);
  const uint64_t inputHash = fem::SimStudyInputKey(ctx, studyId);
  auto job = std::make_shared<TopoJob>();
  if (!fem::PrepareSimTopology(ctx, studyId, job->input, error)) {
    s->error = error ? *error : "";
    return false;
  }
  job->studyId = studyId;
  job->maxIterations = job->input.params.maxIterations;
  PickTopologySolver(state, *job);
  job->started = std::chrono::steady_clock::now();

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "topo";
  bg->label = SimStudyLabel(*sim, studyId, "Optimizing");
  bg->cancellable = true; // between iterations, keeping the best so far
  bg->studyId = studyId;
  bg->owner = jobStart ? jobStart->owner : 0;
  LaunchTopologyWorker(*bg, job);
  bg->finish = [job, inputHash, simSlotId,
                stamps = ctx.subjectStamps](AppState& st) {
    TopoJob& j = *job;
    if (fem::SimDocument* target =
            LandingSim(st, simSlotId, "topology optimization")) {
      if (j.result) {
        fem::StudyRuntime rt = TopologyRuntimeOf(j);
        rt.inputHash = inputHash;
        RememberStudyResult(st, rt);
        fem::StoreSimStudyResult(*target, j.studyId, std::move(rt), inputHash,
                                 stamps);
      } else if (fem::Study* found = target->StudyForRun(j.studyId)) {
        found->error = "body '" + j.input.bodyName + "': " + j.error;
      }
    }
    if (st.topoJob == job)
      st.topoJob.reset();
    return true;
  };
  state.topoJob = job;
  BackgroundJob& started = StartBackgroundJob(state, std::move(bg));
  if (jobStart)
    jobStart->jobId = started.id;
  return true;
}

std::string SimMeshKeyNow(AppState& state, const fem::SimDocument& sim,
                          const fem::Study& study) {
  std::string key = MeshSettingsKey(sim.Effective(study));
  const fem::SimSubject& subject = sim.Subject();
  key += "subject=" + std::to_string(subject.docId) + "/" +
         std::to_string(subject.configuration) + "@" +
         std::to_string(SimStampOf(state, subject.docId)) + ";";
  if (subject.kind == fem::SubjectKind::Assembly)
    // Every part and assembly: cheaper than walking the tree for which it
    // places (SimRenderVersion's rule).
    for (size_t i = 0; i < state.parts.size(); i++)
      if (state.parts[i].kind == DocKind::Part ||
          state.parts[i].kind == DocKind::Assembly)
        key += std::to_string(state.parts[i].id) + "@" +
               std::to_string(SimStampOf(state, state.parts[i].id)) + ";";
  return key;
}

namespace {
// The report kept for a mesh, keyed as it was planned: `settingsKey` is
// SimMeshKeyNow taken then.
MeshReport ReportOf(fem::SimMeshRun& run, const fem::SimDocument& sim,
                    std::string settingsKey) {
  MeshReport report =
      MakeMeshReport(std::move(run.meshes), std::move(run.reports));
  report.docVersion = sim.Version();
  report.settingsKey = std::move(settingsKey);
  return report;
}
} // namespace

bool MeshSimStudyNow(AppState& state, uint64_t simSlotId, uint64_t studyId,
                     std::string* error) {
  fem::SimDocument* sim = SimBySlotId(state, simSlotId);
  if (sim == nullptr)
    return Fail(error, "no such simulation");
  fem::Study* s = sim->StudyForRun(studyId);
  if (s == nullptr)
    return Fail(error, "no such study");
  const auto started = std::chrono::steady_clock::now();
  const std::string settingsKey = SimMeshKeyNow(state, *sim, *s);
  fem::SimMeshRun run;
  std::string err;
  if (!fem::PlanSimMesh(SimContextFor(state, *sim), studyId, run, &err) ||
      !fem::MeshSimPlanned(run, &err)) {
    s->error = err;
    return Fail(error, err);
  }
  fem::StoreSimMesh(*sim, run);
  MeshReport report = ReportOf(run, *sim, settingsKey);
  report.seconds = Seconds(started);
  KeepStudyMesh(state, simSlotId, studyId, std::move(report));
  return true;
}

bool StartSimMeshRun(AppState& state, uint64_t simSlotId, uint64_t studyId,
                     std::string* error, JobStart* jobStart) {
  if (BackgroundJobRunning(state, "mesh"))
    return Fail(error, "a mesh run is already in progress");
  fem::SimDocument* sim = SimBySlotId(state, simSlotId);
  if (sim == nullptr)
    return Fail(error, "no such simulation");
  fem::Study* s = sim->StudyForRun(studyId);
  if (s == nullptr)
    return Fail(error, "no such study");
  auto run = std::make_shared<fem::SimMeshRun>();
  if (!fem::PlanSimMesh(SimContextFor(state, *sim), studyId, *run, error)) {
    s->error = error ? *error : "";
    return false;
  }
  // The status study_status reads (GetMeshRunStatus); the meshes ride in
  // `run`.
  auto job = std::make_shared<MeshJob>();
  job->studyId = studyId;
  job->settingsKey = SimMeshKeyNow(state, *sim, *s);
  job->started = std::chrono::steady_clock::now();
  state.meshReports.erase({simSlotId, studyId});

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "mesh";
  bg->label = SimStudyLabel(*sim, studyId, "Meshing");
  bg->cancellable = true; // lands when the mesh does, and is dropped
  bg->studyId = studyId;
  bg->owner = jobStart ? jobStart->owner : 0;
  BackgroundJob* raw = bg.get();
  bg->thread = std::thread([run, job, raw]() {
    ArmOccSignalHandlers();
    std::string err;
    if (!fem::MeshSimPlanned(*run, &err))
      job->error = err.empty() ? "meshing failed" : err;
    raw->error = job->error;
    raw->done.store(true); // last
  });
  bg->finish = [run, job, raw, simSlotId](AppState& st) {
    MeshJob& j = *job;
    if (!raw->cancel.load())
      if (fem::SimDocument* target = LandingSim(st, simSlotId, "meshing")) {
        if (j.error.empty())
          fem::StoreSimMesh(*target, *run);
        else if (fem::Study* found = target->StudyForRun(j.studyId))
          found->error = j.error;
        MeshReport report = ReportOf(*run, *target, j.settingsKey);
        report.error = j.error;
        report.seconds = Seconds(j.started);
        KeepStudyMesh(st, simSlotId, j.studyId, std::move(report));
      }
    if (st.meshJob == job)
      st.meshJob.reset();
    return true;
  };
  state.meshJob = job;
  BackgroundJob& started = StartBackgroundJob(state, std::move(bg));
  if (jobStart)
    jobStart->jobId = started.id;
  return true;
}

const MeshReport* SimMeshReportFor(const AppState& state, uint64_t simSlotId,
                                   uint64_t studyId) {
  const auto it = state.meshReports.find({simSlotId, studyId});
  return it == state.meshReports.end() ? nullptr : &it->second;
}

std::vector<fem::StudyProblem>
CheckSimStudyNow(AppState& state, uint64_t simSlotId, uint64_t studyId) {
  std::vector<fem::StudyProblem> out;
  fem::SimDocument* sim = SimBySlotId(state, simSlotId);
  const fem::Study* s = sim ? sim->FindStudy(studyId) : nullptr;
  if (s == nullptr)
    return out;
  const fem::SimContext ctx = SimContextFor(state, *sim);
  // The mesh on show, while it is the one a run would solve on.
  std::vector<fem::CheckMesh> meshes;
  const MeshReport* mr = SimMeshReportFor(state, simSlotId, studyId);
  const bool stale = mr != nullptr && mr->error.empty() &&
                     mr->settingsKey != SimMeshKeyNow(state, *sim, *s);
  if (mr != nullptr && mr->error.empty() && !stale)
    for (const MeshReport::Body& b : mr->bodies)
      meshes.push_back({b.bodyIndex, b.mesh.get()});
  out = fem::CheckSimStudy(ctx, studyId, meshes.empty() ? nullptr : &meshes);
  if (stale) {
    fem::StudyProblem p;
    p.code = "stale_mesh";
    p.message = "the mesh on show was made before the subject or the mesh "
                "settings changed: a run meshes again, and what is drawn is "
                "not what it will solve";
    p.fixMethod = "mesh_study";
    p.fixParams = {{"id", s->id}};
    out.push_back(std::move(p));
  }
  const fem::Study effective = sim->Effective(*s);
  if (fem::EffectiveSolver(effective) == fem::Solver::Calculix) {
    fem::SolveOptions options;
    std::string err;
    if (!CalculixRunFor(state, effective, 0.0, options, &err)) {
      fem::StudyProblem p;
      p.code = "solver";
      p.message = err;
      out.push_back(std::move(p));
    }
  }
  return out;
}

uint64_t SimBodyOccurrence(AppState& state, size_t bodyIndex) {
  SimSubjectBodies(state); // fills the map beside the bodies
  return bodyIndex < state.simBodyOccurrences.size()
             ? state.simBodyOccurrences[bodyIndex]
             : 0;
}

} // namespace ui
