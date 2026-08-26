#include "ui/StudyPanel.h"
#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/Command.h"
#include "core/fem/FemStudy.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/DockLayout.h" // PanelHeading
#include "ui/ExprInput.h"   // expression-capable numeric fields
#include "ui/PanelLayout.h" // LeftLabel / WrapRow (readable at dock width)
#include "ui/PanelTitles.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (BC glyphs)
#include "ui/SimulationActions.h"
#include "ui/ViewportHandle.h"
#include <utility>

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ui {

namespace {

const ImVec4 kError(0.95f, 0.5f, 0.4f, 1.0f);
const ImVec4 kStale(0.95f, 0.75f, 0.35f, 1.0f);
const ImVec4 kMuted(0.6f, 0.6f, 0.6f, 1.0f);

// Runs one study edit as an undoable ModifyStudyCommand: snapshot before,
// apply `mutate` to a copy, snapshot after, execute. Mirrors the RPC path.
void ModifyStudy(AppState& state, uint64_t id, const std::string& desc,
                 const std::function<void(fem::Study&)>& mutate) {
  Document* doc = state.document.get();
  const fem::Study* cur = doc->FindStudy(id);
  if (!cur || !state.commands)
    return;
  nlohmann::json before = DocumentIO::StudyToJson(*cur);
  fem::Study copy = *cur;
  mutate(copy);
  nlohmann::json after = DocumentIO::StudyToJson(copy);
  state.commands->Execute(
      std::make_unique<ModifyStudyCommand>(desc, before, after), *doc);
}

const char* LoadLabel(fem::LoadType t) {
  switch (t) {
  case fem::LoadType::Fixed:
    return "Fixed";
  case fem::LoadType::Force:
    return "Force";
  case fem::LoadType::Pressure:
    return "Pressure";
  }
  return "?";
}

void DrawStudyList(AppState& state) {
  Document* doc = state.document.get();
  SimulationState& sim = state.simulation;

  ImGui::SeparatorText("Studies");
  if (ImGui::Button(ICON_FA_PLUS " Add study###study_add")) {
    auto cmd = std::make_unique<AddStudyCommand>("");
    AddStudyCommand* raw = cmd.get();
    state.commands->Execute(std::move(cmd), *doc);
    sim.activeStudyId = raw->StudyId();
  }

  for (const fem::Study& s : doc->GetStudies()) {
    // PushID(int) leaves a row addressable only by the numeric id ui_items
    // prints, which is why nothing had ever clicked this panel. The id goes
    // in the label instead (docs/ui/gui-coverage.md).
    const std::string id = s.name + "###study_sel_" + std::to_string(s.id);
    if (ImGui::Selectable(id.c_str(), sim.activeStudyId == s.id))
      sim.activeStudyId = s.id;
  }
}

// Every new body starts with the DEFAULT material, which deliberately carries
// no stiffness data (CADMaterials.h: fail rather than guess). So the FIRST Run
// on any new part fails — and it used to fail only after the click, as a
// tooltip on a small triangle. Say it up front, and offer the fix here rather
// than sending the user out of simulation mode to find the feature's Material
// combo.
void DrawMaterialCheck(AppState& state) {
  using CADMaterials::MaterialLibrary;
  Document* doc = state.document.get();
  if (!doc || !state.commands)
    return;
  const auto& types = MaterialLibrary::AllTypes();
  for (const Body& body : doc->GetBodies()) {
    const auto& mp =
        MaterialLibrary::GetMaterial(MaterialLibrary::FromSlug(body.material));
    if (mp.youngGPa > 0.0f)
      continue;
    size_t index = 0;
    Feature* owner = doc->FindFeatureById(body.createdBy, &index);
    if (!owner)
      continue;
    // A VISIBLE label, and no PushID: an ID scope or a hidden label leaves the
    // widget addressable only by the numeric id ui_items prints, and this is
    // the control that unblocks a user's first study. One combo per body, so
    // the id carries the feature it edits.
    const std::string comboId = "###material_" + std::to_string(body.createdBy);
    ImGui::TextColored(kError,
                       ICON_FA_TRIANGLE_EXCLAMATION " %s has no "
                                                    "material",
                       body.name.c_str());
    ImGui::TextDisabled("A study needs stiffness data to solve.");
    LeftLabel("Material");
    std::vector<const char*> labels;
    labels.reserve(types.size() + 1);
    labels.push_back("Assign a material...");
    for (auto t : types)
      labels.push_back(MaterialLibrary::GetMaterial(t).name.c_str());
    int sel = 0;
    if (ImGui::Combo(comboId.c_str(), &sel, labels.data(),
                     static_cast<int>(labels.size())) &&
        sel > 0) {
      nlohmann::json before = DocumentIO::FeatureToJson(*owner);
      owner->SetMaterial(MaterialLibrary::Slug(types[sel - 1]));
      nlohmann::json after = DocumentIO::FeatureToJson(*owner);
      state.commands->Execute(
          std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    }
  }
}

void DrawLoads(AppState& state, const fem::Study& s) {
  SimulationState& sim = state.simulation;

  ImGui::SeparatorText("Boundary conditions");
  ImGui::TextDisabled("Arm a tool, then click a face in the viewport:");
  WrapRow bcRow;
  auto arm = [&](const char* label, SimulationState::Tool t) {
    const bool armed = sim.tool == t;
    bcRow.Next(ButtonWidth(label));
    if (armed)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(label))
      SimulationArmTool(state, armed ? SimulationState::Tool::None : t);
    if (armed)
      ImGui::PopStyleColor();
  };
  arm(ICON_FA_LOCK " Fixed###sim_bc_fixed", SimulationState::Tool::Fixed);
  arm(ICON_FA_BOLT " Force###sim_bc_force", SimulationState::Tool::Force);
  arm(ICON_FA_DOWN_LONG " Pressure###sim_bc_pressure",
      SimulationState::Tool::Pressure);

  // Pending magnitude/direction applied to the next placed force/pressure.
  if (sim.tool == SimulationState::Tool::Force ||
      sim.tool == SimulationState::Tool::Pressure) {
    LeftLabel(sim.tool == SimulationState::Tool::Force ? "Magnitude (N)"
                                                       : "Pressure (MPa)");
    ExprInputDouble(state, "###sim_magnitude", &sim.pendingMagnitude, "%.1f");
    if (sim.tool == SimulationState::Tool::Force) {
      // Off by default: the force pushes INTO whichever face is clicked, which
      // is what placing a force on a face means. A world-space vector typed
      // before the face is even picked can only be right by luck.
      ImGui::Checkbox("Custom direction###sim_custom_dir",
                      &sim.directionOverridden);
      if (sim.directionOverridden) {
        float dir[3] = {(float)sim.pendingDirection.x,
                        (float)sim.pendingDirection.y,
                        (float)sim.pendingDirection.z};
        LeftLabel("Direction");
        if (ImGui::InputFloat3("###sim_direction", dir))
          sim.pendingDirection = glm::dvec3(dir[0], dir[1], dir[2]);
      } else {
        ImGui::TextDisabled("Direction: into the face you click");
      }
    }
  }

  if (s.loads.empty()) {
    ImGui::TextColored(kMuted, "No loads yet.");
    return;
  }
  for (const fem::Load& l : s.loads) {
    std::string line = std::string(LoadLabel(l.type)) + "  " + l.face.name;
    if (l.type != fem::LoadType::Fixed) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "  (%.1f%s)", l.magnitude,
                    l.type == fem::LoadType::Force ? " N" : " MPa");
      line += buf;
    }
    if (l.type == fem::LoadType::Force) {
      // A force's direction decides the answer; showing only its magnitude
      // hid the one number most likely to be wrong.
      char dir[64];
      std::snprintf(dir, sizeof(dir), "  [%.2g, %.2g, %.2g]", l.direction.x,
                    l.direction.y, l.direction.z);
      line += dir;
    }
    ImGui::TextUnformatted(line.c_str());
    ImGui::SameLine();
    const std::string del =
        std::string(ICON_FA_TRASH "###load_del_") + std::to_string(l.id);
    if (ImGui::SmallButton(del.c_str())) {
      const uint64_t loadId = l.id;
      ModifyStudy(state, s.id, "Remove load", [&](fem::Study& cs) {
        cs.loads.erase(
            std::remove_if(cs.loads.begin(), cs.loads.end(),
                           [&](const fem::Load& x) { return x.id == loadId; }),
            cs.loads.end());
      });
      break; // list mutated; rebuild next frame
    }
  }
}

