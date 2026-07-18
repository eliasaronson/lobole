#include "ui/FeatureEditor.h"

#include "CADMaterials.h"
#include "Camera.h"
#include "core/BooleanFeature.h"
#include "core/DatumAxisFeature.h"
#include "core/DatumPlaneFeature.h"
#include "core/ExtrudeFeature.h"
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/FilletFeature.h"
#include "core/DraftFeature.h"
#include "core/HoleFeature.h"
#include "core/LoftFeature.h"
#include "core/SweepFeature.h"
#include "core/ShellFeature.h"
#include "core/PatternFeature.h"
#include "core/RevolveFeature.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"
#include "ui/AppActions.h" // BodyProducers

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

// The integer sibling of ExprDim: a literal InputInt with an "fx" button to
// switch to an expression (evaluated against the parameter table, rounded).
void ExprCount(AppState& state, Feature& feature, size_t index, bool commit,
               const char* label, std::string& exprText, int& value) {
  Document& doc = *state.document;
  ImGui::PushID(label);

  if (exprText.empty()) {
    HandleEdit(state, feature, ImGui::InputInt(label, &value), index, commit);
    if (value < 1)
      value = 1;
    ImGui::SameLine();
    if (ImGui::SmallButton("fx")) {
      nlohmann::json before = DocumentIO::FeatureToJson(feature);
      exprText = std::to_string(value);
      CommitDiscrete(state, feature, before, index, commit);
    }
    ImGui::PopID();
    return;
  }

  static std::string activeField;
  static char buf[128];
  const std::string key = std::to_string(feature.GetId()) + "/" + label;
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
    const long n = std::strtol(buf, &end, 10);
    while (end != nullptr && *end == ' ')
      end++;
    if (end != buf && end != nullptr && *end == '\0') {
      exprText.clear(); // a plain integer reverts to a literal
      value = std::max(1, static_cast<int>(n));
    } else {
      exprText = buf;
    }
    CommitDiscrete(state, feature, before, index, commit);
  }

  ExprError err;
  const auto v =
      ExpressionEngine::Default().EvaluateText(exprText, doc.Symbols(), &err);
  if (v.has_value())
    ImGui::TextDisabled("= %d", std::max(1, static_cast<int>(std::lround(*v))));
  else
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                       err.message.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("123")) {
    nlohmann::json before = DocumentIO::FeatureToJson(feature);
    exprText.clear();
    CommitDiscrete(state, feature, before, index, commit);
  }
  ImGui::PopID();
}

