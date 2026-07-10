#include "ui/FeatureEditor.h"

#include "CADMaterials.h"
#include "Camera.h"
#include "core/DatumPlaneFeature.h"
#include "core/ExtrudeFeature.h"
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"

#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

// A dimension that can be driven by an expression over the document's
// variables (variables plan V2). Literal fields keep the usual drag plus an
// "fx" button that switches to expression mode; expression fields show an
// editable text input with the evaluated value (or error) underneath and a
// "123" button back to a literal. Committing a plain number also reverts to
// a literal — expressions stay opt-in per field.
void ExprDim(AppState& state, Feature& feature, size_t index, bool commit,
             const char* label, std::string& exprText, double& value) {
  Document& doc = *state.document;
  ImGui::PushID(label);

  if (exprText.empty()) {
    HandleEdit(state, feature, DragDim(label, &value), index, commit);
    ImGui::SameLine();
    if (ImGui::SmallButton("fx")) {
      nlohmann::json before = DocumentIO::FeatureToJson(feature);
      char buf[64];
      snprintf(buf, sizeof(buf), "%g", value);
      exprText = buf;
      CommitDiscrete(state, feature, before, index, commit);
    }
    ImGui::PopID();
    return;
  }

  // One shared buffer; claimed by this field on activation (only one ImGui
  // item is ever active) — same recipe as the Variables panel cells.
  static std::string activeField;
  static char buf[256];
  const std::string key =
      std::to_string(feature.GetId()) + "/" + label;
  const bool editing = activeField == key;
  char tmp[sizeof(buf)];
  snprintf(tmp, sizeof(tmp), "%s", exprText.c_str());
  ImGui::InputText(label, editing ? buf : tmp, sizeof(buf));
  if (ImGui::IsItemActivated()) {
    activeField = key;
    snprintf(buf, sizeof(buf), "%s", exprText.c_str());
  }
  if (editing && ImGui::IsItemDeactivatedAfterEdit()) {
    activeField.clear();
    nlohmann::json before = DocumentIO::FeatureToJson(feature);
    char* end = nullptr;
    const double number = std::strtod(buf, &end);
    while (end != nullptr && *end == ' ') {
      end++;
    }
    if (end != buf && end != nullptr && *end == '\0') {
      exprText.clear(); // a plain number reverts the field to a literal
      value = number;
    } else {
      exprText = buf; // empty or symbolic: regen (re)evaluates it
    }
    CommitDiscrete(state, feature, before, index, commit);
  }

  if (!exprText.empty()) {
    ExprError err;
    const auto v =
        ExpressionEngine::Default().EvaluateText(exprText, doc.Symbols(), &err);
    if (v.has_value()) {
      ImGui::TextDisabled("= %.4g", *v);
    } else {
      ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                         err.message.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("123")) {
      nlohmann::json before = DocumentIO::FeatureToJson(feature);
      exprText.clear(); // back to a literal holding the last good value
      CommitDiscrete(state, feature, before, index, commit);
    }
  }
  ImGui::PopID();
}