// Topology results: an async Run (worker thread) with a live progress bar +
// Cancel, then the optimization summary and the Shape/Density display controls.
void DrawTopoResults(AppState& state, const fem::Study& s) {
  Document* doc = state.document.get();
  SimulationState& sim = state.simulation;
  ImGui::SeparatorText("Optimization");

  // Store a finished run (the GUI's per-frame join point).
  PollTopologyRun(state, *doc);
  PollStudyRun(state, *doc);
  const TopoRunStatus st = GetTopologyStatus(state, s.id);
  if (st.running) {
    const float frac =
        st.maxIterations > 0
            ? std::clamp(static_cast<float>(st.iteration) / st.maxIterations,
                         0.0f, 1.0f)
            : 0.0f;
    char ov[48];
    std::snprintf(ov, sizeof(ov), "iter %d / %d", st.iteration,
                  st.maxIterations);
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0), ov);
    ImGui::Text("Compliance: %.4g   Vf: %.3f", st.compliance,
                st.volumeFraction);
    ImGui::TextColored(kMuted, "%s   %.0fs", st.solver.c_str(),
                       st.elapsedSeconds);
    if (ImGui::Button(ICON_FA_STOP " Cancel###topo_cancel"))
      CancelTopologyRun(state, s.id);
    return;
  }

  if (ImGui::Button(ICON_FA_PLAY " Run###topo_run")) {
    std::string err;
    if (!StartTopologyRun(state, *doc, s.id, &err))
      LOG_WARN("topology run failed: {}", err);
  }
  if (!s.error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kError, ICON_FA_TRIANGLE_EXCLAMATION);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", s.error.c_str());
  }

  const fem::StudyRuntime* rt = doc->StudyResults(s.id);
  if (!rt) {
    ImGui::TextColored(kMuted, "Not run yet.");
    return;
  }
  if (doc->StudyResultsStale(s.id))
    ImGui::TextColored(kStale, ICON_FA_TRIANGLE_EXCLAMATION
                       " Stale (model changed since last run)");

  size_t nodes = 0, elements = 0;
  for (const auto& b : rt->bodies) {
    nodes += b.mesh.stats.nodeCount;
    elements += b.mesh.stats.tetCount;
  }
  ImGui::Text("Mesh: %zu nodes / %zu elements", nodes, elements);
  // In a static study the mesh resolves the stress field; in a topology study
  // it resolves the STRUCTURE — members thinner than an element cannot be
  // found, so a coarse mesh returns a coarse shape and looks like a bad
  // optimizer. The auto size is chosen for stress, and on a small part that
  // is a few hundred elements, which is one to two orders below what a 3D
  // SIMP design needs. Say it rather than let the shape speak for itself.
  if (elements < 2000)
    ImGui::TextColored(kStale, ICON_FA_TRIANGLE_EXCLAMATION
                       " Coarse for a shape: set a smaller mesh size");
  ImGui::Text("Iterations: %d%s", rt->iterations,
              rt->converged   ? " (converged)"
              : rt->cancelled ? " (cancelled)"
                              : "");
  ImGui::Text("Volume fraction: %.3f", rt->volumeFractionAchieved);
  if (!rt->solver.empty()) { // transient provenance; empty on a reload
    ImGui::TextColored(kMuted, "Solver: %s (%.0f s)", rt->solver.c_str(),
                       rt->solveSeconds);
    // The string carries WHY a run fell back off the GPU, and that is the
    // half a 210 px panel clips.
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", rt->solver.c_str());
  }
  if (!rt->complianceHistory.empty()) {
    ImGui::Text("Compliance: %.4g -> %.4g", rt->complianceHistory.front(),
                rt->complianceHistory.back());
    std::vector<float> hv(rt->complianceHistory.begin(),
                          rt->complianceHistory.end());
    ImGui::PlotLines("##compliance", hv.data(), static_cast<int>(hv.size()), 0,
                     nullptr, FLT_MAX, FLT_MAX, ImVec2(-FLT_MIN, 40));
  }

  // Display: the grey iso-surface (Shape), the density colormap (Density), or
  // the normal model (Off), plus the iso-surface threshold.
  ImGui::SeparatorText("Display");
  int field = sim.field == SimulationState::Field::Shape     ? 1
              : sim.field == SimulationState::Field::Density ? 2
                                                             : 0;
  WrapRow topoRow;
  topoRow.Next(RadioWidth("Off"));
  bool changed = ImGui::RadioButton("Off###topo_field_off", &field, 0);
  topoRow.Next(RadioWidth("Shape"));
  changed |= ImGui::RadioButton("Shape###topo_field_shape", &field, 1);
  topoRow.Next(RadioWidth("Density"));
  changed |= ImGui::RadioButton("Density###topo_field_density", &field, 2);
  if (changed)
    sim.field = field == 1   ? SimulationState::Field::Shape
                : field == 2 ? SimulationState::Field::Density
                             : SimulationState::Field::None;
  float thr = sim.topoThreshold;
  LeftLabel("Threshold (0=auto)");
  if (ImGui::SliderFloat("###topo_threshold", &thr, 0.0f, 1.0f, "%.2f"))
    sim.topoThreshold = thr;

  // Shape guide: show the optimized shape translucent in the modeling
  // workspace (visible after exiting simulation mode) to re-model against.
  bool ghost = state.topoGhostStudyId == s.id;
  if (ImGui::Checkbox("Shape guide (modeling overlay)###topo_ghost", &ghost)) {
    state.topoGhostStudyId = ghost ? s.id : 0;
    state.syncedDocumentVersion = UINT64_MAX; // rebuild renderers
  }
}

