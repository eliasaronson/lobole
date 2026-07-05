#include "ui/SketchToolbar.h"

#include "SketchManager.h"

#include "imgui.h"

namespace ui {

void DrawSketchToolbar(AppState& state) {
  SketchManager* sketch = state.sketchManager;
  if (!sketch || !sketch->IsSketchMode())
    return;

  ImGui::Begin("Sketch Tools");

  ImGui::Text("Sketch Mode Active");
  ImGui::Separator();

  if (ImGui::Button("Select")) {
    sketch->SetSketchMode(SketchMode::Select);
  }
  ImGui::SameLine();
  if (ImGui::Button("Point")) {
    sketch->SetSketchMode(SketchMode::Point);
  }
  ImGui::SameLine();
  if (ImGui::Button("Line")) {
    sketch->SetSketchMode(SketchMode::Line);
  }
  ImGui::SameLine();
  if (ImGui::Button("Circle")) {
    sketch->SetSketchMode(SketchMode::Circle);
  }
  ImGui::SameLine();
  if (ImGui::Button("Rectangle")) {
    sketch->SetSketchMode(SketchMode::Rectangle);
  }

  ImGui::Separator();

  SketchMode currentMode = sketch->GetSketchMode();
  const char* modeNames[] = {"Select", "Point", "Line", "Circle", "Rectangle"};
  ImGui::Text("Current Tool: %s", modeNames[(int)currentMode]);

  if (auto activeSketch = sketch->GetActiveSketch()) {
    ImGui::Text("Elements: %zu", activeSketch->GetElementCount());
    ImGui::Text("Can Extrude: %s", sketch->CanExtrude() ? "Yes" : "No");
  }

  ImGui::Separator();

  if (ImGui::Button("Exit Sketch")) {
    sketch->ExitSketch();
  }

  ImGui::End();
}

} // namespace ui
