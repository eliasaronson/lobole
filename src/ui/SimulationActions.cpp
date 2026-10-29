// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SimulationActions.h"
#include "core/io/Serialization.h" // StudyToJson (undo pairs, contact key)
#include "ui/AssemblyActions.h"
#include "ui/PrintJobActions.h"
#include "ui/StudyHost.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/MassProps.h"
#include "core/Parallel.h"
#include "core/fem/Calculix.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/MeshRenderer.h"
#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/MotionActions.h"
#ifdef HAVE_FEM_GPU
#include "femgpu/TopoGpuSolver.h"
#endif

#include "cad/CADKernel.h" // ArmOccSignalHandlers
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ui {

void AppendStudyMarker(const AppState& state,
                       std::vector<LineVertex3D>& lines) {
  const SimulationState& sim = state.simulation;
  // The peak marker only applies to the FEM scalar fields (max von Mises / max
  // displacement). Topology views (Shape/Density) have no such peak — and a
  // topology runtime's maxVm/DispAt are (0,0,0), which would draw a stray cross
  // at the origin.
  const StudyHost host = StudyHostOf(const_cast<AppState&>(state));
  if (!sim.IsActive() ||
      (sim.field != SimulationState::Field::VonMises &&
       sim.field != SimulationState::Field::Displacement) ||
      !host)
    return;
  const fem::StudyRuntime* rt = host.Results(sim.activeStudyId);
  if (!rt)
    return;
  // A magenta cross at the peak of the displayed field (max von Mises or max
  // displacement), warped by the same deform scale so it tracks the overlay.
  const bool vm = sim.field == SimulationState::Field::VonMises;
  // The peak of the case on show (the only one without cases).
  const int k = rt->caseNames.empty() ? -1 : sim.activeCase;
  glm::vec3 at(0.0f);
  double best = -1.0;
  for (const fem::StudyBodyResult& b : rt->bodies) {
    const fem::FemResults& r = b.Case(k);
    const double v = vm ? r.maxVonMises : r.maxDisplacement;
    const int node = vm ? r.maxVmNode : r.maxDispNode;
    if (v <= best || node < 0 || node >= (int)r.displacement.size())
      continue;
    best = v;
    // Warped by the same deform scale so it tracks the overlay.
    at = glm::vec3(b.mesh.nodes[node]) +
         sim.deformScale * glm::vec3(r.displacement[node]);
  }
  if (best < 0.0)
    return;
  const float L =
      state.camera ? std::max(0.06f * state.camera->GetDistance(), 0.1f) : 1.0f;
  const glm::vec3 col(1.0f, 0.1f, 0.8f); // magenta, distinct from CoG amber
  const glm::vec3 ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (const glm::vec3& a : ax) {
    lines.push_back(LineVertex(at - a * L, col));
    lines.push_back(LineVertex(at + a * L, col));
  }
}
bool RunStudyAndCache(Document& doc, uint64_t studyId, std::string* error) {
  fem::Study* s = doc.StudyForRun(studyId);
  if (!s) {
    if (error)
      *error = "no such study";
    return false;
  }
  fem::StudyRuntime rt;
  const uint64_t inputHash = fem::StudyInputKey(doc, *s);
  // A topology study runs the SIMP optimizer (minutes, serialized result); a
  // static/modal study runs the linear solver (seconds, transient result).
  const bool ok = s->type == fem::AnalysisType::Topology
                      ? fem::RunTopology(doc, *s, rt, error)
                      : fem::RunStudy(doc, *s, rt, error);
  if (!ok)
    return false;
  rt.solvedAtVersion = doc.Version();
  rt.inputHash = inputHash;
  doc.StoreStudyResults(studyId, std::move(rt));
  return true;
}

bool CalculixRunFor(const AppState& state, const fem::Study& s,
                    double timeoutSeconds, fem::SolveOptions& options,
                    std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (const std::string why = fem::SolverRefusal(s.solver, s.type);
      !why.empty())
    return fail(why);
  if (s.solver != fem::Solver::Calculix) {
    if (timeoutSeconds > 0)
      return fail("timeout_s bounds a CalculiX run, and this study is solved "
                  "in-house (set_study solver: calculix to run it on "
                  "CalculiX)");
    options.calculix = nullptr;
    return true;
  }
  std::string why;
  const auto ccx = fem::FindCalculix(state.settings.calculixPath, &why);
  if (!ccx)
    return fail(why);
  auto run = std::make_shared<fem::CalculixRun>();
  run->ccx = *ccx;
  run->runRoot = (RuntimePaths::UserCacheDir() / "calculix").string();
  run->threads = static_cast<int>(parallel::MachineWorkers());
  run->timeoutSeconds = timeoutSeconds;
  options.calculix = std::move(run);
  return true;
}

bool RunStudyAndCache(AppState& state, Document& doc, uint64_t studyId,
                      std::string* error, double timeoutSeconds) {
  fem::Study* s = doc.StudyForRun(studyId);
  if (!s) {
    if (error)
      *error = "no such study";
    return false;
  }
  fem::SolveOptions options;
  if (!CalculixRunFor(state, *s, timeoutSeconds, options, error)) {
    s->error = error ? *error : "";
    return false;
  }
  if (!options.calculix)
    return RunStudyAndCache(doc, studyId, error);
  // On CalculiX: the in-house pipeline up to the solve, then ccx.
  const uint64_t inputHash = fem::StudyInputKey(doc, *s);
  fem::StudyJobInput input;
  fem::StudyRuntime rt;
  if (!fem::PrepareStudy(doc, *s, input, error))
    return false;
  options.cancelled = state.rpcCallerGone;
  const auto started = std::chrono::steady_clock::now();
  if (!fem::SolvePreparedStudy(input, rt, error, options)) {
    s->error = error ? *error : "solve failed";
    return false;
  }
  rt.solvedAtVersion = doc.Version();
  rt.inputHash = inputHash;
  rt.solveSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  doc.StoreStudyResults(studyId, std::move(rt));
  return true;
}

uint64_t StudyInputHash(AppState& state, const Document* doc,
                        const AssemblyDocument* asmDoc, uint64_t studyId) {
  if (doc != nullptr) {
    const fem::Study* s = doc->FindStudy(studyId);
    return s ? fem::StudyInputKey(*doc, *s) : 0;
  }
  if (asmDoc != nullptr) {
    const fem::Study* s = asmDoc->FindStudy(studyId);
    return s ? fem::AssemblyStudyInputKey(*asmDoc, *s,
                                          AssemblyStudyContextFor(state))
             : 0;
  }
  return 0;
}

bool ReuseStudyResult(AppState& state, Document* doc, AssemblyDocument* asmDoc,
                      uint64_t studyId, JobStart* job) {
  if (job != nullptr && job->recompute)
    return false;
  const uint64_t hash = StudyInputHash(state, doc, asmDoc, studyId);
  if (hash == 0)
    return false;
  // The study's own result first (a reopened project's topology result is
  // there, with the hash it was saved with), then the session's.
  const fem::StudyRuntime* own =
      doc ? doc->StudyResults(studyId) : asmDoc->StudyResults(studyId);
  std::shared_ptr<const fem::StudyRuntime> stored;
  if (own != nullptr && own->inputHash == hash && !own->cancelled)
    stored = std::make_shared<const fem::StudyRuntime>(*own);
  else
    stored = state.studyResultsByInput.Find(hash);
  if (!stored || stored->cancelled)
    return false;
  fem::StudyRuntime rt = *stored;
  if (doc != nullptr) {
    rt.solvedAtVersion = doc->Version();
    doc->StoreStudyResults(studyId, std::move(rt));
  } else {
    const fem::Study* s = asmDoc->FindStudy(studyId);
    rt.solvedAtVersion = asmDoc->Version();
    rt.solvedPartVersions = AssemblyStudyPartVersions(state, *asmDoc, *s);
    asmDoc->StoreStudyResults(studyId, std::move(rt));
  }
  if (job != nullptr)
    job->reused = true;
  return true;
}

void RememberStudyResult(AppState& state, const fem::StudyRuntime& rt) {
  if (rt.inputHash != 0 && !rt.cancelled)
    state.studyResultsByInput.Put(
        rt.inputHash, std::make_shared<const fem::StudyRuntime>(rt));
}

namespace {

// What a study job's worker solves with: an iterative solve stops when the
// job is cancelled (a factorisation still cannot), the job learns which
// solver it is waiting on, and a solve large enough to go iterative tries
// the GPU first, under the rules topology's does (StartTopologyRun):
// LOBOLE_TOPO_GPU=0 keeps it on the CPU, =1 admits a software device, and
// the user's preferred device is asked for. The device is made on the
// worker, and only for a solve that wants one.
fem::SolveOptions JobSolveOptions(const AppState& state, BackgroundJob* raw,
                                  StudyJob* job) {
  fem::SolveOptions o;
  o.cancelled = [raw] { return raw->cancel.load(); };
  if (job != nullptr)
    o.solving = [job](const char* name) { job->solverNow.store(name); };
#ifdef HAVE_FEM_GPU
  const char* pref = std::getenv("LOBOLE_TOPO_GPU");
  if (!pref || std::string(pref) != "0") {
    const bool allowSoftware = pref && std::string(pref) == "1";
    o.makeExternal =
        [allowSoftware, device = state.settings.gpuDevice](
            std::string* why) -> std::unique_ptr<fem::TopoLinearSolver> {
      return gpu::TopoGpuSolver::Create(allowSoftware, why, device);
    };
  }
#else
  (void)state;
#endif
  return o;
}

// A CalculiX run that tells `job` its memory estimate, for study_status.
std::shared_ptr<const fem::CalculixRun>
WithEstimateTo(const std::shared_ptr<const fem::CalculixRun>& run,
               StudyJob* job) {
  if (!run)
    return nullptr;
  auto told = std::make_shared<fem::CalculixRun>(*run);
  told->estimated = [job](uint64_t bytes) { job->memoryEstimate.store(bytes); };
  return told;
}

// The solver a result records: the one that ran, which a static solve
// reports, else the analysis type's.
std::string SolverOf(const fem::StudyRuntime& rt, fem::AnalysisType type) {
  return rt.solver.empty() ? std::string(fem::CpuSolverName(type)) : rt.solver;
}

// The part slot `doc` belongs to: the active one (its slot holds no
// document while it is active) or a stashed one. 0 when `doc` is not a part
// tab's document at all.
uint64_t SlotIdOfDoc(const AppState& state, const Document& doc) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    const PartSlot& slot = state.parts[i];
    if (slot.kind != DocKind::Part)
      continue;
    const Document* d = slot.doc                ? slot.doc.get()
                        : i == state.activePart ? state.document.get()
                                                : nullptr;
    if (d == &doc)
      return slot.id;
  }
  return 0;
}