void DrawResults(AppState& state, const fem::Study& s) {
  Document* doc = state.document.get();
  SimulationState& sim = state.simulation;

  if (s.type == fem::AnalysisType::Topology) {
    DrawTopoResults(state, s);
    return;
  }

  ImGui::SeparatorText("Results");
  // The solve runs on a worker thread (StartStudyRun), the same as topology
  // and the same as the run_study RPC has always done. Clicking Run used to
  // call the blocking RunStudyAndCache straight from the draw, so the whole
  // app — not just this panel — stopped answering for the length of a solve,
  // with no progress and nothing to cancel. On a real part that is a hang.
  PollStudyRun(state, *doc);
  const StudyRunStatus run = GetStudyRunStatus(state, s.id);
  if (run.running) {
    ImGui::TextColored(kMuted, ICON_FA_HOURGLASS_HALF " Solving... %.0fs",
                       run.elapsedSeconds);
  } else if (ImGui::Button(ICON_FA_PLAY " Run###study_run")) {
    std::string err;
    if (!StartStudyRun(state, *doc, s.id, &err))
      LOG_WARN("run study failed: {}", err);
  }
  if (!s.error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kError, ICON_FA_TRIANGLE_EXCLAMATION);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", s.error.c_str());
  }

  const fem::StudyRuntime* rt = doc->StudyResults(s.id);
  if (!rt) {
    ImGui::TextColored(kMuted, "Not run yet.");
    return;
  }
  if (doc->StudyResultsStale(s.id))
    ImGui::TextColored(kStale, ICON_FA_TRIANGLE_EXCLAMATION
                       " Stale (model changed since last run)");

  size_t nodes = 0, elements = 0;
  for (const auto& b : rt->bodies) {
    nodes += b.mesh.stats.nodeCount;
    elements += b.mesh.stats.tetCount;
  }
  ImGui::Text("Mesh: %zu nodes / %zu elements", nodes, elements);

  if (s.type == fem::AnalysisType::Modal) {
    // Natural frequencies: a selectable list; picking one shows that mode
    // shape in the viewport.
    ImGui::SeparatorText("Natural frequencies");
    const fem::StudyBodyResult* b = nullptr;
    for (const auto& br : rt->bodies)
      if (!br.results.modes.empty()) {
        b = &br;
        break;
      }
    if (b) {
      for (int i = 0; i < (int)b->results.modes.size(); ++i) {
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "Mode %d:  %.4g Hz", i + 1,
                      b->results.modes[i].frequencyHz);
        if (ImGui::Selectable(lbl, sim.activeMode == i)) {
          sim.activeMode = i;
          sim.field = SimulationState::Field::Displacement; // show the shape
        }
      }
    }
    ImGui::SeparatorText("Display");
    bool showing = sim.field != SimulationState::Field::None;
    if (ImGui::Checkbox("Show mode shape###modal_show", &showing))
      sim.field = showing ? SimulationState::Field::Displacement
                          : SimulationState::Field::None;
    LeftLabel("Deform");
    ImGui::SliderFloat("###modal_deform", &sim.deformScale, 0.0f, 100.0f,
                       "%.0fx");
    return;
  }

  ImGui::Text("Max von Mises: %.3g MPa", rt->maxVonMises);
  ImGui::Text("Max displacement: %.4g mm", rt->maxDisplacement);

  // Field overlay controls (display only; no command). Each RadioButton must
  // draw every frame, so evaluate them independently (no short-circuit).
  ImGui::SeparatorText("Display");
  int field = std::to_underlying(sim.field);
  WrapRow fieldRow;
  fieldRow.Next(RadioWidth("Off"));
  bool changed = ImGui::RadioButton("Off###field_off", &field, 0);
  fieldRow.Next(RadioWidth("von Mises"));
  changed |=
      ImGui::RadioButton("von Mises###field_von_mises", &field,
                         std::to_underlying(SimulationState::Field::VonMises));
  fieldRow.Next(RadioWidth("Displacement"));
  changed |= ImGui::RadioButton(
      "Displacement###field_displacement", &field,
      std::to_underlying(SimulationState::Field::Displacement));
  if (changed)
    sim.field = static_cast<SimulationState::Field>(field);
  LeftLabel("Deform");
  ImGui::SliderFloat("###deform", &sim.deformScale, 0.0f, 100.0f, "%.0fx");
}

} // namespace

