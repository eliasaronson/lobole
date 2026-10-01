#include "ui/StudyPanel.h"
#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/Command.h"
#include "core/Conventions.h"
#include "core/MassProps.h"
#include "core/fem/FemStudy.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "render/MeshRenderer.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h" // FindBackgroundJob // OccurrenceSubtreeBodies, PartDocById
#include "ui/DockLayout.h"      // PanelHeading
#include "ui/ExprInput.h"       // expression-capable numeric fields
#include "ui/Overlays.h"        // ViewportDrawList
#include "ui/PanelLayout.h"     // LeftLabel / WrapRow (readable at dock width)
#include "ui/PanelTitles.h"
#include "ui/PreviewFeature.h"    // BeginToolWindowFrame
#include "ui/SelectionPipeline.h" // ResolveFaceItem (BC glyphs)
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h" // WorldToScreen (labels)
#include "ui/SketchOverlay.h"
#include "ui/StudyHost.h"
#include "ui/ViewportHandle.h"
#include "ui/Widgets.h"
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

// A message line, wrapped at the width the participants legend uses. The
// Simulation window sizes itself to its content, so ONE unwrapped sentence
// (a contact planner's refusal, a missing material) set its width: it grew
// to the whole app and covered the viewport it asks you to click in. And its
// rows' right-aligned tails then hold the width they were given, so the next
// tool session opened just as wide (docs/ui/panels.md#floating-windows).
void MessageText(const ImVec4& color, const std::string& text) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 22.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
}

// Runs one study edit as an undoable ModifyStudyCommand: snapshot before,
// apply `mutate` to a copy, snapshot after, execute. Mirrors the RPC path.
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

// Deletes a study from its host, as one undo step, and moves the selection
// to the first that is left.
void DeleteStudy(AppState& state, uint64_t id) {
  const StudyHost host = StudyHostOf(state);
  if (host.part) {
    state.commands->Execute(std::make_unique<RemoveStudyCommand>(id),
                            *host.part);
  } else {
    SnapshotAssembly(state);
    host.asmDoc->RemoveStudy(id);
  }
  state.simulation.activeStudyId =
      host.Studies().empty() ? 0 : host.Studies().front().id;
}

const char* StudyGlyph(fem::AnalysisType type) {
  switch (type) {
  case fem::AnalysisType::Static:
    return ICON_FA_WEIGHT_HANGING;
  case fem::AnalysisType::Modal:
    return ICON_FA_WAVE_SQUARE;
  case fem::AnalysisType::Topology:
    return ICON_FA_CIRCLE_NODES;
  }
  return ICON_FA_WEIGHT_HANGING;
}

// Where a study stands, in a word, for its row's tail.
std::string StudyStateWord(AppState& state, const fem::Study& s) {
  if (GetStudyRunStatus(state, s.id).running)
    return "solving";
  if (!s.error.empty())
    return "failed";
  if (StudyHostOf(state).Results(s.id) == nullptr)
    return "not run";
  return StudyStale(state, s.id) ? "stale" : "solved";
}

// The studies as rows in the feature list's style (ui::ListRow,
// docs/ui/panels.md#simulation-the-study-panel-and-window): a glyph for the
// analysis, the name, and where it stands as the tail. Click selects -- its
// editor is the Simulation window -- and Delete is in the row's menu, where a
// big button at the bottom of the editor used to be.
void DrawStudyList(AppState& state) {
  const StudyHost host = StudyHostOf(state);
  SimulationState& sim = state.simulation;

  ImGui::SeparatorText("Studies");
  if (ImGui::Button(ICON_FA_PLUS " Add study###study_add"))
    sim.activeStudyId = AddStudyOn(state, "");

  uint64_t toDelete = 0;
  for (const fem::Study& s : host.Studies()) {
    // The id goes in the label, not a PushID(int): a PushID scope is not
    // addressable from a test path
    // (docs/ui/testing.md#making-a-widget-addressable).
    ListRowSpec row;
    row.glyph = StudyGlyph(s.type);
    row.label = s.name + "###study_sel_" + std::to_string(s.id);
    row.selected = sim.activeStudyId == s.id;
    row.tail = StudyStateWord(state, s);
    if (!s.error.empty()) {
      row.glyphColor = kError;
      row.tooltip = s.error;
    }
    row.menuId = "###study_ctx_" + std::to_string(s.id);
    const uint64_t id = s.id;
    row.menu = [&toDelete, id] {
      if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Delete###study_delete"))
        toDelete = id;
    };
    const ListRowResult res = ListRow(row);
    if (res.clicked || res.doubleClicked)
      sim.activeStudyId = s.id;
  }
  if (toDelete != 0)
    DeleteStudy(state, toDelete);
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
    const auto& mp = MaterialLibrary::GetMaterialOrDefault(body.material);
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
    MessageText(kError, ICON_FA_TRIANGLE_EXCLAMATION " " + body.name +
                            " has no material");
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
  ImGui::Checkbox("Labels in viewport###sim_show_labels", &sim.showLabels);

  // Pending magnitude/direction applied to the next placed force/pressure.
  if (sim.tool == SimulationState::Tool::Force ||
      sim.tool == SimulationState::Tool::Pressure) {
    LeftLabel(sim.tool == SimulationState::Tool::Force ? "Magnitude (N)"
                                                       : "Pressure (MPa)");
    std::string magExpr;
    if (ExprInputDouble(state, "###sim_magnitude", &sim.pendingMagnitude,
                        "%.1f", &magExpr))
      sim.pendingMagnitudeExpr = magExpr; // empty for a plain number
    if (sim.tool == SimulationState::Tool::Force) {
      // Off by default: the force pushes INTO whichever face is clicked, which
      // is what placing a force on a face means. A world-space vector typed
      // before the face is even picked can only be right by luck.
      ImGui::Checkbox("Custom direction###sim_custom_dir",
                      &sim.directionOverridden);
      if (sim.directionOverridden) {
        LeftLabel("Direction");
        ExprInputDouble3(state, "###sim_direction", &sim.pendingDirection.x,
                         "%.3f");
      } else {
        ImGui::TextDisabled("Direction: into the face you click");
      }
    }
  }

  if (s.loads.empty()) {
    ImGui::TextColored(kMuted, "No loads yet.");
    return;
  }
  if (!sim.placeRefusal.empty())
    MessageText(kError, sim.placeRefusal);
  const StudyHost host = StudyHostOf(state);
  // Rows, as every list (ui::ListRow): the face as the label, the magnitude
  // as the tail, and Remove in the row's menu instead of a trash button.
  uint64_t removeLoad = 0;
  for (const fem::Load& l : s.loads) {
    std::string where;
    // An assembly study's anchor says whose face, or whose contact (S1).
    if (l.contactWith != 0)
      where = "contact with " + host.OccurrenceName(l.contactWith);
    else if (l.occurrenceId != 0)
      where = host.OccurrenceName(l.occurrenceId) + " " + l.face.name;
    else
      where = l.face.name;
    ListRowSpec row;
    row.glyph = l.type == fem::LoadType::Fixed   ? ICON_FA_LOCK
                : l.type == fem::LoadType::Force ? ICON_FA_BOLT
                                                 : ICON_FA_DOWN_LONG;
    row.label = std::string(LoadLabel(l.type)) + "  " + where + "###load_row_" +
                std::to_string(l.id);
    if (l.type != fem::LoadType::Fixed) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.1f%s", l.magnitude,
                    l.type == fem::LoadType::Force ? " N" : " MPa");
      row.tail = buf;
    }
    if (l.type == fem::LoadType::Force) {
      // A force's direction decides the answer; showing only its magnitude
      // hid the one number most likely to be wrong.
      char dir[80];
      std::snprintf(dir, sizeof(dir), "Direction [%.2g, %.2g, %.2g]",
                    l.direction.x, l.direction.y, l.direction.z);
      row.tooltip = dir;
    }
    row.menuId = "###load_ctx_" + std::to_string(l.id);
    const uint64_t loadId = l.id;
    row.menu = [&removeLoad, loadId] {
      if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Remove###load_del"))
        removeLoad = loadId;
    };
    ListRow(row);
  }
  if (removeLoad != 0)
    ModifyStudy(state, s.id, "Remove load", [&](fem::Study& cs) {
      cs.loads.erase(std::remove_if(cs.loads.begin(), cs.loads.end(),
                                    [&](const fem::Load& x) {
                                      return x.id == removeLoad;
                                    }),
                     cs.loads.end());
    });
}

