#include "ui/Toolbar.h"

#include "common/Logger.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h" // BuildLoops (extrude gating)
#include "render/RayTracingRenderer.h"
#include "ui/AppActions.h"
#include "ui/FileDialogs.h"
#include "ui/Placement.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"
#include "ui/SketchToolbar.h" // BeginSketchExtrude/Revolve (U8a)

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <cfloat>
#include <initializer_list>
#include <string>

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
    float frac = job.totalSamples ? static_cast<float>(job.done) /
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

    // What the ray tracer will actually be given: a part's bodies, or — on an
    // assembly tab, where `document` is null — its visible occurrences. The
    // part-only test left this button dead behind a working Render entry.
    const bool hasBodies =
        state.assembly
            ? state.assembly->OccurrenceCount() > 0
            : (state.document && !state.document->GetBodies().empty());
    const bool rtReady = state.rayTracer && state.rayTracer->IsInitialized();
    const bool canRender = hasBodies && rtReady;

    ImGui::Spacing();
    ImGui::BeginDisabled(!canRender);
    // "###" ids: the visible label starts with an icon glyph, which no test
    // or script should have to spell in a ref.
    if (ImGui::Button(ICON_FA_CAMERA " Render###render_go", ImVec2(150, 0))) {
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
    if (ImGui::Button("Close###render_close", ImVec2(120, 0)))
      ImGui::CloseCurrentPopup();

    if (!rtReady)
      ImGui::TextDisabled("Ray tracing not available on this GPU.");
    else if (!hasBodies)
      ImGui::TextDisabled(state.assembly ? "Insert a part to render."
                                         : "Add a body to render.");
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

  // Icon buttons carry a stable "###tb_*" id (hidden from the glyph label) so
  // the GUI/remote tests can address them regardless of the icon codepoint.
  if (ToolButton(ICON_FA_PENCIL "###tb_new_sketch", "New Sketch (S)",
                 hasDoc && idle))
    StartNewSketch(state);

  Divider();

  if (ToolButton(ICON_FA_CUBE "###tb_box", "Box", hasDoc && idle))
    CreateBox(state);
  if (ToolButton(ICON_FA_CIRCLE "###tb_sphere", "Sphere", hasDoc && idle))
    CreateSphere(state);
  if (ToolButton(ICON_FA_DATABASE "###tb_cylinder", "Cylinder", hasDoc && idle))
    CreateCylinder(state);

  Divider();

  // Turn a FINISHED sketch into a solid. The same operations live in the
  // sketch toolbar (Shift+E/R) for a sketch you are still editing; without
  // these, closing a sketch with ✓ left no way to extrude it at all.
  const bool hasProfile = hasDoc && ClosedProfileSketch(state) != nullptr;
  if (ToolButton(ICON_FA_UP_LONG "###tb_extrude",
                 "Extrude a sketch (the selected one, else the last)",
                 hasDoc && idle && hasProfile)) {
    if (SketchFeature* sk = ClosedProfileSketch(state))
      BeginExtrudeOfSketch(state, *sk);
  }
  if (ToolButton(ICON_FA_ROTATE "###tb_revolve",
                 "Revolve a sketch (the selected one, else the last)",
                 hasDoc && idle && hasProfile)) {
    if (SketchFeature* sk = ClosedProfileSketch(state))
      BeginRevolveOfSketch(state, *sk);
  }

  Divider();

  // Curve-driven features (curves-3d): a 3D curve, and the sweep/loft/pipe that
  // consume it. Gated on the references each needs (a Sweep wants a profile
  // sketch, a Loft two sections, a Pipe any curve/sketch to run along).
  const size_t sketches = hasDoc ? SketchFeatureCount(*state.document) : 0;
  const bool hasPathLike = hasDoc && HasCurveOrSketch(*state.document);
  if (ToolButton(ICON_FA_WAVE_SQUARE "###tb_curve3d",
                 "3D Curve (helix / fit-spline)", hasDoc && idle))
    CreateCurve3D(state);
  if (ToolButton(ICON_FA_ROUTE "###tb_sweep", "Sweep a profile along a path",
                 hasDoc && idle && sketches >= 1))
    CreateSweep(state);
  if (ToolButton(ICON_FA_BEZIER_CURVE "###tb_loft", "Loft through sections",
                 hasDoc && idle && sketches >= 2))
    CreateLoft(state);
  if (ToolButton(ICON_FA_ROAD "###tb_pipe", "Pipe / tube along a path",
                 hasDoc && idle && hasPathLike))
    CreatePipe(state);

  Divider();

  // Datums.
  if (ToolButton(ICON_FA_TABLE_CELLS_LARGE "###tb_datum_plane", "Datum Plane",
                 hasDoc && idle))
    CreateDatumPlane(state);
  if (ToolButton(ICON_FA_ARROWS_UP_DOWN "###tb_datum_axis", "Datum Axis",
                 hasDoc && idle))
    CreateDatumAxis(state);

  Divider();

  // Body combinators (need existing bodies).
  const size_t producers =
      hasDoc ? BodyProducers(*state.document, state.document->FeatureCount())
                   .size()
             : 0;
  if (ToolButton(ICON_FA_OBJECT_GROUP "###tb_boolean",
                 "Boolean (union / subtract / intersect)",
                 idle && producers >= 2))
    CreateBoolean(state);
  if (ToolButton(ICON_FA_LEFT_RIGHT "###tb_mirror", "Mirror a body",
                 idle && producers >= 1))
    CreateMirror(state);
  if (ToolButton(ICON_FA_GRIP "###tb_pattern", "Pattern (linear / circular)",
                 idle && producers >= 1))
    CreatePattern(state);

  Divider();

  // Edge / wall / detail ops on a body.
  const bool hasBody = hasDoc && !state.document->GetBodies().empty();
  if (ToolButton(ICON_FA_BORDER_TOP_LEFT "###tb_fillet", "Fillet edges",
                 hasBody && idle))
    CreateFillet(state);
  if (ToolButton(ICON_FA_CROP_SIMPLE "###tb_chamfer", "Chamfer edges",
                 hasBody && idle))
    CreateChamfer(state);
  if (ToolButton(ICON_FA_BARS_STAGGERED "###tb_rib",
                 "Rib (thin web from an open sketch)", hasBody && idle))
    CreateRib(state);
  if (ToolButton(ICON_FA_BULLSEYE "###tb_hole",
                 "Hole: pick a flat face, click the centres", hasBody && idle))
    StartHoleTool(state);
  if (ToolButton(ICON_FA_CIRCLE_NOTCH "###tb_thread",
                 "Thread (cosmetic; pick a cyl. face)", hasBody && idle))
    CreateThread(state);
  if (ToolButton(ICON_FA_BOX_OPEN "###tb_shell",
                 "Shell (hollow a body; pick faces to open)", hasBody && idle))
    CreateShell(state);
  if (ToolButton(ICON_FA_ANGLES_RIGHT "###tb_draft",
                 "Draft (taper faces for moulding)", hasBody && idle))
    CreateDraft(state);
  if (ToolButton(ICON_FA_SCISSORS "###tb_split",
                 "Split a body with a plane or one of its faces",
                 hasBody && idle))
    CreateSplit(state);
  if (ToolButton(ICON_FA_BROOM "###tb_refine",
                 "Refine (merge coplanar faces and boolean seams)",
                 hasBody && idle))
    CreateRefine(state);

  Divider();

  if (ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER "###tb_fit",
                 "Fit view (F)", hasDoc))
    FitCameraToDocument(state);
  if (ToolButton(ICON_FA_TABLE_CELLS "###tb_planes", "Toggle planes (Shift+P)",
                 hasDoc))
    state.showPlanes = !state.showPlanes;

  // Enter the FEM simulation workspace (needs a body to analyze).
  if (ToolButton(ICON_FA_GAUGE_HIGH "###tb_simulation",
                 "Simulation (FEM stress study)", hasBody && idle))
    EnterSimulation(state);

  // Measure tool (C3): a viewport mode — pick a face/edge for its own measure,
  // a second element for the distance. The button reflects the armed state.
  // Snapshot the armed state: the button toggles state.measure.active, so
  // guarding the pop on the live value (which the click just flipped) would
  // unbalance the style stack and trip ImGui's PopStyleColor assert.
  const bool measureActive = state.measure.active;
  if (measureActive)
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  if (ToolButton(ICON_FA_RULER "###tb_measure",
                 "Measure (pick 1-2 faces/edges; Esc to exit)", hasBody)) {
    const bool wasActive = state.measure.active;
    state.measure = MeasureState{};    // clear any in-progress measurement
    state.measure.active = !wasActive; // toggle
  }
  if (measureActive)
    ImGui::PopStyleColor();

  Divider();

  // Offline path-traced still (KeyShot-style). Opens the render dialog.
  const bool rtReady = state.rayTracer && state.rayTracer->IsInitialized();
  if (ToolButton(ICON_FA_CAMERA "###tb_render", "Render (offline raytrace)",
                 hasDoc && rtReady))
    ImGui::OpenPopup("Render Still");
  DrawRenderModal(state);

  ImGui::NewLine();
}