void DrawStudyPanel(AppState& state) {
  ImGui::Begin(kPanelModel);
  PanelHeading(kPanelModel);
  Document* doc = state.document.get();
  if (!doc) {
    ImGui::End();
    return;
  }

  DrawStudyList(state);

  fem::Study* s = doc->FindStudy(state.simulation.activeStudyId);
  if (!s) {
    ImGui::TextColored(kMuted, "Select or add a study.");
    ImGui::End();
    return;
  }

  ImGui::Spacing();
  char name[128];
  std::snprintf(name, sizeof(name), "%s", s->name.c_str());
  LeftLabel("Name");
  if (ImGui::InputText("###study_name", name, sizeof(name),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    const std::string newName = name;
    ModifyStudy(state, s->id, "Rename study",
                [&](fem::Study& cs) { cs.name = newName; });
  }
  // Analysis type: Static (stress), Modal (frequencies), or Topology
  // (generative shape).
  const char* kTypes[] = {"Static", "Modal", "Topology"};
  int typeIdx = s->type == fem::AnalysisType::Modal      ? 1
                : s->type == fem::AnalysisType::Topology ? 2
                                                         : 0;
  LeftLabel("Analysis");
  if (ImGui::Combo("###study_analysis", &typeIdx, kTypes, 3)) {
    const auto nt = typeIdx == 1   ? fem::AnalysisType::Modal
                    : typeIdx == 2 ? fem::AnalysisType::Topology
                                   : fem::AnalysisType::Static;
    ModifyStudy(state, s->id, "Set analysis type",
                [&](fem::Study& cs) { cs.type = nt; });
    // The displayed field belongs to the analysis that produced it. Leaving
    // "von Mises" selected across a switch to Topology left a colorbar on
    // screen titled von Mises with a 0 MPa maximum — a legend for a field
    // this study does not have, which reads as an answer.
    state.simulation.field = SimulationState::Field::None;
    state.simulation.activeMode = 0;
  }
  // Numeric fields commit on deactivation (Enter OR clicking away), not only on
  // Enter — so typing a value then clicking Run applies it instead of
  // reverting.
  double meshSize = s->meshSize;
  LeftLabel("Mesh size (mm, 0=auto)");
  ExprInputDouble(state, "###study_mesh_size", &meshSize, "%.3g");
  if (ImGui::IsItemDeactivatedAfterEdit() && meshSize != s->meshSize)
    ModifyStudy(state, s->id, "Set mesh size", [&](fem::Study& cs) {
      cs.meshSize = meshSize;
      cs.meshSizeExpr.clear();
    });

  if (s->type == fem::AnalysisType::Modal) {
    int nm = s->numModes;
    LeftLabel("Modes");
    const bool nmChanged = ImGui::InputInt("###study_modes", &nm);
    if ((nmChanged || ImGui::IsItemDeactivatedAfterEdit()) && nm >= 1 &&
        nm != s->numModes)
      ModifyStudy(state, s->id, "Set mode count",
                  [&](fem::Study& cs) { cs.numModes = nm; });
  } else if (s->type == fem::AnalysisType::Topology) {
    // Topology: target volume, minimum-member (filter) size, iteration cap.
    float vf = static_cast<float>(s->volumeFraction);
    LeftLabel("Volume fraction");
    if (ImGui::SliderFloat("###topo_volume_fraction", &vf, 0.05f, 0.9f, "%.2f"))
      ModifyStudy(state, s->id, "Set volume fraction", [&](fem::Study& cs) {
        cs.volumeFraction = vf;
        cs.volumeFractionExpr.clear();
      });
    double fr = s->filterRadius;
    LeftLabel("Filter radius (mm, 0=auto)");
    ExprInputDouble(state, "###topo_filter_radius", &fr, "%.3g");
    if (ImGui::IsItemDeactivatedAfterEdit() && fr != s->filterRadius)
      ModifyStudy(state, s->id, "Set filter radius", [&](fem::Study& cs) {
        cs.filterRadius = fr;
        cs.filterRadiusExpr.clear();
      });
    int mi = s->maxIterations;
    LeftLabel("Max iterations");
    const bool miChanged = ImGui::InputInt("###topo_max_iterations", &mi);
    if ((miChanged || ImGui::IsItemDeactivatedAfterEdit()) && mi >= 1 &&
        mi != s->maxIterations)
      ModifyStudy(state, s->id, "Set max iterations",
                  [&](fem::Study& cs) { cs.maxIterations = mi; });
    // Keep faces: frozen solid, added by picking a face with the Keep tool.
    SimulationState& sim = state.simulation;
    const bool arming = sim.tool == SimulationState::Tool::Keep;
    if (arming)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_FA_LOCK " Keep face###topo_keep_face"))
      sim.tool =
          arming ? SimulationState::Tool::None : SimulationState::Tool::Keep;
    if (arming)
      ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%zu kept", s->keepFaces.size());
    for (size_t i = 0; i < s->keepFaces.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImGui::TextColored(kMuted, "%s", s->keepFaces[i].name.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton(ICON_FA_TRASH)) {
        const ElementRef f = s->keepFaces[i];
        ModifyStudy(state, s->id, "Remove keep face", [&](fem::Study& cs) {
          cs.keepFaces.erase(
              std::remove_if(cs.keepFaces.begin(), cs.keepFaces.end(),
                             [&](const ElementRef& k) {
                               return k.featureId == f.featureId &&
                                      k.name == f.name;
                             }),
              cs.keepFaces.end());
        });
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
    }
  } else {
    // Static: an optional gravity / self-weight load.
    bool on = s->gravity.x != 0 || s->gravity.y != 0 || s->gravity.z != 0;
    if (ImGui::Checkbox("Gravity (self-weight)###study_gravity", &on)) {
      // -Y, not -Z: world up is +Y here (Camera::Up, the XZ "top" plane, the
      // height axis of every primitive). Self-weight pointing along -Z sagged
      // the part sideways.
      const glm::dvec3 g = on ? glm::dvec3(0, -9.81, 0) : glm::dvec3(0, 0, 0);
      ModifyStudy(state, s->id, "Toggle gravity",
                  [&](fem::Study& cs) { cs.gravity = g; });
    }
    if (on) {
      float g[3] = {(float)s->gravity.x, (float)s->gravity.y,
                    (float)s->gravity.z};
      LeftLabel("g (m/s^2)");
      // Commit on deactivation (Enter OR clicking away), like the mesh-size
      // field above. NOT EnterReturnsTrue: that flag is InputText-only —
      // InputScalar ASSERTS on it, so this line aborted every Debug build
      // (a fresh clone's default) the moment gravity was switched on.
      ImGui::InputFloat3("###study_gravity_vec", g, "%.2f");
      if (ImGui::IsItemDeactivatedAfterEdit())
        ModifyStudy(state, s->id, "Set gravity", [&](fem::Study& cs) {
          cs.gravity = glm::dvec3(g[0], g[1], g[2]);
        });
    }
  }

  DrawMaterialCheck(state);
  DrawLoads(state, *s);
  DrawResults(state, *s);

  if (ImGui::Button(ICON_FA_TRASH " Delete study###study_delete")) {
    const uint64_t id = s->id;
    state.commands->Execute(std::make_unique<RemoveStudyCommand>(id), *doc);
    state.simulation.activeStudyId =
        doc->GetStudies().empty() ? 0 : doc->GetStudies().front().id;
  }

  ImGui::End();
}