// Where a finished job stores its result: the document it was started
// from, even when another tab is active now. Null when that tab is gone.
Document* JobDocument(AppState& state, uint64_t partSlotId, const char* what) {
  Document* doc = PartDocById(state, partSlotId);
  if (doc == nullptr)
    LOG_WARN("{}: the part it ran on was closed before it finished; the "
             "result is dropped",
             what);
  return doc;
}

std::string StudyLabel(const Document& doc, uint64_t studyId,
                       const char* verb) {
  const fem::Study* s = doc.FindStudy(studyId);
  return std::string(verb) + " '" +
         (s != nullptr && !s->name.empty()
              ? s->name
              : "study " + std::to_string(studyId)) +
         "'";
}

double Elapsed(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since)
      .count();
}

} // namespace

bool TopologyGpuBuiltIn() {
#ifdef HAVE_FEM_GPU
  return true;
#else
  return false;
#endif
}

bool StartTopologyRun(AppState& state, Document& doc, uint64_t studyId,
                      std::string* error, JobStart* jobStart) {
  fem::Study* s = doc.StudyForRun(studyId);
  if (!s) {
    if (error)
      *error = "no such study";
    return false;
  }
  if (s->type != fem::AnalysisType::Topology) {
    if (error)
      *error = "study is not a topology study";
    return false;
  }
  // A stored result for these very inputs answers first: it starts nothing,
  // so it waits for nothing.
  if (ReuseStudyResult(state, &doc, nullptr, studyId, jobStart))
    return true;
  if (BackgroundJobRunning(state, "topo") ||
      BackgroundJobRunning(state, "study") ||
      BackgroundJobRunning(state, "check")) {
    if (error)
      *error = "a study run is already in progress";
    return false;
  }
  const uint64_t inputHash = fem::StudyInputKey(doc, *s);
  // Prepare the immutable snapshot on this (main) thread — meshing needs the
  // Document. Everything past this point is pure fem data.
  auto job = std::make_shared<TopoJob>();
  if (!fem::PrepareTopology(doc, *s, job->input, error))
    return false;
  job->studyId = studyId;
  job->partSlotId = SlotIdOfDoc(state, doc);
  job->maxIterations = job->input.params.maxIterations;

  // Pick the linear solver here on the main thread, so study_status can
  // report it from the first poll: the GPU matrix-free CG when a usable
  // hardware device exists, else the built-in Eigen CG-IC. The worker is the
  // only thread that touches it after launch. LOBOLE_TOPO_GPU=0 forces CPU;
  // =1 also admits a software (llvmpipe) device — for exercising the GPU path.
  job->solverName = fem::CpuSolverName(fem::AnalysisType::Topology);
#ifdef HAVE_FEM_GPU
  {
    const char* pref = std::getenv("LOBOLE_TOPO_GPU");
    if (!pref || std::string(pref) != "0") {
      std::string gerr;
      auto gs = gpu::TopoGpuSolver::Create(pref && std::string(pref) == "1",
                                           &gerr, state.settings.gpuDevice);
      if (gs) {
        LOG_INFO("topology: GPU solver on {}", gs->DeviceName());
        job->solverName = "gpu: " + gs->DeviceName();
        job->solver = std::move(gs);
      } else {
        LOG_INFO("topology: CPU solver ({})", gerr);
        job->solverName = "cpu (" + gerr + ")";
      }
    }
  }
#endif
  job->started = std::chrono::steady_clock::now();

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "topo";
  bg->label = StudyLabel(doc, studyId, "Optimizing");
  bg->cancellable = true; // between iterations, keeping the best so far
  bg->studyId = studyId;
  bg->owner = jobStart ? jobStart->owner : 0;
  BackgroundJob* raw = bg.get();
  TopoJob* j = job.get(); // the shared_ptr below outlives the thread
  // Its device waits are cut into slices that ask this: a GPU that never
  // answers still lets the run be cancelled.
  if (j->solver)
    j->solver->SetCancel([raw] { return raw->cancel.load(); });
  bg->thread = std::thread([j, job, raw]() {
    std::string err;
    auto res = fem::TopologyOptimize(
        j->input.mesh, j->input.material, j->input.fixedNodes, j->input.cases,
        j->input.params, &err,
        [j, raw](const fem::TopoProgress& p) {
          j->iteration.store(p.iteration, std::memory_order_relaxed);
          j->compliance.store(p.compliance, std::memory_order_relaxed);
          j->maxChange.store(p.maxChange, std::memory_order_relaxed);
          j->volumeFraction.store(p.volumeFraction, std::memory_order_relaxed);
          if (!p.solverFallback.empty() &&
              !j->fellBack.load(std::memory_order_relaxed)) {
            j->fallbackReason = p.solverFallback;
            j->fellBack.store(true, std::memory_order_release);
          }
          if (j->maxIterations > 0)
            raw->fraction.store(static_cast<double>(p.iteration) /
                                j->maxIterations);
          return !raw->cancel.load(std::memory_order_relaxed);
        },
        j->input.keepNodes, j->solver.get());
    if (res)
      j->result = std::move(*res);
    else
      j->error = err;
    raw->error = j->error;
    raw->done.store(true); // last
  });
  bg->finish = [job, inputHash](AppState& s) {
    TopoJob& j = *job;
    if (Document* target =
            JobDocument(s, j.partSlotId, "topology optimization")) {
      if (j.result) {
        fem::StudyRuntime rt = fem::MakeTopoRuntime(j.input, *j.result);
        rt.solvedAtVersion = target->Version();
        rt.inputHash = inputHash;
        // Name the solver that actually did the work: TopologyOptimize drops
        // an external solver that fails mid-run (on a device that was not
        // lost) and finishes on the CPU, and
        // a result labelled with the GPU it started on would be a lie.
        rt.solver = j.result->solverFallback.empty()
                        ? j.solverName
                        : "cpu (" + j.solverName +
                              " failed: " + j.result->solverFallback + ")";
        rt.solveSeconds = Elapsed(j.started);
        RememberStudyResult(s, rt);
        target->StoreStudyResults(j.studyId, std::move(rt));
      } else if (fem::Study* st = target->StudyForRun(j.studyId)) {
        st->error = "body '" + j.input.bodyName + "': " + j.error;
      }
    }
    if (s.topoJob == job)
      s.topoJob.reset();
    return true;
  };
  state.topoJob = job;
  BackgroundJob& started = StartBackgroundJob(state, std::move(bg));
  if (jobStart)
    jobStart->jobId = started.id;
  return true;
}

