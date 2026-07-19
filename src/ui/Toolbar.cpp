#include "ui/Toolbar.h"

#include "Logger.h"
#include "RayTracingRenderer.h"
#include "core/features/SketchFeature.h"
#include "ui/AppActions.h"
#include "ui/FileDialogs.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <cfloat>

namespace ui {

namespace {

// The offline-render dialog: resolution + sample count, a live progress bar
// while a render is in flight, and the last result. Opened by the toolbar's
// Render button; the actual tracing is pumped by main.cpp (PumpRenderJob).
void DrawRenderModal(AppState& state) {
  struct Res {
    const char* label;
    uint32_t w, h;
  };
  static const Res kRes[] = {{"1280 x 720", 1280, 720},
                             {"1920 x 1080", 1920, 1080},
                             {"960 x 540", 960, 540},
                             {"640 x 480", 640, 480}};
  static int resIdx = 0;
  static int samples = 128;
  static bool ground = true;

  ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Render Still", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize))
    return;

  RenderJob& job = state.renderJob;

  if (job.active) {
    float frac = job.totalSamples
                     ? static_cast<float>(job.done) /
                           static_cast<float>(job.totalSamples)
                     : 0.0f;
    ImGui::Text("Rendering %u x %u ...", job.width, job.height);
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0));
    ImGui::Text("%u / %u samples", job.done, job.totalSamples);
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
      job.cancel = true;
  } else {
    if (job.finished) {
      if (job.success)
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "%s Saved: %s",
                           ICON_FA_CIRCLE_CHECK, job.resultPath.c_str());
      else
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "Failed: %s",
                           job.error.c_str());
      ImGui::Separator();
    }

    ImGui::TextUnformatted("Resolution");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##res", kRes[resIdx].label)) {
      for (int i = 0; i < IM_ARRAYSIZE(kRes); ++i)
        if (ImGui::Selectable(kRes[i].label, i == resIdx))
          resIdx = i;
      ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Samples");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##spp", &samples, 16, 1024);
    ImGui::Checkbox("Ground shadows", &ground);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Catch soft shadows on an invisible floor plane");

    const bool hasBodies =
        state.document && !state.document->GetBodies().empty();
    const bool rtReady =
        state.rayTracer && state.rayTracer->IsInitialized();
    const bool canRender = hasBodies && rtReady;

    ImGui::Spacing();
    ImGui::BeginDisabled(!canRender);
    if (ImGui::Button(ICON_FA_CAMERA " Render", ImVec2(150, 0))) {
      job = RenderJob{};
      job.active = true;
      job.width = kRes[resIdx].w;
      job.height = kRes[resIdx].h;
      job.totalSamples = static_cast<uint32_t>(samples);
      job.seed = 1;
      job.groundPlane = ground;
      job.outPath = "render.png";
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(120, 0)))
      ImGui::CloseCurrentPopup();

    if (!rtReady)
      ImGui::TextDisabled("Ray tracing not available on this GPU.");
    else if (!hasBodies)
      ImGui::TextDisabled("Add a body to render.");
  }
  ImGui::EndPopup();
}

// An icon button with a tooltip. Returns true when clicked.
bool ToolButton(const char* icon, const char* tooltip, bool enabled = true) {
  ImGui::BeginDisabled(!enabled);
  const bool clicked = ImGui::Button(icon);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", tooltip);
  ImGui::SameLine();
  return clicked;
}

void Divider() {
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
}

// The model-mode toolbar: create features, fit, planes.
void DrawModelToolbar(AppState& state) {
  const bool hasDoc = state.document != nullptr;
  const bool idle = !HasPendingFeature(state); // no creation while previewing

  if (ToolButton(ICON_FA_PENCIL, "New Sketch (S)", hasDoc && idle))
    StartNewSketch(state);

  Divider();

  if (ToolButton(ICON_FA_CUBE, "Box", hasDoc && idle))
    CreateBox(state);
  if (ToolButton(ICON_FA_CIRCLE, "Sphere", hasDoc && idle))
    CreateSphere(state);
  if (ToolButton(ICON_FA_DATABASE, "Cylinder", hasDoc && idle))
    CreateCylinder(state);

  Divider();

  const bool hasBody =
      hasDoc && !state.document->GetBodies().empty();
  if (ToolButton(ICON_FA_BORDER_TOP_LEFT, "Fillet edges", hasBody && idle))
    CreateFillet(state);
  if (ToolButton(ICON_FA_CROP_SIMPLE, "Chamfer edges", hasBody && idle))
    CreateChamfer(state);

  Divider();

  if (ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Fit view (F)",
                 hasDoc))
    FitCameraToDocument(state);
  if (ToolButton(ICON_FA_TABLE_CELLS, "Toggle planes (Shift+P)", hasDoc))
    state.showPlanes = !state.showPlanes;

  // Enter the FEM simulation workspace (needs a body to analyze).
  if (ToolButton(ICON_FA_GAUGE_HIGH, "Simulation (FEM stress study)", hasBody && idle))
    EnterSimulation(state);

  // Measure tool (C3): a viewport mode — pick a face/edge for its own measure,
  // a second element for the distance. The button reflects the armed state.
  if (state.measure.active)
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  if (ToolButton(ICON_FA_RULER, "Measure (pick 1-2 faces/edges; Esc to exit)",
                 hasBody)) {
    const bool wasActive = state.measure.active;
    state.measure = MeasureState{};       // clear any in-progress measurement
    state.measure.active = !wasActive;    // toggle
  }
  if (state.measure.active)
    ImGui::PopStyleColor();

  Divider();

  // Offline path-traced still (KeyShot-style). Opens the render dialog.
  const bool rtReady = state.rayTracer && state.rayTracer->IsInitialized();
  if (ToolButton(ICON_FA_CAMERA, "Render (offline raytrace)",
                 hasDoc && rtReady))
    ImGui::OpenPopup("Render Still");
  DrawRenderModal(state);

  ImGui::NewLine();
}