namespace {
// Analytic Turbo colormap (matches field.frag) for the legend colorbar.
ImU32 Turbo(float x) {
  x = x < 0 ? 0 : (x > 1 ? 1 : x);
  const float r = 0.13572138f +
                  x * (4.61539260f + x * (-42.66032258f + x * 132.13108234f)) +
                  x * x * x * x * (-152.94239396f + x * 59.28637943f);
  const float g = 0.09140261f +
                  x * (2.19418839f + x * (4.84296658f + x * -14.18503333f)) +
                  x * x * x * x * (4.27729857f + x * 2.82956604f);
  const float b = 0.10667330f +
                  x * (12.64194608f + x * (-60.58204836f + x * 110.36276771f)) +
                  x * x * x * x * (-89.90310912f + x * 27.34824973f);
  auto ch = [](float v) {
    return (int)(255.0f * (v < 0 ? 0 : (v > 1 ? 1 : v)));
  };
  return IM_COL32(ch(r), ch(g), ch(b), 255);
}
} // namespace

void DrawFieldLegend(AppState& state) {
  const SimulationState& sim = state.simulation;
  // No colorbar for "off" or the grey Shape iso-surface (it has no scalar).
  if (!sim.IsActive() || sim.field == SimulationState::Field::None ||
      sim.field == SimulationState::Field::Shape || !state.document)
    return;
  const fem::StudyRuntime* rt = state.document->StudyResults(sim.activeStudyId);
  if (!rt)
    return;
  const fem::Study* study = state.document->FindStudy(sim.activeStudyId);
  const bool modal = study && study->type == fem::AnalysisType::Modal;
  const bool density = sim.field == SimulationState::Field::Density;
  // A stress/displacement colorbar over a topology result (or a density one
  // over a static result) describes a field that study never computed.
  const bool topo = study && study->type == fem::AnalysisType::Topology;
  if (topo != density)
    return;
  const bool vm = sim.field == SimulationState::Field::VonMises && !modal;
  // Modal shows a normalized mode shape (0..1); density is 0..1; static shows
  // the real field.
  const double maxVal =
      (modal || density) ? 1.0 : (vm ? rt->maxVonMises : rt->maxDisplacement);
  const char* unit = (modal || density) ? "" : (vm ? "MPa" : "mm");
  const char* title = density ? "density"
                      : modal ? "mode shape"
                      : vm    ? "von Mises"
                              : "displacement";

  // A vertical Turbo colorbar anchored to the viewport's lower-left corner.
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float x = state.viewportPos.x + 16.0f;
  const float barW = 18.0f, barH = 180.0f;
  const float y1 = state.viewportPos.y + state.viewportSize.y - 28.0f;
  const float y0 = y1 - barH;
  const int segs = 48;
  for (int i = 0; i < segs; ++i) {
    const float t0 = i / (float)segs, t1 = (i + 1) / (float)segs;
    // High value at the top.
    dl->AddRectFilledMultiColor(ImVec2(x, y1 - t1 * barH),
                                ImVec2(x + barW, y1 - t0 * barH), Turbo(t1),
                                Turbo(t1), Turbo(t0), Turbo(t0));
  }
  dl->AddRect(ImVec2(x, y0), ImVec2(x + barW, y1),
              IM_COL32(255, 255, 255, 160));
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3g %s", maxVal, unit);
  dl->AddText(ImVec2(x + barW + 6, y0 - 6), IM_COL32_WHITE, buf);
  dl->AddText(ImVec2(x + barW + 6, y1 - 6), IM_COL32_WHITE, "0");
  dl->AddText(ImVec2(x, y0 - 22), IM_COL32_WHITE, title);
}