bool EnsureTopologyCheckStudies(AppState& state, Document& doc, uint64_t topoId,
                                double threshold, uint64_t* shapeId,
                                uint64_t* solidId, std::string* error) {
  const fem::Study* found = doc.FindStudy(topoId);
  if (found == nullptr || found->type != fem::AnalysisType::Topology) {
    if (error)
      *error = "study " + std::to_string(topoId) +
               " is not a topology study of this part";
    return false;
  }
  const fem::Study topo = *found; // a copy: studies are added below
  const auto ensure = [&](bool whole) {
    uint64_t sid = 0;
    for (const fem::Study& s : doc.GetStudies())
      if (s.type == fem::AnalysisType::Static && s.shapeFrom.study == topoId &&
          s.shapeFrom.whole == whole)
        sid = s.id;
    if (sid == 0) {
      auto cmd = std::make_unique<AddStudyCommand>(
          topo.name + (whole ? ": solid" : ": shape"),
          fem::AnalysisType::Static);
      AddStudyCommand* raw = cmd.get();
      state.commands->Execute(std::move(cmd), doc);
      sid = raw->StudyId();
    }
    const fem::Study& have = *doc.FindStudy(sid);
    fem::Study next = have;
    next.loads = topo.loads;
    next.loadCases = topo.loadCases;
    next.refinements.clear();
    next.gravity = glm::dvec3(0.0);
    next.shapeFrom = fem::ShapeSource{topoId, whole ? 0.0 : threshold, whole};
    // Unchanged but for the load ids: left alone, so its input hash -- and
    // a stored answer -- still hold.
    const auto withoutIds = [](fem::Study st) {
      for (fem::Load& l : st.loads)
        l.id = 0;
      return DocumentIO::StudyToJson(st);
    };
    if (withoutIds(next) == withoutIds(have))
      return sid;
    for (fem::Load& l : next.loads)
      l.id = doc.AllocateId();
    state.commands->Execute(std::make_unique<ModifyStudyCommand>(
                                "Check topology", DocumentIO::StudyToJson(have),
                                DocumentIO::StudyToJson(next)),
                            doc);
    return sid;
  };
  const uint64_t a = ensure(false);
  const uint64_t b = ensure(true);
  if (shapeId)
    *shapeId = a;
  if (solidId)
    *solidId = b;
  return true;
}

bool StartStudyRun(AppState& state, Document& doc, uint64_t studyId,
                   std::string* error, JobStart* jobStart) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  fem::Study* s = doc.StudyForRun(studyId);
  if (!s)
    return fail("no such study");
  if (s->type == fem::AnalysisType::Topology)
    return fail("topology studies run through the topology path");
  if (ReuseStudyResult(state, &doc, nullptr, studyId, jobStart))
    return true;
  if (BackgroundJobRunning(state, "topo") ||
      BackgroundJobRunning(state, "study") ||
      BackgroundJobRunning(state, "check"))
    return fail("a study run is already in progress");
  // The solver, before anything starts: a CalculiX study with no ccx is
  // refused here, saying how to install one, and the panel shows why.
  fem::SolveOptions calculix;
  if (!CalculixRunFor(state, *s, jobStart ? jobStart->timeoutSeconds : 0.0,
                      calculix, error)) {
    s->error = error ? *error : "";
    return false;
  }
  const uint64_t inputHash = fem::StudyInputKey(doc, *s);

  // Plan on this thread — expressions, face refs, materials need the
  // Document. The worker meshes the plan's copied shapes and solves; it
  // never sees the Document
  // (docs/ui/long-operations.md#fem-meshing-and-solving).
  auto job = std::make_shared<StudyJob>();
  if (!fem::PlanStudy(doc, *s, job->plan, error))
    return false;
  job->studyId = studyId;
  job->partSlotId = SlotIdOfDoc(state, doc);
  job->started = std::chrono::steady_clock::now();

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "study";
  bg->label = StudyLabel(doc, studyId, "Solving");
  // After the meshing, and within a few iterations of an iterative solve;
  // a direct factorization has no interruption points, so there it lands
  // when the factorization ends. The answer is dropped either way.
  bg->cancellable = true;
  bg->studyId = studyId;
  bg->owner = jobStart ? jobStart->owner : 0;
  BackgroundJob* raw = bg.get();
  // Made here: the worker must not read AppState.
  fem::SolveOptions solveOptions = JobSolveOptions(state, raw, job.get());
  solveOptions.calculix = WithEstimateTo(calculix.calculix, job.get());
  bg->thread = std::thread([job, raw, solveOptions]() {
    ArmOccSignalHandlers(); // OCC's handlers are per thread
    std::string err;
    if (!fem::MeshPlannedStudy(job->plan, job->input, &err))
      job->error = err.empty() ? "meshing failed" : err;
    else if (!raw->cancel.load() &&
             !fem::SolvePreparedStudy(job->input, job->runtime, &err,
                                      solveOptions) &&
             !raw->cancel.load()) // a cancel inside the solve is a cancel
      job->error = err.empty() ? "solve failed" : err;
    raw->error = job->error;
    raw->done.store(true); // last
  });
  bg->finish = [job, raw, inputHash](AppState& s) {
    StudyJob& j = *job;
    if (Document* target = JobDocument(s, j.partSlotId, "study solve")) {
      if (raw->cancel.load()) {
        // Cancelled: whatever it reached is not the answer it was asked for.
      } else if (j.error.empty()) {
        j.runtime.solvedAtVersion = target->Version();
        j.runtime.solver = SolverOf(j.runtime, j.input.type);
        j.runtime.solveSeconds = Elapsed(j.started);
        j.runtime.inputHash = inputHash;
        RememberStudyResult(s, j.runtime);
        target->StoreStudyResults(j.studyId, std::move(j.runtime));
      } else if (fem::Study* st = target->StudyForRun(j.studyId)) {
        st->error = j.error;
      }
    }
    if (s.studyJob == job)
      s.studyJob.reset();
    return true;
  };
  state.studyJob = job;
  BackgroundJob& started = StartBackgroundJob(state, std::move(bg));
  if (jobStart)
    jobStart->jobId = started.id;
  return true;
}