// Which study's last Run was answered from a stored result
// (docs/simulation/README.md#studies-as-jobs), so the panel can say why
// nothing ran. Panel state only: the result itself is the study's.
uint64_t g_reusedStudy = 0;

void NoteReuse(uint64_t studyId, const JobStart& start) {
  g_reusedStudy = start.reused ? studyId : 0;
}

void RunButtonTip() {
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Unchanged inputs reuse the stored result instead of "
                      "computing it again. Shift+click to compute anyway.");
}

// Reused: shown under the result it is about.
void DrawReuseNote(uint64_t studyId) {
  if (g_reusedStudy == studyId)
    ImGui::TextColored(kMuted, "Reused: these inputs were already solved.");
}

// Topology results: an async Run (worker thread) with a live progress bar +
// Cancel, then the optimization summary and the Shape/Density display controls.
void DrawTopoResults(AppState& state, const fem::Study& s) {
  Document* doc = state.document.get();
  if (!doc)
    return; // topology is a part study
  SimulationState& sim = state.simulation;
  ImGui::SeparatorText("Optimization");

  // No poll here any more: this panel is drawn only in Simulation mode, so
  // leaving that mode stranded a finished solve until you came back. Every
  // loop polls instead (ui/BackgroundJobs.h); this panel only reads.
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
    JobProgress bar; // the same widget the import modal draws (ui/Widgets.h)
    bar.fraction = st.maxIterations > 0 ? frac : -1.0;
    bar.overlay = ov;
    // The widget's own Cancel, as every job's: this one drew a second button
    // of its own under the bar.
    const BackgroundJob* bg = FindBackgroundJob(state, "topo");
    bar.cancelAsked = bg != nullptr && bg->cancel.load();
    bar.cancelId = "###topo_cancel";
    bar.cancelTip = "Stop after this iteration, keeping the best so far.";
    if (DrawJobProgress(bar))
      CancelTopologyRun(state, s.id);
    ImGui::Text("Compliance: %.4g   Vf: %.3f", st.compliance,
                st.volumeFraction);
    ImGui::TextColored(kMuted, "%s   %.0fs", st.solver.c_str(),
                       st.elapsedSeconds);
    return;
  }

  if (ImGui::Button(ICON_FA_PLAY " Run###topo_run")) {
    std::string err;
    JobStart start;
    start.recompute = ImGui::GetIO().KeyShift;
    if (!StartTopologyRun(state, *doc, s.id, &err, &start))
      LOG_WARN("topology run failed: {}", err);
    NoteReuse(s.id, start);
  }
  RunButtonTip();
  if (!s.error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kError, ICON_FA_TRIANGLE_EXCLAMATION);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", s.error.c_str());
  }

  DrawReuseNote(s.id);
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
  const StudyHost host = StudyHostOf(state);
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
  // (The join happens in ui::PollBackgroundJobs, once per loop iteration —
  // not here, where it only ran while this panel was on screen.)
  const StudyRunStatus run = GetStudyRunStatus(state, s.id);
  if (run.running) {
    // The shared bar (ui/Widgets.h), indeterminate: the linear solver has no
    // progress to report. Its Cancel lands at the next boundary -- after the
    // meshing, or when the factorization ends -- and drops the answer.
    JobProgress bar;
    bar.status = "Solving";
    bar.overlay = std::to_string(static_cast<int>(run.elapsedSeconds)) + " s";
    BackgroundJob* bg = FindBackgroundJob(state, "study");
    bar.cancellable = bg != nullptr;
    bar.cancelAsked = bg != nullptr && bg->cancel.load();
    bar.cancelId = "###study_cancel";
    bar.cancelTip = "Stop after the meshing, or when the solve ends; the "
                    "answer is dropped.";
    if (DrawJobProgress(bar) && bg != nullptr)
      CancelBackgroundJobById(state, bg->id, "the Cancel button");
  } else {
    if (ImGui::Button(ICON_FA_PLAY " Run###study_run")) {
      std::string err;
      JobStart start;
      start.recompute = ImGui::GetIO().KeyShift;
      const bool started =
          host.IsAssembly()
              ? StartAssemblyStudyRun(state, *host.asmDoc, s.id, &err, &start)
              : StartStudyRun(state, *host.part, s.id, &err, &start);
      if (!started)
        LOG_WARN("run study failed: {}", err);
      NoteReuse(s.id, start);
    }
    RunButtonTip();
  }
  if (!s.error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kError, ICON_FA_TRIANGLE_EXCLAMATION);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", s.error.c_str());
  }

  DrawReuseNote(s.id);
  const fem::StudyRuntime* rt = host.Results(s.id);
  if (!rt) {
    ImGui::TextColored(kMuted, "Not run yet.");
    return;
  }
  if (StudyStale(state, s.id))
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