namespace {
// A world-space arrow from a to b with a two-line head.
void ArrowGlyph(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color,
                std::vector<PrimVertex>& out) {
  ui::AppendPrimLine(a, b, 2.2f, color, out);
  const glm::vec3 dir = glm::normalize(b - a);
  glm::vec3 side =
      glm::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  side = glm::normalize(glm::cross(dir, side));
  const float headLen = glm::length(b - a) * 0.25f;
  ui::AppendPrimLine(b, b - dir * headLen + side * headLen * 0.5f, 2.2f, color,
                     out);
  ui::AppendPrimLine(b, b - dir * headLen - side * headLen * 0.5f, 2.2f, color,
                     out);
}
} // namespace

// Dragging a force arrow changes its MAGNITUDE. The mapping is relative, not
// absolute: pulling one arrow-length doubles the force, pushing back toward
// the face shrinks it to zero. An absolute mapping would need the arrow's
// length to mean newtons, and it cannot — the glyph is view-scaled so it
// stays visible on a 90 mm part and a 3 m dock alike.
//
// Snapshot at grab, mutate live, one ModifyStudyCommand on release: the
// gizmo's contract, so a drag is one undo step and the solved results go
// stale exactly once.
bool HandleLoadDragInput(AppState& state, const Ray& ray, bool clicked,
                         bool down, bool released) {
  SimulationState& sim = state.simulation;
  Document* doc = state.document.get();
  if (!sim.IsActive() || doc == nullptr)
    return false;
  fem::Study* study = doc->FindStudy(sim.activeStudyId);
  if (study == nullptr)
    return false;

  const auto findLoad = [&](uint64_t id) -> fem::Load* {
    for (fem::Load& l : study->loads)
      if (l.id == id)
        return &l;
    return nullptr;
  };

  if (sim.loadDrag.active) {
    fem::Load* load = findLoad(sim.loadDrag.loadId);
    if (released || !down || load == nullptr) {
      sim.loadDrag.active = false;
      if (load != nullptr && !sim.loadDrag.beforeJson.empty()) {
        // One undo entry for the whole drag: replay the before/after pair
        // the command stack expects.
        const nlohmann::json after = DocumentIO::StudyToJson(*study);
        const nlohmann::json before =
            nlohmann::json::parse(sim.loadDrag.beforeJson, nullptr, false);
        if (!before.is_discarded() && before != after && state.commands)
          state.commands->Execute(
              std::make_unique<ModifyStudyCommand>("Drag force", before, after),
              *doc);
      }
      return true;
    }
    const glm::vec3 dir = glm::normalize(
        glm::vec3(load->direction.x, load->direction.y, load->direction.z));
    const float len = state.camera
                          ? std::max(0.055f * state.camera->GetDistance(), 1.2f)
                          : 1.2f;
    const float t =
        RayAxisParam(ray.origin, ray.direction, glm::vec3(0.0f), dir);
    const double scale =
        1.0 + static_cast<double>(t - sim.loadDrag.startParam) / len;
    load->magnitude = std::max(0.0, sim.loadDrag.startMagnitude * scale);
    load->magnitudeExpr.clear(); // a dragged value is a number, not a formula
    // No staleness bump mid-drag: the ModifyStudyCommand on release is what
    // moves the document version, so results go stale exactly once — the
    // gizmo's solve-on-release contract.
    return true;
  }

  if (!clicked || OwnerOf(state.hoveredHandle) != HandleOwner::Load)
    return false;
  fem::Load* load = findLoad(SubOf(state.hoveredHandle));
  if (load == nullptr)
    return false;
  const glm::vec3 dir = glm::normalize(
      glm::vec3(load->direction.x, load->direction.y, load->direction.z));
  sim.loadDrag.active = true;
  sim.loadDrag.loadId = load->id;
  sim.loadDrag.startMagnitude = load->magnitude;
  sim.loadDrag.startParam =
      RayAxisParam(ray.origin, ray.direction, glm::vec3(0.0f), dir);
  sim.loadDrag.beforeJson = DocumentIO::StudyToJson(*study).dump();
  return true; // consumed: do not also re-pick the face behind the arrow
}