// The sketch-mode toolbar: draw tools, generative ops, and constraints — the
// icon ribbon that replaces the model tools while a sketch is being edited.
void DrawSketchModeToolbar(AppState& state) {
  SketchEditState& edit = state.sketchEdit;

  // Finish (accept) the sketch.
  if (ToolButton(ICON_FA_CHECK, "Finish sketch (Esc)"))
    edit = SketchEditState{};
  Divider();

  auto toolBtn = [&](const char* icon, const char* tip, SketchTool t) {
    const bool active = edit.tool == t;
    if (active)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(icon)) {
      edit.tool = t;
      edit.hasPendingPoint = edit.hasPendingPoint2 = false;
      edit.mirrorAxisPhase = false;
      edit.actionMsg.clear();
    }
    if (active)
      ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
  };
  toolBtn(ICON_FA_SLASH, "Line (L)", SketchTool::Line);
  toolBtn(ICON_FA_CIRCLE_NOTCH, "Circle (C)", SketchTool::Circle);
  toolBtn(ICON_FA_VECTOR_SQUARE, "Rectangle (G)", SketchTool::Rectangle);
  toolBtn(ICON_FA_BEZIER_CURVE, "3-Point Arc (A)", SketchTool::ArcThreePoint);
  toolBtn(ICON_FA_EGG, "Ellipse", SketchTool::Ellipse);
  toolBtn(ICON_FA_CIRCLE_DOT, "Point", SketchTool::Point);
  toolBtn(ICON_FA_SCISSORS, "Trim (M)", SketchTool::Trim);
  Divider();
  toolBtn(ICON_FA_LEFT_RIGHT, "Mirror", SketchTool::Mirror);
  toolBtn(ICON_FA_DRAW_POLYGON, "Fillet (Shift+F)", SketchTool::Fillet);
  toolBtn(ICON_FA_GRIP, "Pattern", SketchTool::Pattern);
  toolBtn(ICON_FA_CLONE, "Offset (O)", SketchTool::Offset);
  Divider();

  // Constraints act on the current selection (validated in
  // ApplySketchConstraint; an invalid combination just no-ops with a hint).
  using K = sketch::ConstraintKind;
  const bool hasSel = !edit.selected.empty();
  auto conBtn = [&](const char* label, const char* tip, K kind) {
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::Button(label))
      ApplySketchConstraint(state, kind, 0.0);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
  };
  conBtn("H", "Horizontal (H)", K::Horizontal);
  conBtn("V", "Vertical (V)", K::Vertical);
  conBtn(ICON_FA_ARROWS_TO_DOT, "Coincident (N)", K::Coincident);
  conBtn(ICON_FA_ARROWS_LEFT_RIGHT, "Parallel (B)", K::Parallel);
  conBtn(ICON_FA_ANGLE_RIGHT, "Perpendicular", K::Perpendicular);
  conBtn("T", "Tangent (T)", K::Tangent);
  conBtn(ICON_FA_EQUALS, "Equal (E)", K::Equal);
  conBtn(ICON_FA_CIRCLE, "Concentric", K::Concentric);
  Divider();

  ImGui::BeginDisabled(!hasSel);
  if (ImGui::Button(ICON_FA_MINUS "##constr"))
    ToggleConstruction(state);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Construction (Q) — reference geometry, dashed");
  ImGui::SameLine();
  Divider();

  if (ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Fit view (F)"))
    FitCameraToDocument(state);
  ImGui::NewLine();
}

