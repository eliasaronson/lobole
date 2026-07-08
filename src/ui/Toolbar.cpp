#include "ui/Toolbar.h"

#include "core/SketchFeature.h"
#include "ui/AppActions.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

namespace ui {

namespace {

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

  if (ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Fit view (F)",
                 hasDoc))
    FitCameraToDocument(state);
  if (ToolButton(ICON_FA_TABLE_CELLS, "Toggle planes (Shift+P)", hasDoc))
    state.showPlanes = !state.showPlanes;

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

} // namespace

void DrawToolbar(AppState& state) {
  if (state.sketchEdit.IsEditing())
    DrawSketchModeToolbar(state);
  else
    DrawModelToolbar(state);
}

} // namespace ui