// A reusable axis picker: global X/Y/Z, every earlier DatumAxisFeature by
// name, and (when sketchIdForLines names a sketch) that sketch's construction
// lines by entity id. Commits a change to owner.axis like a discrete combo.
void DrawAxisRefCombo(AppState& state, Feature& owner, AxisRef& axis,
                      size_t index, bool commit, uint64_t sketchIdForLines) {
  std::vector<std::string> labels = {"X", "Y", "Z"};
  std::vector<AxisRef> refs = {{AxisRef::Kind::X}, {AxisRef::Kind::Y},
                               {AxisRef::Kind::Z}};

  for (size_t i = 0; i < index; i++) {
    if (auto* da =
            dynamic_cast<DatumAxisFeature*>(state.document->GetFeature(i))) {
      AxisRef r;
      r.kind = AxisRef::Kind::DatumAxis;
      r.datumAxisId = da->GetId();
      labels.push_back(da->GetName());
      refs.push_back(r);
    }
  }

  if (sketchIdForLines != 0) {
    if (auto* sk = dynamic_cast<SketchFeature*>(
            state.document->FindFeatureById(sketchIdForLines))) {
      for (const auto& e : sk->entities) {
        const auto* line = std::get_if<sketch::Line>(&e);
        if (line == nullptr || !line->construction)
          continue; // only construction lines make sensible axes
        AxisRef r;
        r.kind = AxisRef::Kind::SketchLine;
        r.sketchId = sketchIdForLines;
        r.entityId = line->id;
        labels.push_back("Line " + std::to_string(line->id));
        refs.push_back(r);
      }
    }
  }

  int sel = 0;
  for (size_t i = 0; i < refs.size(); i++) {
    const AxisRef& r = refs[i];
    if (r.kind != axis.kind)
      continue;
    if ((r.kind == AxisRef::Kind::DatumAxis &&
         r.datumAxisId != axis.datumAxisId) ||
        (r.kind == AxisRef::Kind::SketchLine && r.entityId != axis.entityId))
      continue;
    sel = static_cast<int>(i);
    break;
  }

  std::vector<const char*> cstrs;
  cstrs.reserve(labels.size());
  for (const auto& s : labels)
    cstrs.push_back(s.c_str());
  if (ImGui::Combo("Axis", &sel, cstrs.data(),
                   static_cast<int>(cstrs.size()))) {
    nlohmann::json before = DocumentIO::FeatureToJson(owner);
    axis = refs[static_cast<size_t>(sel)];
    CommitDiscrete(state, owner, before, index, commit);
  }
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
    DrawAxisRefCombo(state, dp, dp.axis, index, commit, /*sketch=*/0);
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

// Properties widgets for a datum axis: kind selector plus the active variant's
// fields (world points, or the referenced sketch line shown read-only).
void DrawDatumAxisParams(AppState& state, DatumAxisFeature& da, size_t index,
                         bool commit) {
  const char* kinds[] = {"Two Points", "Sketch Line", "Global X", "Global Y",
                         "Global Z"};
  int kind = static_cast<int>(da.kind);
  if (ImGui::Combo("Type", &kind, kinds, 5)) {
    nlohmann::json before = DocumentIO::FeatureToJson(da);
    da.kind = static_cast<DatumAxisKind>(kind);
    CommitDiscrete(state, da, before, index, commit);
  }

  switch (da.kind) {
  case DatumAxisKind::TwoPoints:
    HandleEdit(state, da, ImGui::DragFloat3("Point 1", &da.p0.x, 0.05f), index,
               commit);
    HandleEdit(state, da, ImGui::DragFloat3("Point 2", &da.p1.x, 0.05f), index,
               commit);
    break;
  case DatumAxisKind::SketchLine:
    if (Feature* sk = state.document->FindFeatureById(da.sketchId)) {
      ImGui::Text("Line %u of %s", da.entityId, sk->GetName().c_str());
    } else {
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Sketch line: missing");
    }
    break;
  case DatumAxisKind::GlobalX:
  case DatumAxisKind::GlobalY:
  case DatumAxisKind::GlobalZ:
    break; // fully defined by the kind
  }
}

// Properties widgets for a boolean: the operation plus two operand combos
// listing the body-producing features earlier in the history.
void DrawBooleanParams(AppState& state, BooleanFeature& boo, size_t index,
                       bool commit) {
  const char* ops[] = {"Union", "Subtract", "Intersect"};
  int op = static_cast<int>(boo.op);
  if (ImGui::Combo("Operation", &op, ops, 3)) {
    nlohmann::json before = DocumentIO::FeatureToJson(boo);
    boo.op = static_cast<BooleanOp>(op);
    CommitDiscrete(state, boo, before, index, commit);
  }

  const auto producers = BodyProducers(*state.document, index);
  const auto operandCombo = [&](const char* label, uint64_t& operandId) {
    std::vector<const char*> labels;
    std::vector<uint64_t> ids;
    int sel = -1;
    for (size_t i = 0; i < producers.size(); i++) {
      labels.push_back(producers[i].second.c_str());
      ids.push_back(producers[i].first);
      if (producers[i].first == operandId) {
        sel = static_cast<int>(i);
      }
    }
    if (ImGui::Combo(label, &sel, labels.data(),
                     static_cast<int>(labels.size())) &&
        sel >= 0) {
      nlohmann::json before = DocumentIO::FeatureToJson(boo);
      operandId = ids[static_cast<size_t>(sel)];
      CommitDiscrete(state, boo, before, index, commit);
    }
    if (operandId != 0 && sel < 0) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "(missing)");
    }
  };
  operandCombo("Target", boo.targetId);
  operandCombo("Tool", boo.toolId);
}