bool StartTopologyCheck(AppState& state, Document& doc, uint64_t topoId,
                        double threshold, uint64_t* shapeId, uint64_t* solidId,
                        std::string* error, JobStart* jobStart) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  uint64_t ids[2] = {0, 0};
  if (!EnsureTopologyCheckStudies(state, doc, topoId, threshold, &ids[0],
                                  &ids[1], error))
    return false;
  if (shapeId)
    *shapeId = ids[0];
  if (solidId)
    *solidId = ids[1];
  // Each one a stored result answers is not solved again.
  struct Part {
    uint64_t studyId = 0;
    uint64_t inputHash = 0;
    fem::StudyPlan plan;
    fem::StudyJobInput input;
    fem::StudyRuntime runtime;
  };
  struct CheckJob {
    uint64_t partSlotId = 0;
    std::vector<Part> parts;
    std::string error;
    uint64_t errorStudy = 0;
    std::chrono::steady_clock::time_point started;
  };
  auto job = std::make_shared<CheckJob>();
  bool running = BackgroundJobRunning(state, "topo") ||
                 BackgroundJobRunning(state, "study") ||
                 BackgroundJobRunning(state, "check");
  for (const uint64_t sid : ids) {
    JobStart one;
    one.recompute = jobStart != nullptr && jobStart->recompute;
    if (ReuseStudyResult(state, &doc, nullptr, sid, &one))
      continue;
    if (running)
      return fail("a study run is already in progress");
    fem::Study* s = doc.StudyForRun(sid);
    if (s == nullptr)
      return fail("no such study");
    Part part;
    part.studyId = sid;
    part.inputHash = fem::StudyInputKey(doc, *s);
    if (!fem::PlanStudy(doc, *s, part.plan, error))
      return false;
    job->parts.push_back(std::move(part));
  }
  if (job->parts.empty()) {
    if (jobStart)
      jobStart->reused = true;
    return true;
  }
  job->partSlotId = SlotIdOfDoc(state, doc);
  job->started = std::chrono::steady_clock::now();

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "check";
  bg->label = StudyLabel(doc, topoId, "Checking");
  // Between the two solves, inside an iterative one, and after a
  // factorization (it has no interruption points): what was not finished
  // is dropped.
  bg->cancellable = true;
  bg->studyId = topoId;
  bg->owner = jobStart ? jobStart->owner : 0;
  BackgroundJob* raw = bg.get();
  // Made here: the worker must not read AppState.
  fem::SolveOptions solveOptions = JobSolveOptions(state, raw, nullptr);
  bg->thread = std::thread([job, raw, solveOptions]() {
    ArmOccSignalHandlers(); // OCC's handlers are per thread
    const double n = static_cast<double>(job->parts.size());
    for (size_t i = 0; i < job->parts.size() && !raw->cancel.load(); ++i) {
      Part& p = job->parts[i];
      std::string err;
      if (!fem::MeshPlannedStudy(p.plan, p.input, &err))
        job->error = err.empty() ? "meshing failed" : err;
      else if (!raw->cancel.load() &&
               !fem::SolvePreparedStudy(p.input, p.runtime, &err,
                                        solveOptions) &&
               !raw->cancel.load()) // a cancel inside the solve is a cancel
        job->error = err.empty() ? "solve failed" : err;
      if (!job->error.empty()) {
        job->errorStudy = p.studyId;
        break;
      }
      raw->fraction.store((static_cast<double>(i) + 1.0) / n);
    }
    raw->error = job->error;
    raw->done.store(true); // last
  });
  bg->finish = [job, raw](AppState& s) {
    CheckJob& j = *job;
    Document* target = JobDocument(s, j.partSlotId, "topology check");
    if (target == nullptr)
      return true;
    for (Part& p : j.parts) {
      // Solved whole: a cancel after it does not take it back. The one the
      // cancel or the failure landed in, and what came after, are dropped.
      if (p.runtime.bodies.empty())
        continue;
      p.runtime.solvedAtVersion = target->Version();
      p.runtime.solver = SolverOf(p.runtime, p.input.type);
      p.runtime.solveSeconds = Elapsed(j.started);
      p.runtime.inputHash = p.inputHash;
      RememberStudyResult(s, p.runtime);
      target->StoreStudyResults(p.studyId, std::move(p.runtime));
    }
    if (!j.error.empty() && !raw->cancel.load())
      if (fem::Study* st = target->StudyForRun(j.errorStudy))
        st->error = j.error;
    return true;
  };
  BackgroundJob& started = StartBackgroundJob(state, std::move(bg));
  if (jobStart)
    jobStart->jobId = started.id;
  return true;
}

void AdoptStoredResults(AppState& state) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (Document* d = SlotDoc(state, i))
      fem::RestampByInputs(*d);
  if (state.document)
    fem::RestampByInputs(*state.document);
  if (!state.assembly)
    return;
  AssemblyDocument& asmDoc = *state.assembly;
  for (const fem::Study& s : asmDoc.GetStudies()) {
    const fem::StudyRuntime* rt = asmDoc.StudyResults(s.id);
    if (rt == nullptr || rt->inputHash == 0 ||
        !AssemblyStudyStale(state, asmDoc, s.id))
      continue;
    if (fem::AssemblyStudyInputKey(asmDoc, s, AssemblyStudyContextFor(state)) ==
        rt->inputHash)
      asmDoc.MarkStudyResultsCurrent(
          s.id, AssemblyStudyPartVersions(state, asmDoc, s));
  }
  AdoptSavedMotionResults(state);
}

bool StartMeshRun(AppState& state, Document& doc, uint64_t studyId,
                  std::string* error, JobStart* jobStart) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (BackgroundJobRunning(state, "mesh"))
    return fail("a mesh run is already in progress");
  fem::Study* s = doc.StudyForRun(studyId);
  if (!s)
    return fail("no such study");
  auto job = std::make_shared<MeshJob>();
  if (!fem::PlanStudyMesh(doc, *s, job->plan, error))
    return false;
  job->studyId = studyId;
  job->partSlotId = SlotIdOfDoc(state, doc);
  job->started = std::chrono::steady_clock::now();
  state.meshReports.erase({job->partSlotId, studyId});

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "mesh";
  bg->label = StudyLabel(doc, studyId, "Meshing");
  // Netgen has no interruption points: a cancel lands when the mesh does,
  // and the report is dropped.
  bg->cancellable = true;
  bg->studyId = studyId;
  bg->owner = jobStart ? jobStart->owner : 0;
  BackgroundJob* raw = bg.get();
  bg->thread = std::thread([job, raw]() {
    ArmOccSignalHandlers();
    std::string err;
    if (!fem::MeshPlanned(job->plan, job->meshes, &err))
      job->error = err.empty() ? "meshing failed" : err;
    raw->error = job->error;
    raw->done.store(true); // last
  });
  bg->finish = [job, raw](AppState& s) {
    MeshJob& j = *job;
    if (raw->cancel.load()) {
      if (s.meshJob == job)
        s.meshJob.reset();
      return true;
    }
    MeshReport report;
    report.error = j.error;
    report.seconds = Elapsed(j.started);
    for (const fem::StudyBodyMesh& bm : j.meshes)
      report.bodies.push_back({bm.bodyName, bm.mesh.stats.nodeCount,
                               bm.mesh.stats.tetCount,
                               bm.mesh.stats.worstQuality});
    s.meshReports[{j.partSlotId, j.studyId}] = std::move(report);
    if (!j.error.empty())
      if (Document* target = PartDocById(s, j.partSlotId))
        if (fem::Study* st = target->StudyForRun(j.studyId))
          st->error = j.error;
    if (s.meshJob == job)
      s.meshJob.reset();
    return true;
  };
  state.meshJob = job;
  BackgroundJob& started = StartBackgroundJob(state, std::move(bg));
  if (jobStart)
    jobStart->jobId = started.id;
  return true;
}

