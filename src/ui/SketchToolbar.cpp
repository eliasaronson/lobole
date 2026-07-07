#include "ui/SketchToolbar.h"

#include "core/ExtrudeFeature.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"
#include "core/SketchSolver.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

namespace ui {

void DrawSketchToolbar(AppState& state) {
  SketchEditState& edit = state.sketchEdit;

  if (edit.pickingPlane) {
    ImGui::Begin("Sketch Tools");
    ImGui::TextWrapped(
        "Pick a plane or a model face to sketch on. (Esc to cancel)");
    if (ImGui::Button("Cancel"))
      edit.pickingPlane = false;
    ImGui::End();
    return;
  }

  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return;

  ImGui::Begin("Sketch Tools");
  ImGui::TextUnformatted(sketch->GetName().c_str());
  ImGui::Separator();

  auto toolButton = [&](const char* label, SketchTool tool) {
    const bool active = edit.tool == tool;
    if (active)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(label)) {
      edit.tool = tool;
      edit.hasPendingPoint = false; // break any in-progress line chain
    }
    if (active)
      ImGui::PopStyleColor();
  };
  toolButton("Select", SketchTool::Select);
  ImGui::SameLine();
  toolButton("Point", SketchTool::Point);
  ImGui::SameLine();
  toolButton("Line", SketchTool::Line);
  ImGui::SameLine();
  toolButton("Circle", SketchTool::Circle);
  ImGui::SameLine();
  toolButton("Rect", SketchTool::Rectangle);
  ImGui::SameLine();
  toolButton(ICON_FA_SCISSORS " Trim", SketchTool::Trim);

  // Constraints (enabled by the current Select-mode selection)
  ImGui::SeparatorText("Constraints");
  const auto& sel = edit.selected;
  using K = sketch::ConstraintKind;
  auto entityOf = [&](const sketch::PointRef& r) -> const sketch::Entity* {
    size_t idx = sketch->IndexOfEntity(r.entity);
    return idx == SIZE_MAX ? nullptr : &sketch->entities[idx];
  };
  auto isLine = [&](const sketch::PointRef& r) {
    const auto* e = entityOf(r);
    return e && std::holds_alternative<sketch::Line>(*e);
  };
  auto isCircle = [&](const sketch::PointRef& r) {
    const auto* e = entityOf(r);
    return e && std::holds_alternative<sketch::Circle>(*e);
  };
  auto isPoint = [&](const sketch::PointRef& r) {
    const auto* e = entityOf(r);
    return e && std::holds_alternative<sketch::Point>(*e);
  };
  auto conButton = [&](const char* label, const char* tip, bool enabled,
                       K kind, double value = 0.0) {
    ImGui::BeginDisabled(!enabled);
    if (ImGui::Button(label))
      ApplySketchConstraint(state, kind, value);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
  };
  const bool twoPts = sel.size() >= 2;
  const bool twoLines = sel.size() >= 2 && isLine(sel[0]) && isLine(sel[1]);
  conButton(ICON_FA_ARROWS_TO_DOT, "Coincident (2 points)", twoPts,
            K::Coincident);
  conButton("H", "Horizontal (line)", !sel.empty() && isLine(sel[0]),
            K::Horizontal);
  conButton("V", "Vertical (line)", !sel.empty() && isLine(sel[0]),
            K::Vertical);
  conButton(ICON_FA_ARROWS_LEFT_RIGHT, "Parallel (2 lines)", twoLines,
            K::Parallel);
  conButton(ICON_FA_ANGLE_RIGHT, "Perpendicular (2 lines)", twoLines,
            K::Perpendicular);
  conButton("=", "Equal (2 lines/circles)", sel.size() >= 2, K::Equal);
  const bool lineCircle =
      sel.size() >= 2 && ((isLine(sel[0]) && isCircle(sel[1])) ||
                          (isCircle(sel[0]) && isLine(sel[1])));
  conButton("T", "Tangent (line + circle)", lineCircle, K::Tangent);
  const bool ptOnLine =
      sel.size() >= 2 &&
      ((isPoint(sel[0]) && isLine(sel[1])) ||
       (isLine(sel[0]) && isPoint(sel[1])) ||
       (isLine(sel[0]) && isLine(sel[1]) && sel[0].entity != sel[1].entity));
  conButton("L", "Point on line (point + line)", ptOnLine, K::PointOnLine);
  ImGui::NewLine();

  // Dimensional constraints with a value input
  static float dimValue = 1.0f;
  ImGui::SetNextItemWidth(70);
  ImGui::DragFloat("##dim", &dimValue, 0.05f, 0.01f, 1e6f, "%.3f");
  ImGui::SameLine();
  if (ImGui::Button(ICON_FA_RULER " Distance") && sel.size() >= 2)
    ApplySketchConstraint(state, K::Distance, dimValue);
  ImGui::SameLine();
  if (ImGui::Button("Radius") && !sel.empty() && isCircle(sel[0]))
    ApplySketchConstraint(state, K::Radius, dimValue);

  // Angle dimension (degrees in the UI, radians in the model).
  static float angleDeg = 90.0f;
  ImGui::SetNextItemWidth(70);
  ImGui::DragFloat("##angle", &angleDeg, 0.5f, 0.0f, 360.0f, "%.1f deg");
  ImGui::SameLine();
  if (ImGui::Button(ICON_FA_ANGLE_RIGHT " Angle") && twoLines)
    ApplySketchConstraint(state, K::Angle, glm::radians(angleDeg));

  ImGui::SeparatorText("Profile");
  ImGui::Text("Entities: %zu  Constraints: %zu", sketch->entities.size(),
              sketch->constraints.size());

  const bool canExtrude = !sketch::BuildLoops(sketch->entities).empty();
  ImGui::Text("Closed profile: %s", canExtrude ? "yes" : "no");

  ImGui::Separator();
  ImGui::BeginDisabled(!canExtrude);
  if (ImGui::Button("Extrude...")) {
    const int n = static_cast<int>(state.document->FeatureCount()) + 1;
    const uint64_t sketchId = edit.activeSketchId;
    edit = SketchEditState{}; // exit sketch mode so the solid + dialog show
    // Preview the extrude; the user accepts/cancels it.
    BeginPreviewFeature(state, std::make_unique<ExtrudeFeature>(
                                   "Extrude " + std::to_string(n), sketchId,
                                   1.0, ExtrudeMode::NewBody));
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  if (ImGui::Button("Exit Sketch"))
    edit = SketchEditState{};

  ImGui::End();
}

} // namespace ui