// A combo listing earlier body-producing features; sets `sourceId`.
void SourceBodyCombo(AppState& state, Feature& owner, uint64_t& sourceId,
                     size_t index, bool commit) {
  const auto producers = BodyProducers(*state.document, index);
  std::vector<const char*> labels;
  std::vector<uint64_t> ids;
  int sel = -1;
  for (size_t i = 0; i < producers.size(); i++) {
    labels.push_back(producers[i].second.c_str());
    ids.push_back(producers[i].first);
    if (producers[i].first == sourceId)
      sel = static_cast<int>(i);
  }
  if (ImGui::Combo("Source", &sel, labels.data(),
                   static_cast<int>(labels.size())) &&
      sel >= 0) {
    nlohmann::json before = DocumentIO::FeatureToJson(owner);
    sourceId = ids[static_cast<size_t>(sel)];
    CommitDiscrete(state, owner, before, index, commit);
  }
  if (sourceId != 0 && sel < 0) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "(missing)");
  }
}

// A plane picker (base planes + earlier datum planes); sets planeId/basePlane
// the same way DrawDatumPlaneParams' parent combo does.
void PlaneRefCombo(AppState& state, Feature& owner, uint64_t& planeId,
                   PlaneType& basePlane, size_t index, bool commit) {
  std::vector<const char*> labels = {"Front (XY)", "Top (XZ)", "Right (YZ)"};
  std::vector<uint64_t> ids = {0, 0, 0};
  std::vector<PlaneType> bases = {PlaneType::XY, PlaneType::XZ, PlaneType::YZ};
  for (size_t i = 0; i < index; i++) {
    if (auto* dp =
            dynamic_cast<DatumPlaneFeature*>(state.document->GetFeature(i))) {
      labels.push_back(dp->GetName().c_str());
      ids.push_back(dp->GetId());
      bases.push_back(PlaneType::XY);
    }
  }
  int sel = 0;
  if (planeId != 0) {
    for (size_t i = 3; i < ids.size(); i++)
      if (ids[i] == planeId) sel = static_cast<int>(i);
  } else {
    sel = static_cast<int>(basePlane);
  }
  if (ImGui::Combo("Plane", &sel, labels.data(),
                   static_cast<int>(labels.size()))) {
    nlohmann::json before = DocumentIO::FeatureToJson(owner);
    planeId = ids[static_cast<size_t>(sel)];
    basePlane = bases[static_cast<size_t>(sel)];
    CommitDiscrete(state, owner, before, index, commit);
  }
}

