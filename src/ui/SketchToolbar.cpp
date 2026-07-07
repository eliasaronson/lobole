#include "ui/SketchToolbar.h"

#include "core/ExtrudeFeature.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"
#include "core/SketchSolver.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <set>

namespace ui {

namespace {
const char* ConstraintKindName(sketch::ConstraintKind k) {
  using K = sketch::ConstraintKind;
  switch (k) {
  case K::Coincident: return "coincident";
  case K::Horizontal: return "horizontal";
  case K::Vertical: return "vertical";
  case K::Parallel: return "parallel";
  case K::Perpendicular: return "perpendicular";
  case K::Equal: return "equal";
  case K::Distance: return "distance";
  case K::Radius: return "radius";
  case K::Angle: return "angle";
  case K::Tangent: return "tangent";
  case K::PointOnLine: return "point-on-line";
  case K::Symmetric: return "symmetric";
  case K::Midpoint: return "midpoint";
  case K::Fixed: return "fixed";
  case K::Concentric: return "concentric";
  case K::Collinear: return "collinear";
  case K::DistanceX: return "distance-x";
  case K::DistanceY: return "distance-y";
  case K::Diameter: return "diameter";
  }
  return "?";
}
} // namespace

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
      edit.hasPendingPoint2 = false;
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
  toolButton("Arc", SketchTool::Arc);
  ImGui::SameLine();
  toolButton("3-Pt Arc", SketchTool::ArcThreePoint);
  ImGui::SameLine();
  toolButton(ICON_FA_SCISSORS " Trim", SketchTool::Trim);

  ImGui::Checkbox("Auto-constrain", &edit.autoConstrain);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Snap and add constraints while drawing (hold Ctrl to "
                      "place freely)");
  ImGui::SameLine();
  ImGui::BeginDisabled(edit.selected.empty());
  if (ImGui::Button("Construction"))
    ToggleConstruction(state);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Toggle selected geometry as construction/reference "
                      "(dashed, excluded from the profile)");

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
  auto isArc = [&](const sketch::PointRef& r) {
    const auto* e = entityOf(r);
    return e && std::holds_alternative<sketch::Arc>(*e);
  };
  // Circle or arc: both carry a radius and a center for radial constraints.
  auto isCurve = [&](const sketch::PointRef& r) {
    return isCircle(r) || isArc(r);
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
  const bool lineCurve =
      sel.size() >= 2 && ((isLine(sel[0]) && isCurve(sel[1])) ||
                          (isCurve(sel[0]) && isLine(sel[1])));
  conButton("T", "Tangent (line + circle/arc)", lineCurve, K::Tangent);
  const bool ptOnLine =
      sel.size() >= 2 &&
      ((isPoint(sel[0]) && isLine(sel[1])) ||
       (isLine(sel[0]) && isPoint(sel[1])) ||
       (isLine(sel[0]) && isLine(sel[1]) && sel[0].entity != sel[1].entity));
  conButton("L", "Point on line (point + line)", ptOnLine, K::PointOnLine);
  const bool twoCurves = sel.size() >= 2 && isCurve(sel[0]) && isCurve(sel[1]);
  int symLines = 0;
  for (size_t i = 0; i < sel.size() && i < 3; i++)
    if (isLine(sel[i])) symLines++;
  const bool symmetric = sel.size() >= 3 && symLines >= 1;
  const bool midpoint =
      sel.size() >= 2 && (isLine(sel[0]) != isLine(sel[1])); // exactly one line
  conButton("Sym", "Symmetric (2 points + a line)", symmetric, K::Symmetric);
  conButton("Mid", "Midpoint (point + line)", midpoint, K::Midpoint);
  conButton(ICON_FA_LOCK, "Fix (pin a point)", !sel.empty(), K::Fixed);
  conButton("Con", "Concentric (2 circles/arcs)", twoCurves, K::Concentric);
  conButton("Col", "Collinear (2 lines)", twoLines, K::Collinear);
  ImGui::NewLine();

  // Dimensional constraints with a value input
  static float dimValue = 1.0f;
  ImGui::SetNextItemWidth(70);
  ImGui::DragFloat("##dim", &dimValue, 0.05f, 0.01f, 1e6f, "%.3f");
  ImGui::SameLine();
  if (ImGui::Button(ICON_FA_RULER " Distance") && sel.size() >= 2)
    ApplySketchConstraint(state, K::Distance, dimValue);
  ImGui::SameLine();
  if (ImGui::Button("Radius") && !sel.empty() && isCurve(sel[0]))
    ApplySketchConstraint(state, K::Radius, dimValue);
  ImGui::SameLine();
  if (ImGui::Button("Ø") && !sel.empty() && isCurve(sel[0]))
    ApplySketchConstraint(state, K::Diameter, dimValue);
  ImGui::SameLine();
  if (ImGui::Button("dX") && sel.size() >= 2)
    ApplySketchConstraint(state, K::DistanceX, dimValue);
  ImGui::SameLine();
  if (ImGui::Button("dY") && sel.size() >= 2)
    ApplySketchConstraint(state, K::DistanceY, dimValue);

  // Angle dimension (degrees in the UI, radians in the model).
  static float angleDeg = 90.0f;
  ImGui::SetNextItemWidth(70);
  ImGui::DragFloat("##angle", &angleDeg, 0.5f, 0.0f, 360.0f, "%.1f deg");
  ImGui::SameLine();
  if (ImGui::Button(ICON_FA_ANGLE_RIGHT " Angle") && twoLines)
    ApplySketchConstraint(state, K::Angle, glm::radians(angleDeg));

  // Constraint status + management.
  ImGui::SeparatorText("Status");
  if (edit.dof == 0)
    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.60f, 1.0f), "Fully constrained");
  else if (edit.dof > 0)
    ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%d DOF free", edit.dof);
  else
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Not solved");
  if (!edit.conflictingConstraints.empty())
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%zu conflicting",
                       edit.conflictingConstraints.size());
  ImGui::Checkbox("Show constraints", &edit.showConstraintGlyphs);

  if (!sketch->constraints.empty() &&
      ImGui::TreeNode("Constraints##list")) {
    std::set<size_t> bad(edit.conflictingConstraints.begin(),
                         edit.conflictingConstraints.end());
    int toDelete = -1;
    for (size_t i = 0; i < sketch->constraints.size(); i++) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::SmallButton("x"))
        toDelete = static_cast<int>(i);
      ImGui::SameLine();
      const char* name = ConstraintKindName(sketch->constraints[i].kind);
      if (bad.count(i))
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", name);
      else
        ImGui::TextUnformatted(name);
      ImGui::PopID();
    }
    ImGui::TreePop();
    if (toDelete >= 0)
      DeleteConstraint(state, static_cast<size_t>(toDelete));
  }

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
