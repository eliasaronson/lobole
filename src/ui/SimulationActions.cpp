#include "ui/AppActions.h"

#include "Camera.h"
#include "FieldRenderer.h"
#include "MeshRenderer.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/FemSolver.h"
#include "core/FemStudy.h"
#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace ui {

void AppendStudyMarker(const AppState& state, std::vector<LineVertex3D>& lines) {
  const SimulationState& sim = state.simulation;
  if (!sim.IsActive() || sim.field == SimulationState::Field::None ||
      !state.document)
    return;
  const fem::StudyRuntime* rt = state.document->StudyResults(sim.activeStudyId);
  if (!rt) return;
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
  const float L = state.camera
                      ? std::max(0.06f * state.camera->GetDistance(), 0.1f)
                      : 1.0f;
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
    if (error) *error = "no such study";
    return false;
  }
  fem::StudyRuntime rt;
  // A topology study runs the SIMP optimizer (minutes, serialized result); a
  // static/modal study runs the linear solver (seconds, transient result).
  const bool ok = s->type == fem::AnalysisType::Topology
                      ? fem::RunTopology(doc, *s, rt, error)
                      : fem::RunStudy(doc, *s, rt, error);
  if (!ok) return false;
  rt.solvedAtVersion = doc.Version();
  doc.StoreStudyResults(studyId, std::move(rt));
  return true;
}
bool StartTopologyRun(AppState& state, Document& doc, uint64_t studyId,
                      std::string* error) {
  if (state.topoJob) {
    if (error) *error = "a study run is already in progress";
    return false;
  }
  fem::Study* s = doc.FindStudy(studyId);
  if (!s) {
    if (error) *error = "no such study";
    return false;
  }
  if (s->type != fem::AnalysisType::Topology) {
    if (error) *error = "study is not a topology study";
    return false;
  }
  // Prepare the immutable snapshot on this (main) thread — meshing needs the
  // Document. Everything past this point is pure fem data.
  auto job = std::make_unique<TopoJob>();
  if (!fem::PrepareTopology(doc, *s, job->input, error)) return false;
  job->studyId = studyId;
  job->maxIterations = job->input.params.maxIterations;

  TopoJob* j = job.get();  // stable: the job outlives the thread (joined first)
  job->thread = std::thread([j]() {
    std::string err;
    auto res = fem::TopologyOptimize(
        j->input.mesh, j->input.material, j->input.fixedNodes, j->input.loads,
        j->input.params, &err,
        [j](const fem::TopoProgress& p) {
          j->iteration.store(p.iteration, std::memory_order_relaxed);
          j->compliance.store(p.compliance, std::memory_order_relaxed);
          j->maxChange.store(p.maxChange, std::memory_order_relaxed);
          j->volumeFraction.store(p.volumeFraction, std::memory_order_relaxed);
          return !j->cancel.load(std::memory_order_relaxed);
        },
        j->input.keepNodes);
    if (res)
      j->result = std::move(*res);
    else
      j->error = err;
    j->done.store(true, std::memory_order_release);
  });
  state.topoJob = std::move(job);
  return true;
}

void PollTopologyRun(AppState& state, Document& doc) {
  if (!state.topoJob || !state.topoJob->done.load(std::memory_order_acquire))
    return;
  TopoJob& job = *state.topoJob;
  job.thread.join();  // finished; consume the result (single join point)
  if (job.result) {
    fem::StudyRuntime rt = fem::MakeTopoRuntime(job.input, *job.result);
    rt.solvedAtVersion = doc.Version();
    doc.StoreStudyResults(job.studyId, std::move(rt));
  } else if (fem::Study* s = doc.FindStudy(job.studyId)) {
    s->error = "body '" + job.input.bodyName + "': " + job.error;
  }
  state.topoJob.reset();
}

bool CancelTopologyRun(AppState& state, uint64_t studyId) {
  if (!state.topoJob || state.topoJob->studyId != studyId) return false;
  state.topoJob->cancel.store(true, std::memory_order_relaxed);
  return true;
}

TopoRunStatus GetTopologyStatus(const AppState& state, uint64_t studyId) {
  TopoRunStatus st;
  const TopoJob* job = state.topoJob.get();
  if (!job || job->studyId != studyId ||
      job->done.load(std::memory_order_acquire))
    return st;  // running == false
  st.running = true;
  st.iteration = job->iteration.load(std::memory_order_relaxed);
  st.maxIterations = job->maxIterations;
  st.compliance = job->compliance.load(std::memory_order_relaxed);
  st.maxChange = job->maxChange.load(std::memory_order_relaxed);
  st.volumeFraction = job->volumeFraction.load(std::memory_order_relaxed);
  return st;
}