namespace {
// The participants table of an ASSEMBLY study (S1): every top-level
// occurrence with its role, a mesh size on the meshed one, and on each rigid
// support the clamp at its contact — "fix this panel where that part touches
// it", which is the cheap half of an assembly study and the reason S1
// exists.
void DrawParticipants(AppState& state, const fem::Study& s) {
  const StudyHost host = StudyHostOf(state);
  if (!host.IsAssembly())
    return;
  ImGui::SeparatorText("Participants");
  // Wrapped, or this one line sets the whole auto-sized window's width.
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 22.0f);
  ImGui::TextDisabled("Meshed: solved. Bonded: glued to it, solved with "
                      "it. Rigid: a support, clamped where it touches. "
                      "Ignored: absent.");
  ImGui::PopTextWrapPos();
  // All four roles. Bonded (S2) was missing: the GUI could not bond a
  // participant, and one bonded over the RPC read here as "Ignored", because
  // the index below fell through to 0.
  const char* kRoles[] = {"Ignored", "Rigid", "Meshed", "Bonded"};
  // No PushID(int) around the rows: every widget carries its occurrence id
  // in its own ###label, which is what a test path can address (a PushID
  // scope is not — docs/ui/testing.md#making-a-widget-addressable).
  for (const Occurrence& occ : host.asmDoc->GetOccurrences()) {
    const fem::Participant* pt = s.FindParticipant(occ.id);
    int role = !pt                                        ? 0
               : pt->role == fem::ParticipantRole::Meshed ? 2
               : pt->role == fem::ParticipantRole::Rigid  ? 1
               : pt->role == fem::ParticipantRole::Bonded ? 3
                                                          : 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    const std::string comboId = "###role_" + std::to_string(occ.id);
    if (ImGui::Combo(comboId.c_str(), &role, kRoles, 4)) {
      const uint64_t id = occ.id;
      const fem::ParticipantRole want =
          role == 3   ? fem::ParticipantRole::Bonded
          : role == 2 ? fem::ParticipantRole::Meshed
          : role == 1 ? fem::ParticipantRole::Rigid
                      : fem::ParticipantRole::Ignored;
      ModifyStudy(state, s.id, "Set participant role", [&](fem::Study& cs) {
        auto it = std::find_if(
            cs.participants.begin(), cs.participants.end(),
            [&](const fem::Participant& x) { return x.occurrenceId == id; });
        if (want == fem::ParticipantRole::Ignored) {
          if (it != cs.participants.end())
            cs.participants.erase(it);
          // Its contact clamps go with it: a clamp on a support that is
          // absent clamps nothing.
          cs.loads.erase(std::remove_if(cs.loads.begin(), cs.loads.end(),
                                        [&](const fem::Load& l) {
                                          return l.contactWith == id;
                                        }),
                         cs.loads.end());
          return;
        }
        if (it == cs.participants.end()) {
          fem::Participant np;
          np.occurrenceId = id;
          np.role = want;
          cs.participants.push_back(np);
        } else {
          it->role = want;
        }
        if (want == fem::ParticipantRole::Meshed) // one meshed in S1
          for (fem::Participant& other : cs.participants)
            if (other.occurrenceId != id &&
                other.role == fem::ParticipantRole::Meshed)
              other.role = fem::ParticipantRole::Rigid;
      });
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(occ.name.c_str());
    if (pt && pt->role == fem::ParticipantRole::Rigid) {
      bool clamped = false;
      for (const fem::Load& l : s.loads)
        clamped = clamped || l.contactWith == occ.id;
      ImGui::SameLine();
      const std::string clampId =
          std::string(clamped ? ICON_FA_LOCK " Clamped###clamp_"
                              : ICON_FA_LOCK_OPEN " Clamp here###clamp_") +
          std::to_string(occ.id);
      if (ImGui::SmallButton(clampId.c_str())) {
        const uint64_t id = occ.id;
        ModifyStudy(state, s.id, clamped ? "Unclamp contact" : "Clamp contact",
                    [&](fem::Study& cs) {
                      if (clamped) {
                        cs.loads.erase(
                            std::remove_if(cs.loads.begin(), cs.loads.end(),
                                           [&](const fem::Load& l) {
                                             return l.contactWith == id;
                                           }),
                            cs.loads.end());
                        return;
                      }
                      fem::Load clamp;
                      clamp.id = host.AllocateId();
                      clamp.type = fem::LoadType::Fixed;
                      clamp.contactWith = id;
                      cs.loads.push_back(clamp);
                    });
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fix the meshed part where %s touches it. The "
                          "patch is found by imprinting when the study "
                          "meshes; it exists in neither part.",
                          occ.name.c_str());
    }
    if (pt && pt->role == fem::ParticipantRole::Meshed) {
      double size = pt->meshSize;
      std::string expr;
      LeftLabel("Its mesh size (mm, 0=study's)");
      const std::string sizeId = "###part_mesh_" + std::to_string(occ.id);
      const bool changed =
          ExprInputDouble(state, sizeId.c_str(), &size, "%.3g", &expr);
      if ((changed || ImGui::IsItemDeactivatedAfterEdit()) &&
          (size != pt->meshSize || expr != pt->meshSizeExpr)) {
        const uint64_t id = occ.id;
        ModifyStudy(state, s.id, "Set participant mesh size",
                    [&](fem::Study& cs) {
                      for (fem::Participant& x : cs.participants)
                        if (x.occurrenceId == id) {
                          x.meshSize = size;
                          x.meshSizeExpr = expr;
                        }
                    });
      }
    }
  }
  // The plan's own word on the setup, before anyone presses Run: the
  // contact cache asks the planner, and a refusal (a support in mid-air,
  // an interpenetration) is shown here rather than after a solve.
  SimulationContacts(state);
  if (!state.simulation.contactError.empty())
    MessageText(kError, ICON_FA_TRIANGLE_EXCLAMATION " " +
                            state.simulation.contactError);
  // Materials come from the parts: say so when the meshed one has none.
  for (const fem::Participant& pt : s.participants) {
    if (pt.role != fem::ParticipantRole::Meshed)
      continue;
    for (const Body& b : OccurrenceSubtreeBodies(state, pt.occurrenceId)) {
      const auto& mp =
          CADMaterials::MaterialLibrary::GetMaterialOrDefault(b.material);
      if (mp.youngGPa <= 0.0f)
        MessageText(kError, ICON_FA_TRIANGLE_EXCLAMATION " " + b.name +
                                " has no material with stiffness data; "
                                "assign one on its part tab");
    }
  }
}

// Everything below the study list, for either host.
void DrawStudyBody(AppState& state, const fem::Study* s) {
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
  // A typed EXPRESSION keeps its text (the study carries a slot for it);
  // a plain number clears the slot back to a literal.
  std::string meshExpr;
  const bool meshChanged = ExprInputDouble(state, "###study_mesh_size",
                                           &meshSize, "%.3g", &meshExpr);
  if ((meshChanged || ImGui::IsItemDeactivatedAfterEdit()) &&
      (meshSize != s->meshSize || meshExpr != s->meshSizeExpr))
    ModifyStudy(state, s->id, "Set mesh size", [&](fem::Study& cs) {
      cs.meshSize = meshSize;
      cs.meshSizeExpr = meshExpr;
    });

  if (s->type == fem::AnalysisType::Modal) {
    int nm = s->numModes;
    LeftLabel("Modes");
    const bool nmChanged = ExprInputInt(state, "###study_modes", &nm);
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
    std::string frExpr;
    const bool frChanged =
        ExprInputDouble(state, "###topo_filter_radius", &fr, "%.3g", &frExpr);
    if ((frChanged || ImGui::IsItemDeactivatedAfterEdit()) &&
        (fr != s->filterRadius || frExpr != s->filterRadiusExpr))
      ModifyStudy(state, s->id, "Set filter radius", [&](fem::Study& cs) {
        cs.filterRadius = fr;
        cs.filterRadiusExpr = frExpr;
      });
    int mi = s->maxIterations;
    LeftLabel("Max iterations");
    const bool miChanged = ExprInputInt(state, "###topo_max_iterations", &mi);
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
    std::string removeKeep;
    uint64_t removeKeepFeature = 0;
    for (size_t i = 0; i < s->keepFaces.size(); ++i) {
      const ElementRef& f = s->keepFaces[i];
      ListRowSpec row;
      row.glyph = ICON_FA_LOCK;
      row.label = f.name + "###keep_row_" + std::to_string(i);
      row.menuId = "###keep_ctx_" + std::to_string(i);
      row.menu = [&, f] {
        if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Remove###keep_del")) {
          removeKeep = f.name;
          removeKeepFeature = f.featureId;
        }
      };
      ListRow(row);
    }
    if (!removeKeep.empty())
      ModifyStudy(state, s->id, "Remove keep face", [&](fem::Study& cs) {
        cs.keepFaces.erase(
            std::remove_if(cs.keepFaces.begin(), cs.keepFaces.end(),
                           [&](const ElementRef& k) {
                             return k.featureId == removeKeepFeature &&
                                    k.name == removeKeep;
                           }),
            cs.keepFaces.end());
      });
  } else {
    // Static: an optional gravity / self-weight load.
    bool on = s->gravity.x != 0 || s->gravity.y != 0 || s->gravity.z != 0;
    if (ImGui::Checkbox("Gravity (self-weight)###study_gravity", &on)) {
      // Straight down in the WORLD frame — a hand-written axis here once
      // sagged the part sideways (docs/conventions.md#world-axes).
      const glm::dvec3 g = on ? conv::kGravity : glm::dvec3(0, 0, 0);
      ModifyStudy(state, s->id, "Toggle gravity",
                  [&](fem::Study& cs) { cs.gravity = g; });
    }
    if (on) {
      double g[3] = {s->gravity.x, s->gravity.y, s->gravity.z};
      LeftLabel("g (m/s^2)");
      // Commit on deactivation (Enter OR clicking away), like the mesh-size
      // field above. NOT EnterReturnsTrue: that flag is InputText-only —
      // InputScalar ASSERTS on it, so this line aborted every Debug build
      // (a fresh clone's default) the moment gravity was switched on.
      const bool gChanged =
          ExprInputDouble3(state, "###study_gravity_vec", g, "%.2f");
      if (gChanged || ImGui::IsItemDeactivatedAfterEdit())
        ModifyStudy(state, s->id, "Set gravity", [&](fem::Study& cs) {
          cs.gravity = glm::dvec3(g[0], g[1], g[2]);
        });
    }
  }

