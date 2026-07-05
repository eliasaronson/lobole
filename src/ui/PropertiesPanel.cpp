#include "ui/PropertiesPanel.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "core/Feature.h"
#include "core/Serialization.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace ui {

namespace {

// Per-widget edit lifecycle for live parameter editing:
// - on activation, snapshot the feature (undo baseline)
// - while the widget changes the value, regenerate for live preview
// - on deactivation after an edit, push one undoable ModifyFeatureCommand
//   spanning the whole drag (its execute is idempotent over live edits)
// - on deactivation without an edit (e.g. Esc revert), regenerate once so
//   the geometry matches the restored value
void HandleEdit(AppState& state, Feature& feature, bool changedThisFrame) {
  Document& doc = *state.document;

  if (ImGui::IsItemActivated()) {
    state.editBaseline = DocumentIO::FeatureToJson(feature);
  }
  if (changedThisFrame) {
    doc.Regenerate();
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    nlohmann::json after = DocumentIO::FeatureToJson(feature);
    if (after != state.editBaseline) {
      state.commands->Execute(
          std::make_unique<ModifyFeatureCommand>(
              static_cast<size_t>(state.selectedFeature), state.editBaseline,
              after),
          doc);
    }
  } else if (ImGui::IsItemDeactivated()) {
    doc.Regenerate();
  }
}

bool DragDim(const char* label, double* value) {
  static const double kMin = 0.01, kMax = 1e6;
  return ImGui::DragScalar(label, ImGuiDataType_Double, value, 0.05f, &kMin,
                           &kMax, "%.3f", ImGuiSliderFlags_AlwaysClamp);
}

void DrawFeatureEditor(AppState& state, Feature& feature) {
  // Name (committed on Enter or focus loss, undoable like any other edit)
  static char nameBuf[128];
  static int nameBufFeature = -1;
  if (nameBufFeature != state.selectedFeature ||
      !ImGui::IsAnyItemActive()) {
    snprintf(nameBuf, sizeof(nameBuf), "%s", feature.GetName().c_str());
    nameBufFeature = state.selectedFeature;
  }
  ImGui::InputText("Name", nameBuf, sizeof(nameBuf));
  if (ImGui::IsItemActivated()) {
    state.editBaseline = DocumentIO::FeatureToJson(feature);
  }
  if (ImGui::IsItemDeactivatedAfterEdit() && nameBuf[0] != '\0' &&
      feature.GetName() != nameBuf) {
    feature.SetName(nameBuf);
    state.document->Regenerate(); // body names come from the feature
    nlohmann::json after = DocumentIO::FeatureToJson(feature);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(
            static_cast<size_t>(state.selectedFeature), state.editBaseline,
            after),
        *state.document);
  }

  if (auto* box = dynamic_cast<BoxFeature*>(&feature)) {
    HandleEdit(state, feature,
               ImGui::DragFloat3("Position", &box->position.x, 0.05f));
    HandleEdit(state, feature, DragDim("Width", &box->width));
    HandleEdit(state, feature, DragDim("Height", &box->height));
    HandleEdit(state, feature, DragDim("Depth", &box->depth));
  } else if (auto* sphere = dynamic_cast<SphereFeature*>(&feature)) {
    HandleEdit(state, feature,
               ImGui::DragFloat3("Center", &sphere->center.x, 0.05f));
    HandleEdit(state, feature, DragDim("Radius", &sphere->radius));
  } else if (auto* cyl = dynamic_cast<CylinderFeature*>(&feature)) {
    HandleEdit(state, feature,
               ImGui::DragFloat3("Center", &cyl->center.x, 0.05f));
    HandleEdit(state, feature,
               ImGui::DragFloat3("Direction", &cyl->direction.x, 0.05f));
    HandleEdit(state, feature, DragDim("Radius", &cyl->radius));
    HandleEdit(state, feature, DragDim("Height", &cyl->height));
  } else if (auto* import = dynamic_cast<ImportFeature*>(&feature)) {
    ImGui::TextDisabled("File: %s", import->path.c_str());
  }
}

} // namespace

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
    DrawFeatureEditor(state, *feature);
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
  ImGui::BulletText("Left click: select face");
  ImGui::BulletText("Ctrl+Left click: align camera to face");
  ImGui::BulletText("Right click: orbit camera");
  ImGui::BulletText("Scroll: zoom");

  ImGui::End();
}

} // namespace ui