// The assembly-mode toolbar: mate-creation tools (A3). Shown when an assembly
// tab is active (no part document). Fit reuses FitCameraToDocument.
void DrawAssemblyToolbar(AppState& state) {
  // Insert part: place an occurrence of an existing part. Opens a picker of
  // the project's part tabs (the only GUI path to add an occurrence).
  if (ToolButton(ICON_FA_CIRCLE_PLUS, "Insert part (place an occurrence)"))
    ImGui::OpenPopup("insert_part");
  if (ImGui::BeginPopup("insert_part")) {
    ImGui::TextDisabled("Insert part");
    ImGui::Separator();
    bool any = false;
    for (const PartSlot& s : state.parts) {
      if (s.kind != DocKind::Part) continue;
      any = true;
      if (ImGui::MenuItem(s.name.c_str())) {
        SnapshotAssembly(state);
        Occurrence occ;
        occ.partId = s.id;
        occ.name = s.name;
        occ.fixed = state.assembly->OccurrenceCount() == 0;  // first = anchor
        const uint64_t id = state.assembly->AddOccurrence(occ);
        SelectOccurrence(state, id);
        SolveActiveAssembly(state);
      }
    }
    if (!any)
      ImGui::TextDisabled("No parts yet — add a part with +Part first.");
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  Divider();

  auto mateBtn = [&](const char* icon, const char* tip, MateType t) {
    const bool active = state.mateTool.active && state.mateTool.type == t;
    if (active)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(icon))
      StartMateTool(state, t);
    if (active)
      ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
  };
  mateBtn(ICON_FA_LOCK, "Fastened mate (rigid)", MateType::Fastened);
  mateBtn(ICON_FA_ARROWS_SPIN, "Revolute mate (hinge)", MateType::Revolute);
  mateBtn(ICON_FA_ARROWS_LEFT_RIGHT, "Slider mate", MateType::Slider);
  mateBtn(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "Cylindrical mate (spin + slide)",
          MateType::Cylindrical);
  mateBtn(ICON_FA_TABLE_CELLS_LARGE, "Planar mate (slide in a plane)",
          MateType::Planar);
  mateBtn(ICON_FA_CIRCLE_DOT, "Ball mate (free rotation)", MateType::Ball);
  Divider();
  if (ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Fit view (F)"))
    FitCameraToDocument(state);

  // Measure across the assembly: same tool as the part tab — picks carry
  // their occurrence, so distances span occurrences in world space.
  if (state.measure.active)
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  if (ToolButton(ICON_FA_RULER, "Measure (pick 1-2 faces/edges; Esc to exit)",
                 state.assembly->OccurrenceCount() > 0)) {
    const bool wasActive = state.measure.active;
    state.measure = MeasureState{}; // clear any in-progress measurement
    state.measure.active = !wasActive; // toggle
  }
  if (state.measure.active)
    ImGui::PopStyleColor();

  if (state.mateTool.active && !state.mateTool.message.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", state.mateTool.message.c_str());
  }
  ImGui::NewLine();
}

// The simulation-mode toolbar: boundary-condition tools, run, exit — replaces
// the model tools while a FEM study is being set up.
void DrawSimulationToolbar(AppState& state) {
  SimulationState& sim = state.simulation;
  Document* doc = state.document.get();
  const bool hasStudy = doc && doc->FindStudy(sim.activeStudyId);

  // A boundary-condition tool button: armed state highlighted like Measure.
  auto bcBtn = [&](const char* icon, const char* tip, SimulationState::Tool t) {
    const bool armed = sim.tool == t;
    if (armed)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ToolButton(icon, tip, hasStudy))
      sim.tool = armed ? SimulationState::Tool::None : t; // toggle
    if (armed)
      ImGui::PopStyleColor();
  };
  bcBtn(ICON_FA_LOCK, "Fixed support (click a face)", SimulationState::Tool::Fixed);
  bcBtn(ICON_FA_BOLT, "Force load (click a face)", SimulationState::Tool::Force);
  bcBtn(ICON_FA_DOWN_LONG, "Pressure load (click a face)",
        SimulationState::Tool::Pressure);

  Divider();

  if (ToolButton(ICON_FA_PLAY, "Run study (mesh + solve)", hasStudy)) {
    std::string err;
    if (!RunStudyAndCache(*doc, sim.activeStudyId, &err))
      LOG_WARN("run study failed: {}", err);
  }

  Divider();

  if (ToolButton(ICON_FA_ARROW_RIGHT_FROM_BRACKET, "Exit Simulation (Esc)"))
    ExitSimulation(state);

  // Hint while a BC tool is armed.
  if (sim.tool != SimulationState::Tool::None) {
    ImGui::SameLine();
    const char* what = sim.tool == SimulationState::Tool::Fixed ? "fixed support"
                       : sim.tool == SimulationState::Tool::Force ? "force"
                                                                  : "pressure";
    ImGui::TextDisabled("click a face to place the %s", what);
  }
  ImGui::NewLine();
}

} // namespace