// Properties widgets for a datum plane: variant selector, parent (Offset/
// Angled), and the variant's expression-driven fields. Parent/kind/axis are
// discrete combos committed like the extrude Mode combo.
void DrawDatumPlaneParams(AppState& state, DatumPlaneFeature& dp, size_t index,
                          bool commit) {
  const auto discrete = [&](const nlohmann::json& before) {
    CommitDiscrete(state, dp, before, index, commit);
  };

  // Variant.
  const char* kinds[] = {"Offset", "Angled", "Through 3 Points"};
  int kind = static_cast<int>(dp.kind);
  if (ImGui::Combo("Type", &kind, kinds, 3)) {
    nlohmann::json before = DocumentIO::FeatureToJson(dp);
    dp.kind = static_cast<DatumPlaneKind>(kind);
    discrete(before);
  }

  // Parent plane (Offset/Angled only): the three base planes, then every
  // datum earlier in the history.
  if (dp.kind != DatumPlaneKind::ThroughPoints) {
    std::vector<const char*> labels = {"Front (XY)", "Top (XZ)", "Right (YZ)"};
    std::vector<uint64_t> ids = {0, 0, 0}; // base planes carry id 0
    std::vector<PlaneType> bases = {PlaneType::XY, PlaneType::XZ,
                                    PlaneType::YZ};
    for (size_t i = 0; i < index; i++) {
      if (auto* other =
              dynamic_cast<DatumPlaneFeature*>(state.document->GetFeature(i))) {
        labels.push_back(other->GetName().c_str());
        ids.push_back(other->GetId());
        bases.push_back(PlaneType::XY);
      }
    }
    int sel = 0;
    if (dp.parentId != 0) {
      for (size_t i = 3; i < ids.size(); i++)
        if (ids[i] == dp.parentId) sel = static_cast<int>(i);
    } else {
      sel = static_cast<int>(dp.basePlane); // 0/1/2
    }
    if (ImGui::Combo("Parent", &sel, labels.data(),
                     static_cast<int>(labels.size()))) {
      nlohmann::json before = DocumentIO::FeatureToJson(dp);
      dp.parentId = ids[static_cast<size_t>(sel)];
      dp.basePlane = bases[static_cast<size_t>(sel)];
      discrete(before);
    }
  }

  switch (dp.kind) {
  case DatumPlaneKind::Offset:
    ExprDim(state, dp, index, commit, "Offset", dp.offsetExpr, dp.offset);
    HandleEdit(state, dp, ImGui::Checkbox("Flip", &dp.flip), index, commit);
    break;
  case DatumPlaneKind::Angled: {
    const char* axes[] = {"X", "Y", "Z"};
    // The M1 UI offers the global axes; sketch-line axes are settable via RPC
    // and get their pick UI with M2 revolve.
    int a = dp.axis.kind == AxisRef::Kind::SketchLine
                ? -1
                : static_cast<int>(dp.axis.kind);
    if (dp.axis.kind == AxisRef::Kind::SketchLine) {
      ImGui::Text("Axis: sketch line");
    } else if (ImGui::Combo("Axis", &a, axes, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(dp);
      dp.axis.kind = static_cast<AxisRef::Kind>(a);
      discrete(before);
    }
    ExprDim(state, dp, index, commit, "Angle (deg)", dp.angleExpr, dp.angleDeg);
    break;
  }
  case DatumPlaneKind::ThroughPoints:
    HandleEdit(state, dp, ImGui::DragFloat3("Point 1", &dp.p0.x, 0.05f), index,
               commit);
    HandleEdit(state, dp, ImGui::DragFloat3("Point 2", &dp.p1.x, 0.05f), index,
               commit);
    HandleEdit(state, dp, ImGui::DragFloat3("Point 3", &dp.p2.x, 0.05f), index,
               commit);
    break;
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

  // Material — applies to any body this feature creates (index 0 = Default,
  // i.e. the empty slug; the rest mirror MaterialLibrary::AllTypes()). Datum
  // planes make no body, so the control is meaningless for them.
  if (dynamic_cast<DatumPlaneFeature*>(&feature) == nullptr) {
    using CADMaterials::MaterialLibrary;
    const auto& types = MaterialLibrary::AllTypes();
    int sel = 0;
    for (size_t i = 0; i < types.size(); ++i) {
      if (MaterialLibrary::Slug(types[i]) == feature.GetMaterial()) {
        sel = static_cast<int>(i) + 1;
        break;
      }
    }
    std::vector<const char*> labels;
    labels.reserve(types.size() + 1);
    labels.push_back("Default");
    for (auto t : types)
      labels.push_back(MaterialLibrary::GetMaterial(t).name.c_str());
    if (ImGui::Combo("Material", &sel, labels.data(),
                     static_cast<int>(labels.size()))) {
      nlohmann::json before = DocumentIO::FeatureToJson(feature);
      feature.SetMaterial(sel == 0 ? std::string{}
                                   : MaterialLibrary::Slug(types[sel - 1]));
      CommitDiscrete(state, feature, before, index, commit);
    }
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
    if (sk->planeRef != 0) {
      if (Feature* datum = state.document->FindFeatureById(sk->planeRef)) {
        ImGui::Text("Plane: %s", datum->GetName().c_str());
      } else {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Plane: missing");
      }
    }
    glm::vec3 n = sk->plane.GetNormal();
    ImGui::Text("Plane normal: (%.2f, %.2f, %.2f)", n.x, n.y, n.z);
    ImGui::Text("Entities: %zu", sk->entities.size());
    if (ImGui::Button("Edit Sketch")) {
      state.sketchEdit = SketchEditState{};
      state.sketchEdit.activeSketchId = sk->GetId();
      if (state.camera)
        state.camera->AlignToFace(sk->plane.GetOrigin(), sk->plane.GetNormal());
    }
  } else if (auto* dp = dynamic_cast<DatumPlaneFeature*>(&feature)) {
    DrawDatumPlaneParams(state, *dp, index, commit);
  } else if (auto* ext = dynamic_cast<ExtrudeFeature*>(&feature)) {
    ExprDim(state, feature, index, commit, "Distance", ext->distanceExpr,
            ext->distance);
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