void AppendSimulationBCGlyphs(AppState& state, std::vector<PrimVertex>& out) {
  SimulationState& sim = state.simulation;
  Document* doc = state.document.get();
  if (!sim.IsActive() || doc == nullptr || !state.faceSelector)
    return;
  const fem::Study* s = doc->FindStudy(sim.activeStudyId);
  if (s == nullptr)
    return;
  const glm::vec4 kFixed{0.95f, 0.35f, 0.35f, 1.0f};
  const glm::vec4 kForce{1.00f, 0.65f, 0.20f, 1.0f};
  const glm::vec4 kPressure{0.95f, 0.85f, 0.30f, 1.0f};
  const glm::vec4 kKeep{0.35f, 0.85f, 0.95f, 1.0f};

  const auto faceFrame = [&](const ElementRef& ref, glm::vec3& center,
                             glm::vec3& normal) {
    uint32_t mesh = 0, index = 0;
    if (!ResolveFaceItem(state, {SelectionItem::Kind::Face, 0, ref}, mesh,
                         index))
      return false;
    center = state.faceSelector->GetFaceCenter(mesh, index);
    normal = state.faceSelector->GetFaceNormal(mesh, index);
    return true;
  };
  // Glyph scale tracks the view (like the peak marker): a fixed 1.2 model-mm
  // arrow vanished entirely on a 90 mm part.
  const float len = state.camera
                        ? std::max(0.055f * state.camera->GetDistance(), 1.2f)
                        : 1.2f;

  for (const fem::Load& load : s->loads) {
    glm::vec3 c(0.0f), n(0.0f);
    if (!faceFrame(load.face, c, n))
      continue;
    switch (load.type) {
    case fem::LoadType::Fixed: {
      // Anchor hatch: a ground line with strokes fanning off it, in BOTH
      // in-plane axes. It used to draw along `side` only — `other` was
      // computed and discarded with a (void) cast — so from most angles a
      // fixed support was a single red stick lying on the face.
      glm::vec3 side =
          glm::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
      side = glm::normalize(glm::cross(n, side));
      const glm::vec3 other = glm::normalize(glm::cross(n, side));
      for (const glm::vec3& axis : {side, other}) {
        ui::AppendPrimLine(c - axis * len * 0.4f, c + axis * len * 0.4f, 2.4f,
                           kFixed, out);
        for (int i = -1; i <= 1; i++) {
          const glm::vec3 base = c + axis * (len * 0.3f * i);
          ui::AppendPrimLine(base, base - n * len * 0.3f - axis * len * 0.15f,
                             1.8f, kFixed, out);
        }
      }
      break;
    }
    case fem::LoadType::Force: {
      const glm::vec3 dir = glm::normalize(
          glm::vec3(load.direction.x, load.direction.y, load.direction.z));
      // The arrow flies ALONG the force; which end sits on the face depends
      // on where the shaft is visible. A push approaches from outside and
      // lands head-on the face. A pull drawn the same way would put the whole
      // shaft INSIDE the solid — depth-tested away, an invisible load — so a
      // pull takes off from the face instead: tail on the face, head out.
      const glm::vec3 tail = glm::dot(dir, n) > 0.0f ? c : c - dir * len;
      // Draggable (docs/ui/viewport-handles.md): the shaft is the handle, so
      // what highlights is exactly what a click grabs.
      ViewportHandle vh;
      vh.id = HandleId(HandleOwner::Load, load.id);
      vh.kind = HandleKind::Linear;
      vh.origin = tail;
      vh.dir = dir;
      vh.length = len;
      vh.color = glm::vec3(kForce);
      state.viewportHandles.push_back(vh);
      const glm::vec3 tint = HandleTint(
          vh, state.hoveredHandle,
          sim.loadDrag.active && sim.loadDrag.loadId == load.id ? vh.id : 0);
      ArrowGlyph(tail, tail + dir * len, glm::vec4(tint, kForce.a), out);
      break;
    }
    case fem::LoadType::Pressure: {
      // Pressure presses along -normal: a shorter arrow at the face.
      ArrowGlyph(c + n * len * 0.8f, c, kPressure, out);
      break;
    }
    }
  }
  for (const ElementRef& keep : s->keepFaces) {
    glm::vec3 c(0.0f), n(0.0f);
    if (!faceFrame(keep, c, n))
      continue;
    glm::vec3 side =
        glm::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    side = glm::normalize(glm::cross(n, side));
    const glm::vec3 other = glm::normalize(glm::cross(n, side));
    const float r = len * 0.35f;
    const glm::vec3 pts[4] = {c + side * r, c + other * r, c - side * r,
                              c - other * r};
    for (int i = 0; i < 4; i++)
      ui::AppendPrimLine(pts[i], pts[(i + 1) % 4], 2.0f, kKeep, out);
  }
}