// The sketch-mode toolbar: draw tools, generative ops, and constraints — the
// icon ribbon that replaces the model tools while a sketch is being edited.
void DrawSketchModeToolbar(AppState& state) {
  SketchEditState& edit = state.sketchEdit;

  // Finish (accept) the sketch. Stable id: the GUI tests (and remote
  // click-driving) need to address this button. In a Hole session the ✓ is
  // the hand-off to the hole options rather than a plain exit.
  const bool holeSession = edit.purpose == SketchPurpose::Hole;
  bool finishHole = false;
  if (ToolButton(ICON_FA_CHECK "###tb_sk_finish",
                 holeSession ? "Hole options (Enter) - Esc cancels the hole"
                             : "Finish sketch (Esc)")) {
    if (holeSession)
      finishHole = true; // acted on after the toolbar, see below
    else
      edit = SketchEditState{};
  }
  Divider();

  // Switch to a sketch tool, resetting the transient per-tool edit state.
  // Shared by the single-tool buttons and the flyout groups.
  auto activateTool = [&](SketchTool t) {
    edit.tool = t;
    edit.hasPendingPoint = edit.hasPendingPoint2 = false;
    edit.mirrorAxisPhase = false;
    edit.dimPickBody.clear();
    edit.dimPlacing = false;
    edit.lastDrawn.clear(); // dimension-on-create doesn't cross tools
    edit.dimOnCreateOpen = false;
    if (t == SketchTool::Dimension)
      edit.selected.clear(); // dimension targets accumulate from scratch
    edit.actionMsg.clear();
    if (t == SketchTool::Mirror)
      MirrorToolActivated(state); // preselected construction line = axis
  };
  auto toolBtn = [&](const char* icon, const char* tip, SketchTool t) {
    const bool active = edit.tool == t;
    if (active)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(icon))
      activateTool(t);
    if (active)
      ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
  };
  // A flyout group (Onshape-style): the face shows the last-picked variant —
  // click it to use that tool — and the caret opens a popup to switch which
  // variant the group holds. `current` persists the pick across frames. The
  // face reflects the active tool if it is one of the group's variants.
  struct ToolVariant {
    const char* icon;
    const char* label;
    SketchTool tool;
  };
  auto toolGroup = [&](const char* id, int& current,
                       std::initializer_list<ToolVariant> variants) {
    const ToolVariant* v = variants.begin();
    const int n = static_cast<int>(variants.size());
    if (current < 0 || current >= n)
      current = 0;
    bool groupActive = false;
    for (int i = 0; i < n; i++)
      if (edit.tool == v[i].tool) {
        groupActive = true;
        current = i;
        break;
      }
    ImGui::PushID(id);
    if (groupActive)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(v[current].icon))
      activateTool(v[current].tool);
    if (groupActive)
      ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", v[current].label);
    ImGui::SameLine(0.0f, 1.0f);
    if (ImGui::Button(ICON_FA_CARET_DOWN "##more"))
      ImGui::OpenPopup("variants");
    if (ImGui::BeginPopup("variants")) {
      for (int i = 0; i < n; i++) {
        const std::string item = std::string(v[i].icon) + "   " + v[i].label;
        if (ImGui::Selectable(item.c_str(), i == current)) {
          current = i;
          activateTool(v[i].tool);
        }
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::SameLine();
  };
  // Which variant each group currently holds (remembered across frames).
  static int circleVariant = 0;
  static int rectVariant = 0;
  static int arcVariant = 0;

  toolBtn(ICON_FA_SLASH, "Line (L)", SketchTool::Line);
  toolGroup("circlegrp", circleVariant,
            {{ICON_FA_CIRCLE_NOTCH, "Circle (C)", SketchTool::Circle},
             {ICON_FA_EGG, "Ellipse", SketchTool::Ellipse}});
  toolGroup("rectgrp", rectVariant,
            {{ICON_FA_VECTOR_SQUARE, "Rectangle (G)", SketchTool::Rectangle},
             {ICON_FA_EXPAND, "Center Rectangle (Shift+G)",
              SketchTool::RectangleCenter}});
  toolGroup(
      "arcgrp", arcVariant,
      {{ICON_FA_BEZIER_CURVE, "3-Point Arc (A)", SketchTool::ArcThreePoint},
       {ICON_FA_BEZIER_CURVE, "Center Arc (centre, start, end)",
        SketchTool::Arc},
       {ICON_FA_BEZIER_CURVE, "Elliptic Arc (centre, major, minor, start, end)",
        SketchTool::EllipseArc}});
  toolBtn(ICON_FA_PEN_NIB, "Spline (S)", SketchTool::Spline);
  toolBtn(ICON_FA_CIRCLE_DOT, "Point", SketchTool::Point);
  toolBtn(ICON_FA_SCISSORS, "Trim (M)", SketchTool::Trim);
  toolBtn(ICON_FA_RULER_COMBINED, "Dimension (D)", SketchTool::Dimension);
  Divider();
  toolBtn(ICON_FA_LEFT_RIGHT, "Mirror", SketchTool::Mirror);
  toolBtn(ICON_FA_DRAW_POLYGON, "Fillet (Shift+F)", SketchTool::Fillet);
  toolBtn(ICON_FA_GRIP, "Pattern", SketchTool::Pattern);
  toolBtn(ICON_FA_CLONE, "Offset (O)", SketchTool::Offset);
  toolBtn(ICON_FA_DIAGRAM_PROJECT,
          "Project geometry (X) — click a body edge/face -> live reference",
          SketchTool::Project);
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
  // The everyday constraints stay visible; the rest live in the flyout below.
  conBtn("H", "Horizontal (H)", K::Horizontal);
  conBtn("V", "Vertical (V)", K::Vertical);
  conBtn(ICON_FA_ARROWS_TO_DOT, "Coincident (N)", K::Coincident);
  conBtn(ICON_FA_ARROWS_LEFT_RIGHT, "Parallel (B)", K::Parallel);
  conBtn(ICON_FA_ANGLE_RIGHT, "Perpendicular", K::Perpendicular);

  // "More constraints" flyout: the less-common relational constraints, one
  // click away. Always openable (so the menu is discoverable); each item
  // applies to the current selection and ApplySketchConstraint no-ops with a
  // hint if the combination doesn't fit. Dimensional constraints are NOT here —
  // the Dimension tool (D) covers distance / radius / diameter / angle.
  if (ImGui::Button(ICON_FA_ELLIPSIS "##morecon"))
    ImGui::OpenPopup("more_constraints");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("More constraints…");
  ImGui::SameLine();
  if (ImGui::BeginPopup("more_constraints")) {
    auto conItem = [&](const char* label, const char* tip, K kind) {
      if (ImGui::Selectable(label))
        ApplySketchConstraint(state, kind, 0.0);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    };
    conItem("Tangent", "Tangent (line/curve + curve) — (T)", K::Tangent);
    conItem("Equal", "Equal size (2 curves, or 2 lines) — (E)", K::Equal);
    conItem("Concentric", "Concentric (2 curves share a centre)",
            K::Concentric);
    ImGui::Separator();
    conItem("Point on line", "Point lies on a line (point + line)",
            K::PointOnLine);
    conItem("Symmetric", "Symmetric (2 points + a line)", K::Symmetric);
    conItem("Midpoint", "Point at a line's midpoint (point + line)",
            K::Midpoint);
    conItem("Fix / pin", "Pin a point in place", K::Fixed);
    conItem("Collinear", "Collinear (2 lines)", K::Collinear);
    ImGui::EndPopup();
  }
  Divider();

  ImGui::BeginDisabled(!hasSel);
  if (ImGui::Button(ICON_FA_MINUS "##constr"))
    ToggleConstruction(state);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Construction (Q) — reference geometry, dashed");
  ImGui::SameLine();
  Divider();

  // Mode toggles (were the Sketch panel's checkboxes, U8a).
  auto toggleBtn = [&](const char* label, const char* tip, bool& value) {
    if (value)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(label))
      value = !value;
    if (value)
      ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
  };
  toggleBtn(ICON_FA_MAGNET "###tb_sk_autocon",
            "Auto-constrain — snap and add constraints while drawing (hold "
            "Ctrl to place freely; Shift+S toggles)",
            edit.autoConstrain);
  toggleBtn(ICON_FA_EYE "###tb_sk_glyphs", "Show constraint glyphs",
            edit.showConstraintGlyphs);
  Divider();

  // Insert reference image (U8 polish): file dialog, then the fresh image's
  // session (opacity/calibrate/delete) opens in the Tool window. Editing
  // later: double-click the image's Operations row in the Model panel.
  {
    SketchFeature* sk0 = ResolveActiveSketch(state);
    const size_t imagesBefore = sk0 != nullptr ? sk0->images.size() : 0;
    if (ImGui::Button(ICON_FA_IMAGE "###tb_sk_image")) {
      OpenReferenceImageDialog(state); // commits + REPLACES the sketch feature
      SketchFeature* sk1 = ResolveActiveSketch(state);
      if (sk1 != nullptr && sk1->images.size() > imagesBefore) {
        activateTool(SketchTool::Image);
        edit.selectedImageId = sk1->images.back().id;
      }
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Insert reference image\xe2\x80\xa6");
    ImGui::SameLine();
  }
  Divider();

  // Extrude/Revolve straight from the sketch (were panel buttons, U8a);
  // Finish (✓) already exits without a solid.
  {
    SketchFeature* sketch = ResolveActiveSketch(state);
    const bool canExtrude =
        sketch != nullptr && !sketch::BuildLoops(sketch->entities).empty();
    ImGui::BeginDisabled(!canExtrude);
    if (ImGui::Button(ICON_FA_UP_LONG "###tb_sk_extrude"))
      BeginSketchExtrude(state); // Shift+E
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Extrude the sketch (Shift+E)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE "###tb_sk_revolve"))
      BeginSketchRevolve(state); // Shift+R
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Revolve the sketch (Shift+R)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Drill the sketch's points. The only GUI path to a hole on a sketch you
    // already have (e.g. a patterned bolt circle), and the visible twin of
    // the ✓ hand-off in a Hole session.
    ImGui::BeginDisabled(!CanBeginSketchHole(state));
    if (ImGui::Button(ICON_FA_BULLSEYE "###tb_sk_hole"))
      finishHole = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Drill holes at the sketch's points (Shift+H)");
    ImGui::SameLine();
  }
  // BeginSketchHole tears down the sketch session, so it must run after the
  // toolbar's widgets are done touching `edit` (the CalibrateImage rule).
  if (finishHole)
    BeginSketchHole(state);
  Divider();

  if (ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Fit view (F)"))
    FitCameraToDocument(state);
  ImGui::NewLine();
}