void EnterSimulation(AppState& state) {
  Document* doc = state.document.get();
  if (!doc) return; // parts only (an assembly tab has no studies)
  state.simulation.active = true;
  if (doc->GetStudies().empty() && state.commands)
    state.commands->Execute(std::make_unique<AddStudyCommand>(""), *doc);
  // Select a study to edit (the first, unless one is already active).
  if (!doc->FindStudy(state.simulation.activeStudyId) && !doc->GetStudies().empty())
    state.simulation.activeStudyId = doc->GetStudies().front().id;
}
void ExitSimulation(AppState& state) {
  // Restore any body instances the field overlay hid.
  if (state.meshRenderer)
    for (uint32_t meshId : state.simulation.hiddenMeshes)
      state.meshRenderer->SetInstanceVisible(meshId, true);
  if (state.fieldRenderer) state.fieldRenderer->SetMesh({}, {});
  state.simulation = SimulationState{};
  state.syncedDocumentVersion = UINT64_MAX; // force a clean renderer resync
}
void SyncFieldRenderer(AppState& state) {
  if (!state.fieldRenderer || !state.meshRenderer) return;
  SimulationState& sim = state.simulation;
  Document* doc = state.document.get();

  auto restoreBodies = [&]() {
    for (uint32_t meshId : sim.hiddenMeshes)
      state.meshRenderer->SetInstanceVisible(meshId, true);
    sim.hiddenMeshes.clear();
  };

  const fem::StudyRuntime* rt =
      doc ? doc->StudyResults(sim.activeStudyId) : nullptr;
  const bool show = sim.IsActive() &&
                    sim.field != SimulationState::Field::None && rt != nullptr;
  if (!show) {
    if (sim.fieldBuiltKey != 0) {
      restoreBodies();
      state.fieldRenderer->SetMesh({}, {});
      sim.fieldBuiltKey = 0;
    }
    return;
  }

  // A modal study displays a normalized mode shape (displacement field of the
  // active mode); a static study displays von Mises or displacement magnitude.
  const fem::Study* study = doc->FindStudy(sim.activeStudyId);
  const bool modal = study && study->type == fem::AnalysisType::Modal;

  // Rebuild only when the field, study, results generation, or active mode
  // changes; the deform scale is a push constant (updated every frame).
  const uint64_t key = ((static_cast<uint64_t>(sim.field) * 1000003ull +
                         sim.activeStudyId) * 1000003ull +
                        doc->StudyResultsEpoch()) * 1000003ull +
                       (modal ? static_cast<uint64_t>(sim.activeMode) + 1 : 0);
  if (key == sim.fieldBuiltKey) return;

  const bool vm = sim.field == SimulationState::Field::VonMises && !modal;
  const double fmax = vm ? rt->maxVonMises : rt->maxDisplacement;
  const double inv = modal ? 1.0 : (fmax > 0 ? 1.0 / fmax : 0.0);

  std::vector<FieldVertex> verts;
  std::vector<uint32_t> indices;
  // tri6 -> 4 linear sub-triangles via the midside nodes (corners 0,1,2,
  // midsides 3,4,5): (0,3,5)(3,1,4)(5,4,2)(3,4,5).
  static const int kSub[4][3] = {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}};
  for (const fem::StudyBodyResult& b : rt->bodies) {
    // Pick the displacement source: the active mode shape (modal) or the
    // static solution.
    const std::vector<glm::dvec3>* disp = &b.results.displacement;
    if (modal) {
      if (b.results.modes.empty()) continue;
      const int mi = std::clamp(sim.activeMode, 0,
                                static_cast<int>(b.results.modes.size()) - 1);
      disp = &b.results.modes[mi].displacement;
    }
    for (const auto& tri : b.mesh.boundary) {
      const uint32_t base = static_cast<uint32_t>(verts.size());
      for (int i = 0; i < 6; ++i) {
        const int n = tri.nodes[i];
        const glm::dvec3& p = b.mesh.nodes[n];
        const glm::dvec3& d = (*disp)[n];
        const double val = vm ? b.results.vonMises[n] : glm::length(d);
        FieldVertex fv;
        fv.position = glm::vec3(p);
        fv.displacement = glm::vec3(d);
        fv.scalar = static_cast<float>(std::clamp(val * inv, 0.0, 1.0));
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

  // Hide the solved bodies' normal render so the overlay stands in for them.
  restoreBodies();
  for (const fem::StudyBodyResult& b : rt->bodies)
    for (const DocumentRenderMesh& m : state.documentMeshes)
      if (static_cast<int>(m.bodyIndex) == b.bodyIndex) {
        state.meshRenderer->SetInstanceVisible(m.meshId, false);
        sim.hiddenMeshes.push_back(m.meshId);
      }
  sim.fieldBuiltKey = key;
}

}  // namespace ui