// Properties widgets for a mirror/pattern: kind, source body, and the kind's
// plane (mirror) or axis + count + spacing/angle (linear/circular).
void DrawPatternParams(AppState& state, PatternFeature& pat, size_t index,
                       bool commit) {
  const char* kinds[] = {"Mirror", "Linear", "Circular"};
  int kind = static_cast<int>(pat.kind);
  if (ImGui::Combo("Type", &kind, kinds, 3)) {
    nlohmann::json before = DocumentIO::FeatureToJson(pat);
    pat.kind = static_cast<PatternKind>(kind);
    CommitDiscrete(state, pat, before, index, commit);
  }

  SourceBodyCombo(state, pat, pat.sourceId, index, commit);

  switch (pat.kind) {
  case PatternKind::Mirror:
    PlaneRefCombo(state, pat, pat.planeId, pat.basePlane, index, commit);
    break;
  case PatternKind::Linear:
    DrawAxisRefCombo(state, pat, pat.axis, index, commit, /*sketch=*/0);
    ExprCount(state, pat, index, commit, "Count", pat.countExpr, pat.count);
    ExprDim(state, pat, index, commit, "Spacing", pat.spacingExpr, pat.spacing);
    break;
  case PatternKind::Circular:
    DrawAxisRefCombo(state, pat, pat.axis, index, commit, /*sketch=*/0);
    ExprCount(state, pat, index, commit, "Count", pat.countExpr, pat.count);
    ExprDim(state, pat, index, commit, "Angle (deg)", pat.angleExpr,
            pat.angleDeg);
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
      state.measure = MeasureState{}; // sketch mode replaces the measure tool
      if (state.camera)
        state.camera->AlignToFace(sk->plane.GetOrigin(), sk->plane.GetNormal());
    }
  } else if (auto* dp = dynamic_cast<DatumPlaneFeature*>(&feature)) {
    DrawDatumPlaneParams(state, *dp, index, commit);
  } else if (auto* da = dynamic_cast<DatumAxisFeature*>(&feature)) {
    DrawDatumAxisParams(state, *da, index, commit);
  } else if (auto* ext = dynamic_cast<ExtrudeFeature*>(&feature)) {
    const char* ends[] = {"Blind", "Symmetric", "Two-sided", "Up to face"};
    int end = static_cast<int>(ext->endMode);
    if (ImGui::Combo("End", &end, ends, 4)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*ext);
      ext->endMode = static_cast<ExtrudeEnd>(end);
      CommitDiscrete(state, feature, before, index, commit);
    }
    if (ext->endMode != ExtrudeEnd::UpToFace) {
      ExprDim(state, feature, index, commit, "Distance", ext->distanceExpr,
              ext->distance);
    } else {
      // Target face: every named face of every body (stable element names).
      const std::string current =
          ext->endTarget.IsSet() ? ext->endTarget.name : "(pick a face)";
      if (ImGui::BeginCombo("Target face", current.c_str())) {
        for (const auto& body : state.document->GetBodies()) {
          for (const auto& [idx, name] : body.elementMap.names) {
            const std::string label = body.name + ": " + name;
            const bool selected = ext->endTarget.featureId ==
                                      body.createdBy &&
                                  ext->endTarget.name == name;
            if (ImGui::Selectable(label.c_str(), selected)) {
              nlohmann::json before = DocumentIO::FeatureToJson(*ext);
              ext->endTarget = ElementRef{body.createdBy, name};
              CommitDiscrete(state, feature, before, index, commit);
            }
          }
        }
        ImGui::EndCombo();
      }
    }
    if (ext->endMode == ExtrudeEnd::TwoSided) {
      ExprDim(state, feature, index, commit, "Distance 2", ext->distance2Expr,
              ext->distance2);
    }
    if (ext->endMode == ExtrudeEnd::Blind ||
        ext->endMode == ExtrudeEnd::TwoSided) // no effect otherwise
      HandleEdit(state, feature, ImGui::Checkbox("Reversed", &ext->reversed),
                 index, commit);

    HandleEdit(state, feature, ImGui::Checkbox("Thin", &ext->thin), index,
               commit);
    if (ext->thin) {
      ExprDim(state, feature, index, commit, "Thickness",
              ext->thinThicknessExpr, ext->thinThickness);
      const char* sides[] = {"Outward", "Inward", "Both"};
      int side = static_cast<int>(ext->thinSide);
      if (ImGui::Combo("Wall side", &side, sides, 3)) {
        nlohmann::json before = DocumentIO::FeatureToJson(*ext);
        ext->thinSide = static_cast<sketch::ThinSide>(side);
        CommitDiscrete(state, feature, before, index, commit);
      }
    }

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
  } else if (auto* rev = dynamic_cast<RevolveFeature*>(&feature)) {
    ExprDim(state, feature, index, commit, "Angle (deg)", rev->angleExpr,
            rev->angleDeg);
    DrawAxisRefCombo(state, feature, rev->axis, index, commit, rev->sketchId);
    HandleEdit(state, feature, ImGui::Checkbox("Symmetric", &rev->symmetric),
               index, commit);
    if (!rev->symmetric) // reversed has no effect on a symmetric sweep
      HandleEdit(state, feature, ImGui::Checkbox("Reversed", &rev->reversed),
                 index, commit);

    const char* modes[] = {"New Body", "Add", "Cut"};
    int mode = static_cast<int>(rev->mode);
    if (ImGui::Combo("Mode", &mode, modes, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*rev);
      rev->mode = static_cast<ExtrudeMode>(mode);
      CommitDiscrete(state, feature, before, index, commit);
    }

    if (Feature* src = state.document->FindFeatureById(rev->sketchId)) {
      ImGui::Text("Sketch: %s", src->GetName().c_str());
    } else {
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Sketch: missing");
    }
  } else if (auto* boo = dynamic_cast<BooleanFeature*>(&feature)) {
    DrawBooleanParams(state, *boo, index, commit);
  } else if (auto* pat = dynamic_cast<PatternFeature*>(&feature)) {
    DrawPatternParams(state, *pat, index, commit);
  } else if (auto* dr = dynamic_cast<DraftFeature*>(&feature)) {
    ExprDim(state, feature, index, commit, "Angle (deg)", dr->angleExpr,
            dr->angleDeg);
    const char* planes[] = {"XY (pull +Z)", "XZ (pull +Y)", "YZ (pull +X)"};
    int pl = static_cast<int>(dr->neutralPlane);
    if (ImGui::Combo("Neutral", &pl, planes, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*dr);
      dr->neutralPlane = static_cast<PlaneType>(pl);
      CommitDiscrete(state, feature, before, index, commit);
    }
    ImGui::Text("Faces: %zu", dr->faces.size());
    for (size_t e = 0; e < dr->faces.size(); e++) {
      ImGui::PushID(static_cast<int>(e));
      if (ImGui::SmallButton("x")) {
        nlohmann::json before = DocumentIO::FeatureToJson(*dr);
        dr->faces.erase(dr->faces.begin() + static_cast<long>(e));
        CommitDiscrete(state, feature, before, index, commit);
        ImGui::PopID();
        break;
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(dr->faces[e].name.c_str());
      ImGui::PopID();
    }
  } else if (auto* ho = dynamic_cast<HoleFeature*>(&feature)) {
    const char* kinds[] = {"Simple", "Counterbore", "Countersink"};
    int k = static_cast<int>(ho->kind);
    if (ImGui::Combo("Type", &k, kinds, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*ho);
      ho->kind = static_cast<HoleKind>(k);
      CommitDiscrete(state, feature, before, index, commit);
    }
    ExprDim(state, feature, index, commit, "Diameter", ho->diameterExpr,
            ho->diameter);
    HandleEdit(state, feature, ImGui::Checkbox("Through all", &ho->through),
               index, commit);
    if (!ho->through)
      ExprDim(state, feature, index, commit, "Depth", ho->depthExpr,
              ho->depth);
    if (ho->kind == HoleKind::Counterbore) {
      HandleEdit(state, feature, DragDim("C'bore dia", &ho->cbDiameter),
                 index, commit);
      HandleEdit(state, feature, DragDim("C'bore depth", &ho->cbDepth), index,
                 commit);
    } else if (ho->kind == HoleKind::Countersink) {
      HandleEdit(state, feature, DragDim("C'sink dia", &ho->csDiameter),
                 index, commit);
      HandleEdit(state, feature, DragDim("C'sink depth", &ho->csDepth), index,
                 commit);
    }
    if (Feature* src = state.document->FindFeatureById(ho->sketchId))
      ImGui::Text("Points: %s", src->GetName().c_str());
    else
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Points: missing");
  } else if (auto* sw = dynamic_cast<SweepFeature*>(&feature)) {
    Feature* pf = state.document->FindFeatureById(sw->profileSketchId);
    Feature* pa = state.document->FindFeatureById(sw->pathSketchId);
    ImGui::Text("Profile: %s", pf ? pf->GetName().c_str() : "(missing)");
    ImGui::Text("Path: %s", pa ? pa->GetName().c_str() : "(missing)");
  } else if (auto* lo = dynamic_cast<LoftFeature*>(&feature)) {
    ImGui::Text("Sections: %zu", lo->sketchIds.size());
    for (uint64_t sid : lo->sketchIds) {
      Feature* src = state.document->FindFeatureById(sid);
      ImGui::BulletText("%s", src ? src->GetName().c_str() : "(missing)");
    }
    ImGui::TextDisabled("Add sections via add_feature / the RPC");
  } else if (auto* sh = dynamic_cast<ShellFeature*>(&feature)) {
    ExprDim(state, feature, index, commit, "Thickness", sh->thicknessExpr,
            sh->thickness);
    if (Feature* src = state.document->FindFeatureById(sh->bodyId))
      ImGui::Text("Body: %s", src->GetName().c_str());
    else
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Body: missing");
    ImGui::Text("Open faces: %zu", sh->openFaces.size());
    for (size_t e = 0; e < sh->openFaces.size(); e++) {
      ImGui::PushID(static_cast<int>(e));
      if (ImGui::SmallButton("x")) {
        nlohmann::json before = DocumentIO::FeatureToJson(*sh);
        sh->openFaces.erase(sh->openFaces.begin() + static_cast<long>(e));
        CommitDiscrete(state, feature, before, index, commit);
        ImGui::PopID();
        break;
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(sh->openFaces[e].name.c_str());
      ImGui::PopID();
    }
  } else if (auto* fil = dynamic_cast<FilletFeature*>(&feature)) {
    const char* ops[] = {"Fillet", "Chamfer"};
    int op = static_cast<int>(fil->op);
    if (ImGui::Combo("Operation", &op, ops, 2)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*fil);
      fil->op = static_cast<EdgeBlendOp>(op);
      CommitDiscrete(state, feature, before, index, commit);
    }
    ExprDim(state, feature, index, commit,
            fil->op == EdgeBlendOp::Chamfer ? "Distance" : "Radius",
            fil->radiusExpr, fil->radius);

    // The edge list, each entry removable — the repair path for a dangling
    // ref (its edge is gone, so it can't be un-clicked in the viewport).
    ImGui::Text("Edges: %zu", fil->edges.size());
    for (size_t e = 0; e < fil->edges.size(); e++) {
      ImGui::PushID(static_cast<int>(e));
      if (ImGui::SmallButton("x")) {
        nlohmann::json before = DocumentIO::FeatureToJson(*fil);
        fil->edges.erase(fil->edges.begin() + static_cast<long>(e));
        CommitDiscrete(state, feature, before, index, commit);
        ImGui::PopID();
        break; // list changed; redraw next frame
      }
      ImGui::SameLine();
      ImGui::TextDisabled("%s", fil->edges[e].name.c_str());
      ImGui::PopID();
    }
    if (state.edgePick.filletId == fil->GetId()) {
      ImGui::TextColored(ImVec4(0.35f, 0.80f, 1.00f, 1.0f),
                         "Click edges in the viewport to add/remove");
      if (ImGui::Button("Done picking")) {
        state.edgePick = {};
      }
    } else if (ImGui::Button("Pick edges…")) {
      // Also the repair action for a dangling ref: re-pick, click the new edge.
      state.edgePick = {};
      state.edgePick.filletId = fil->GetId();
    }
  }
}

} // namespace ui
