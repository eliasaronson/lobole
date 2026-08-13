#include "ui/FeatureEditor.h"
#include <utility>

#include "cad/CADMaterials.h"
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/ThreadStandards.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/RibFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/features/SweepFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "ui/AppActions.h" // BodyProducers

#include "imgui.h"
#include "imgui_internal.h" // GetCurrentWindowRead (LeftLabel auto-resize case)

#include "ui/ExprInput.h"         // expression-capable numeric fields
#include "ui/SelectionPipeline.h" // ParseRegionIds (U2 region slots)

#include <algorithm>
#include <cfloat>
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
      // Applied live during the drag: record only (see CommitDiscrete).
      state.commands->PushApplied(std::make_unique<ModifyFeatureCommand>(
          index, state.editBaseline, after));
    }
  } else if (ImGui::IsItemDeactivated()) {
    doc.Regenerate();
  }
}

// Labels-left rows (U4): the label in a left column, the widget filling the
// rest of the row. ImGui's default label-right layout clips labels as soon as
// the panel narrows; this keeps them readable at any width. `reserveRight`
// keeps room for a trailing SmallButton ("fx"/"123") on the same row.
void LeftLabel(const char* label, float reserveRight = 0.0f) {
  // Auto-resize windows (the preview dialog): avail-based widths are circular
  // there (the window sizes to content), so use a fixed two-column layout —
  // this also keeps the dialog one row per field, not towering over the
  // viewport clicks the GUI tests make.
  if (ImGui::GetCurrentWindowRead()->Flags &
      ImGuiWindowFlags_AlwaysAutoResize) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(std::max(ImGui::GetFontSize() * 7.0f,
                             ImGui::CalcTextSize(label).x +
                                 ImGui::GetStyle().ItemSpacing.x * 2.0f));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f - reserveRight);
    return;
  }
  const float avail = ImGui::GetContentRegionAvail().x;
  const float col = std::max(ImGui::GetFontSize() * 7.0f, avail * 0.40f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  // Two columns only when the field keeps a usable width; in a narrow panel
  // stack the label above the full-width field instead of squeezing it.
  if (avail - col >= ImGui::GetFontSize() * 8.0f)
    ImGui::SameLine(col);
  ImGui::SetNextItemWidth(reserveRight > 0.0f ? -reserveRight : -FLT_MIN);
}

// Width a SmallButton takes on the row, for LeftLabel's reserveRight.
float SmallButtonReserve(const char* text) {
  const ImGuiStyle& style = ImGui::GetStyle();
  return ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f +
         style.ItemSpacing.x;
}

bool DragDim(AppState& state, const char* label, double* value,
             float reserveRight = 0.0f) {
  LeftLabel(label, reserveRight);
  const std::string id = std::string("##") + label;
  // Expression-capable (Ctrl+click, type "width/2"): the shared widget.
  return ExprDragDouble(state, id.c_str(), value, 0.05f, 0.01, 1e6, "%.3f");
}

// Labels-left drop-in for ImGui::Combo.
bool LabeledCombo(const char* label, int* sel, const char* const items[],
                  int count) {
  LeftLabel(label);
  const std::string id = std::string("##") + label;
  return ImGui::Combo(id.c_str(), sel, items, count);
}

// Labels-left drop-in for the xyz drag rows.
bool DragFloat3Row(const char* label, float* v) {
  LeftLabel(label);
  const std::string id = std::string("##") + label;
  return ImGui::DragFloat3(id.c_str(), v, 0.05f);
}

void CommitDiscrete(AppState& state, Feature& feature,
                    const nlohmann::json& before, size_t index, bool commit);

// A dialog pick slot (U3/U7): label left, the current value as a full-width
// button. Clicking arms the slot — viewport picks then fill it (the armed
// slot renders in the accent color); clicking again disarms. Kinds, label
// and prompt come from the feature's SlotSpecsFor table.
void PickSlotRow(AppState& state, const Feature& feature, PickSlot slot,
                 const std::string& value) {
  const std::vector<SlotSpec> specs = SlotSpecsFor(feature);
  const auto spec =
      std::find_if(specs.begin(), specs.end(),
                   [&](const SlotSpec& s) { return s.slot == slot; });
  if (spec == specs.end())
    return; // the feature's current state doesn't expose this slot
  const bool armed = state.slotPick.featureId == feature.GetId() &&
                     state.slotPick.slot == static_cast<int>(slot);
  LeftLabel(spec->label);
  if (armed) {
    const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImVec4(accent.x, accent.y, accent.z, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(accent.x, accent.y, accent.z, 0.55f));
  }
  // "###" keeps the widget id stable while the shown value changes (tests
  // reference slots as "###slotN").
  const std::string text =
      (value.empty() ? std::string(armed ? "(pick in viewport)" : "(none)")
                     : value) +
      "###slot" + std::to_string(static_cast<int>(slot));
  if (ImGui::Button(text.c_str(), ImVec2(ImGui::CalcItemWidth(), 0.0f))) {
    if (armed)
      state.slotPick = {};
    else
      ArmSlot(state, feature, slot);
  }
  if (armed)
    ImGui::PopStyleColor(2);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", armed ? "Armed - pick in the viewport (Esc "
                                    "cancels); click to disarm"
                                  : "Click, then pick in the viewport");
}