  DrawMaterialCheck(state);
  DrawParticipants(state, *s);
  DrawLoads(state, *s);
  DrawResults(state, *s);
}
} // namespace

void DrawStudyPanel(AppState& state) {
  ImGui::Begin(kPanelModel);
  PanelHeading(kPanelModel);
  const StudyHost host = StudyHostOf(state);
  if (!host.part) {
    ImGui::End();
    return;
  }
  // The list only. The selected study is edited, run and read in the
  // Simulation window, as an assembly's studies already were: one home for
  // the editor, and a panel that does not change length under the cursor
  // (docs/ui/panels.md#simulation-the-study-panel-and-window).
  DrawStudyList(state);
  if (host.Find(state.simulation.activeStudyId) == nullptr)
    ImGui::TextColored(kMuted, "Select or add a study.");
  ImGui::End();
}

void DrawSimulationToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "Simulation");
  const StudyHost host = StudyHostOf(state);
  // An assembly's studies are listed here (its dock is the occurrence
  // panel); a part's are listed in its dock, and edited here.
  if (host.IsAssembly())
    DrawStudyList(state);
  if (const fem::Study* s = host.Find(state.simulation.activeStudyId))
    DrawStudyBody(state, s);
  else
    ImGui::TextColored(kMuted, "Select or add a study.");
  ImGui::Separator();
  // Close, not Esc: the window is a way in to a display state (the field
  // overlay) that outlives it, and it applies as you go.
  if (ImGui::Button("Close###simulation_close"))
    ExitSimulation(state);
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
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || sim.field == SimulationState::Field::None ||
      sim.field == SimulationState::Field::Shape || !host)
    return;
  const fem::StudyRuntime* rt = host.Results(sim.activeStudyId);
  if (!rt)
    return;
  const fem::Study* study = host.Find(sim.activeStudyId);
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

  // What the picture is a picture OF, above the bar: the study and its
  // analysis, the mode and its frequency, the factor the deformation is
  // drawn at. A colorbar with a number and no subject was a picture a
  // reader had to trust.
  ImDrawList* dl = ViewportDrawList();
  const float x = state.viewportPos.x + 16.0f;
  const float barW = 18.0f, barH = 180.0f;
  const float y1 = state.viewportPos.y + state.viewportSize.y - 28.0f;
  const float y0 = y1 - barH;
  const float lineH = ImGui::GetTextLineHeight();
  std::vector<std::string> head;
  if (study) {
    head.push_back(study->name + "  " +
                   (modal  ? "modal"
                    : topo ? "topology"
                           : "static"));
  }
  if (modal) {
    const fem::FemResults::ModeShape* mode = nullptr;
    size_t modes = 0;
    for (const fem::StudyBodyResult& b : rt->bodies) {
      if (b.results.modes.empty())
        continue;
      modes = b.results.modes.size();
      const int mi = std::clamp(sim.activeMode, 0, (int)modes - 1);
      mode = &b.results.modes[mi];
      break;
    }
    if (mode) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "mode %d of %zu  %.4g Hz",
                    std::clamp(sim.activeMode, 0, (int)modes - 1) + 1, modes,
                    mode->frequencyHz);
      head.push_back(buf);
    }
  }
  if (sim.deformScale > 0.0f && !density) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "deformation x%.3g", sim.deformScale);
    head.push_back(buf);
  }
  head.push_back(title);
  float ty = y0 - 10.0f - lineH * (float)head.size();
  for (const std::string& line : head) {
    dl->AddText(ImVec2(x, ty), IM_COL32_WHITE, line.c_str());
    ty += lineH;
  }

  // A vertical Turbo colorbar anchored to the viewport's lower-left corner.
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
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%.3g %s", maxVal, unit);
  dl->AddText(ImVec2(x + barW + 6, y0 - 6), IM_COL32_WHITE, buf);
  // Intermediate ticks, so a colour halfway up the bar is a number too.
  for (int k = 1; k < 4; ++k) {
    const float yk = y1 - barH * (k / 4.0f);
    dl->AddLine(ImVec2(x + barW, yk), ImVec2(x + barW + 4, yk),
                IM_COL32(255, 255, 255, 160));
    std::snprintf(buf, sizeof(buf), "%.3g", maxVal * (k / 4.0));
    dl->AddText(ImVec2(x + barW + 8, yk - lineH * 0.5f),
                IM_COL32(255, 255, 255, 200), buf);
  }
  dl->AddText(ImVec2(x + barW + 6, y1 - 6), IM_COL32_WHITE, "0");
  // Where the maximum is, in world millimetres and on which body: the
  // magenta cross in the viewport marks it, and this is its caption.
  if (!modal && !density) {
    const glm::dvec3 at = vm ? rt->maxVmAt : rt->maxDispAt;
    const std::string& body = vm ? rt->maxVmBody : rt->maxDispBody;
    std::snprintf(buf, sizeof(buf), "max at (%.1f, %.1f, %.1f)%s%s", at.x, at.y,
                  at.z, body.empty() ? "" : " on ", body.c_str());
    dl->AddText(ImVec2(x, y1 + 8.0f), IM_COL32(255, 255, 255, 200), buf);
  }
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
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || !host)
    return false;
  // Mutable for the drag's live preview; the release records the edit.
  fem::Study* study = host.FindForRun(sim.activeStudyId);
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
        // the command stack expects -- or, on an assembly, the snapshot
        // taken at the grab plus one version bump now.
        const nlohmann::json after = DocumentIO::StudyToJson(*study);
        const nlohmann::json before =
            nlohmann::json::parse(sim.loadDrag.beforeJson, nullptr, false);
        if (host.part) {
          if (!before.is_discarded() && before != after && state.commands)
            state.commands->Execute(std::make_unique<ModifyStudyCommand>(
                                        "Drag force", before, after),
                                    *host.part);
        } else if (before != after) {
          host.asmDoc->BumpVersion();
        }
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
  if (host.IsAssembly())
    SnapshotAssembly(state); // the undo step, taken before the live edits
  return true; // consumed: do not also re-pick the face behind the arrow
}

