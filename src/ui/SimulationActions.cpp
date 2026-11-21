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
#include "ui/DocumentSlots.h"
#include "ui/MotionActions.h"
#include "ui/SimRunActions.h"
#include "ui/SimulationView.h"
#include "ui/SimulationViewInternal.h"
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
  if (!sim.IsActive())
    return;
  const float L =
      state.camera ? std::max(0.06f * state.camera->GetDistance(), 0.1f) : 1.0f;
  const auto cross = [&](const glm::vec3& at, const glm::vec3& col, float len) {
    const glm::vec3 ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const glm::vec3& a : ax) {
      lines.push_back(LineVertex(at - a * len, col));
      lines.push_back(LineVertex(at + a * len, col));
    }
  };
  // The place a report row framed (a worst element, a thin wall).
  if (sim.placeMarked && sim.meshView != SimulationState::MeshView::Off)
    cross(glm::vec3(sim.placeAt), glm::vec3(1.0f, 0.85f, 0.2f), 0.35f * L);
  // The ends of the field on show (ui::ShownField, built with the overlay,
  // so a topology view or a mesh has none and a stale one cannot be drawn):
  // a magenta cross at the maximum and a pale blue one at the minimum, each
  // warped by the deform scale so it tracks the overlay.
  const ShownField* shown = sim.shown.get();
  if (shown == nullptr || shown->mesh || shown->normalized ||
      !shown->hasExtremes || !sim.showExtremes)
    return;
  // A factor's hot end is its least value, and its greatest is only where
  // the stress is nearly zero: it is marked at its least alone.
  const glm::vec3 hot(1.0f, 0.1f, 0.8f); // magenta, distinct from CoG amber
  const glm::vec3 lo =
      glm::vec3(shown->loAt) + sim.deformScale * glm::vec3(shown->loDisp);
  const glm::vec3 hi =
      glm::vec3(shown->hiAt) + sim.deformScale * glm::vec3(shown->hiDisp);
  if (shown->reversed) {
    cross(lo, hot, L);
    return;
  }
  cross(hi, hot, L);
  if (shown->hi > shown->lo)
    cross(lo, glm::vec3(0.55f, 0.8f, 1.0f), 0.6f * L);
}
// The one solver choice, for a run on the frame and a job alike: a solve
// large enough to go iterative tries the GPU first, under the rules
// topology's does (PickTopologySolver): LOBOLE_TOPO_GPU=0 keeps it on the
// CPU, =1 admits a software device, and the user's preferred device is
// asked for. The device is made on the solving thread, and only for a
// solve that wants one. A synchronous run once went straight to the CPU.
void OfferGpuSolver(const AppState& state, fem::SolveOptions& o) {
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
  (void)o;
#endif
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
  if (const std::string why = fem::ContactRefusal(s); !why.empty())
    return fail(why);
  // A nonlinear study (large deformation, or a contact) set to auto solves
  // on CalculiX.
  if (fem::EffectiveSolver(s) != fem::Solver::Calculix) {
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

fem::ConvergeOptions ConvergeOptionsFor(const AppState& state,
                                        const fem::Study& study,
                                        const JobStart* job) {
  fem::ConvergeOptions o;
  o.mode = job != nullptr && job->converge >= 0
               ? static_cast<fem::ConvergeMode>(job->converge)
               : study.converge;
  o.autoSeconds = state.settings.convergeAutoS;
  if (job != nullptr && job->hasLayerNormal) {
    o.hasLayerNormal = true;
    o.layerNormal = glm::dvec3(job->layerNormal[0], job->layerNormal[1],
                               job->layerNormal[2]);
  }
  return o;
}

void RememberStudyResult(AppState& state, const fem::StudyRuntime& rt) {
  if (rt.inputHash == 0 || rt.cancelled)
    return;
  // A whole result is not replaced by its own surface: the one remembered
  // before it was stored is what a VTU export writes
  // (docs/simulation/README.md#vtu-export).
  const auto whole = [](const fem::StudyRuntime& r) {
    for (const fem::StudyBodyResult& b : r.bodies)
      if (b.mesh.tets.empty())
        return false;
    return !r.bodies.empty();
  };
  if (const auto have = state.studyResultsByInput.Find(rt.inputHash);
      have && whole(*have) && !whole(rt))
    return;
  state.studyResultsByInput.Put(rt.inputHash,
                                std::make_shared<const fem::StudyRuntime>(rt));
}

// What a study job's worker solves with: an iterative solve stops when the
// job is cancelled (a factorisation still cannot), the job learns which
// solver it is waiting on and how many iterations it has made, and the GPU
// is offered (OfferGpuSolver).
fem::SolveOptions JobSolveOptions(const AppState& state, BackgroundJob* raw,
                                  StudyJob* job) {
  fem::SolveOptions o;
  o.cancelled = [raw] { return raw->cancel.load(); };
  if (job != nullptr) {
    o.solving = [job](const char* name) { job->solverNow.store(name); };
    o.iterated = [job](int n) { job->iterations.store(n); };
  }
  OfferGpuSolver(state, o);
  return o;
}

// A CalculiX run that tells `job` its memory estimate and a nonlinear
// run's progress (the load reached over the load it runs to), for
// study_status and the job's bar.
std::shared_ptr<const fem::CalculixRun>
WithEstimateTo(const std::shared_ptr<const fem::CalculixRun>& run,
               StudyJob* job, BackgroundJob* raw) {
  if (!run)
    return nullptr;
  auto told = std::make_shared<fem::CalculixRun>(*run);
  told->estimated = [job](uint64_t bytes) { job->memoryEstimate.store(bytes); };
  told->progress = [job, raw](double f) {
    job->loadFraction.store(f);
    if (raw)
      raw->fraction.store(f);
  };
  return told;
}

// The solver a result records: the one that ran, which a static solve
// reports, else the analysis type's.
std::string SolverOf(const fem::StudyRuntime& rt, fem::AnalysisType type) {
  return rt.solver.empty() ? std::string(fem::CpuSolverName(type)) : rt.solver;
}

namespace {

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

void PickTopologySolver(const AppState& state, TopoJob& job) {
  // Pick the linear solver here on the main thread, so study_status can
  // report it from the first poll: the GPU matrix-free CG when a usable
  // hardware device exists, else the built-in Eigen CG-IC. The worker is the
  // only thread that touches it after launch. LOBOLE_TOPO_GPU=0 forces CPU;
  // =1 also admits a software (llvmpipe) device — for exercising the GPU path.
  job.solverName = fem::CpuSolverName(fem::AnalysisType::Topology);
#ifdef HAVE_FEM_GPU
  {
    const char* pref = std::getenv("LOBOLE_TOPO_GPU");
    if (!pref || std::string(pref) != "0") {
      std::string gerr;
      auto gs = gpu::TopoGpuSolver::Create(pref && std::string(pref) == "1",
                                           &gerr, state.settings.gpuDevice);
      if (gs) {
        LOG_INFO("topology: GPU solver on {}", gs->DeviceName());
        job.solverName = "gpu: " + gs->DeviceName();
        job.solver = std::move(gs);
      } else {
        LOG_INFO("topology: CPU solver ({})", gerr);
        job.solverName = "cpu (" + gerr + ")";
      }
    }
  }
#else
  (void)state;
#endif
}

void LaunchTopologyWorker(BackgroundJob& bg,
                          const std::shared_ptr<TopoJob>& job) {
  BackgroundJob* raw = &bg;
  TopoJob* j = job.get(); // the shared_ptr below outlives the thread
  // Its device waits are cut into slices that ask this: a GPU that never
  // answers still lets the run be cancelled.
  if (j->solver)
    j->solver->SetCancel([raw] { return raw->cancel.load(); });
  bg.thread = std::thread([j, job, raw]() {
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
}

fem::StudyRuntime TopologyRuntimeOf(TopoJob& j) {
  fem::StudyRuntime rt = fem::MakeTopoRuntime(j.input, *j.result);
  // Name the solver that actually did the work: TopologyOptimize drops an
  // external solver that fails mid-run (on a device that was not lost) and
  // finishes on the CPU, and a result labelled with the GPU it started on
  // would be a lie.
  rt.solver = j.result->solverFallback.empty()
                  ? j.solverName
                  : "cpu (" + j.solverName +
                        " failed: " + j.result->solverFallback + ")";
  rt.solveSeconds = Elapsed(j.started);
  return rt;
}

StudyRunStatus GetStudyRunStatus(const AppState& state, uint64_t studyId,
                                 uint64_t simSlotId) {
  StudyRunStatus st;
  const StudyJob* job = state.studyJob.get();
  if (!job || job->studyId != studyId || job->simSlotId != simSlotId ||
      !BackgroundJobRunning(state, "study"))
    return st;
  st.running = true;
  st.elapsedSeconds = Elapsed(job->started);
  // From the PLAN, not job->input: the worker rebuilds `input` wholesale
  // (MeshPlannedStudy's first line) while this polls; the plan is written
  // here before the thread starts and the worker only reads it.
  const char* now = job->solverNow.load();
  st.solver = now != nullptr ? now : fem::CpuSolverName(job->plan.type);
  st.memoryEstimateBytes = job->memoryEstimate.load();
  st.loadFraction = job->loadFraction.load();
  st.iterations = job->iterations.load();
  return st;
}

MeshRunStatus GetMeshRunStatus(const AppState& state, uint64_t studyId,
                               uint64_t simSlotId) {
  MeshRunStatus st;
  const MeshJob* job = state.meshJob.get();
  if (!job || job->studyId != studyId || job->simSlotId != simSlotId ||
      !BackgroundJobRunning(state, "mesh"))
    return st;
  st.running = true;
  st.elapsedSeconds = Elapsed(job->started);
  return st;
}

bool CancelTopologyRun(AppState& state, uint64_t studyId) {
  if (!state.topoJob || state.topoJob->studyId != studyId)
    return false;
  return CancelBackgroundJobs(state, "topo") > 0;
}

TopoRunStatus GetTopologyStatus(const AppState& state, uint64_t studyId,
                                uint64_t simSlotId) {
  TopoRunStatus st;
  const TopoJob* job = state.topoJob.get();
  if (!job || job->studyId != studyId || job->simSlotId != simSlotId ||
      !BackgroundJobRunning(state, "topo"))
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
  const StudyHost host = StudyHostOf(state);
  if (const Document* doc = host.GeometryPart()) {
    bodies = &doc->GetBodies();
  } else if (host.sim) {
    if (const fem::Study* s = host.sim->FindStudy(sim.activeStudyId)) {
      (void)s;
      for (const fem::Participant& pt : host.sim->Roles())
        if (pt.role == fem::ParticipantRole::Meshed) {
          const std::vector<Body>& flat = SimSubjectBodies(state);
          for (size_t i = 0; i < flat.size(); i++)
            if (SimBodyOccurrence(state, i) == pt.occurrenceId)
              placed.push_back(flat[i]);
        }
    }
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

namespace {
uint64_t g_hostOverride = 0; // main thread only, as every RPC is
} // namespace

StudyHostOverride::StudyHostOverride(uint64_t simSlotId)
    : m_prev(g_hostOverride) {
  g_hostOverride = simSlotId;
}
StudyHostOverride::~StudyHostOverride() { g_hostOverride = m_prev; }
uint64_t StudyHostOverrideSlot() { return g_hostOverride; }

StudyHost StudyHostOf(AppState& state) {
  fem::SimDocument* named =
      g_hostOverride != 0 ? SimBySlotId(state, g_hostOverride) : nullptr;
  if (named != nullptr || state.sim) {
    StudyHost host;
    host.sim = named != nullptr ? named : state.sim.get();
    host.simSlotId = named != nullptr ? g_hostOverride : ActiveSimSlotId(state);
    // Read on every frame the panel draws: the subject by id, nothing
    // walked (SimContextFor flattens an assembly).
    const fem::SimSubject& subject = host.sim->Subject();
    host.symbols = &state.projectSymbols;
    if (subject.kind == fem::SubjectKind::Assembly)
      host.subjectAssembly = AssemblyDocById(state, subject.docId);
    else if (const Document* part = PartDocById(state, subject.docId)) {
      host.subjectPart = part;
      host.symbols = &part->Symbols();
    }
    return host;
  }
  return {}; // studies live on a simulation tab, and this is none
}

bool StudyStale(AppState& state, uint64_t studyId) {
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return true;
  return SimStudyStale(state, *host.sim, studyId);
}

void ModifyStudy(AppState& state, uint64_t id, const std::string& desc,
                 const std::function<void(fem::Study&)>& mutate) {
  const StudyHost host = StudyHostOf(state);
  const fem::Study* cur = host ? host.Find(id) : nullptr;
  if (!cur)
    return;
  // A simulation edits by snapshot (SimUndoGuard): ReplaceStudy moves its
  // version and the study's revision. Its roles are the document's: an
  // edit to the study's participants is handed the document's table and
  // lands there, so every edit the panel makes to a table still works.
  (void)desc; // the simulation's undo step is the snapshot, not a name
  fem::Study copy = *cur;
  copy.participants = host.sim->Roles();
  mutate(copy);
  SimUndoGuard guard(state);
  const auto same = [](const std::vector<fem::Participant>& a,
                       const std::vector<fem::Participant>& b) {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); i++)
      if (a[i].occurrenceId != b[i].occurrenceId || a[i].role != b[i].role ||
          a[i].friction != b[i].friction ||
          a[i].meshSize.value != b[i].meshSize.value ||
          a[i].meshSize.expr != b[i].meshSize.expr)
        return false;
    return true;
  };
  if (!same(copy.participants, host.sim->Roles()))
    host.sim->SetRoles(copy.participants);
  copy.participants.clear();
  host.sim->ReplaceStudy(std::move(copy));
}

uint64_t AddStudyOn(AppState& state, const std::string& name) {
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return 0;
  SimUndoGuard guard(state);
  return host.sim->AddStudy(name).id;
}

const std::vector<fem::ContactPatch>& SimulationContacts(AppState& state) {
  SimulationState& sim = state.simulation;
  static const std::vector<fem::ContactPatch> kNone;
  // A simulation of an assembly: its own plan, cached on the study, the
  // roles and every subject document's stamp.
  if (state.sim && sim.IsActive() &&
      state.sim->Subject().kind == fem::SubjectKind::Assembly) {
    const fem::Study* st = state.sim->FindStudy(sim.activeStudyId);
    if (st == nullptr) {
      sim.contacts.clear();
      sim.bonds.clear();
      sim.contactKey = 0;
      sim.contactError.clear();
      return kNone;
    }
    uint64_t key = std::hash<std::string>{}(
        DocumentIO::StudyToJson(state.sim->Effective(*st)).dump());
    key ^= SimRenderVersion(state) * 0x9E3779B97F4A7C15ull;
    key ^= 0x5157ull; // never a part tab's key
    if (key == sim.contactKey)
      return sim.contacts;
    sim.contactKey = key;
    sim.contacts.clear();
    sim.bonds.clear();
    sim.contactError.clear();
    fem::StudyPlan plan;
    std::string err;
    if (fem::PlanSimStudy(SimContextFor(state, *state.sim), st->id, plan,
                          &err)) {
      sim.contacts = std::move(plan.contacts);
      sim.bonds = std::move(plan.bonds);
    } else {
      sim.contactError = err;
    }
    return sim.contacts;
  }
  sim.contacts.clear();
  sim.bonds.clear();
  sim.contactKey = 0;
  sim.contactError.clear();
  return kNone;
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

void EnterSimulation(AppState& state) {
  // The studies live on a simulation tab: from a part or an assembly, its
  // simulation is opened -- made first where there is none -- and simulation
  // mode is that tab being in front
  // (docs/simulation/README.md#the-simulation-tab).
  if (!state.sim) {
    if (!state.document && !state.assembly)
      return;
    if (state.activePart < 0)
      return;
    std::string why;
    if (OpenSimulationFor(state, static_cast<size_t>(state.activePart), &why) ==
        SIZE_MAX) {
      LOG_WARN("simulation: {}", why);
      return;
    }
  }
  if (!state.sim)
    return;
  // Drop the measure tool: it owns the viewport while active, and the
  // simulation toolbar has no Measure toggle to turn it back off.
  state.measure = MeasureState{};
  state.simulation.active = true;
  state.explodeToolOpen = false;
  state.motionToolOpen = false;
  // A new simulation starts with a study, as the mode always did: a quick
  // check is still one click.
  if (state.sim->GetStudies().empty()) {
    SimUndoGuard guard(state);
    state.sim->AddStudy("");
  }
  if (!state.sim->FindStudy(state.simulation.activeStudyId))
    state.simulation.activeStudyId = state.sim->GetStudies().front().id;
  state.focusPanel = "Model";
  state.focusPanelFrames = 60;
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
  state.simulationToolOpen = false;
  // Leaving a simulation tab goes back to what it analyses.
  if (state.sim) {
    const fem::SimSubject subject = state.sim->Subject();
    if (const std::optional<size_t> at = SlotIndexById(state, subject.docId)) {
      SetActivePart(state, *at); // resets the display (ResetActiveTabUiState)
      state.gpu.syncedDocumentVersion = UINT64_MAX;
      return;
    }
  }
  ResetSimulationDisplay(state);
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
  if (host.OnAssembly())
    if (const fem::Study* s = host.Find(sim.activeStudyId))
      for (const fem::Participant& pt : host.Roles(*s)) {
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
  auto hideBodies = [&](const std::vector<int>& bodyIndices,
                        bool capsToo = false) {
    std::vector<uint32_t> want;
    // The mesh view cuts along whole elements, which stand proud of the
    // section plane: the body's flat cap would slice through them.
    if (capsToo)
      for (const AppState::GpuProjection::SectionCapMesh& cap :
           state.gpu.sectionCapMeshes)
        want.push_back(cap.meshId);
    // A simulation tab draws its subject flattened: a mesh's occurrence is
    // SimBodyOccurrence, and its body index within that occurrence is its
    // place among that occurrence's flattened bodies.
    const auto occOf = [&](const DocumentRenderMesh& m) {
      return state.sim ? SimBodyOccurrence(state, m.bodyIndex) : m.occurrenceId;
    };
    const auto localIndex = [&](const DocumentRenderMesh& m) {
      if (!state.sim)
        return static_cast<int>(m.bodyIndex);
      const uint64_t occ = SimBodyOccurrence(state, m.bodyIndex);
      int k = 0;
      for (size_t i = 0; i < m.bodyIndex; i++)
        if (SimBodyOccurrence(state, i) == occ)
          k++;
      return k;
    };
    for (int bodyIndex : bodyIndices)
      for (const DocumentRenderMesh& m : state.gpu.documentMeshes) {
        if (bondedGroup) {
          // The group's field mesh covers every solved occurrence.
          for (uint64_t occ : solvedOccs)
            if (occOf(m) == occ)
              want.push_back(m.meshId);
          continue;
        }
        if (localIndex(m) == bodyIndex && occOf(m) == meshedOcc)
          want.push_back(m.meshId);
      }
    for (uint32_t id : want)
      if (std::find(state.gpu.hiddenMeshes.begin(),
                    state.gpu.hiddenMeshes.end(),
                    id) == state.gpu.hiddenMeshes.end())
        state.meshRenderer->SetInstanceVisible(id, false);
    state.gpu.hiddenMeshes = std::move(want);
  };
  const auto bodiesOf = [](const fem::StudyRuntime* rt) {
    std::vector<int> out;
    for (const fem::StudyBodyResult& b : rt->bodies)
      out.push_back(b.bodyIndex);
    return out;
  };
  const auto clear = [&]() {
    if (state.gpu.fieldBuiltKey != 0 || state.gpu.topoIsoInstance != 0 ||
        !state.gpu.hiddenMeshes.empty()) {
      restoreBodies();
      removeIso();
      state.fieldRenderer->SetMesh({}, {});
      state.fieldRenderer->SetStyle(FieldStyle{});
      state.gpu.fieldBuiltKey = 0;
    }
    sim.shown.reset();
  };

  // --- The mesh the Mesh action kept, in place of the body
  // (docs/simulation/README.md#the-mesh-you-look-at).
  if (sim.IsActive() && sim.meshView != SimulationState::MeshView::Off) {
    const MeshReport* mr = StudyMesh(state, sim.activeStudyId);
    if (mr == nullptr) {
      clear();
      return;
    }
    removeIso();
    std::vector<int> bodies;
    for (const MeshReport::Body& b : mr->bodies)
      bodies.push_back(b.bodyIndex);
    const uint64_t key = MeshOverlayKey(state, *mr);
    if (key != state.gpu.fieldBuiltKey) {
      std::vector<FieldVertex> verts;
      std::vector<uint32_t> indices;
      FieldStyle style;
      auto shown = std::make_shared<ShownField>();
      BuildMeshOverlay(state, *mr, verts, indices, style, *shown);
      shown->key = key;
      state.fieldRenderer->SetMesh(std::move(verts), std::move(indices));
      state.fieldRenderer->SetStyle(style);
      sim.shown = std::move(shown);
      state.gpu.fieldBuiltKey = key;
    }
    hideBodies(bodies, true);
    return;
  }

  const fem::StudyRuntime* rt =
      host ? host.Results(sim.activeStudyId) : nullptr;
  const fem::Study* study = host ? host.Find(sim.activeStudyId) : nullptr;
  const bool show = sim.IsActive() &&
                    sim.field != SimulationState::Field::None &&
                    rt != nullptr && study != nullptr;
  if (!show) {
    clear();
    return;
  }

  const bool topo = study->type == fem::AnalysisType::Topology;
  // --- Topology "Shape": the optimized iso-surface as a grey MeshRenderer
  // body.
  if (topo && sim.field == SimulationState::Field::Shape) {
    if (state.gpu.fieldBuiltKey != 0) {
      state.fieldRenderer->SetMesh({}, {});
      state.gpu.fieldBuiltKey = 0;
    }
    sim.shown.reset();
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
    hideBodies(bodiesOf(rt));
    return;
  }

  // --- A scalar field through the FieldRenderer: what the result holds at
  // its nodes, over the legend's range (ui/SimulationView.h). A field that
  // does not match the study's result -- one left armed across a change of
  // analysis type, or a stress asked of a result that kept none -- builds
  // nothing, and is treated as off.
  removeIso();
  // A buckling study shows its modes as a modal one does.
  const bool modal = study->type == fem::AnalysisType::Modal ||
                     study->type == fem::AnalysisType::Buckling;
  const uint64_t key = ResultOverlayKey(state, host, *rt, modal);
  if (key == state.gpu.fieldBuiltKey) {
    hideBodies(bodiesOf(rt));
    return;
  }
  std::vector<FieldVertex> verts;
  std::vector<uint32_t> indices;
  FieldStyle style;
  auto shown = std::make_shared<ShownField>();
  if (!BuildResultOverlay(state, host, *rt, *study, verts, indices, style,
                          *shown)) {
    clear();
    return;
  }
  shown->key = key;
  state.fieldRenderer->SetMesh(std::move(verts), std::move(indices));
  state.fieldRenderer->SetStyle(style);
  sim.shown = std::move(shown);
  hideBodies(bodiesOf(rt));
  state.gpu.fieldBuiltKey = key;
}

} // namespace ui
