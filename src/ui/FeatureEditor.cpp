#include "ui/FeatureEditor.h"

#include "Camera.h"
#include "core/ExtrudeFeature.h"
#include "core/Feature.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"

#include "imgui.h"

#include <cstdio>

namespace ui {

namespace {

// Live-drag lifecycle: snapshot on activate, regenerate while changing, and
// (when committing) push one ModifyFeatureCommand spanning the whole drag.
void HandleEdit(AppState& state, Feature& feature, bool changedThisFrame,
                size_t index, bool commit) {
  Document& doc = *state.document;

  if (ImGui::IsItemActivated()) {
    state.editBaseline = DocumentIO::FeatureToJson(feature);
  }
  if (changedThisFrame) {
    doc.Regenerate();
  }
  if (!commit)
    return; // preview: mutate + regenerate only, no undo entry

  if (ImGui::IsItemDeactivatedAfterEdit()) {
    nlohmann::json after = DocumentIO::FeatureToJson(feature);
    if (after != state.editBaseline) {
      state.commands->Execute(
          std::make_unique<ModifyFeatureCommand>(index, state.editBaseline,
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

// Snapshot/mutate/commit for a discrete (non-drag) change like a combo.
void CommitDiscrete(AppState& state, Feature& feature,
                    const nlohmann::json& before, size_t index, bool commit) {
  state.document->Regenerate();
  if (!commit)
    return;
  nlohmann::json after = DocumentIO::FeatureToJson(feature);
  if (after != before) {
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after),
        *state.document);
  }
}

} // namespace

void DrawFeatureParamWidgets(AppState& state, Feature& feature, size_t index,
                             bool commit) {
  // Name
  static char nameBuf[128];
  static uint64_t nameBufId = 0;
  if (nameBufId != feature.GetId() || !ImGui::IsAnyItemActive()) {
    snprintf(nameBuf, sizeof(nameBuf), "%s", feature.GetName().c_str());
    nameBufId = feature.GetId();
  }
  ImGui::InputText("Name", nameBuf, sizeof(nameBuf));
  if (ImGui::IsItemActivated()) {
    state.editBaseline = DocumentIO::FeatureToJson(feature);
  }
  if (ImGui::IsItemDeactivatedAfterEdit() && nameBuf[0] != '\0' &&
      feature.GetName() != nameBuf) {
    nlohmann::json before = state.editBaseline;
    feature.SetName(nameBuf);
    CommitDiscrete(state, feature, before, index, commit);
  }

  if (auto* box = dynamic_cast<BoxFeature*>(&feature)) {
    HandleEdit(state, feature,
               ImGui::DragFloat3("Position", &box->position.x, 0.05f), index,
               commit);
    HandleEdit(state, feature, DragDim("Width", &box->width), index, commit);
    HandleEdit(state, feature, DragDim("Height", &box->height), index, commit);
    HandleEdit(state, feature, DragDim("Depth", &box->depth), index, commit);
  } else if (auto* sphere = dynamic_cast<SphereFeature*>(&feature)) {
    HandleEdit(state, feature,
               ImGui::DragFloat3("Center", &sphere->center.x, 0.05f), index,
               commit);
    HandleEdit(state, feature, DragDim("Radius", &sphere->radius), index,
               commit);
  } else if (auto* cyl = dynamic_cast<CylinderFeature*>(&feature)) {
    HandleEdit(state, feature,
               ImGui::DragFloat3("Center", &cyl->center.x, 0.05f), index,
               commit);
    HandleEdit(state, feature,
               ImGui::DragFloat3("Direction", &cyl->direction.x, 0.05f), index,
               commit);
    HandleEdit(state, feature, DragDim("Radius", &cyl->radius), index, commit);
    HandleEdit(state, feature, DragDim("Height", &cyl->height), index, commit);
  } else if (auto* import = dynamic_cast<ImportFeature*>(&feature)) {
    ImGui::TextDisabled("File: %s", import->path.c_str());
  } else if (auto* sk = dynamic_cast<SketchFeature*>(&feature)) {
    glm::vec3 n = sk->plane.GetNormal();
    ImGui::Text("Plane normal: (%.2f, %.2f, %.2f)", n.x, n.y, n.z);
    ImGui::Text("Entities: %zu", sk->entities.size());
    if (ImGui::Button("Edit Sketch")) {
      state.sketchEdit = SketchEditState{};
      state.sketchEdit.activeSketchId = sk->GetId();
      if (state.camera)
        state.camera->AlignToFace(sk->plane.GetOrigin(), sk->plane.GetNormal());
    }
  } else if (auto* ext = dynamic_cast<ExtrudeFeature*>(&feature)) {
    HandleEdit(state, feature, DragDim("Distance", &ext->distance), index,
               commit);
    HandleEdit(state, feature, ImGui::Checkbox("Reversed", &ext->reversed),
               index, commit);

    const char* modes[] = {"New Body", "Add", "Cut"};
    int mode = static_cast<int>(ext->mode);
    if (ImGui::Combo("Mode", &mode, modes, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*ext);
      ext->mode = static_cast<ExtrudeMode>(mode);
      CommitDiscrete(state, feature, before, index, commit);
    }

    if (Feature* src = state.document->FindFeatureById(ext->sketchId)) {
      ImGui::Text("Sketch: %s", src->GetName().c_str());
    } else {
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Sketch: missing");
    }
  }
}

} // namespace ui