namespace {
// The view-scaled glyph length every simulation glyph is built from (the
// peak marker uses the same rule): a fixed 1.2 model-mm arrow vanished
// entirely on a 90 mm part.
float GlyphLength(const AppState& state) {
  return state.camera ? std::max(0.055f * state.camera->GetDistance(), 1.2f)
                      : 1.2f;
}

// Radical-inverse (van der Corput) sequence in base b: the low-discrepancy
// numbers behind SampleFace, so the arrows over a face are evenly spread
// and identical from one frame to the next.
float Halton(int i, int base) {
  float f = 1.0f, r = 0.0f;
  while (i > 0) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(i % base);
    i /= base;
  }
  return r;
}

const glm::vec4 kFixed{0.20f, 0.80f, 0.35f, 1.0f};    // green, like its tint
const glm::vec4 kForce{1.00f, 0.65f, 0.20f, 1.0f};    // the handle's orange
const glm::vec4 kPressure{0.85f, 0.30f, 0.75f, 1.0f}; // magenta, like its tint
const glm::vec4 kKeep{0.35f, 0.85f, 0.95f, 1.0f};
const glm::vec4 kGravity{0.55f, 0.55f, 1.00f, 1.0f};

// An anchor hatch: a ground line with strokes fanning off it, in BOTH
// in-plane axes. It used to draw along one axis only, so from most angles
// a fixed support was a single stick lying on the face. The strokes lean
// OUT of the face (+n): leaning in, they were inside the solid and the
// depth test left only the ground lines, a cross that read as a marker.
void AnchorGlyph(const glm::vec3& c, const glm::vec3& n, float len,
                 std::vector<PrimVertex>& out) {
  glm::vec3 side =
      glm::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  side = glm::normalize(glm::cross(n, side));
  const glm::vec3 other = glm::normalize(glm::cross(n, side));
  for (const glm::vec3& axis : {side, other}) {
    ui::AppendPrimLine(c - axis * len * 0.4f, c + axis * len * 0.4f, 2.4f,
                       kFixed, out);
    for (int i = -1; i <= 1; i++) {
      const glm::vec3 base = c + axis * (len * 0.3f * i);
      ui::AppendPrimLine(base, base + n * len * 0.3f - axis * len * 0.15f, 1.8f,
                         kFixed, out);
    }
  }
}
} // namespace