StudyRunStatus GetStudyRunStatus(const AppState& state, uint64_t studyId) {
  StudyRunStatus st;
  const StudyJob* job = state.studyJob.get();
  if (!job || job->studyId != studyId || !BackgroundJobRunning(state, "study"))
    return st;
  st.running = true;
  st.elapsedSeconds = Elapsed(job->started);
  // From the PLAN, not job->input: the worker rebuilds `input` wholesale
  // (MeshPlannedStudy's first line) while this polls; the plan is written
  // here before the thread starts and the worker only reads it.
  const char* now = job->solverNow.load();
  st.solver = now != nullptr ? now : fem::CpuSolverName(job->plan.type);
  st.memoryEstimateBytes = job->memoryEstimate.load();
  return st;
}

MeshRunStatus GetMeshRunStatus(const AppState& state, uint64_t studyId) {
  MeshRunStatus st;
  const MeshJob* job = state.meshJob.get();
  if (!job || job->studyId != studyId || !BackgroundJobRunning(state, "mesh"))
    return st;
  st.running = true;
  st.elapsedSeconds = Elapsed(job->started);
  return st;
}

const MeshReport* MeshReportFor(const AppState& state, const Document& doc,
                                uint64_t studyId) {
  const auto it = state.meshReports.find({SlotIdOfDoc(state, doc), studyId});
  return it == state.meshReports.end() ? nullptr : &it->second;
}

bool CancelTopologyRun(AppState& state, uint64_t studyId) {
  if (!state.topoJob || state.topoJob->studyId != studyId)
    return false;
  return CancelBackgroundJobs(state, "topo") > 0;
}

TopoRunStatus GetTopologyStatus(const AppState& state, uint64_t studyId) {
  TopoRunStatus st;
  const TopoJob* job = state.topoJob.get();
  if (!job || job->studyId != studyId || !BackgroundJobRunning(state, "topo"))
    return st; // running == false
  st.running = true;
  st.iteration = job->iteration.load(std::memory_order_relaxed);
  st.maxIterations = job->maxIterations;
  st.compliance = job->compliance.load(std::memory_order_relaxed);
  st.maxChange = job->maxChange.load(std::memory_order_relaxed);
  st.volumeFraction = job->volumeFraction.load(std::memory_order_relaxed);
  st.solver = job->solverName; // written before the thread launched
  // Named as the stored result will name it, from the iteration the worker
  // dropped the external solver.
  if (job->fellBack.load(std::memory_order_acquire))
    st.solver =
        "cpu (" + job->solverName + " failed: " + job->fallbackReason + ")";
  st.elapsedSeconds = Elapsed(job->started);
  return st;
}

namespace {
// The mass properties of what the active study solves: the part's own
// bodies -- or, on an assembly, the MESHED occurrence's.
std::optional<Measure::DocumentMass> SimulationMass(AppState& state) {
  SimulationState& sim = state.simulation;
  std::vector<Body> placed;
  const std::vector<Body>* bodies = nullptr;
  if (Document* doc = state.document.get()) {
    bodies = &doc->GetBodies();
  } else if (state.assembly) {
    const fem::Study* s = state.assembly->FindStudy(sim.activeStudyId);
    if (s)
      for (const fem::Participant& pt : s->participants)
        if (pt.role == fem::ParticipantRole::Meshed)
          placed = OccurrenceSubtreeBodies(state, pt.occurrenceId);
    bodies = &placed;
  }
  if (!bodies || bodies->empty())
    return std::nullopt;
  return Measure::MassOfBodies(*bodies);
}
} // namespace

double SimulationWeightN(AppState& state) {
  const auto mass = SimulationMass(state);
  if (!mass || !mass->massKnown || mass->massG <= 0.0)
    return 0.0;
  // A printed part weighs what it prints at (walls, shells, infill), when
  // its slicer settings say: the printed estimate per body, its share of
  // that body's solid mass.
  double grams = mass->massG;
  if (Document* doc = state.document.get(); doc && doc->PrintProcess()) {
    const auto& bodies = doc->GetBodies();
    std::vector<const Body*> weighed;
    for (const Body& b : bodies)
      if (!b.reference)
        weighed.push_back(&b);
    double printed = 0.0;
    bool all = weighed.size() == mass->bodies.size();
    for (size_t i = 0; all && i < weighed.size(); i++) {
      const auto pm = PrintedMassOf(state, *doc, *weighed[i], nullptr);
      if (!pm || !(pm->solidMm3 > 0.0)) {
        all = false;
        break;
      }
      printed += mass->bodies[i].massG * pm->effectiveMm3 / pm->solidMm3;
    }
    if (all)
      grams = printed;
  }
  return grams * 0.001 * 9.81;
}

void SimulationArmTool(AppState& state, SimulationState::Tool tool) {
  SimulationState& sim = state.simulation;
  sim.tool = tool;
  if (tool != SimulationState::Tool::Force &&
      tool != SimulationState::Tool::Pressure &&
      tool != SimulationState::Tool::Remote)
    return; // a torque seeds when it is placed, from the face's radius
  if (sim.pendingMagnitude != 0.0)
    return; // the user's own number survives re-arming
  // Seed from the part itself. A 0 N force is placeable, solvable and
  // meaningless — the study runs and reports zero stress everywhere, which
  // looks like an answer. Self-weight is a real number derived from what the
  // load acts on: "this part's own weight" is a scale a user can reason about
  // and edit (docs/ui/tool-sessions.md#defaults-and-refusals).
  const auto mass = SimulationMass(state);
  if (!mass || !mass->massKnown || mass->massG <= 0.0)
    return; // no material yet: the panel says so, and says why
  const double weightN = mass->massG * 0.001 * 9.81;
  if (tool != SimulationState::Tool::Pressure) {
    sim.pendingMagnitude = weightN;
    sim.pendingMagnitudeExpr.clear(); // a derived default is a number
    return;
  }
  // Pressure is MPa over the part's surface: the same weight spread over the
  // area it would press on.
  double areaMm2 = 0.0;
  for (const auto& b : mass->bodies)
    areaMm2 += b.surfaceArea;
  if (areaMm2 > 0.0)
    sim.pendingMagnitude = weightN / areaMm2; // N/mm^2 == MPa
  sim.pendingMagnitudeExpr.clear();
}

StudyHost StudyHostOf(AppState& state) {
  return StudyHost{state.document.get(), state.assembly.get()};
}

bool StudyStale(AppState& state, uint64_t studyId) {
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return true;
  return host.part ? host.part->StudyResultsStale(studyId)
                   : AssemblyStudyStale(state, *host.asmDoc, studyId);
}

void ModifyStudy(AppState& state, uint64_t id, const std::string& desc,
                 const std::function<void(fem::Study&)>& mutate) {
  const StudyHost host = StudyHostOf(state);
  const fem::Study* cur = host ? host.Find(id) : nullptr;
  if (!cur)
    return;
  if (host.part) {
    if (!state.commands)
      return;
    nlohmann::json before = DocumentIO::StudyToJson(*cur);
    fem::Study copy = *cur;
    mutate(copy);
    nlohmann::json after = DocumentIO::StudyToJson(copy);
    state.commands->Execute(
        std::make_unique<ModifyStudyCommand>(desc, before, after), *host.part);
    return;
  }
  // An assembly edits by full-state snapshot (AssemblyActions): one
  // snapshot, one mutation, one version bump — the same one undo step.
  fem::Study copy = *cur;
  mutate(copy);
  SnapshotAssembly(state);
  *host.asmDoc->FindStudy(id) = std::move(copy);
  host.asmDoc->BumpVersion();
}