void SimulationApplyToFace(AppState& state, const ElementRef& face) {
  SimulationState& sim = state.simulation;
  Document* doc = state.document.get();
  if (!doc || sim.tool == SimulationState::Tool::None)
    return;
  fem::Study* s = doc->FindStudy(sim.activeStudyId);
  if (!s)
    return;

  // Keep tool (topology): freeze the picked face solid.
  if (sim.tool == SimulationState::Tool::Keep) {
    ModifyStudy(state, s->id, "Add keep face", [&](fem::Study& cs) {
      const bool dup = std::any_of(
          cs.keepFaces.begin(), cs.keepFaces.end(), [&](const ElementRef& k) {
            return k.featureId == face.featureId && k.name == face.name;
          });
      if (!dup)
        cs.keepFaces.push_back(face);
    });
    sim.tool = SimulationState::Tool::None;
    return;
  }

  fem::Load load;
  load.id = doc->AllocateId();
  load.type = sim.tool == SimulationState::Tool::Fixed ? fem::LoadType::Fixed
              : sim.tool == SimulationState::Tool::Force
                  ? fem::LoadType::Force
                  : fem::LoadType::Pressure;
  load.face = face;
  if (load.type != fem::LoadType::Fixed)
    load.magnitude = sim.pendingMagnitude;
  if (load.type == fem::LoadType::Force) {
    // The face decides the direction unless the user asked for a specific
    // one: a force placed on a face pushes into it.
    const auto into = fem::DefaultForceDirection(*doc, face);
    load.direction =
        sim.directionOverridden || !into ? sim.pendingDirection : *into;
  }

  ModifyStudy(state, s->id,
              std::string("Add ") + LoadLabel(load.type) + " load",
              [&](fem::Study& cs) { cs.loads.push_back(load); });
  sim.tool = SimulationState::Tool::None; // one placement per arm
}

} // namespace ui
