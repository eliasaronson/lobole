#include "ui/PropertiesPanel.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "core/Feature.h"
#include "ui/FeatureEditor.h"

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
    ImGui::Text("%s", feature->TypeName().c_str());
    // A pending (preview) feature isn't on the undo stack; edit it live
    // without committing (the preview dialog owns accept/cancel).
    const bool commit = feature->GetId() != state.pendingFeatureId;
    DrawFeatureParamWidgets(state, *feature,
                            static_cast<size_t>(state.selectedFeature), commit);
  } else {
    ImGui::TextDisabled("No feature selected");
    ImGui::TextDisabled("Select one in the Model panel");
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
  ImGui::BulletText("Left click: select face (Ctrl: align camera)");
  ImGui::BulletText("Middle drag: rotate");
  ImGui::BulletText("Ctrl+Middle: pan   Shift+Middle: zoom");
  ImGui::BulletText("Alt+Middle: roll   Scroll: zoom");
  ImGui::BulletText("Arrows: rotate (Shift: 90\xc2\xb0)   Z: zoom");

  ImGui::End();
}

} // namespace ui