// The picked-element rows under a multi slot: each ref removable with "x" —
// also the repair path for a dangling ref (its element is gone, so it can't
// be un-clicked in the viewport). One shared widget (U7) — was copy-pasted
// per feature.
void RefListRows(AppState& state, Feature& feature, size_t index, bool commit,
                 std::vector<ElementRef>& refs) {
  for (size_t e = 0; e < refs.size(); e++) {
    ImGui::PushID(static_cast<int>(e));
    if (ImGui::SmallButton("x")) {
      nlohmann::json before = DocumentIO::FeatureToJson(feature);
      refs.erase(refs.begin() + static_cast<long>(e));
      CommitDiscrete(state, feature, before, index, commit);
      ImGui::PopID();
      break; // list changed; redraw next frame
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", refs[e].name.c_str());
    ImGui::PopID();
  }
}

constexpr uint32_t SlotKind(SelectionItem::Kind k) {
  return 1u << static_cast<int>(k);
}

// The display label for a body reference (its producing feature's name).
std::string BodyRefLabel(AppState& state, uint64_t featureId) {
  if (featureId == 0 || !state.document)
    return "";
  if (Feature* f = state.document->FindFeatureById(featureId))
    return f->GetName();
  return "(missing)";
}

// Snapshot/mutate/commit for a discrete (non-drag) change like a combo.
void CommitDiscrete(AppState& state, Feature& feature,
                    const nlohmann::json& before, size_t index, bool commit) {
  state.document->Regenerate();
  if (!commit)
    return;
  nlohmann::json after = DocumentIO::FeatureToJson(feature);
  if (after != before) {
    // The live feature already holds the after-state: record without
    // executing, so the object (and every pointer the widgets still hold
    // this frame) survives. Execute would ReplaceFeature with an identical
    // copy — the dangling-pointer class behind the material-change segfault.
    state.commands->PushApplied(
        std::make_unique<ModifyFeatureCommand>(index, before, after));
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
    HandleEdit(state, feature,
               DragDim(state, label, &value, SmallButtonReserve("fx")), index,
               commit);
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
  const std::string key = std::to_string(feature.GetId()) + "/" + label;
  const bool editing = activeField == key;
  char tmp[sizeof(buf)];
  snprintf(tmp, sizeof(tmp), "%s", exprText.c_str());
  LeftLabel(label);
  ImGui::InputText("##expr", editing ? buf : tmp, sizeof(buf));
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
    const auto v =
        ExpressionEngine::Default().EvaluateText(exprText, doc.Symbols());
    if (v.has_value()) {
      ImGui::TextDisabled("= %.4g", *v);
    } else {
      ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                         v.error().message.c_str());
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
    LeftLabel(label, SmallButtonReserve("fx"));
    HandleEdit(state, feature, ImGui::InputInt("##count", &value), index,
               commit);
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
  LeftLabel(label);
  ImGui::InputText("##expr", editing ? buf : tmp, sizeof(buf));
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

  const auto v =
      ExpressionEngine::Default().EvaluateText(exprText, doc.Symbols());
  if (v.has_value())
    ImGui::TextDisabled("= %d", std::max(1, static_cast<int>(std::lround(*v))));
  else
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                       v.error().message.c_str());
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
  std::vector<AxisRef> refs = {
      {AxisRef::Kind::X}, {AxisRef::Kind::Y}, {AxisRef::Kind::Z}};

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
  if (LabeledCombo("Axis", &sel, cstrs.data(),
                   static_cast<int>(cstrs.size()))) {
    nlohmann::json before = DocumentIO::FeatureToJson(owner);
    axis = refs[static_cast<size_t>(sel)];
    CommitDiscrete(state, owner, before, index, commit);
  }
}

bool AcceptSketch(const Feature* f) {
  return dynamic_cast<const SketchFeature*>(f) != nullptr;
}
// A sketch (open chain) or a Curve3D — anything usable as a sweep/loft path or
// guide (mirrors SweepFeature's ResolveSweepPathWire).
bool AcceptPathLike(const Feature* f) {
  return dynamic_cast<const SketchFeature*>(f) != nullptr ||
         dynamic_cast<const CurveFeature*>(f) != nullptr;
}

// A combo over earlier features (index-filtered, i.e. only what this feature is
// allowed to reference) whose type passes `accept`, optionally led by a
// "(none)" = 0 entry. Reads/writes *target and commits like a discrete combo.
// The curve/sketch reference picker — the DrawAxisRefCombo of Curve3D-driven
// features.
void DrawFeatureRefCombo(AppState& state, Feature& owner, const char* label,
                         uint64_t* target, size_t index, bool commit,
                         bool allowNone, bool (*accept)(const Feature*)) {
  std::vector<std::string> labels;
  std::vector<uint64_t> ids;
  if (allowNone) {
    labels.emplace_back("(none)");
    ids.push_back(0);
  }
  for (size_t i = 0; i < index; i++) {
    Feature* f = state.document->GetFeature(i);
    if (accept(f)) {
      labels.push_back(f->GetName());
      ids.push_back(f->GetId());
    }
  }
  if (ids.empty()) {
    LeftLabel(label);
    ImGui::TextDisabled("(no candidates)");
    return;
  }
  int sel = 0;
  for (size_t i = 0; i < ids.size(); i++)
    if (ids[i] == *target) {
      sel = static_cast<int>(i);
      break;
    }
  std::vector<const char*> cstrs;
  cstrs.reserve(labels.size());
  for (const auto& s : labels)
    cstrs.push_back(s.c_str());
  if (LabeledCombo(label, &sel, cstrs.data(), static_cast<int>(cstrs.size()))) {
    nlohmann::json before = DocumentIO::FeatureToJson(owner);
    *target = ids[static_cast<size_t>(sel)];
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
  int kind = std::to_underlying(dp.kind);
  if (LabeledCombo("Type", &kind, kinds, 3)) {
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
        if (ids[i] == dp.parentId)
          sel = static_cast<int>(i);
    } else {
      sel = static_cast<int>(dp.basePlane); // 0/1/2
    }
    if (LabeledCombo("Parent", &sel, labels.data(),
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
    HandleEdit(state, dp, DragFloat3Row("Point 1", &dp.p0.x), index, commit);
    HandleEdit(state, dp, DragFloat3Row("Point 2", &dp.p1.x), index, commit);
    HandleEdit(state, dp, DragFloat3Row("Point 3", &dp.p2.x), index, commit);
    break;
  }
}

// Properties widgets for a datum axis: kind selector plus the active variant's
// fields (world points, or the referenced sketch line shown read-only).
void DrawDatumAxisParams(AppState& state, DatumAxisFeature& da, size_t index,
                         bool commit) {
  const char* kinds[] = {"Two Points", "Sketch Line", "Global X", "Global Y",
                         "Global Z"};
  int kind = std::to_underlying(da.kind);
  if (LabeledCombo("Type", &kind, kinds, 5)) {
    nlohmann::json before = DocumentIO::FeatureToJson(da);
    da.kind = static_cast<DatumAxisKind>(kind);
    CommitDiscrete(state, da, before, index, commit);
  }

  switch (da.kind) {
  case DatumAxisKind::TwoPoints:
    HandleEdit(state, da, DragFloat3Row("Point 1", &da.p0.x), index, commit);
    HandleEdit(state, da, DragFloat3Row("Point 2", &da.p1.x), index, commit);
    break;
  case DatumAxisKind::SketchLine:
    if (Feature* sk = state.document->FindFeatureById(da.sketchId)) {
      ImGui::Text("Line %u of %s", da.entityId, sk->GetName().c_str());
    } else {
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f),
                         "Sketch line: missing");
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
  int op = std::to_underlying(boo.op);
  if (LabeledCombo("Operation", &op, ops, 3)) {
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
    if (LabeledCombo(label, &sel, labels.data(),
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
  // Pick slots first (click a body in the viewport); the combos below stay
  // as the precision path.
  PickSlotRow(state, boo, PickSlot::BooleanTarget,
              BodyRefLabel(state, boo.targetId));
  PickSlotRow(state, boo, PickSlot::BooleanTool,
              BodyRefLabel(state, boo.toolId));
  operandCombo("Target (list)", boo.targetId);
  operandCombo("Tool (list)", boo.toolId);
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
  if (LabeledCombo("Source", &sel, labels.data(),
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
      if (ids[i] == planeId)
        sel = static_cast<int>(i);
  } else {
    sel = static_cast<int>(basePlane);
  }
  if (LabeledCombo("Plane", &sel, labels.data(),
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
  int kind = std::to_underlying(pat.kind);
  if (LabeledCombo("Type", &kind, kinds, 3)) {
    nlohmann::json before = DocumentIO::FeatureToJson(pat);
    pat.kind = static_cast<PatternKind>(kind);
    CommitDiscrete(state, pat, before, index, commit);
  }

  PickSlotRow(state, pat, PickSlot::PatternSource,
              BodyRefLabel(state, pat.sourceId));
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

// Toggles `r` in a face-ref list (multi slots): present → removed.
bool ToggleRefInList(std::vector<ElementRef>& v, const ElementRef& r) {
  const auto it = std::find_if(v.begin(), v.end(), [&](const ElementRef& e) {
    return e.featureId == r.featureId && e.name == r.name;
  });
  if (it != v.end())
    v.erase(it);
  else
    v.push_back(r);
  return true;
}

// Writes one picked item into the feature field `slot` names. Returns true
// when the feature changed (the caller commits). The blend-edge case needs
// the pick's render handles (meshId/index) for the input-body remap; other
// slots ignore them.
bool ApplySlotItem(AppState& state, Feature& f, PickSlot slot,
                   const SelectionItem& item, uint32_t meshId, uint32_t index,
                   bool liveRegen = true) {
  switch (slot) {
  case PickSlot::ExtrudeEndFace:
    if (auto* ext = dynamic_cast<ExtrudeFeature*>(&f);
        ext != nullptr && item.kind == SelectionItem::Kind::Face) {
      ext->endTarget = item.element;
      return true;
    }
    return false;
  case PickSlot::BooleanTarget:
  case PickSlot::BooleanTool:
    if (auto* boo = dynamic_cast<BooleanFeature*>(&f);
        boo != nullptr && item.kind == SelectionItem::Kind::Body) {
      (slot == PickSlot::BooleanTarget ? boo->targetId : boo->toolId) = item.id;
      return true;
    }
    return false;
  case PickSlot::PatternSource:
    if (auto* pat = dynamic_cast<PatternFeature*>(&f);
        pat != nullptr && item.kind == SelectionItem::Kind::Body) {
      pat->sourceId = item.id;
      return true;
    }
    return false;
  case PickSlot::ShellBody:
    if (auto* sh = dynamic_cast<ShellFeature*>(&f);
        sh != nullptr && item.kind == SelectionItem::Kind::Body) {
      sh->bodyId = item.id;
      return true;
    }
    return false;
  case PickSlot::RibBody:
    if (auto* rb = dynamic_cast<RibFeature*>(&f);
        rb != nullptr && item.kind == SelectionItem::Kind::Body) {
      rb->bodyId = item.id;
      return true;
    }
    return false;
  case PickSlot::ThreadFace:
    if (auto* th = dynamic_cast<ThreadFeature*>(&f);
        th != nullptr && item.kind == SelectionItem::Kind::Face) {
      th->face = item.element;
      return true;
    }
    return false;
  case PickSlot::ShellOpenFaces:
    if (auto* sh = dynamic_cast<ShellFeature*>(&f);
        sh != nullptr && item.kind == SelectionItem::Kind::Face) {
      // Execute demands every open face belong to `bodyId`. The first face
      // therefore NAMES the body (overriding the default, since a face is the
      // more specific statement); once a list exists, a face from elsewhere is
      // refused rather than left to break regeneration.
      if (sh->openFaces.empty())
        sh->bodyId = item.element.featureId;
      else if (item.element.featureId != sh->bodyId)
        return false;
      const bool changed = ToggleRefInList(sh->openFaces, item.element);
      return changed;
    }
    return false;
  case PickSlot::DraftFaces:
    if (auto* dr = dynamic_cast<DraftFeature*>(&f);
        dr != nullptr && item.kind == SelectionItem::Kind::Face) {
      // Draft has no body slot at all, and Execute rejects faces whose
      // featureId != bodyId — so the picks have to write it (nothing in the
      // UI ever did, which is why the tool could not work from the GUI).
      if (dr->faces.empty())
        dr->bodyId = item.element.featureId;
      else if (item.element.featureId != dr->bodyId)
        return false;
      const bool changed = ToggleRefInList(dr->faces, item.element);
      if (dr->faces.empty())
        dr->bodyId = 0; // emptied the list: the next pick re-chooses the body
      return changed;
    }
    return false;
  case PickSlot::SplitBody:
    if (auto* sp = dynamic_cast<SplitFeature*>(&f);
        sp != nullptr && item.kind == SelectionItem::Kind::Body) {
      sp->bodyId = item.id;
      return true;
    }
    return false;
  case PickSlot::SplitKnifeFace:
    if (auto* sp = dynamic_cast<SplitFeature*>(&f);
        sp != nullptr && item.kind == SelectionItem::Kind::Face) {
      // The knife face must be a face OF the body being split, so the pick
      // sets both — picking a face on another body re-targets the split.
      sp->face = item.element;
      sp->bodyId = item.element.featureId;
      return true;
    }
    return false;
  case PickSlot::RefineBody:
    if (auto* rf = dynamic_cast<RefineFeature*>(&f);
        rf != nullptr && item.kind == SelectionItem::Kind::Body) {
      rf->bodyId = item.id;
      return true;
    }
    return false;
  case PickSlot::BlendEdges:
    if (auto* fil = dynamic_cast<FilletFeature*>(&f);
        fil != nullptr && item.kind == SelectionItem::Kind::Edge) {
      // Toggles + commits internally (the remap needs its own error path).
      ApplyBlendEdgePick(state, *fil, item.element, meshId, index, liveRegen);
      return false;
    }
    return false;
  case PickSlot::ExtrudeRegions:
  case PickSlot::RevolveRegions:
    if (item.kind == SelectionItem::Kind::SketchRegion) {
      const std::vector<uint32_t> ids = ParseRegionIds(item.element.name);
      std::vector<std::vector<uint32_t>>* regions = nullptr;
      if (auto* ext = dynamic_cast<ExtrudeFeature*>(&f))
        regions = &ext->regions;
      else if (auto* rev = dynamic_cast<RevolveFeature*>(&f))
        regions = &rev->regions;
      if (regions != nullptr && !ids.empty()) {
        const auto it = std::find(regions->begin(), regions->end(), ids);
        if (it != regions->end())
          regions->erase(it); // click a selected region again = remove
        else
          regions->push_back(ids);
        return true;
      }
    }
    return false;
  }
  return false;
}

} // namespace

std::vector<SlotSpec> SlotSpecsFor(const Feature& feature) {
  constexpr uint32_t kFace = SlotKind(SelectionItem::Kind::Face);
  constexpr uint32_t kEdge = SlotKind(SelectionItem::Kind::Edge);
  constexpr uint32_t kBody = SlotKind(SelectionItem::Kind::Body);
  constexpr uint32_t kRegion = SlotKind(SelectionItem::Kind::SketchRegion);

  if (auto* ext = dynamic_cast<const ExtrudeFeature*>(&feature)) {
    std::vector<SlotSpec> specs = {
        {PickSlot::ExtrudeRegions, "Profiles", kRegion, true,
         "Click sketch regions to extrude (again removes; none = all)",
         /*required=*/false}};
    if (ext->endMode == ExtrudeEnd::UpToFace) {
      specs.push_back({PickSlot::ExtrudeEndFace, "Target face", kFace, false,
                       "Pick the face the extrude ends at"});
    }
    return specs;
  }
  if (dynamic_cast<const RevolveFeature*>(&feature) != nullptr) {
    return {{PickSlot::RevolveRegions, "Profiles", kRegion, true,
             "Click sketch regions to revolve (again removes; none = all)",
             /*required=*/false}};
  }
  if (dynamic_cast<const BooleanFeature*>(&feature) != nullptr) {
    return {
        {PickSlot::BooleanTarget, "Target", kBody, false,
         "Pick the target body"},
        {PickSlot::BooleanTool, "Tool", kBody, false, "Pick the tool body"}};
  }
  if (dynamic_cast<const PatternFeature*>(&feature) != nullptr) {
    return {{PickSlot::PatternSource, "Source", kBody, false,
             "Pick the body to pattern"}};
  }
  if (dynamic_cast<const ShellFeature*>(&feature) != nullptr) {
    return {
        {PickSlot::ShellBody, "Body", kBody, false, "Pick the body to hollow"},
        {PickSlot::ShellOpenFaces, "Open faces", kFace, true,
         "Click faces to open (click again to remove)", /*required=*/false}};
  }
  if (dynamic_cast<const DraftFeature*>(&feature) != nullptr) {
    return {{PickSlot::DraftFaces, "Faces", kFace, true,
             "Click side faces to taper (click again to remove)"}};
  }
  if (dynamic_cast<const RibFeature*>(&feature) != nullptr) {
    // Optional: bodyId 0 = nearest overlapping body.
    return {{PickSlot::RibBody, "Body", kBody, false,
             "Pick the body the rib attaches to", /*required=*/false}};
  }
  if (dynamic_cast<const ThreadFeature*>(&feature) != nullptr) {
    return {{PickSlot::ThreadFace, "Face", kFace, false,
             "Pick the cylindrical face to thread"}};
  }
  if (auto* sp = dynamic_cast<const SplitFeature*>(&feature)) {
    std::vector<SlotSpec> specs = {
        {PickSlot::SplitBody, "Body", kBody, false, "Pick the body to split"}};
    if (sp->knife == SplitKnifeKind::Face) {
      specs.push_back({PickSlot::SplitKnifeFace, "Knife face", kFace, false,
                       "Pick a flat face to cut along (its plane, extended)"});
    }
    return specs;
  }
  if (dynamic_cast<const RefineFeature*>(&feature) != nullptr) {
    return {{PickSlot::RefineBody, "Body", kBody, false,
             "Pick the body to refine"}};
  }
  if (dynamic_cast<const FilletFeature*>(&feature) != nullptr) {
    return {{PickSlot::BlendEdges, "Edges", kEdge, true,
             "Click edges to add or remove them"}};
  }
  return {};
}

bool ArmSlot(AppState& state, const Feature& feature, PickSlot slot) {
  const std::vector<SlotSpec> specs = SlotSpecsFor(feature);
  const auto spec =
      std::find_if(specs.begin(), specs.end(),
                   [&](const SlotSpec& s) { return s.slot == slot; });
  if (spec == specs.end())
    return false;
  state.slotPick = {feature.GetId(), static_cast<int>(slot), spec->kinds,
                    spec->multi, spec->prompt};
  return true;
}

uint64_t ArmedBlendFeatureId(const AppState& state) {
  return state.slotPick.Active() &&
                 state.slotPick.slot == static_cast<int>(PickSlot::BlendEdges)
             ? state.slotPick.featureId
             : 0;
}

bool SlotFilled(const Feature& feature, PickSlot slot) {
  switch (slot) {
  case PickSlot::ExtrudeEndFace: {
    auto* ext = dynamic_cast<const ExtrudeFeature*>(&feature);
    return ext != nullptr && ext->endTarget.IsSet();
  }
  case PickSlot::BooleanTarget:
  case PickSlot::BooleanTool: {
    auto* boo = dynamic_cast<const BooleanFeature*>(&feature);
    return boo != nullptr &&
           (slot == PickSlot::BooleanTarget ? boo->targetId : boo->toolId) != 0;
  }
  case PickSlot::PatternSource: {
    auto* pat = dynamic_cast<const PatternFeature*>(&feature);
    return pat != nullptr && pat->sourceId != 0;
  }
  case PickSlot::ShellBody: {
    auto* sh = dynamic_cast<const ShellFeature*>(&feature);
    return sh != nullptr && sh->bodyId != 0;
  }
  case PickSlot::ShellOpenFaces: {
    auto* sh = dynamic_cast<const ShellFeature*>(&feature);
    return sh != nullptr && !sh->openFaces.empty();
  }
  case PickSlot::DraftFaces: {
    auto* dr = dynamic_cast<const DraftFeature*>(&feature);
    return dr != nullptr && !dr->faces.empty();
  }
  case PickSlot::ExtrudeRegions: {
    auto* ext = dynamic_cast<const ExtrudeFeature*>(&feature);
    return ext != nullptr && !ext->regions.empty();
  }
  case PickSlot::RevolveRegions: {
    auto* rev = dynamic_cast<const RevolveFeature*>(&feature);
    return rev != nullptr && !rev->regions.empty();
  }
  case PickSlot::RibBody: {
    auto* rb = dynamic_cast<const RibFeature*>(&feature);
    return rb != nullptr && rb->bodyId != 0;
  }
  case PickSlot::ThreadFace: {
    auto* th = dynamic_cast<const ThreadFeature*>(&feature);
    return th != nullptr && !th->face.name.empty();
  }
  case PickSlot::BlendEdges: {
    auto* fil = dynamic_cast<const FilletFeature*>(&feature);
    return fil != nullptr && !fil->edges.empty();
  }
  case PickSlot::SplitBody: {
    auto* sp = dynamic_cast<const SplitFeature*>(&feature);
    return sp != nullptr && sp->bodyId != 0;
  }
  case PickSlot::SplitKnifeFace: {
    auto* sp = dynamic_cast<const SplitFeature*>(&feature);
    return sp != nullptr && !sp->face.name.empty();
  }
  case PickSlot::RefineBody: {
    auto* rf = dynamic_cast<const RefineFeature*>(&feature);
    return rf != nullptr && rf->bodyId != 0;
  }
  }
  return false;
}

void ArmNextUnfilledSlot(AppState& state, const Feature& feature) {
  for (const SlotSpec& spec : SlotSpecsFor(feature)) {
    if (spec.required && !SlotFilled(feature, spec.slot)) {
      ArmSlot(state, feature, spec.slot);
      return;
    }
  }
  // Everything required is satisfied; disarm (unless a slot of ANOTHER
  // feature is armed — don't steal, though in practice sessions are sole).
  if (state.slotPick.featureId == feature.GetId())
    state.slotPick = {};
}

void SeedSlotsFromSelection(AppState& state, Feature& feature) {
  if (state.selection.Empty())
    return;
  std::vector<bool> consumed(state.selection.items.size(), false);
  bool changed = false;
  for (const SlotSpec& spec : SlotSpecsFor(feature)) {
    bool single = !spec.multi;
    for (size_t i = 0; i < state.selection.items.size(); i++) {
      if (consumed[i])
        continue;
      const SelectionItem& item = state.selection.items[i];
      if ((spec.kinds & SlotKind(item.kind)) == 0)
        continue;
      // Resolve the item's render handles when they exist — the blend edge
      // remap prefers anchoring on the displayed polyline.
      uint32_t mesh = 0, index = 0;
      if (item.kind == SelectionItem::Kind::Edge)
        ResolveEdgeItem(state, item, mesh, index);
      // liveRegen=false: ONE regen after the whole batch (below). A regen
      // between blend-edge toggles would execute the blend mid-seed, and
      // the remaining pre-picked names stop resolving on the now-blended
      // body — only the first pre-selected edge survived.
      if (ApplySlotItem(state, feature, spec.slot, item, mesh, index,
                        /*liveRegen=*/false)) {
        changed = true;
        consumed[i] = true;
      } else if (item.kind == SelectionItem::Kind::Edge &&
                 spec.slot == PickSlot::BlendEdges) {
        consumed[i] = true; // blend edges commit internally
        changed = true;
      }
      if (single)
        break; // one item per single slot; the rest seed later slots
    }
  }
  if (changed && state.document)
    state.document->Regenerate();
}

void SlotPickClicked(AppState& state, const SelectionItem& item,
                     uint32_t meshId, uint32_t index) {
  if (!state.slotPick.Active())
    return;
  if (state.slotPick.featureId == kMateSessionSlot) {
    // The mate session's connector slots live on the assembly, not the
    // feature list; routing/advance/disarm happen in AssemblyActions.
    MateConnectorPicked(state, item);
    return;
  }
  if (!state.document)
    return;
  size_t featureIndex = 0;
  Feature* f =
      state.document->FindFeatureById(state.slotPick.featureId, &featureIndex);
  if (f == nullptr) {
    state.slotPick = {}; // the slot ends with its feature
    return;
  }
  const bool commit = f->GetId() != state.pendingFeatureId;
  nlohmann::json before = DocumentIO::FeatureToJson(*f);
  if (ApplySlotItem(state, *f, static_cast<PickSlot>(state.slotPick.slot), item,
                    meshId, index)) {
    CommitDiscrete(state, *f, before, featureIndex, commit);
  }
  // Auto-advance (U7b): a filled single slot hands off to the next unfilled
  // required one (boolean: target pick arms tool — pick, pick, done); multi
  // slots stay armed until the user moves on.
  if (!state.slotPick.multi)
    ArmNextUnfilledSlot(state, *f);
}

void DrawFeatureParamWidgets(AppState& state, Feature& feature, size_t index,
                             bool commit) {
  // Name
  static char nameBuf[128];
  static uint64_t nameBufId = 0;
  if (nameBufId != feature.GetId() || !ImGui::IsAnyItemActive()) {
    snprintf(nameBuf, sizeof(nameBuf), "%s", feature.GetName().c_str());
    nameBufId = feature.GetId();
  }
  LeftLabel("Name");
  ImGui::InputText("##Name", nameBuf, sizeof(nameBuf));
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
    if (LabeledCombo("Material", &sel, labels.data(),
                     static_cast<int>(labels.size()))) {
      nlohmann::json before = DocumentIO::FeatureToJson(feature);
      feature.SetMaterial(sel == 0 ? std::string{}
                                   : MaterialLibrary::Slug(types[sel - 1]));
      CommitDiscrete(state, feature, before, index, commit);
    }
  }

  if (auto* box = dynamic_cast<BoxFeature*>(&feature)) {
    HandleEdit(state, feature, DragFloat3Row("Position", &box->position.x),
               index, commit);
    HandleEdit(state, feature, DragDim(state, "Width", &box->width), index,
               commit);
    HandleEdit(state, feature, DragDim(state, "Height", &box->height), index,
               commit);
    HandleEdit(state, feature, DragDim(state, "Depth", &box->depth), index,
               commit);
  } else if (auto* sphere = dynamic_cast<SphereFeature*>(&feature)) {
    HandleEdit(state, feature, DragFloat3Row("Center", &sphere->center.x),
               index, commit);
    HandleEdit(state, feature, DragDim(state, "Radius", &sphere->radius), index,
               commit);
  } else if (auto* cyl = dynamic_cast<CylinderFeature*>(&feature)) {
    HandleEdit(state, feature, DragFloat3Row("Center", &cyl->center.x), index,
               commit);
    HandleEdit(state, feature, DragFloat3Row("Direction", &cyl->direction.x),
               index, commit);
    HandleEdit(state, feature, DragDim(state, "Radius", &cyl->radius), index,
               commit);
    HandleEdit(state, feature, DragDim(state, "Height", &cyl->height), index,
               commit);
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
    const char* ends[] = {"Blind", "Symmetric", "Two-sided", "Up to face",
                          "Through all"};
    int end = std::to_underlying(ext->endMode);
    if (LabeledCombo("End", &end, ends, 5)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*ext);
      ext->endMode = static_cast<ExtrudeEnd>(end);
      CommitDiscrete(state, feature, before, index, commit);
    }
    if (ext->endMode == ExtrudeEnd::UpToFace) {
      // Target face: a pick slot (U3) — arm it, click the face in the
      // viewport. Far saner than the old every-named-face combo.
      PickSlotRow(state, feature, PickSlot::ExtrudeEndFace,
                  ext->endTarget.IsSet() ? ext->endTarget.name : "");
    } else if (ext->endMode != ExtrudeEnd::ThroughAll) { // no distance there
      ExprDim(state, feature, index, commit, "Distance", ext->distanceExpr,
              ext->distance);
    }
    if (ext->endMode == ExtrudeEnd::TwoSided) {
      ExprDim(state, feature, index, commit, "Distance 2", ext->distance2Expr,
              ext->distance2);
    }
    if (ext->endMode == ExtrudeEnd::Blind ||
        ext->endMode == ExtrudeEnd::TwoSided ||
        ext->endMode == ExtrudeEnd::ThroughAll) // no effect otherwise
      HandleEdit(state, feature, ImGui::Checkbox("Reversed", &ext->reversed),
                 index, commit);

    HandleEdit(state, feature, ImGui::Checkbox("Thin", &ext->thin), index,
               commit);
    if (ext->thin) {
      ExprDim(state, feature, index, commit, "Thickness",
              ext->thinThicknessExpr, ext->thinThickness);
      const char* sides[] = {"Outward", "Inward", "Both"};
      int side = std::to_underlying(ext->thinSide);
      if (LabeledCombo("Wall side", &side, sides, 3)) {
        nlohmann::json before = DocumentIO::FeatureToJson(*ext);
        ext->thinSide = static_cast<sketch::ThinSide>(side);
        CommitDiscrete(state, feature, before, index, commit);
      }
    }

    const char* modes[] = {"New Body", "Add", "Cut"};
    int mode = std::to_underlying(ext->mode);
    if (LabeledCombo("Mode", &mode, modes, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*ext);
      ext->mode = static_cast<ExtrudeMode>(mode);
      CommitDiscrete(state, feature, before, index, commit);
    }

    if (Feature* src = state.document->FindFeatureById(ext->sketchId)) {
      ImGui::Text("Sketch: %s", src->GetName().c_str());
    } else {
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Sketch: missing");
    }
    PickSlotRow(state, feature, PickSlot::ExtrudeRegions,
                ext->regions.empty()
                    ? "All regions"
                    : std::to_string(ext->regions.size()) + " selected");
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
    int mode = std::to_underlying(rev->mode);
    if (LabeledCombo("Mode", &mode, modes, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*rev);
      rev->mode = static_cast<ExtrudeMode>(mode);
      CommitDiscrete(state, feature, before, index, commit);
    }

    if (Feature* src = state.document->FindFeatureById(rev->sketchId)) {
      ImGui::Text("Sketch: %s", src->GetName().c_str());
    } else {
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Sketch: missing");
    }
    PickSlotRow(state, feature, PickSlot::RevolveRegions,
                rev->regions.empty()
                    ? "All regions"
                    : std::to_string(rev->regions.size()) + " selected");
  } else if (auto* boo = dynamic_cast<BooleanFeature*>(&feature)) {
    DrawBooleanParams(state, *boo, index, commit);
  } else if (auto* pat = dynamic_cast<PatternFeature*>(&feature)) {
    DrawPatternParams(state, *pat, index, commit);
  } else if (auto* dr = dynamic_cast<DraftFeature*>(&feature)) {
    ExprDim(state, feature, index, commit, "Angle (deg)", dr->angleExpr,
            dr->angleDeg);
    const char* planes[] = {"XY (pull +Z)", "XZ (pull +Y)", "YZ (pull +X)"};
    int pl = std::to_underlying(dr->neutralPlane);
    if (LabeledCombo("Neutral", &pl, planes, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*dr);
      dr->neutralPlane = static_cast<PlaneType>(pl);
      CommitDiscrete(state, feature, before, index, commit);
    }
    PickSlotRow(
        state, feature, PickSlot::DraftFaces,
        dr->faces.empty() ? "" : std::to_string(dr->faces.size()) + " picked");
    RefListRows(state, feature, index, commit, dr->faces);
  } else if (auto* ho = dynamic_cast<HoleFeature*>(&feature)) {
    const char* kinds[] = {"Simple", "Counterbore", "Countersink"};
    int k = std::to_underlying(ho->kind);
    if (LabeledCombo("Type", &k, kinds, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*ho);
      ho->kind = static_cast<HoleKind>(k);
      // Size the head from the bore. The class defaults are absolute
      // (Ø2 x 0.5), which on a Ø0.5 hole is a four-times-oversize crater —
      // switching Type used to disfigure the part instead of previewing a
      // head. Only re-seeds while the values are the untouched defaults.
      const bool cbUntouched = ho->cbDiameter == 2.0 && ho->cbDepth == 0.5;
      const bool csUntouched = ho->csDiameter == 2.0 && ho->csDepth == 0.5;
      if (ho->kind == HoleKind::Counterbore && cbUntouched) {
        ho->cbDiameter = ho->diameter * 2.0;
        ho->cbDepth = ho->diameter;
      } else if (ho->kind == HoleKind::Countersink && csUntouched) {
        ho->csDiameter = ho->diameter * 2.0; // 90 deg included angle
        ho->csDepth = ho->diameter * 0.5;
      }
      CommitDiscrete(state, feature, before, index, commit);
    }
    ExprDim(state, feature, index, commit, "Diameter", ho->diameterExpr,
            ho->diameter);
    HandleEdit(state, feature, ImGui::Checkbox("Through all", &ho->through),
               index, commit);
    if (!ho->through)
      ExprDim(state, feature, index, commit, "Depth", ho->depthExpr, ho->depth);
    if (ho->kind == HoleKind::Counterbore) {
      HandleEdit(state, feature, DragDim(state, "C'bore dia", &ho->cbDiameter),
                 index, commit);
      HandleEdit(state, feature, DragDim(state, "C'bore depth", &ho->cbDepth),
                 index, commit);
    } else if (ho->kind == HoleKind::Countersink) {
      HandleEdit(state, feature, DragDim(state, "C'sink dia", &ho->csDiameter),
                 index, commit);
      HandleEdit(state, feature, DragDim(state, "C'sink depth", &ho->csDepth),
                 index, commit);
    }
    if (Feature* src = state.document->FindFeatureById(ho->sketchId)) {
      // The centre count is the confirmation the user wants right here: it
      // says how many holes this feature will drill.
      size_t centres = 0;
      if (const auto* psk = dynamic_cast<const SketchFeature*>(src))
        for (const sketch::Entity& e : psk->entities)
          if (std::get_if<sketch::Point>(&e) != nullptr)
            centres++;
      ImGui::Text("Points: %s (%zu centres)", src->GetName().c_str(), centres);
    } else {
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Points: missing");
    }
  } else if (auto* cv = dynamic_cast<CurveFeature*>(&feature)) {
    const char* kinds[] = {"Fit spline", "Helix", "Control poly"};
    int k = std::to_underlying(cv->kind);
    if (LabeledCombo("Kind", &k, kinds, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*cv);
      cv->kind = static_cast<Curve3DKind>(k);
      CommitDiscrete(state, feature, before, index, commit);
    }
    if (cv->kind == Curve3DKind::Helix) {
      HandleEdit(state, feature, DragFloat3Row("Origin", &cv->axisOrigin.x),
                 index, commit);
      HandleEdit(state, feature, DragFloat3Row("Axis", &cv->axisDir.x), index,
                 commit);
      ExprDim(state, feature, index, commit, "Radius", cv->radiusExpr,
              cv->radius);
      ExprDim(state, feature, index, commit, "Pitch", cv->pitchExpr, cv->pitch);
      ExprDim(state, feature, index, commit, "Turns", cv->turnsExpr, cv->turns);
      HandleEdit(state, feature,
                 ImGui::Checkbox("Right-handed", &cv->rightHanded), index,
                 commit);
    } else {
      if (cv->kind == Curve3DKind::ControlPoly) {
        LeftLabel("Degree");
        HandleEdit(state, feature, ImGui::InputInt("##Degree", &cv->degree),
                   index, commit);
        if (cv->degree < 1)
          cv->degree = 1;
      }
      HandleEdit(state, feature, ImGui::Checkbox("Closed", &cv->closed), index,
                 commit);
      // The 3D point list — each row a drag + remove, plus an append button.
      ImGui::Text("Points: %zu", cv->points.size());
      for (size_t p = 0; p < cv->points.size(); p++) {
        ImGui::PushID(static_cast<int>(p));
        if (ImGui::SmallButton("x")) {
          nlohmann::json before = DocumentIO::FeatureToJson(*cv);
          cv->points.erase(cv->points.begin() + static_cast<long>(p));
          CommitDiscrete(state, feature, before, index, commit);
          ImGui::PopID();
          break;
        }
        ImGui::SameLine();
        HandleEdit(state, feature,
                   ImGui::DragFloat3("##pt", &cv->points[p].x, 0.05f), index,
                   commit);
        ImGui::PopID();
      }
      if (ImGui::Button("Add point")) {
        nlohmann::json before = DocumentIO::FeatureToJson(*cv);
        const glm::vec3 last =
            cv->points.empty() ? glm::vec3(0.0f) : cv->points.back();
        cv->points.push_back(last + glm::vec3(1.0f, 0.0f, 0.0f));
        CommitDiscrete(state, feature, before, index, commit);
      }
    }
  } else if (auto* pi = dynamic_cast<PipeFeature*>(&feature)) {
    DrawFeatureRefCombo(state, feature, "Path", &pi->pathId, index, commit,
                        /*allowNone=*/false, AcceptPathLike);
    ExprDim(state, feature, index, commit, "Outer dia", pi->outerDiameterExpr,
            pi->outerDiameter);
    // Wall allows 0 (a solid rod), so a plain drag (ExprDim's DragDim clamps to
    // >= 0.01).
    LeftLabel("Wall");
    static const double wmin = 0.0, wmax = 1e6;
    HandleEdit(state, feature,
               ImGui::DragScalar("##Wall", ImGuiDataType_Double,
                                 &pi->wallThickness, 0.02f, &wmin, &wmax,
                                 pi->wallThickness <= 0.0 ? "solid" : "%.3f",
                                 ImGuiSliderFlags_AlwaysClamp),
               index, commit);
  } else if (auto* sw = dynamic_cast<SweepFeature*>(&feature)) {
    DrawFeatureRefCombo(state, feature, "Profile", &sw->profileSketchId, index,
                        commit, /*allowNone=*/false, AcceptSketch);
    DrawFeatureRefCombo(state, feature, "Path", &sw->pathSketchId, index,
                        commit,
                        /*allowNone=*/false, AcceptPathLike);
    DrawFeatureRefCombo(state, feature, "Guide", &sw->guideId, index, commit,
                        /*allowNone=*/true, AcceptPathLike);
    const char* frames[] = {"Corrected Frenet", "Frenet", "Fixed"};
    int fr = static_cast<int>(sw->frame);
    if (LabeledCombo("Frame", &fr, frames, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*sw);
      sw->frame = static_cast<SweepFrame>(fr);
      CommitDiscrete(state, feature, before, index, commit);
    }
  } else if (auto* lo = dynamic_cast<LoftFeature*>(&feature)) {
    // Sections (ordered), each removable.
    ImGui::Text("Sections: %zu", lo->sketchIds.size());
    for (size_t s = 0; s < lo->sketchIds.size(); s++) {
      ImGui::PushID(static_cast<int>(s));
      if (ImGui::SmallButton("x")) {
        nlohmann::json before = DocumentIO::FeatureToJson(*lo);
        lo->sketchIds.erase(lo->sketchIds.begin() + static_cast<long>(s));
        CommitDiscrete(state, feature, before, index, commit);
        ImGui::PopID();
        break;
      }
      ImGui::SameLine();
      Feature* src = state.document->FindFeatureById(lo->sketchIds[s]);
      ImGui::TextUnformatted(src ? src->GetName().c_str() : "(missing)");
      ImGui::PopID();
    }
    // Append a section: a combo of earlier sketches that resets each frame.
    {
      std::vector<std::string> labels = {"+ Add section"};
      std::vector<uint64_t> ids = {0};
      for (size_t i = 0; i < index; i++) {
        Feature* f = state.document->GetFeature(i);
        if (dynamic_cast<SketchFeature*>(f)) {
          labels.push_back(f->GetName());
          ids.push_back(f->GetId());
        }
      }
      std::vector<const char*> cstrs;
      for (const auto& s : labels)
        cstrs.push_back(s.c_str());
      int sel = 0;
      if (cstrs.size() > 1 &&
          LabeledCombo("Add", &sel, cstrs.data(),
                       static_cast<int>(cstrs.size())) &&
          ids[static_cast<size_t>(sel)] != 0) {
        nlohmann::json before = DocumentIO::FeatureToJson(*lo);
        lo->sketchIds.push_back(ids[static_cast<size_t>(sel)]);
        CommitDiscrete(state, feature, before, index, commit);
      }
    }
    // Optional spine + guide turn this into a guided loft (MakePipeShell).
    DrawFeatureRefCombo(state, feature, "Path", &lo->pathId, index, commit,
                        /*allowNone=*/true, AcceptPathLike);
    DrawFeatureRefCombo(state, feature, "Guide", &lo->guideId, index, commit,
                        /*allowNone=*/true, AcceptPathLike);
  } else if (auto* th = dynamic_cast<ThreadFeature*>(&feature)) {
    // Designation combo: the ISO series + Custom (manual pitch).
    const auto& series = threads::All();
    const bool known = threads::Lookup(th->designation).has_value();
    LeftLabel("Thread");
    if (ImGui::BeginCombo("##Thread",
                          known ? th->designation.c_str() : "Custom")) {
      for (const auto& s : series) {
        if (ImGui::Selectable(s.designation.c_str(),
                              th->designation == s.designation)) {
          nlohmann::json before = DocumentIO::FeatureToJson(*th);
          th->designation = s.designation;
          th->pitch = s.pitch;
          CommitDiscrete(state, feature, before, index, commit);
        }
      }
      if (ImGui::Selectable("Custom", !known)) {
        nlohmann::json before = DocumentIO::FeatureToJson(*th);
        th->designation = "custom";
        CommitDiscrete(state, feature, before, index, commit);
      }
      ImGui::EndCombo();
    }
    if (known) {
      LeftLabel("Pitch");
      ImGui::Text("%.3g (from table)", th->pitch);
    } else {
      HandleEdit(state, feature, DragDim(state, "Pitch", &th->pitch), index,
                 commit);
    }
    {
      LeftLabel("Length");
      static const double lmin = 0.0, lmax = 1e6;
      HandleEdit(state, feature,
                 ImGui::DragScalar("##Length", ImGuiDataType_Double,
                                   &th->length, 0.05f, &lmin, &lmax,
                                   th->length <= 0.0 ? "full face" : "%.3f",
                                   ImGuiSliderFlags_AlwaysClamp),
                 index, commit);
    }
    PickSlotRow(state, feature, PickSlot::ThreadFace,
                th->face.name.empty() ? "" : th->face.name);
  } else if (auto* rb = dynamic_cast<RibFeature*>(&feature)) {
    ExprDim(state, feature, index, commit, "Thickness", rb->thicknessExpr,
            rb->thickness);
    HandleEdit(state, feature, ImGui::Checkbox("Flip", &rb->flipDirection),
               index, commit);
    PickSlotRow(state, feature, PickSlot::RibBody,
                rb->bodyId == 0 ? "(nearest)"
                                : BodyRefLabel(state, rb->bodyId));
  } else if (auto* sh = dynamic_cast<ShellFeature*>(&feature)) {
    ExprDim(state, feature, index, commit, "Thickness", sh->thicknessExpr,
            sh->thickness);
    PickSlotRow(state, feature, PickSlot::ShellBody,
                BodyRefLabel(state, sh->bodyId));
    PickSlotRow(state, feature, PickSlot::ShellOpenFaces,
                sh->openFaces.empty()
                    ? ""
                    : std::to_string(sh->openFaces.size()) + " picked");
    RefListRows(state, feature, index, commit, sh->openFaces);
  } else if (auto* sp = dynamic_cast<SplitFeature*>(&feature)) {
    const char* knives[] = {"Plane", "Face of the body"};
    int knife = std::to_underlying(sp->knife);
    if (LabeledCombo("Knife", &knife, knives, 2)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*sp);
      sp->knife = static_cast<SplitKnifeKind>(knife);
      CommitDiscrete(state, feature, before, index, commit);
    }
    PickSlotRow(state, feature, PickSlot::SplitBody,
                BodyRefLabel(state, sp->bodyId));
    if (sp->knife == SplitKnifeKind::Plane) {
      PlaneRefCombo(state, feature, sp->datumId, sp->basePlane, index, commit);
      ExprDim(state, feature, index, commit, "Offset", sp->offsetExpr,
              sp->offset);
    } else {
      PickSlotRow(state, feature, PickSlot::SplitKnifeFace,
                  sp->face.name.empty() ? "" : sp->face.name);
    }
  } else if (auto* rf = dynamic_cast<RefineFeature*>(&feature)) {
    PickSlotRow(state, feature, PickSlot::RefineBody,
                BodyRefLabel(state, rf->bodyId));
  } else if (auto* fil = dynamic_cast<FilletFeature*>(&feature)) {
    const char* ops[] = {"Fillet", "Chamfer"};
    int op = std::to_underlying(fil->op);
    if (LabeledCombo("Operation", &op, ops, 2)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*fil);
      fil->op = static_cast<EdgeBlendOp>(op);
      CommitDiscrete(state, feature, before, index, commit);
    }
    ExprDim(state, feature, index, commit,
            fil->op == EdgeBlendOp::Chamfer ? "Distance" : "Radius",
            fil->radiusExpr, fil->radius);

    // The edge collection as a regular multi slot (U7 — was EdgePickState).
    PickSlotRow(state, feature, PickSlot::BlendEdges,
                fil->edges.empty()
                    ? ""
                    : std::to_string(fil->edges.size()) + " picked");
    RefListRows(state, feature, index, commit, fil->edges);
  }
}

} // namespace ui