uint64_t AddStudyOn(AppState& state, const std::string& name) {
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return 0;
  if (host.part) {
    if (!state.commands)
      return 0;
    auto cmd = std::make_unique<AddStudyCommand>(name);
    AddStudyCommand* raw = cmd.get();
    state.commands->Execute(std::move(cmd), *host.part);
    return raw->StudyId();
  }
  SnapshotAssembly(state);
  return host.asmDoc->AddStudy(name).id;
}

const std::vector<fem::ContactPatch>& SimulationContacts(AppState& state) {
  SimulationState& sim = state.simulation;
  static const std::vector<fem::ContactPatch> kNone;
  AssemblyDocument* asmDoc = state.assembly.get();
  const fem::Study* s = asmDoc ? asmDoc->FindStudy(sim.activeStudyId) : nullptr;
  if (!s || !sim.IsActive()) {
    sim.contacts.clear();
    sim.bonds.clear();
    sim.contactKey = 0;
    sim.contactError.clear();
    return kNone;
  }
  // The key: the study as text, the assembly's version, and every part the
  // study reads — the same record a result is stamped with.
  uint64_t key = std::hash<std::string>{}(DocumentIO::StudyToJson(*s).dump());
  key ^= asmDoc->Version() * 0x9E3779B97F4A7C15ull;
  for (const auto& [partId, version] :
       AssemblyStudyPartVersions(state, *asmDoc, *s))
    key ^= (partId * 0x100000001B3ull) ^ (version * 0x9E3779B97F4A7C15ull);
  if (key == sim.contactKey)
    return sim.contacts;
  sim.contactKey = key;
  sim.contacts.clear();
  sim.bonds.clear();
  sim.contactError.clear();
  fem::Study copy = *s; // the plan writes its refusal into the study
  fem::StudyPlan plan;
  std::string err;
  if (fem::PlanAssemblyStudy(*asmDoc, copy, AssemblyStudyContextFor(state),
                             plan, &err)) {
    sim.contacts = std::move(plan.contacts);
    sim.bonds = std::move(plan.bonds);
  } else {
    sim.contactError = err;
  }
  return sim.contacts;
}

const std::vector<fem::BondPatch>& SimulationBonds(AppState& state) {
  SimulationContacts(state); // one plan feeds both
  return state.simulation.bonds;
}

uint64_t ActiveSlotId(const AppState& state) {
  if (state.activePart < 0 ||
      static_cast<size_t>(state.activePart) >= state.parts.size())
    return 0;
  return state.parts[state.activePart].id;
}

fem::AssemblyStudyContext AssemblyStudyContextFor(AppState& state) {
  fem::AssemblyStudyContext ctx;
  ctx.placedBodies = [&state](uint64_t occurrenceId) {
    return OccurrenceSubtreeBodies(state, occurrenceId);
  };
  ctx.symbols = &state.projectSymbols;
  return ctx;
}

std::vector<std::pair<uint64_t, uint64_t>>
AssemblyStudyPartVersions(AppState& state, const AssemblyDocument& asmDoc,
                          const fem::Study& study) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  for (const fem::Participant& pt : study.participants) {
    if (pt.role == fem::ParticipantRole::Ignored)
      continue;
    const Occurrence* occ = asmDoc.FindById(pt.occurrenceId);
    if (!occ)
      continue;
    bool seen = false;
    for (const auto& [id, v] : out)
      seen = seen || id == occ->partId;
    if (seen)
      continue;
    // the live part: its stamp covers every configuration of it
    const Document* part = PartDocById(state, occ->partId);
    out.push_back({occ->partId, part ? PartStamp(*part) : 0});
  }
  return out;
}

bool AssemblyStudyStale(AppState& state, const AssemblyDocument& asmDoc,
                        uint64_t studyId) {
  return asmDoc.StudyResultsStale(studyId, [&state](uint64_t partId) {
    const Document* part = PartDocById(state, partId);
    return part ? PartStamp(*part) : uint64_t{0};
  });
}

namespace {
// Plan an assembly study for a run: the imprint and every refusal happen
// here, on the main thread, against the placed geometry.
bool PlanForRun(AppState& state, AssemblyDocument& asmDoc, uint64_t studyId,
                fem::StudyPlan& plan, fem::Study** studyOut,
                std::string* error) {
  fem::Study* s = asmDoc.FindStudy(studyId);
  if (!s) {
    if (error)
      *error = "no such study";
    return false;
  }
  if (s->type == fem::AnalysisType::Topology) {
    if (error)
      *error = "topology optimisation is a part study";
    return false;
  }
  if (studyOut)
    *studyOut = s;
  return fem::PlanAssemblyStudy(asmDoc, *s, AssemblyStudyContextFor(state),
                                plan, error);
}
} // namespace

bool RunAssemblyStudyAndCache(AppState& state, AssemblyDocument& asmDoc,
                              uint64_t studyId, std::string* error,
                              double timeoutSeconds) {
  fem::StudyPlan plan;
  fem::Study* s = nullptr;
  fem::SolveOptions options;
  if (fem::Study* found = asmDoc.FindStudy(studyId);
      found != nullptr &&
      !CalculixRunFor(state, *found, timeoutSeconds, options, error)) {
    found->error = error ? *error : "";
    return false;
  }
  options.cancelled = state.rpcCallerGone;
  if (!PlanForRun(state, asmDoc, studyId, plan, &s, error))
    return false;
  const auto partVersions = AssemblyStudyPartVersions(state, asmDoc, *s);
  const uint64_t inputHash =
      fem::AssemblyStudyInputKey(asmDoc, *s, AssemblyStudyContextFor(state));
  fem::StudyJobInput input;
  fem::StudyRuntime rt;
  std::string err;
  if (!fem::MeshPlannedStudy(plan, input, &err) ||
      !fem::SolvePreparedStudy(input, rt, &err, options)) {
    s->error = err;
    if (error)
      *error = err;
    return false;
  }
  rt.solvedAtVersion = asmDoc.Version();
  rt.solvedPartVersions = partVersions;
  rt.solver = SolverOf(rt, input.type);
  rt.inputHash = inputHash;
  RememberStudyResult(state, rt);
  asmDoc.StoreStudyResults(studyId, std::move(rt));
  return true;
}

