#include "ui/SimulationActions.h"
#include "ui/AssemblyActions.h"

#include "common/Logger.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/MassProps.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/MeshRenderer.h"
#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#ifdef HAVE_FEM_GPU
#include "femgpu/TopoGpuSolver.h"
#endif

#include <OSD.hxx>
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
  if (!sim.IsActive() ||
      (sim.field != SimulationState::Field::VonMises &&
       sim.field != SimulationState::Field::Displacement) ||
      !state.document)
    return;
  const fem::StudyRuntime* rt = state.document->StudyResults(sim.activeStudyId);
  if (!rt)
    return;
  // A magenta cross at the peak of the displayed field (max von Mises or max
  // displacement), warped by the same deform scale so it tracks the overlay.
  const bool vm = sim.field == SimulationState::Field::VonMises;
  glm::vec3 at = vm ? glm::vec3(rt->maxVmAt) : glm::vec3(rt->maxDispAt);
  // Find the peak node's displacement to warp the marker with the mesh.
  for (const fem::StudyBodyResult& b : rt->bodies) {
    const int node = vm ? b.results.maxVmNode : b.results.maxDispNode;
    if (node >= 0 && node < (int)b.results.displacement.size() &&
        ((vm && b.bodyName == rt->maxVmBody) ||
         (!vm && b.bodyName == rt->maxDispBody))) {
      at += sim.deformScale * glm::vec3(b.results.displacement[node]);
      break;
    }
  }
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
  fem::Study* s = doc.FindStudy(studyId);
  if (!s) {
    if (error)
      *error = "no such study";
    return false;
  }
  fem::StudyRuntime rt;
  // A topology study runs the SIMP optimizer (minutes, serialized result); a
  // static/modal study runs the linear solver (seconds, transient result).
  const bool ok = s->type == fem::AnalysisType::Topology
                      ? fem::RunTopology(doc, *s, rt, error)
                      : fem::RunStudy(doc, *s, rt, error);
  if (!ok)
    return false;
  rt.solvedAtVersion = doc.Version();
  doc.StoreStudyResults(studyId, std::move(rt));
  return true;
}
namespace {

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

bool StartTopologyRun(AppState& state, Document& doc, uint64_t studyId,
                      std::string* error) {
  if (BackgroundJobRunning(state, "topo") ||
      BackgroundJobRunning(state, "study")) {
    if (error)
      *error = "a study run is already in progress";
    return false;
  }
  fem::Study* s = doc.FindStudy(studyId);
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
  // only thread that touches it after launch. DESIGNER_TOPO_GPU=0 forces CPU;
  // =1 also admits a software (llvmpipe) device — for exercising the GPU path.
  job->solverName = "cpu (Eigen CG-IC)";
#ifdef HAVE_FEM_GPU
  {
    const char* pref = std::getenv("DESIGNER_TOPO_GPU");
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
  BackgroundJob* raw = bg.get();
  TopoJob* j = job.get(); // the shared_ptr below outlives the thread
  bg->thread = std::thread([j, job, raw]() {
    std::string err;
    auto res = fem::TopologyOptimize(
        j->input.mesh, j->input.material, j->input.fixedNodes, j->input.loads,
        j->input.params, &err,
        [j, raw](const fem::TopoProgress& p) {
          j->iteration.store(p.iteration, std::memory_order_relaxed);
          j->compliance.store(p.compliance, std::memory_order_relaxed);
          j->maxChange.store(p.maxChange, std::memory_order_relaxed);
          j->volumeFraction.store(p.volumeFraction, std::memory_order_relaxed);
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
    raw->done.store(true); // last
  });
  bg->finish = [job](AppState& s) {
    TopoJob& j = *job;
    if (Document* target =
            JobDocument(s, j.partSlotId, "topology optimization")) {
      if (j.result) {
        fem::StudyRuntime rt = fem::MakeTopoRuntime(j.input, *j.result);
        rt.solvedAtVersion = target->Version();
        // Name the solver that actually did the work: TopologyOptimize drops
        // an external solver that fails mid-run and finishes on the CPU, and
        // a result labelled with the GPU it started on would be a lie.
        rt.solver = j.result->solverFallback.empty()
                        ? j.solverName
                        : "cpu (" + j.solverName +
                              " failed: " + j.result->solverFallback + ")";
        rt.solveSeconds = Elapsed(j.started);
        target->StoreStudyResults(j.studyId, std::move(rt));
      } else if (fem::Study* st = target->FindStudy(j.studyId)) {
        st->error = "body '" + j.input.bodyName + "': " + j.error;
      }
    }
    if (s.topoJob == job)
      s.topoJob.reset();
    return true;
  };
  state.topoJob = job;
  StartBackgroundJob(state, std::move(bg));
  return true;
}

bool StartStudyRun(AppState& state, Document& doc, uint64_t studyId,
                   std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (BackgroundJobRunning(state, "topo") ||
      BackgroundJobRunning(state, "study"))
    return fail("a study run is already in progress");
  fem::Study* s = doc.FindStudy(studyId);
  if (!s)
    return fail("no such study");
  if (s->type == fem::AnalysisType::Topology)
    return fail("topology studies run through the topology path");

  // Plan on this thread — expressions, face refs, materials need the
  // Document. The worker meshes the plan's copied shapes and solves; it
  // never sees the Document (docs/ui/long-operations.md, phase 3).
  auto job = std::make_shared<StudyJob>();
  if (!fem::PlanStudy(doc, *s, job->plan, error))
    return false;
  job->studyId = studyId;
  job->partSlotId = SlotIdOfDoc(state, doc);
  job->started = std::chrono::steady_clock::now();

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "study";
  bg->label = StudyLabel(doc, studyId, "Solving");
  bg->cancellable = false; // the linear solver has no interruption points
  BackgroundJob* raw = bg.get();
  bg->thread = std::thread([job, raw]() {
    OSD::SetSignal(Standard_False); // OCC's handlers are per thread
    std::string err;
    if (!fem::MeshPlannedStudy(job->plan, job->input, &err))
      job->error = err.empty() ? "meshing failed" : err;
    else if (!fem::SolvePreparedStudy(job->input, job->runtime, &err))
      job->error = err.empty() ? "solve failed" : err;
    raw->done.store(true); // last
  });
  bg->finish = [job](AppState& s) {
    StudyJob& j = *job;
    if (Document* target = JobDocument(s, j.partSlotId, "study solve")) {
      if (j.error.empty()) {
        j.runtime.solvedAtVersion = target->Version();
        j.runtime.solver = "cpu (Eigen CG-IC)";
        j.runtime.solveSeconds = Elapsed(j.started);
        target->StoreStudyResults(j.studyId, std::move(j.runtime));
      } else if (fem::Study* st = target->FindStudy(j.studyId)) {
        st->error = j.error;
      }
    }
    if (s.studyJob == job)
      s.studyJob.reset();
    return true;
  };
  state.studyJob = job;
  StartBackgroundJob(state, std::move(bg));
  return true;
}

bool StartMeshRun(AppState& state, Document& doc, uint64_t studyId,
                  std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (BackgroundJobRunning(state, "mesh"))
    return fail("a mesh run is already in progress");
  fem::Study* s = doc.FindStudy(studyId);
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
  bg->cancellable = false;
  BackgroundJob* raw = bg.get();
  bg->thread = std::thread([job, raw]() {
    OSD::SetSignal(Standard_False);
    std::string err;
    if (!fem::MeshPlanned(job->plan, job->meshes, &err))
      job->error = err.empty() ? "meshing failed" : err;
    raw->done.store(true); // last
  });
  bg->finish = [job](AppState& s) {
    MeshJob& j = *job;
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
        if (fem::Study* st = target->FindStudy(j.studyId))
          st->error = j.error;
    if (s.meshJob == job)
      s.meshJob.reset();
    return true;
  };
  state.meshJob = job;
  StartBackgroundJob(state, std::move(bg));
  return true;
}

StudyRunStatus GetStudyRunStatus(const AppState& state, uint64_t studyId) {
  StudyRunStatus st;
  const StudyJob* job = state.studyJob.get();
  if (!job || job->studyId != studyId || !BackgroundJobRunning(state, "study"))
    return st;
  st.running = true;
  st.elapsedSeconds = Elapsed(job->started);
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
  st.elapsedSeconds = Elapsed(job->started);
  return st;
}

void SimulationArmTool(AppState& state, SimulationState::Tool tool) {
  SimulationState& sim = state.simulation;
  sim.tool = tool;
  if (tool != SimulationState::Tool::Force &&
      tool != SimulationState::Tool::Pressure)
    return;
  if (sim.pendingMagnitude != 0.0)
    return; // the user's own number survives re-arming
  // Seed from the part itself. A 0 N force is placeable, solvable and
  // meaningless — the study runs and reports zero stress everywhere, which
  // looks like an answer. Self-weight is a real number derived from what the
  // load acts on: "this part's own weight" is a scale a user can reason about
  // and edit (docs/ui/u10-defaults-and-refusals.md).
  Document* doc = state.document.get();
  if (!doc)
    return;
  const auto mass = Measure::MassOfBodies(doc->GetBodies());
  if (!mass || !mass->massKnown || mass->massG <= 0.0)
    return; // no material yet: the panel says so, and says why
  const double weightN = mass->massG * 0.001 * 9.81;
  if (tool == SimulationState::Tool::Force) {
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

void EnterSimulation(AppState& state) {
  Document* doc = state.document.get();
  if (!doc)
    return; // parts only (an assembly tab has no studies)
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
  state.syncedDocumentVersion = UINT64_MAX;
}
void ExitSimulation(AppState& state) {
  // Restore any body instances the field overlay hid, and drop the topology
  // iso-surface instance.
  if (state.meshRenderer) {
    for (uint32_t meshId : state.simulation.hiddenMeshes)
      state.meshRenderer->SetInstanceVisible(meshId, true);
    if (state.simulation.topoIsoInstance)
      state.meshRenderer->RemoveMesh(state.simulation.topoIsoInstance);
  }
  if (state.fieldRenderer)
    state.fieldRenderer->SetMesh({}, {});
  state.simulation = SimulationState{};
  state.syncedDocumentVersion = UINT64_MAX; // force a clean renderer resync
}
void SyncFieldRenderer(AppState& state) {
  if (!state.fieldRenderer || !state.meshRenderer)
    return;
  SimulationState& sim = state.simulation;
  Document* doc = state.document.get();

  auto restoreBodies = [&]() {
    for (uint32_t meshId : sim.hiddenMeshes)
      state.meshRenderer->SetInstanceVisible(meshId, true);
    sim.hiddenMeshes.clear();
  };
  auto removeIso = [&]() {
    if (sim.topoIsoInstance)
      state.meshRenderer->RemoveMesh(sim.topoIsoInstance);
    sim.topoIsoInstance = 0;
    sim.topoIsoKey = 0;
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
      for (const DocumentRenderMesh& m : state.documentMeshes)
        if (static_cast<int>(m.bodyIndex) == b.bodyIndex)
          want.push_back(m.meshId);
    for (uint32_t id : want)
      if (std::find(sim.hiddenMeshes.begin(), sim.hiddenMeshes.end(), id) ==
          sim.hiddenMeshes.end())
        state.meshRenderer->SetInstanceVisible(id, false);
    sim.hiddenMeshes = std::move(want);
  };

  const fem::StudyRuntime* rt =
      doc ? doc->StudyResults(sim.activeStudyId) : nullptr;
  const fem::Study* study = doc ? doc->FindStudy(sim.activeStudyId) : nullptr;
  const bool show = sim.IsActive() &&
                    sim.field != SimulationState::Field::None && rt != nullptr;
  if (!show) {
    if (sim.fieldBuiltKey != 0 || sim.topoIsoInstance != 0 ||
        !sim.hiddenMeshes.empty()) {
      restoreBodies();
      removeIso();
      state.fieldRenderer->SetMesh({}, {});
      sim.fieldBuiltKey = 0;
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
    sim.fieldBuiltKey = 0;
    return;
  }

  // --- Topology "Shape": the optimized iso-surface as a grey MeshRenderer
  // body.
  if (topo && sim.field == SimulationState::Field::Shape) {
    if (sim.fieldBuiltKey != 0) {
      state.fieldRenderer->SetMesh({}, {});
      sim.fieldBuiltKey = 0;
    }
    // Re-extract only when the study, results, or threshold changes.
    const uint64_t tq = static_cast<uint64_t>(
        std::lround(std::clamp(sim.topoThreshold, 0.0f, 1.0f) * 1000.0f));
    const uint64_t key =
        (sim.activeStudyId * 1000003ull + doc->StudyResultsEpoch()) *
            100003ull +
        tq + 1;
    if (key != sim.topoIsoKey) {
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
      if (sim.topoIsoInstance == 0) {
        sim.topoIsoInstance = state.meshRenderer->AddMesh(verts, idx);
        state.meshRenderer->SetMeshColor(sim.topoIsoInstance,
                                         glm::vec3(0.72f, 0.72f, 0.75f));
      } else {
        state.meshRenderer->UpdateMesh(sim.topoIsoInstance, verts, idx);
      }
      sim.topoIsoKey = key;
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

  const uint64_t key =
      ((static_cast<uint64_t>(sim.field) * 1000003ull + sim.activeStudyId) *
           1000003ull +
       doc->StudyResultsEpoch()) *
          1000003ull +
      (modal ? static_cast<uint64_t>(sim.activeMode) + 1 : 0);
  if (key == sim.fieldBuiltKey) {
    hideBodies(rt);
    return;
  }

  const double fmax = vm ? rt->maxVonMises : rt->maxDisplacement;
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
    const std::vector<glm::dvec3>* disp = &b.results.displacement;
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
          const double val = vm ? b.results.vonMises[n] : glm::length(d);
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
  sim.fieldBuiltKey = key;
}

} // namespace ui
