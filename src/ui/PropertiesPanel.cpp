#include "ui/PropertiesPanel.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "ui/FeatureEditor.h"

#include "imgui.h"

namespace ui {

namespace {
// Mass properties for the active part, shown on demand (the OCC volume-props
// pass isn't per-frame cheap, so it's cached by document version). When the
// section is open the document's center of mass is stamped into AppState so
// the viewport draws a CoG marker.
void DrawMassPropertiesSection(AppState& state) {
  state.showCog = false;
  if (!state.document || state.document->GetBodies().empty())
    return;
  ImGui::Spacing();
  if (!ImGui::CollapsingHeader("Mass properties"))
    return;

  static Measure::DocumentMass cache;
  static const void* cachedDoc = nullptr;
  static uint64_t cachedVersion = UINT64_MAX;
  static bool cachedOk = false;
  const uint64_t version = state.document->Version();
  if (cachedDoc != static_cast<const void*>(state.document.get()) ||
      cachedVersion != version) {
    const auto m = Measure::MassOfBodies(state.document->GetBodies());
    cachedOk = m.has_value();
    if (cachedOk) cache = *m;
    cachedDoc = state.document.get();
    cachedVersion = version;
  }
  if (!cachedOk) {
    ImGui::TextDisabled("Mass properties unavailable.");
    return;
  }

  ImGui::Text("Volume: %.3f mm\xc2\xb3", cache.volume);
  double area = 0.0;
  for (const auto& b : cache.bodies) area += b.surfaceArea;
  ImGui::Text("Surface area: %.3f mm\xc2\xb2", area);
  if (cache.massKnown)
    ImGui::Text("Mass: %.3f g", cache.massG);
  else
    ImGui::TextDisabled("Mass: unknown (assign a material)");
  ImGui::Text("Center of mass:");
  ImGui::Text("  (%.3f, %.3f, %.3f)", cache.centerOfMass.x,
              cache.centerOfMass.y, cache.centerOfMass.z);

  // Draw the CoG marker in the viewport while this section is open.
  state.showCog = true;
  state.cogWorld = glm::vec3(cache.centerOfMass);
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

  // Measure tool readout (C3): live while the tool is armed.
  if (state.measure.active) {
    ImGui::SeparatorText("Measure");
    ImGui::TextDisabled("Pick a face or edge (a second for distance). Esc exits.");
    if (!state.measure.readout.empty())
      ImGui::TextWrapped("%s", state.measure.readout.c_str());
  }

  DrawMassPropertiesSection(state);

  ImGui::SeparatorText("Controls");
  ImGui::BulletText("Left click: select face (Ctrl: align camera)");
  ImGui::BulletText("Middle drag: rotate");
  ImGui::BulletText("Ctrl+Middle: pan   Shift+Middle: zoom");
  ImGui::BulletText("Alt+Middle: roll   Scroll: zoom");
  ImGui::BulletText("Arrows: rotate (Shift: 90\xc2\xb0)   Z: zoom");

  ImGui::End();
}

} // namespace ui