bool StartAssemblyStudyRun(AppState& state, AssemblyDocument& asmDoc,
                           uint64_t studyId, std::string* error,
                           JobStart* jobStart) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (asmDoc.FindStudy(studyId) != nullptr &&
      ReuseStudyResult(state, nullptr, &asmDoc, studyId, jobStart))
    return true;
  if (BackgroundJobRunning(state, "topo") ||
      BackgroundJobRunning(state, "study") ||
      BackgroundJobRunning(state, "check"))
    return fail("a study run is already in progress");
  fem::SolveOptions calculix;
  if (fem::Study* found = asmDoc.FindStudy(studyId);
      found != nullptr &&
      !CalculixRunFor(state, *found, jobStart ? jobStart->timeoutSeconds : 0.0,
                      calculix, error)) {
    found->error = error ? *error : "";
    return false;
  }
  auto job = std::make_shared<StudyJob>();
  fem::Study* s = nullptr;
  if (!PlanForRun(state, asmDoc, studyId, job->plan, &s, error))
    return false;
  const uint64_t inputHash =
      fem::AssemblyStudyInputKey(asmDoc, *s, AssemblyStudyContextFor(state));
  job->studyId = studyId;
  job->assemblySlotId = ActiveSlotId(state);
  job->partVersions = AssemblyStudyPartVersions(state, asmDoc, *s);
  job->started = std::chrono::steady_clock::now();

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "study";
  bg->label = "Solving '" + (s->name.empty() ? "study" : s->name) + "'";
  bg->cancellable = true; // at its boundaries, as StartStudyRun's
  bg->studyId = studyId;
  bg->owner = jobStart ? jobStart->owner : 0;
  BackgroundJob* raw = bg.get();
  // Made here: the worker must not read AppState.
  fem::SolveOptions solveOptions = JobSolveOptions(state, raw, job.get());
  solveOptions.calculix = WithEstimateTo(calculix.calculix, job.get());
  bg->thread = std::thread([job, raw, solveOptions]() {
    ArmOccSignalHandlers();
    std::string err;
    if (!fem::MeshPlannedStudy(job->plan, job->input, &err))
      job->error = err.empty() ? "meshing failed" : err;
    else if (!raw->cancel.load() &&
             !fem::SolvePreparedStudy(job->input, job->runtime, &err,
                                      solveOptions) &&
             !raw->cancel.load()) // a cancel inside the solve is a cancel
      job->error = err.empty() ? "solve failed" : err;
    raw->error = job->error;
    raw->done.store(true);
  });
  bg->finish = [job, raw, inputHash](AppState& st) {
    StudyJob& j = *job;
    AssemblyDocument* target = AssemblyDocById(st, j.assemblySlotId);
    if (!target) {
      LOG_WARN("study solve: the assembly it ran on was closed before it "
               "finished; the result is dropped");
    } else if (raw->cancel.load()) {
      // Cancelled: nothing it reached is the answer it was asked for.
    } else if (j.error.empty()) {
      j.runtime.solvedAtVersion = target->Version();
      j.runtime.solvedPartVersions = j.partVersions;
      j.runtime.solver = SolverOf(j.runtime, j.input.type);
      j.runtime.solveSeconds = Elapsed(j.started);
      j.runtime.inputHash = inputHash;
      RememberStudyResult(st, j.runtime);
      target->StoreStudyResults(j.studyId, std::move(j.runtime));
    } else if (fem::Study* st2 = target->FindStudy(j.studyId)) {
      st2->error = j.error;
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

bool MeshAssemblyStudy(AppState& state, AssemblyDocument& asmDoc,
                       uint64_t studyId, std::vector<fem::StudyBodyMesh>& out,
                       std::string* error) {
  fem::StudyPlan plan;
  fem::Study* s = nullptr;
  if (!PlanForRun(state, asmDoc, studyId, plan, &s, error))
    return false;
  fem::StudyJobInput input;
  std::string err;
  if (!fem::MeshPlannedStudy(plan, input, &err)) {
    s->error = err;
    if (error)
      *error = err;
    return false;
  }
  out.clear();
  for (fem::StudyJobBody& jb : input.bodies) {
    fem::StudyBodyMesh bm;
    bm.bodyIndex = jb.bodyIndex;
    bm.bodyName = jb.bodyName;
    bm.mesh = std::move(jb.mesh);
    out.push_back(std::move(bm));
  }
  return true;
}

void EnterSimulation(AppState& state) {
  Document* doc = state.document.get();
  if (!doc) {
    // An assembly tab: the studies live on the assembly, and the window is
    // the floating tool window beside explode and motion (S1).
    AssemblyDocument* asmDoc = state.assembly.get();
    if (!asmDoc)
      return;
    state.measure = MeasureState{};
    state.simulation.active = true;
    state.simulationToolOpen = true;
    state.explodeToolOpen = false;
    state.motionToolOpen = false;
    if (asmDoc->GetStudies().empty()) {
      SnapshotAssembly(state);
      asmDoc->AddStudy("");
    }
    if (!asmDoc->FindStudy(state.simulation.activeStudyId))
      state.simulation.activeStudyId = asmDoc->GetStudies().front().id;
    state.gpu.syncedDocumentVersion = UINT64_MAX;
    return;
  }
  // Drop the measure tool: it owns the viewport while active, and the
  // simulation toolbar has no Measure toggle to turn it back off.
  state.measure = MeasureState{};
  state.simulation.active = true;
  // The Studies panel draws into the Model dock window; entering simulation
  // mode while another tab (Variables, History, ...) owns the shared node
  // leaves the studies hidden behind it until the user finds the right tab.
  // Repeated frames, because one SetWindowFocus on a shared dock node does
  // not reliably win the tab (DockLayout.cpp).
  state.focusPanel = "Model";
  state.focusPanelFrames = 60;
  if (doc->GetStudies().empty() && state.commands)
    state.commands->Execute(std::make_unique<AddStudyCommand>(""), *doc);
  // Select a study to edit (the first, unless one is already active).
  if (!doc->FindStudy(state.simulation.activeStudyId) &&
      !doc->GetStudies().empty())
    state.simulation.activeStudyId = doc->GetStudies().front().id;
  // Drop the modeling-mode shape-guide ghost while in simulation mode.
  state.gpu.syncedDocumentVersion = UINT64_MAX;
}
void ResetSimulationDisplay(AppState& state) {
  // Restore any body instances the field overlay hid, and drop the topology
  // iso-surface instance.
  if (state.meshRenderer) {
    for (uint32_t meshId : state.gpu.hiddenMeshes)
      state.meshRenderer->SetInstanceVisible(meshId, true);
    if (state.gpu.topoIsoInstance)
      state.meshRenderer->RemoveMesh(state.gpu.topoIsoInstance);
  }
  if (state.fieldRenderer)
    state.fieldRenderer->SetMesh({}, {});
  state.simulation = SimulationState{};
}
void ExitSimulation(AppState& state) {
  ResetSimulationDisplay(state);
  state.simulationToolOpen = false;
  state.gpu.syncedDocumentVersion = UINT64_MAX; // force a clean renderer resync
}
void SyncFieldRenderer(AppState& state) {
  if (!state.fieldRenderer || !state.meshRenderer)
    return;
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  // On an assembly, the solved bodies are the MESHED occurrence's.
  uint64_t meshedOcc = 0;
  std::vector<uint64_t> solvedOccs; // meshed + bonded (S2): one field mesh
  if (host.IsAssembly())
    if (const fem::Study* s = host.Find(sim.activeStudyId))
      for (const fem::Participant& pt : s->participants) {
        if (pt.role == fem::ParticipantRole::Meshed)
          meshedOcc = pt.occurrenceId;
        if (pt.role == fem::ParticipantRole::Meshed ||
            pt.role == fem::ParticipantRole::Bonded)
          solvedOccs.push_back(pt.occurrenceId);
      }
  const bool bondedGroup = solvedOccs.size() > 1;

  auto restoreBodies = [&]() {
    for (uint32_t meshId : state.gpu.hiddenMeshes)
      state.meshRenderer->SetInstanceVisible(meshId, true);
    state.gpu.hiddenMeshes.clear();
  };
  auto removeIso = [&]() {
    if (state.gpu.topoIsoInstance)
      state.meshRenderer->RemoveMesh(state.gpu.topoIsoInstance);
    state.gpu.topoIsoInstance = 0;
    state.gpu.topoIsoKey = 0;
  };
  // Hide the studied bodies' normal render so the overlay stands in for them.
  // Recomputed EVERY sync: mesh ids change on every regeneration (e.g. a
  // study edit between runs bumps the document version), so an "already
  // hidden" early-out can hold only stale ids while the regenerated body
  // renders fully visible over the overlay — the raw-block-instead-of-result
  // bug. Stale ids drop out with the rebuild (their instances are gone).
  auto hideBodies = [&](const fem::StudyRuntime* rt) {
    std::vector<uint32_t> want;
    for (const fem::StudyBodyResult& b : rt->bodies)
      for (const DocumentRenderMesh& m : state.gpu.documentMeshes) {
        if (bondedGroup) {
          // The group's field mesh covers every solved occurrence.
          for (uint64_t occ : solvedOccs)
            if (m.occurrenceId == occ)
              want.push_back(m.meshId);
          continue;
        }
        if (static_cast<int>(m.bodyIndex) == b.bodyIndex &&
            m.occurrenceId == meshedOcc)
          want.push_back(m.meshId);
      }
    for (uint32_t id : want)
      if (std::find(state.gpu.hiddenMeshes.begin(),
                    state.gpu.hiddenMeshes.end(),
                    id) == state.gpu.hiddenMeshes.end())
        state.meshRenderer->SetInstanceVisible(id, false);
    state.gpu.hiddenMeshes = std::move(want);
  };

  const fem::StudyRuntime* rt =
      host ? host.Results(sim.activeStudyId) : nullptr;
  const fem::Study* study = host ? host.Find(sim.activeStudyId) : nullptr;
  const bool show = sim.IsActive() &&
                    sim.field != SimulationState::Field::None && rt != nullptr;
  if (!show) {
    if (state.gpu.fieldBuiltKey != 0 || state.gpu.topoIsoInstance != 0 ||
        !state.gpu.hiddenMeshes.empty()) {
      restoreBodies();
      removeIso();
      state.fieldRenderer->SetMesh({}, {});
      state.gpu.fieldBuiltKey = 0;
    }
    return;
  }

  const bool topo = study && study->type == fem::AnalysisType::Topology;

  // Guard against a field that doesn't match the study's result data: a
  // topology runtime has no von Mises / displacement arrays, and a static/modal
  // runtime has no density field. This happens when the field is left armed
  // across an analysis-type switch (e.g. view modal displacement, then change
  // the study to topology) — reading the empty arrays would be out of bounds.
  // Treat a mismatched field as "off".
  const bool applicable =
      topo ? (sim.field == SimulationState::Field::Shape ||
              sim.field == SimulationState::Field::Density)
           : (sim.field == SimulationState::Field::VonMises ||
              sim.field == SimulationState::Field::Displacement);
  if (!applicable) {
    restoreBodies();
    removeIso();
    state.fieldRenderer->SetMesh({}, {});
    state.gpu.fieldBuiltKey = 0;
    return;
  }

  // --- Topology "Shape": the optimized iso-surface as a grey MeshRenderer
  // body.
  if (topo && sim.field == SimulationState::Field::Shape) {
    if (state.gpu.fieldBuiltKey != 0) {
      state.fieldRenderer->SetMesh({}, {});
      state.gpu.fieldBuiltKey = 0;
    }
    // Re-extract only when the study, results, or threshold changes.
    const uint64_t tq = static_cast<uint64_t>(
        std::lround(std::clamp(sim.topoThreshold, 0.0f, 1.0f) * 1000.0f));
    const uint64_t key =
        (sim.activeStudyId * 1000003ull + host.ResultsEpoch()) * 100003ull +
        tq + 1;
    if (key != state.gpu.topoIsoKey) {
      std::vector<glm::vec3> pos, nrm;
      std::vector<uint32_t> idx;
      for (const fem::StudyBodyResult& b : rt->bodies) {
        const double t =
            sim.topoThreshold > 0.0f
                ? sim.topoThreshold
                : fem::AutoIsoThreshold(b.mesh, b.density,
                                        rt->volumeFractionAchieved);
        fem::ExtractIsoSurface(b.mesh, b.density, t, pos, nrm, idx);
      }
      std::vector<Vertex3D> verts(pos.size());
      for (size_t i = 0; i < pos.size(); ++i) {
        verts[i].position = pos[i];
        verts[i].normal = nrm[i];
      }
      if (state.gpu.topoIsoInstance == 0) {
        state.gpu.topoIsoInstance = state.meshRenderer->AddMesh(verts, idx);
        state.meshRenderer->SetMeshColor(state.gpu.topoIsoInstance,
                                         glm::vec3(0.72f, 0.72f, 0.75f));
      } else {
        state.meshRenderer->UpdateMesh(state.gpu.topoIsoInstance, verts, idx);
      }
      state.gpu.topoIsoKey = key;
    }
    hideBodies(rt);
    return;
  }

  // --- Scalar field via the FieldRenderer (von Mises / displacement /
  // density).
  removeIso();
  const bool modal = study && study->type == fem::AnalysisType::Modal;
  const bool vm = sim.field == SimulationState::Field::VonMises && !modal;
  const bool density = topo && sim.field == SimulationState::Field::Density;

  const int caseIdx = rt->caseNames.empty() ? -1 : sim.activeCase;
  const uint64_t key =
      (((static_cast<uint64_t>(sim.field) * 1000003ull + sim.activeStudyId) *
            1000003ull +
        host.ResultsEpoch()) *
           1000003ull +
       (modal ? static_cast<uint64_t>(sim.activeMode) + 1 : 0)) *
          1000003ull +
      static_cast<uint64_t>(caseIdx + 1);
  if (key == state.gpu.fieldBuiltKey) {
    hideBodies(rt);
    return;
  }

  // The scale is the case on show's own maximum (the study's, without
  // cases).
  double fmax = vm ? rt->maxVonMises : rt->maxDisplacement;
  if (caseIdx >= 0) {
    fmax = 0.0;
    for (const fem::StudyBodyResult& b : rt->bodies)
      fmax = std::max(fmax, vm ? b.Case(caseIdx).maxVonMises
                               : b.Case(caseIdx).maxDisplacement);
  }
  const double inv = (modal || density) ? 1.0 : (fmax > 0 ? 1.0 / fmax : 0.0);

  std::vector<FieldVertex> verts;
  std::vector<uint32_t> indices;
  // tri6 -> 4 linear sub-triangles via the midside nodes (corners 0,1,2,
  // midsides 3,4,5): (0,3,5)(3,1,4)(5,4,2)(3,4,5).
  static const int kSub[4][3] = {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}};
  for (const fem::StudyBodyResult& b : rt->bodies) {
    // Density: the per-tet density averaged onto every mesh node. Otherwise the
    // displacement source (active mode shape for modal, else the static solve).
    std::vector<double> nodeDensity;
    if (density) {
      nodeDensity.assign(b.mesh.nodes.size(), 0.0);
      std::vector<int> cnt(b.mesh.nodes.size(), 0);
      for (size_t e = 0; e < b.mesh.tets.size() && e < b.density.size(); ++e)
        for (int i = 0; i < 10; ++i) {
          const int n = b.mesh.tets[e][i];
          nodeDensity[n] += b.density[e];
          ++cnt[n];
        }
      for (size_t n = 0; n < nodeDensity.size(); ++n)
        if (cnt[n])
          nodeDensity[n] /= cnt[n];
    }
    const fem::FemResults& shown = b.Case(caseIdx);
    const std::vector<glm::dvec3>* disp = &shown.displacement;
    if (modal) {
      if (b.results.modes.empty())
        continue;
      const int mi = std::clamp(sim.activeMode, 0,
                                static_cast<int>(b.results.modes.size()) - 1);
      disp = &b.results.modes[mi].displacement;
    }
    for (const auto& tri : b.mesh.boundary) {
      const uint32_t base = static_cast<uint32_t>(verts.size());
      for (int i = 0; i < 6; ++i) {
        const int n = tri.nodes[i];
        const glm::dvec3& p = b.mesh.nodes[n];
        FieldVertex fv;
        fv.position = glm::vec3(p);
        if (density) {
          fv.displacement = glm::vec3(0.0f);
          fv.scalar = static_cast<float>(std::clamp(nodeDensity[n], 0.0, 1.0));
        } else {
          const glm::dvec3& d = (*disp)[n];
          const double val = vm ? shown.vonMises[n] : glm::length(d);
          fv.displacement = glm::vec3(d);
          fv.scalar = static_cast<float>(std::clamp(val * inv, 0.0, 1.0));
        }
        verts.push_back(fv);
      }
      for (const auto& s : kSub) {
        indices.push_back(base + s[0]);
        indices.push_back(base + s[1]);
        indices.push_back(base + s[2]);
      }
    }
  }
  state.fieldRenderer->SetMesh(std::move(verts), std::move(indices));
  hideBodies(rt);
  state.gpu.fieldBuiltKey = key;
}

} // namespace ui