// The drawing-mode toolbar: add a projected view of a part/assembly (the
// "add part" of a sheet), re-project, the dimension tools, fit, export.
void DrawDrawingToolbar(AppState& state) {
  DrawingDocument& dwg = *state.drawing;

  // Add view: a picker of part/assembly sources, each with orientation
  // submenus — the sheet equivalent of inserting a part.
  if (ToolButton(ICON_FA_CIRCLE_PLUS, "Add view (project a part/assembly)"))
    ImGui::OpenPopup("add_view");
  if (ImGui::BeginPopup("add_view")) {
    ImGui::TextDisabled("Add view of");
    ImGui::Separator();
    struct Ori { const char* label; ViewOrientation o; };
    static const Ori orients[] = {{"Front", ViewOrientation::Front},
                                  {"Top", ViewOrientation::Top},
                                  {"Right", ViewOrientation::Right},
                                  {"Iso", ViewOrientation::Iso}};
    bool any = false;
    for (const PartSlot& s : state.parts) {
      if (s.kind != DocKind::Part && s.kind != DocKind::Assembly) continue;
      any = true;
      if (ImGui::BeginMenu(s.name.c_str())) {
        for (const Ori& ori : orients) {
          if (ImGui::MenuItem(ori.label)) {
            glm::dvec2 ext = SheetExtentMM(dwg.GetSheet().size);
            if (dwg.GetSheet().landscape) ext = {ext.y, ext.x};
            DrawingView v;
            v.name = ori.label;
            v.sourceDocId = s.id;
            v.orientation = ori.o;
            v.scale = 1.0;
            v.position = ext * 0.5;
            SnapshotDrawing(state);
            dwg.AddView(v);
            UpdateDrawingRenders(state);
          }
        }
        ImGui::EndMenu();
      }
    }
    if (!any)
      ImGui::TextDisabled("No parts/assemblies — add one with +Part/+Asm.");
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  Divider();

  if (ToolButton(ICON_FA_ARROWS_ROTATE, "Update views (re-project)"))
    UpdateDrawingRenders(state);

  Divider();

  // Dimension tools: arm state.dimTool; the sheet then picks projected refs.
  auto dimBtn = [&](const char* icon, const char* tip, DrawingDimKind k) {
    const bool armed = state.dimTool.active && state.dimTool.kind == k;
    if (armed)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ToolButton(icon, tip)) {
      state.dimTool = AppState::DimTool{};
      state.dimTool.active = true;
      state.dimTool.kind = k;
    }
    if (armed)
      ImGui::PopStyleColor();
  };
  dimBtn(ICON_FA_LEFT_RIGHT, "Distance dimension", DrawingDimKind::Distance);
  dimBtn(ICON_FA_RULER_HORIZONTAL, "Horizontal (X) distance",
         DrawingDimKind::DistanceX);
  dimBtn(ICON_FA_UP_DOWN, "Vertical (Y) distance", DrawingDimKind::DistanceY);
  dimBtn(ICON_FA_CIRCLE_NOTCH, "Radius dimension", DrawingDimKind::Radius);
  dimBtn(ICON_FA_CIRCLE_HALF_STROKE, "Diameter dimension",
         DrawingDimKind::Diameter);
  dimBtn(ICON_FA_ANGLE_LEFT, "Angle dimension", DrawingDimKind::Angle);

  Divider();

  if (ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Fit sheet (F)"))
    state.sheetView.scale = 0.0;  // refit on next paint
  if (ToolButton(ICON_FA_FILE_EXPORT, "Export drawing (SVG / PDF)",
                 !dwg.GetViews().empty()))
    ExportDialog(state);

  if (state.dimTool.active) {
    ImGui::SameLine();
    const bool radial = state.dimTool.kind == DrawingDimKind::Radius ||
                        state.dimTool.kind == DrawingDimKind::Diameter;
    ImGui::TextDisabled("pick edge %d of %d (Esc to cancel)",
                        static_cast<int>(state.dimTool.refs.size()) + 1,
                        radial ? 1 : 2);
  }
  ImGui::NewLine();
}

void DrawToolbar(AppState& state) {
  if (state.sketchEdit.IsEditing())
    DrawSketchModeToolbar(state);
  else if (state.simulation.IsActive())
    DrawSimulationToolbar(state);
  else if (state.assembly)
    DrawAssemblyToolbar(state);
  else if (state.drawing)
    DrawDrawingToolbar(state);
  else
    DrawModelToolbar(state);
}

} // namespace ui