std::vector<FaceSample> SampleFace(AppState& state, uint64_t occurrenceId,
                                   const ElementRef& face, float spacing,
                                   int maxCount) {
  std::vector<FaceSample> out;
  uint32_t mesh = 0, index = 0;
  if (!state.faceSelector ||
      !ResolveFaceItem(state, {SelectionItem::Kind::Face, occurrenceId, face},
                       mesh, index))
    return out;
  struct Tri {
    glm::vec3 a, b, c, n;
    float cumArea;
  };
  std::vector<Tri> tris;
  float area = 0.0f;
  state.faceSelector->ForEachFaceTriangle(
      mesh, index,
      [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        const glm::vec3 cr = glm::cross(b - a, c - a);
        const float l = glm::length(cr);
        if (l < 1e-12f)
          return;
        area += 0.5f * l;
        tris.push_back({a, b, c, cr / l, area});
      });
  // Always the centre with the face's average normal: the one glyph a face
  // had before, and the one a face too small for the grid still gets.
  out.push_back({state.faceSelector->GetFaceCenter(mesh, index),
                 state.faceSelector->GetFaceNormal(mesh, index)});
  if (tris.empty() || spacing <= 0.0f)
    return out;
  // One sample per spacing^2 of area, capped; below two the centre alone
  // is the honest picture.
  const int count = std::min(
      maxCount, static_cast<int>(std::floor(area / (spacing * spacing))));
  if (count < 2)
    return out;
  out.clear();
  for (int i = 0; i < count; ++i) {
    // Pick the triangle by area (Halton base 2 against the cumulative
    // areas), then a point in it (bases 3 and 5, folded into the triangle).
    const float pick = Halton(i + 1, 2) * area;
    const auto it =
        std::lower_bound(tris.begin(), tris.end(), pick,
                         [](const Tri& t, float v) { return t.cumArea < v; });
    const Tri& t = it == tris.end() ? tris.back() : *it;
    float u = Halton(i + 1, 3), v = Halton(i + 1, 5);
    if (u + v > 1.0f) {
      u = 1.0f - u;
      v = 1.0f - v;
    }
    out.push_back({t.a + (t.b - t.a) * u + (t.c - t.a) * v, t.n});
  }
  return out;
}

void AppendSimulationBCGlyphs(AppState& state, std::vector<PrimVertex>& out) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || !host || !state.faceSelector)
    return;
  const fem::Study* s = host.Find(sim.activeStudyId);
  if (s == nullptr)
    return;

  const auto faceFrame = [&](uint64_t occurrenceId, const ElementRef& ref,
                             glm::vec3& center, glm::vec3& normal) {
    uint32_t mesh = 0, index = 0;
    if (!ResolveFaceItem(state, {SelectionItem::Kind::Face, occurrenceId, ref},
                         mesh, index))
      return false;
    center = state.faceSelector->GetFaceCenter(mesh, index);
    normal = state.faceSelector->GetFaceNormal(mesh, index);
    return true;
  };
  const float len = GlyphLength(state);
  // A distributed condition covers its face at one glyph per cell of this
  // spacing -- derived from the glyph length, so zooming in fills the face
  // in and zooming out thins it, and never fewer than the centre. The cap
  // keeps a sheet from costing thousands of lines a frame.
  const float spacing = len * 1.6f;
  constexpr int kMaxGlyphsPerFace = 160;

  for (const fem::Load& load : s->loads) {
    if (load.contactWith != 0)
      continue; // a contact anchor is drawn as its tinted patch, below
    glm::vec3 c(0.0f), n(0.0f);
    if (!faceFrame(load.occurrenceId, load.face, c, n))
      continue;
    switch (load.type) {
    case fem::LoadType::Fixed: {
      // The hatch repeated over the face, so "what is fixed" reads as an
      // AREA -- and each hatch stands on its own triangle's normal, which on
      // a curved face is not the average one.
      const auto samples = SampleFace(state, load.occurrenceId, load.face,
                                      spacing * 1.5f, kMaxGlyphsPerFace / 2);
      const float hatch = samples.size() > 1 ? len * 0.55f : len;
      for (const FaceSample& fs : samples)
        AnchorGlyph(fs.point, fs.normal, hatch, out);
      break;
    }
    case fem::LoadType::Force: {
      const glm::vec3 dir = glm::normalize(
          glm::vec3(load.direction.x, load.direction.y, load.direction.z));
      // One resultant arrow: a force is "this much in total", and the
      // tinted face says where it is spread. The arrow flies ALONG the
      // force; which end sits on the face depends on where the shaft is
      // visible. A push approaches from outside and lands head-on the face.
      // A pull drawn the same way would put the whole shaft INSIDE the
      // solid -- depth-tested away, an invisible load -- so a pull takes
      // off from the face instead: tail on the face, head out.
      const glm::vec3 tail = glm::dot(dir, n) > 0.0f ? c : c - dir * len;
      // Draggable (docs/ui/viewport-handles.md): the shaft is the handle,
      // so what highlights is exactly what a click grabs. Published, not
      // drawn: DrawViewportHandles draws it with every other handle, on
      // top -- so the pull case above is belt and braces.
      ViewportHandle vh;
      vh.id = HandleId(HandleOwner::Load, load.id);
      vh.kind = HandleKind::Linear;
      vh.origin = tail;
      vh.dir = dir;
      vh.length = len;
      vh.color = glm::vec3(kForce);
      state.viewportHandles.push_back(vh);
      break;
    }
    case fem::LoadType::Pressure: {
      // A pressure is a FIELD: one arrow per cell, each pressing along its
      // own triangle's inward normal. One arrow at the centre read as a
      // point load on a face that carries none.
      const auto samples = SampleFace(state, load.occurrenceId, load.face,
                                      spacing, kMaxGlyphsPerFace);
      const float arrow = samples.size() > 1 ? len * 0.6f : len * 0.8f;
      for (const FaceSample& fs : samples)
        ArrowGlyph(fs.point + fs.normal * arrow, fs.point, kPressure, out);
      break;
    }
    }
  }
  for (const ElementRef& keep : s->keepFaces) {
    glm::vec3 c(0.0f), n(0.0f);
    if (!faceFrame(0, keep, c, n))
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

void AppendSimulationOverlayGlyphs(AppState& state,
                                   std::vector<PrimVertex>& out) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || !host)
    return;
  const fem::Study* s = host.Find(sim.activeStudyId);
  if (s == nullptr)
    return;
  // Gravity: one arrow leaving the centre of mass, the way the weight acts.
  // A self-weight study whose picture showed no weight was a setup nobody
  // could check against the panel. The centre of mass is inside the solid,
  // so this goes in the see-through channel.
  if (s->type != fem::AnalysisType::Static || glm::length(s->gravity) < 1e-9)
    return;
  if (sim.comVersion != host.Version() || !sim.comKnown) {
    sim.comVersion = host.Version();
    sim.comKnown = false;
    // The weight of what is SOLVED: the part, or the meshed occurrence.
    std::vector<Body> placed;
    const std::vector<Body>* bodies =
        host.part ? &host.part->GetBodies() : &placed;
    if (host.IsAssembly())
      for (const fem::Participant& pt : s->participants)
        if (pt.role == fem::ParticipantRole::Meshed)
          placed = OccurrenceSubtreeBodies(state, pt.occurrenceId);
    if (const auto mass = Measure::MassOfBodies(*bodies)) {
      sim.com = glm::vec3(mass->centerOfMass);
      sim.comKnown = mass->volume > 0.0;
    }
  }
  if (!sim.comKnown)
    return;
  const float len = GlyphLength(state);
  const glm::vec3 g = glm::normalize(glm::vec3(s->gravity));
  // A small cross marks the point itself; the arrow leaves it.
  const glm::vec3 ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (const glm::vec3& a : ax)
    ui::AppendPrimLine(sim.com - a * len * 0.12f, sim.com + a * len * 0.12f,
                       2.0f, kGravity, out);
  ArrowGlyph(sim.com, sim.com + g * len * 1.3f, kGravity, out);
}