namespace {
// X to drop the next occurrence at: clear of everything already placed, by
// the shared rule (ui/Placement.h). Measured between the SHAPES, so the gap
// looks right whatever local origin each part models around.
double NextInsertX(AppState& state, uint64_t partId) {
  const auto spanOf = [&](uint64_t id, Span& out) {
    Document* doc = PartDocById(state, id);
    if (doc == nullptr || doc->GetBodies().empty())
      return false;
    out = {1e30, -1e30};
    for (const Body& b : doc->GetBodies()) {
      glm::vec3 bmin, bmax;
      b.shape.GetBoundingBox(bmin, bmax);
      out.lo = std::min(out.lo, static_cast<double>(bmin.x));
      out.hi = std::max(out.hi, static_cast<double>(bmax.x));
    }
    return out.lo <= out.hi;
  };
  std::vector<Span> taken;
  for (const Occurrence& o : state.assembly->GetOccurrences()) {
    Span s;
    if (spanOf(o.partId, s))
      taken.push_back({s.lo + o.position.x, s.hi + o.position.x});
  }
  Span incoming;
  if (!spanOf(partId, incoming))
    return 0.0;
  return ClearOffset(taken, incoming);
}
} // namespace

// The assembly-mode toolbar: mate-creation tools (A3). Shown when an assembly
// tab is active (no part document). Fit reuses FitCameraToDocument.
void DrawAssemblyToolbar(AppState& state) {
  // Insert part: place an occurrence of an existing part. Opens a picker of
  // the project's part tabs (the only GUI path to add an occurrence).
  if (ToolButton(ICON_FA_CIRCLE_PLUS "###tb_insert_part",
                 "Insert part (place an occurrence)"))
    ImGui::OpenPopup("insert_part");
  if (ImGui::BeginPopup("insert_part")) {
    ImGui::TextDisabled("Insert part");
    ImGui::Separator();
    bool any = false;
    for (const PartSlot& s : state.parts) {
      if (s.kind != DocKind::Part)
        continue;
      any = true;
      if (ImGui::MenuItem(s.name.c_str())) {
        SnapshotAssembly(state);
        Occurrence occ;
        occ.partId = s.id;
        occ.name = s.name;
        occ.fixed = state.assembly->OccurrenceCount() == 0; // first = anchor
        // Every part models around its own origin, so inserting two of them
        // buries the second inside the first — you had to drag them apart
        // before you could even see what you inserted. Park each new one
        // clear of what is already placed, along +X (mates move it anyway).
        occ.position.x = NextInsertX(state, s.id);
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

  // Every button carries a stable "###tb_*" id like the model toolbar's, so
  // tests and scripted humans address them by name rather than by icon
  // codepoint (the assembly/drawing/simulation bars had none at all).
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
  mateBtn(ICON_FA_LOCK "###tb_mate_fastened", "Fastened mate (rigid)",
          MateType::Fastened);
  mateBtn(ICON_FA_ARROWS_SPIN "###tb_mate_revolute", "Revolute mate (hinge)",
          MateType::Revolute);
  mateBtn(ICON_FA_ARROWS_LEFT_RIGHT "###tb_mate_slider", "Slider mate",
          MateType::Slider);
  mateBtn(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT "###tb_mate_cylindrical",
          "Cylindrical mate (spin + slide)", MateType::Cylindrical);
  mateBtn(ICON_FA_TABLE_CELLS_LARGE "###tb_mate_planar",
          "Planar mate (slide in a plane)", MateType::Planar);
  mateBtn(ICON_FA_CIRCLE_DOT "###tb_mate_ball", "Ball mate (free rotation)",
          MateType::Ball);
  Divider();
  if (ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER "###tb_asm_fit",
                 "Fit view (F)"))
    FitCameraToDocument(state);

  // Measure across the assembly: same tool as the part tab — picks carry
  // their occurrence, so distances span occurrences in world space. Snapshot
  // the armed state so the toggling click can't unbalance the style stack (see
  // the model-toolbar measure button).
  const bool measureActive = state.measure.active;
  if (measureActive)
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  if (ToolButton(ICON_FA_RULER "###tb_asm_measure",
                 "Measure (pick 1-2 faces/edges; Esc to exit)",
                 state.assembly->OccurrenceCount() > 0)) {
    const bool wasActive = state.measure.active;
    state.measure = MeasureState{};    // clear any in-progress measurement
    state.measure.active = !wasActive; // toggle
  }
  if (measureActive)
    ImGui::PopStyleColor();

  Divider();

  // Offline path-traced still, same as the part tab. SyncRayTracingScene has
  // walked assembly occurrences at their solved placements since the RT work
  // landed; only the buttons and the RPC guard said otherwise, so an assembly
  // could be modelled, mated and exploded but never rendered.
  if (ToolButton(ICON_FA_CAMERA "###tb_asm_render",
                 "Render (offline "
                 "raytrace)",
                 state.rayTracer != nullptr &&
                     state.assembly->OccurrenceCount() > 0))
    ImGui::OpenPopup("Render Still");
  DrawRenderModal(state);

  if (state.mateTool.active && !state.mateTool.message.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", state.mateTool.message.c_str());
  }
  if (state.mateTool.active && state.mateTool.hasHover &&
      state.mateTool.previewMateId == 0) {
    const bool edgeSnap = state.mateTool.hover.anchor.snapEdge.IsSet();
    ImGui::SameLine();
    ImGui::TextColored(edgeSnap ? ImVec4(1.0f, 0.78f, 0.22f, 1.0f)
                                : ImVec4(0.82f, 0.82f, 0.60f, 1.0f),
                       "%s",
                       !edgeSnap ? "Snap: face center"
                       : state.mateTool.hoverSnapIsMidpoint
                           ? "Snap: edge midpoint"
                           : "Snap: hole center");
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
    if (ToolButton(icon, tip, hasStudy)) // toggle
      SimulationArmTool(state, armed ? SimulationState::Tool::None : t);
    if (armed)
      ImGui::PopStyleColor();
  };
  bcBtn(ICON_FA_LOCK "###tb_sim_fixed", "Fixed support (click a face)",
        SimulationState::Tool::Fixed);
  bcBtn(ICON_FA_BOLT "###tb_sim_force", "Force load (click a face)",
        SimulationState::Tool::Force);
  bcBtn(ICON_FA_DOWN_LONG "###tb_sim_pressure", "Pressure load (click a face)",
        SimulationState::Tool::Pressure);

  Divider();

  if (ToolButton(ICON_FA_PLAY "###tb_sim_run", "Run study (mesh + solve)",
                 hasStudy)) {
    std::string err;
    if (!RunStudyAndCache(*doc, sim.activeStudyId, &err))
      LOG_WARN("run study failed: {}", err);
  }

  Divider();

  if (ToolButton(ICON_FA_ARROW_RIGHT_FROM_BRACKET "###tb_sim_exit",
                 "Exit Simulation (Esc)"))
    ExitSimulation(state);

  // Hint while a BC tool is armed.
  if (sim.tool != SimulationState::Tool::None) {
    ImGui::SameLine();
    const char* what = sim.tool == SimulationState::Tool::Fixed
                           ? "fixed support"
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
  if (ToolButton(ICON_FA_CIRCLE_PLUS "###tb_dwg_add_view",
                 "Add view (project a part/assembly)"))
    ImGui::OpenPopup("add_view");
  if (ImGui::BeginPopup("add_view")) {
    ImGui::TextDisabled("Add view of");
    ImGui::Separator();
    struct Ori {
      const char* label;
      ViewOrientation o;
    };
    static const Ori orients[] = {{"Front", ViewOrientation::Front},
                                  {"Top", ViewOrientation::Top},
                                  {"Right", ViewOrientation::Right},
                                  {"Iso", ViewOrientation::Iso}};
    bool any = false;
    for (const PartSlot& s : state.parts) {
      if (s.kind != DocKind::Part && s.kind != DocKind::Assembly)
        continue;
      any = true;
      if (ImGui::BeginMenu(s.name.c_str())) {
        for (const Ori& ori : orients) {
          if (ImGui::MenuItem(ori.label)) {
            DrawingView v;
            v.name = ori.label;
            v.sourceDocId = s.id;
            v.orientation = ori.o;
            v.scale = 0.0; // 0 = fit a standard scale to the sheet
            AddDrawingViewPlaced(state, v);
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

  if (ToolButton(ICON_FA_ARROWS_ROTATE "###tb_dwg_update",
                 "Update views (re-project)"))
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
  dimBtn(ICON_FA_LEFT_RIGHT "###tb_dwg_dist", "Distance dimension",
         DrawingDimKind::Distance);
  dimBtn(ICON_FA_RULER_HORIZONTAL "###tb_dwg_distx", "Horizontal (X) distance",
         DrawingDimKind::DistanceX);
  dimBtn(ICON_FA_UP_DOWN "###tb_dwg_disty", "Vertical (Y) distance",
         DrawingDimKind::DistanceY);
  dimBtn(ICON_FA_CIRCLE_NOTCH "###tb_dwg_radius", "Radius dimension",
         DrawingDimKind::Radius);
  dimBtn(ICON_FA_CIRCLE_HALF_STROKE "###tb_dwg_diameter", "Diameter dimension",
         DrawingDimKind::Diameter);
  dimBtn(ICON_FA_ANGLE_LEFT "###tb_dwg_angle", "Angle dimension",
         DrawingDimKind::Angle);
  dimBtn(ICON_FA_BULLSEYE "###tb_dwg_hole_callout",
         "Hole callout (pick a drilled hole's circle)",
         DrawingDimKind::HoleCallout);
  dimBtn(ICON_FA_ELLIPSIS "###tb_dwg_pattern_callout",
         "Pattern callout (pick 2+ circles, Enter finishes)",
         DrawingDimKind::PatternCallout);

  Divider();

  if (ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER "###tb_dwg_fit",
                 "Fit sheet (F)"))
    state.sheetView.scale = 0.0; // refit on next paint
  // Export needs a native save dialog; without one (bare session, container)
  // the click used to do nothing at all. Say so on the button instead.
  const bool canSave = FileDialogsAvailable();
  if (ToolButton(ICON_FA_FILE_EXPORT "###tb_dwg_export",
                 canSave ? "Export drawing (SVG / PDF)"
                         : "Export needs a system file dialog (unavailable "
                           "here) - use export_drawing over the script API",
                 !dwg.GetViews().empty() && canSave))
    ExportDialog(state);

  if (state.dimTool.active) {
    ImGui::SameLine();
    const int picked = static_cast<int>(state.dimTool.refs.size());
    if (state.dimTool.kind == DrawingDimKind::PatternCallout) {
      ImGui::TextDisabled("pick circles (%d so far) - Enter finishes, Esc "
                          "cancels",
                          picked);
    } else {
      const bool one = state.dimTool.kind == DrawingDimKind::Radius ||
                       state.dimTool.kind == DrawingDimKind::Diameter ||
                       state.dimTool.kind == DrawingDimKind::HoleCallout;
      ImGui::TextDisabled("pick edge %d of %d (Esc to cancel)", picked + 1,
                          one ? 1 : 2);
    }
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
