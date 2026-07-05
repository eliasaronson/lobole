#include "ui/PropertiesPanel.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "core/Feature.h"

#include "imgui.h"

namespace ui {

void DrawPropertiesPanel(AppState& state) {
  ImGui::Begin("Properties");

  ImGui::SeparatorText("Feature");
  Document* doc = state.document.get();
  Feature* feature =
      (doc && state.selectedFeature >= 0)
          ? doc->GetFeature(static_cast<size_t>(state.selectedFeature))
          : nullptr;
  if (feature) {
    ImGui::Text("%s (%s)", feature->GetName().c_str(),
                feature->TypeName().c_str());
  } else {
    ImGui::TextDisabled("No feature selected");
  }

  ImGui::SeparatorText("Camera");
  if (state.camera) {
    glm::vec3 pos = state.camera->GetPosition();
    ImGui::Text("Position: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
    ImGui::Text("Distance: %.2f", state.camera->GetDistance());
  }

  ImGui::SeparatorText("Selection");
  if (state.faceSelector) {
    if (const Face* face = state.faceSelector->GetSelectedFace()) {
      ImGui::Text("Face %u on mesh %u", face->faceIndex, face->meshId);
      glm::vec3 normal =
          state.faceSelector->GetFaceNormal(face->meshId, face->faceIndex);
      ImGui::Text("Normal: (%.3f, %.3f, %.3f)", normal.x, normal.y, normal.z);
    } else {
      ImGui::TextDisabled("No face selected");
    }
  }

  ImGui::SeparatorText("Controls");
  ImGui::BulletText("Left click: select face");
  ImGui::BulletText("Ctrl+Left click: align camera to face");
  ImGui::BulletText("Right click: orbit camera");
  ImGui::BulletText("Scroll: zoom");

  ImGui::End();
}

} // namespace ui