void AppendSimulationFaceTints(AppState& state) {
  const SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || !host || !state.faceSelector || !state.meshRenderer)
    return;
  const fem::Study* s = host.Find(sim.activeStudyId);
  if (s == nullptr)
    return;
  const auto tintFace = [&](uint64_t occurrenceId, const ElementRef& ref,
                            uint32_t tint) {
    uint32_t mesh = 0, index = 0, first = 0, count = 0;
    if (ResolveFaceItem(state, {SelectionItem::Kind::Face, occurrenceId, ref},
                        mesh, index) &&
        state.faceSelector->GetFaceVertexRange(mesh, index, first, count))
      state.meshRenderer->SetFaceHighlight(mesh, first, count, tint);
  };
  for (const fem::Load& load : s->loads)
    if (load.contactWith == 0)
      tintFace(load.occurrenceId, load.face,
               load.type == fem::LoadType::Fixed ? MeshRenderer::kTintFixture
                                                 : MeshRenderer::kTintLoad);
  for (const ElementRef& keep : s->keepFaces)
    tintFace(0, keep, MeshRenderer::kTintKeep);

  if (!host.IsAssembly())
    return;
  // Roles, on the picture (S1): a rigid support is a GHOST with its contact
  // faces tinted as the fixture they are -- the faces the imprint found,
  // not the whole part -- and an ignored occurrence stays as it is. The
  // renderer sync rebuilds every instance on the next version change or on
  // ExitSimulation's forced resync, so the ghosting needs no undo.
  for (const fem::Participant& pt : s->participants)
    if (pt.role == fem::ParticipantRole::Rigid)
      for (const DocumentRenderMesh& m : state.documentMeshes)
        if (m.occurrenceId == pt.occurrenceId &&
            !state.meshRenderer->IsInstanceGhost(m.meshId))
          state.meshRenderer->SetInstanceGhost(m.meshId, 0.35f);
  // The glued seams (S2): both sides' faces in the bond tint.
  for (const fem::BondPatch& bond : SimulationBonds(state)) {
    const auto tintSide = [&](uint64_t occ, int body,
                              const std::vector<int>& faces) {
      for (const DocumentRenderMesh& m : state.documentMeshes) {
        if (m.occurrenceId != occ || static_cast<int>(m.bodyIndex) != body)
          continue;
        for (int face : faces) {
          uint32_t first = 0, count = 0;
          if (state.faceSelector->GetFaceVertexRange(
                  m.meshId, static_cast<uint32_t>(face), first, count))
            state.meshRenderer->SetFaceHighlight(m.meshId, first, count,
                                                 MeshRenderer::kTintBond);
        }
      }
    };
    tintSide(bond.occurrenceA, bond.bodyA, bond.facesA);
    tintSide(bond.occurrenceB, bond.bodyB, bond.facesB);
  }
  bool clamped = false;
  for (const fem::Load& load : s->loads)
    clamped = clamped || load.contactWith != 0;
  if (!clamped)
    return;
  for (const fem::ContactPatch& patch : SimulationContacts(state)) {
    bool wanted = false;
    for (const fem::Load& load : s->loads)
      wanted = wanted || load.contactWith == patch.rigidOccurrenceId;
    if (!wanted)
      continue;
    for (const DocumentRenderMesh& m : state.documentMeshes) {
      if (m.occurrenceId != patch.rigidOccurrenceId ||
          static_cast<int>(m.bodyIndex) != patch.rigidBody)
        continue;
      for (int face : patch.rigidFaces) {
        uint32_t first = 0, count = 0;
        if (state.faceSelector->GetFaceVertexRange(
                m.meshId, static_cast<uint32_t>(face), first, count))
          state.meshRenderer->SetFaceHighlight(m.meshId, first, count,
                                               MeshRenderer::kTintFixture);
      }
    }
  }
}

std::string SimulationLoadLabel(const fem::Load& load) {
  char buf[64];
  switch (load.type) {
  case fem::LoadType::Fixed:
    return "fixed";
  case fem::LoadType::Force:
    if (std::abs(load.magnitude) >= 1000.0)
      std::snprintf(buf, sizeof(buf), "%.3g kN", load.magnitude / 1000.0);
    else
      std::snprintf(buf, sizeof(buf), "%.3g N", load.magnitude);
    return buf;
  case fem::LoadType::Pressure:
    if (std::abs(load.magnitude) < 0.1 && load.magnitude != 0.0)
      std::snprintf(buf, sizeof(buf), "%.3g kPa", load.magnitude * 1000.0);
    else
      std::snprintf(buf, sizeof(buf), "%.3g MPa", load.magnitude);
    return buf;
  }
  return "";
}

std::string SimulationGravityLabel(const glm::dvec3& gravity) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "g %.3g m/s^2", glm::length(gravity));
  return buf;
}

