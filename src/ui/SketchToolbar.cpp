#include "ui/SketchToolbar.h"

#include "core/ExtrudeFeature.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"
#include "ui/SketchInteraction.h"

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
    if (ImGui::Button(label))
      edit.tool = tool;
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

  ImGui::Separator();
  ImGui::Text("Entities: %zu", sketch->entities.size());

  const bool canExtrude = !sketch::BuildLoops(sketch->entities).empty();
  ImGui::Text("Closed profile: %s", canExtrude ? "yes" : "no");

  ImGui::Separator();
  ImGui::BeginDisabled(!canExtrude);
  if (ImGui::Button("Extrude...")) {
    const int n = static_cast<int>(state.document->FeatureCount()) + 1;
    state.commands->Execute(
        std::make_unique<AddFeatureCommand>(std::make_unique<ExtrudeFeature>(
            "Extrude " + std::to_string(n), edit.activeSketchId, 1.0,
            ExtrudeMode::NewBody)),
        *state.document);
    // Select the new extrude for immediate distance/mode editing
    state.selectedFeature =
        static_cast<int>(state.document->FeatureCount() - 1);
    edit = SketchEditState{}; // exit sketch mode
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  if (ImGui::Button("Exit Sketch"))
    edit = SketchEditState{};

  ImGui::End();
}

} // namespace ui