void DrawSimulationLabels(AppState& state) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || !sim.showLabels || !host || !state.faceSelector ||
      !state.camera)
    return;
  const fem::Study* s = host.Find(sim.activeStudyId);
  if (s == nullptr)
    return;
  ImDrawList* dl = ViewportDrawList();
  const float len = GlyphLength(state);
  const glm::mat4 viewProj =
      state.camera->GetProjectionMatrix() * state.camera->GetViewMatrix();
  // A pill behind the text, so a label stays legible over a lit face or a
  // colormap. Skipped when the anchor is behind the camera.
  const auto label = [&](const glm::vec3& at, const std::string& text,
                         ImU32 colour) {
    const glm::vec4 clip = viewProj * glm::vec4(at, 1.0f);
    if (clip.w <= 1e-6f)
      return;
    const glm::vec2 p = WorldToScreen(state, at);
    if (p.x < state.viewportPos.x ||
        p.x > state.viewportPos.x + state.viewportSize.x ||
        p.y < state.viewportPos.y ||
        p.y > state.viewportPos.y + state.viewportSize.y)
      return;
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pos(p.x + 8.0f, p.y - size.y * 0.5f);
    dl->AddRectFilled(ImVec2(pos.x - 4.0f, pos.y - 2.0f),
                      ImVec2(pos.x + size.x + 4.0f, pos.y + size.y + 2.0f),
                      IM_COL32(20, 20, 24, 190), 4.0f);
    dl->AddText(pos, colour, text.c_str());
  };
  const auto col = [](const glm::vec4& c) {
    return IM_COL32(int(c.r * 255), int(c.g * 255), int(c.b * 255), 255);
  };
  for (const fem::Load& load : s->loads) {
    if (load.contactWith != 0) {
      // At the first contact face the imprint found on the support.
      for (const fem::ContactPatch& patch : SimulationContacts(state)) {
        if (patch.rigidOccurrenceId != load.contactWith ||
            patch.rigidFaces.empty())
          continue;
        for (const DocumentRenderMesh& m : state.documentMeshes)
          if (m.occurrenceId == patch.rigidOccurrenceId &&
              static_cast<int>(m.bodyIndex) == patch.rigidBody) {
            label(state.faceSelector->GetFaceCenter(
                      m.meshId, static_cast<uint32_t>(patch.rigidFaces[0])),
                  "clamped at " + host.OccurrenceName(load.contactWith),
                  col(kFixed));
            break;
          }
        break;
      }
      continue;
    }
    uint32_t mesh = 0, index = 0;
    if (!ResolveFaceItem(
            state, {SelectionItem::Kind::Face, load.occurrenceId, load.face},
            mesh, index))
      continue;
    const glm::vec3 c = state.faceSelector->GetFaceCenter(mesh, index);
    const glm::vec3 n = state.faceSelector->GetFaceNormal(mesh, index);
    switch (load.type) {
    case fem::LoadType::Fixed:
      label(c, SimulationLoadLabel(load), col(kFixed));
      break;
    case fem::LoadType::Force: {
      // At the arrow's free end, whichever end that is (see the glyph).
      const glm::vec3 dir = glm::normalize(glm::vec3(load.direction));
      const glm::vec3 tail = glm::dot(dir, n) > 0.0f ? c : c - dir * len;
      const glm::vec3 free = glm::dot(dir, n) > 0.0f ? tail + dir * len : tail;
      label(free, SimulationLoadLabel(load), col(kForce));
      break;
    }
    case fem::LoadType::Pressure:
      label(c + n * len * 0.8f, SimulationLoadLabel(load), col(kPressure));
      break;
    }
  }
  for (const ElementRef& keep : s->keepFaces) {
    uint32_t mesh = 0, index = 0;
    if (ResolveFaceItem(state, {SelectionItem::Kind::Face, 0, keep}, mesh,
                        index))
      label(state.faceSelector->GetFaceCenter(mesh, index), "keep", col(kKeep));
  }
  if (s->type == fem::AnalysisType::Static && sim.comKnown &&
      glm::length(s->gravity) > 1e-9) {
    const glm::vec3 g = glm::normalize(glm::vec3(s->gravity));
    label(sim.com + g * len * 1.3f, SimulationGravityLabel(s->gravity),
          col(kGravity));
  }
}

void SimulationApplyToFace(AppState& state, const ElementRef& face) {
  SimulationApplyToFace(state, 0, face);
}

void SimulationApplyToFace(AppState& state, uint64_t occurrenceId,
                           const ElementRef& face) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!host || sim.tool == SimulationState::Tool::None)
    return;
  const fem::Study* s = host.Find(sim.activeStudyId);
  if (!s)
    return;
  sim.placeRefusal.clear();
  // On an assembly only the MESHED occurrence takes a load; a click on a
  // support or a bystander is refused where the person can read it, not
  // dropped on the floor.
  const Document* faceDoc = host.part;
  if (host.IsAssembly()) {
    const fem::Participant* pt = s->FindParticipant(occurrenceId);
    if (!pt || pt->role != fem::ParticipantRole::Meshed) {
      sim.placeRefusal = host.OccurrenceName(occurrenceId) +
                         " is not the meshed occurrence; a load goes on the "
                         "part being solved";
      return;
    }
    const Occurrence* occ = host.asmDoc->FindById(occurrenceId);
    faceDoc = occ ? OccurrencePartDoc(state, *occ) : nullptr;
  }

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
  load.id = host.AllocateId();
  load.type = sim.tool == SimulationState::Tool::Fixed ? fem::LoadType::Fixed
              : sim.tool == SimulationState::Tool::Force
                  ? fem::LoadType::Force
                  : fem::LoadType::Pressure;
  load.face = face;
  load.occurrenceId = occurrenceId;
  if (load.type != fem::LoadType::Fixed) {
    load.magnitude = sim.pendingMagnitude;
    load.magnitudeExpr = sim.pendingMagnitudeExpr;
  }
  if (load.type == fem::LoadType::Force && faceDoc) {
    // The face decides the direction unless the user asked for a specific
    // one: a force placed on a face pushes into it.
    auto into = fem::DefaultForceDirection(*faceDoc, face);
    // A part's inward normal, turned into the world the placed part is
    // solved in.
    if (into && occurrenceId != 0 && host.asmDoc)
      if (const Occurrence* occ = host.asmDoc->FindById(occurrenceId))
        into = glm::mat3_cast(occ->orientation) * *into;
    load.direction =
        sim.directionOverridden || !into ? sim.pendingDirection : *into;
  }

  ModifyStudy(state, s->id,
              std::string("Add ") + LoadLabel(load.type) + " load",
              [&](fem::Study& cs) { cs.loads.push_back(load); });
  sim.tool = SimulationState::Tool::None; // one placement per arm
}

} // namespace ui
