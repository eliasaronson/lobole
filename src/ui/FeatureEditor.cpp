// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/FeatureEditor.h"
#include "ui/AssemblyActions.h"
#include "ui/BearingTool.h"
#include "ui/CamPanel.h"
#include "ui/CamTool.h"
#include "ui/DerivedActions.h"
#include "ui/FastenerTool.h"
#include "ui/TemplateActions.h"
#include "ui/ToleranceActions.h" // PrintedThreadFor
#include <functional>
#include <utility>

#include "cad/CADMaterials.h"
#include "cad/SketchPlane.h"
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/ThreadStandards.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CornerReliefFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/DerivedFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/HemFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/JogFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/NormalCutFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/RefoldFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/RibFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/features/SweepFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/features/UnfoldFeature.h"
#include "core/features/WormFeature.h"
#include "core/io/Serialization.h"
#include "core/parts/FastenerTable.h"
#include "core/sketch/SketchProfile.h"
#include "render/Camera.h"
#include "ui/CreateActions.h"
#include "ui/SketchInteraction.h"
#include "ui/SketchOverlay.h"
#include "ui/ViewActions.h" // BodyProducers

#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "imgui_internal.h" // GetCurrentWindowRead (LeftLabel auto-resize case)

#include "ui/ExprComplete.h"
#include "ui/ExprInput.h"         // expression-capable numeric fields
#include "ui/PanelLayout.h"       // LeftLabel / SmallButtonReserve (U4)
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

bool DragDim(AppState& state, const char* label, double* value,
             float reserveRight = 0.0f, std::string* exprOut = nullptr,
             double minValue = 0.01) {
  LeftLabel(label, reserveRight);
  const std::string id = std::string("##") + label;
  // Expression-capable (double-click or Ctrl+click, type "width/2"): the
  // shared widget. `exprOut` is for the fields that can KEEP the text.
  return ExprDragDouble(state, id.c_str(), value, 0.05f, minValue, 1e6, "%.3f",
                        exprOut);
}

// Labels-left drop-in for ImGui::Combo.
bool LabeledCombo(const char* label, int* sel, const char* const items[],
                  int count) {
  LeftLabel(label);
  // "###" rather than "##": both hide the label, but "##" leaves the widget
  // with no NAME, and a nameless widget cannot be addressed by a test ref or
  // by a script — only by the numeric id ui_items reports. Every combo in the
  // app was unreachable that way (the Hole type combo over the RPC, the Loft
  // section combo in a GUI test). Same appearance, addressable as "###Type".
  const std::string id = std::string("###") + label;
  return ImGui::Combo(id.c_str(), sel, items, count);
}

// Labels-left drop-in for the xyz drag rows.
template <class T>
bool DragFloat3Row(AppState& state, const char* label, T* v) {
  LeftLabel(label);
  const std::string id = std::string("##") + label;
  return ExprDragFloat3(state, id.c_str(), v, 0.05f);
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
             const char* label, std::string& exprText, double& value,
             double minValue = 0.01) {
  Document& doc = *state.document;
  ImGui::PushID(label);

  if (exprText.empty()) {
    std::string typed;
    const bool changed = DragDim(state, label, &value, SmallButtonReserve("fx"),
                                 &typed, minValue);
    // An expression typed straight into the field KEEPS ITS TEXT, exactly as
    // the fx field would. Otherwise "wall_d" collapses to the 20 it happened
    // to evaluate to the moment it commits, the next edit edits 20, and
    // nothing ever said the link to the variable was dropped. Set before
    // HandleEdit so the regeneration and the undo entry both see it.
    if (!typed.empty())
      exprText = typed;
    HandleEdit(state, feature, changed, index, commit);
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
  const ExprFieldEvents ev =
      ExprCompletingInput(state, "##expr", editing ? buf : tmp, sizeof(buf));
  if (ev.activated) {
    activeField = key;
    snprintf(buf, sizeof(buf), "%s", exprText.c_str());
  }
  if (editing && ev.left) {
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
    std::string typed;
    const bool changed = ExprInputInt(state, "##count", &value, &typed);
    if (!typed.empty())
      exprText = typed; // same as ExprDim: the text is the point
    HandleEdit(state, feature, changed, index, commit);
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
  const ExprFieldEvents ev =
      ExprCompletingInput(state, "##expr", editing ? buf : tmp, sizeof(buf));
  if (ev.activated) {
    activeField = key;
    snprintf(buf, sizeof(buf), "%s", exprText.c_str());
  }
  if (editing && ev.left) {
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

  // A custom axis (a point and a direction, set through the API) has no row
  // to pick it from; list the one this feature has, so the combo names it
  // instead of showing X, which it is not.
  if (axis.kind == AxisRef::Kind::Custom) {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "Custom (%.3g, %.3g, %.3g) along (%.3g, %.3g, %.3g)",
                  axis.origin.x, axis.origin.y, axis.origin.z, axis.direction.x,
                  axis.direction.y, axis.direction.z);
    labels.push_back(buf);
    refs.push_back(axis);
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
// A sketch with at least one closed profile: what a cut is made from.
bool AcceptClosedSketch(const Feature* f) {
  const auto* sk = dynamic_cast<const SketchFeature*>(f);
  return sk != nullptr && !sketch::BuildLoops(sk->entities).empty();
}

// A sketch holding exactly one line: what a jog is located by.
bool AcceptJogLine(const Feature* f) {
  const auto* sk = dynamic_cast<const SketchFeature*>(f);
  if (sk == nullptr)
    return false;
  int lines = 0;
  for (const sketch::Entity& e : sk->entities)
    if (const auto* l = std::get_if<sketch::Line>(&e);
        l != nullptr && !l->construction)
      ++lines;
  return lines == 1;
}

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
    std::vector<const char*> labels = {SketchPlane::BasePlaneLabel(0),
                                       SketchPlane::BasePlaneLabel(1),
                                       SketchPlane::BasePlaneLabel(2)};
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
    HandleEdit(state, dp, DragFloat3Row(state, "Point 1", &dp.p0.x), index,
               commit);
    HandleEdit(state, dp, DragFloat3Row(state, "Point 2", &dp.p1.x), index,
               commit);
    HandleEdit(state, dp, DragFloat3Row(state, "Point 3", &dp.p2.x), index,
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
  int kind = std::to_underlying(da.kind);
  if (LabeledCombo("Type", &kind, kinds, 5)) {
    nlohmann::json before = DocumentIO::FeatureToJson(da);
    da.kind = static_cast<DatumAxisKind>(kind);
    CommitDiscrete(state, da, before, index, commit);
  }

  switch (da.kind) {
  case DatumAxisKind::TwoPoints:
    HandleEdit(state, da, DragFloat3Row(state, "Point 1", &da.p0.x), index,
               commit);
    HandleEdit(state, da, DragFloat3Row(state, "Point 2", &da.p1.x), index,
               commit);
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

// Properties widgets for a datum frame: kind, then the active variant's
// fields. Elements are shown, not picked, here: a frame from elements is
// authored by name (scripts) or from a selection when created — this editor
// flips its Z and edits the numeric variants.
void DrawFrameParams(AppState& state, FrameFeature& fr, size_t index,
                     bool commit) {
  const char* kinds[] = {"Explicit", "From Element(s)", "Offset"};
  int kind = std::to_underlying(fr.kind);
  if (LabeledCombo("Type", &kind, kinds, 3)) {
    nlohmann::json before = DocumentIO::FeatureToJson(fr);
    fr.kind = static_cast<FrameKind>(kind);
    CommitDiscrete(state, fr, before, index, commit);
  }
  switch (fr.kind) {
  case FrameKind::Explicit:
    ExprDim(state, fr, index, commit, "Origin X", fr.originExpr[0],
            fr.origin[0]);
    ExprDim(state, fr, index, commit, "Origin Y", fr.originExpr[1],
            fr.origin[1]);
    ExprDim(state, fr, index, commit, "Origin Z", fr.originExpr[2],
            fr.origin[2]);
    HandleEdit(state, fr, DragFloat3Row(state, "Z axis", &fr.zAxis.x), index,
               commit);
    HandleEdit(state, fr, DragFloat3Row(state, "X axis", &fr.xAxis.x), index,
               commit);
    break;
  case FrameKind::FromElement:
    if (fr.elements.empty()) {
      ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f),
                         "No element named yet");
    } else {
      for (const ElementRef& e : fr.elements)
        ImGui::Text("%llu / %s", static_cast<unsigned long long>(e.featureId),
                    e.name.c_str());
    }
    HandleEdit(state, fr, ImGui::Checkbox("Flip Z", &fr.flip), index, commit);
    break;
  case FrameKind::Offset: {
    // Parent: the part origin or any earlier Frame.
    std::vector<std::string> names{"Part origin"};
    std::vector<uint64_t> ids{0};
    for (size_t i = 0; i < index; i++)
      if (auto* other =
              dynamic_cast<FrameFeature*>(state.document->GetFeature(i))) {
        names.push_back(other->GetName());
        ids.push_back(other->GetId());
      }
    std::vector<const char*> items;
    for (const std::string& n : names)
      items.push_back(n.c_str());
    int sel = 0;
    for (size_t i = 0; i < ids.size(); i++)
      if (ids[i] == fr.parentId)
        sel = static_cast<int>(i);
    if (LabeledCombo("Parent", &sel, items.data(),
                     static_cast<int>(items.size()))) {
      nlohmann::json before = DocumentIO::FeatureToJson(fr);
      fr.parentId = ids[static_cast<size_t>(sel)];
      CommitDiscrete(state, fr, before, index, commit);
    }
    ExprDim(state, fr, index, commit, "Offset X", fr.offsetExpr[0],
            fr.offset[0]);
    ExprDim(state, fr, index, commit, "Offset Y", fr.offsetExpr[1],
            fr.offset[1]);
    ExprDim(state, fr, index, commit, "Offset Z", fr.offsetExpr[2],
            fr.offset[2]);
    ExprDim(state, fr, index, commit, "Rotate X (deg)", fr.rotationExpr[0],
            fr.rotationDeg[0]);
    ExprDim(state, fr, index, commit, "Rotate Y (deg)", fr.rotationExpr[1],
            fr.rotationDeg[1]);
    ExprDim(state, fr, index, commit, "Rotate Z (deg)", fr.rotationExpr[2],
            fr.rotationDeg[2]);
    break;
  }
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
// `onChanged` runs after the new plane is stored and BEFORE the commit, so
// anything derived from the plane (a split's offset) is part of the same
// edit rather than a second, uncommitted one.
void PlaneRefCombo(AppState& state, Feature& owner, uint64_t& planeId,
                   PlaneType& basePlane, size_t index, bool commit,
                   const std::function<void()>& onChanged = {}) {
  std::vector<const char*> labels = {SketchPlane::BasePlaneLabel(0),
                                     SketchPlane::BasePlaneLabel(1),
                                     SketchPlane::BasePlaneLabel(2)};
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
    if (onChanged)
      onChanged();
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
  case PickSlot::GearBlank:
    if (item.kind != SelectionItem::Kind::Face)
      return false;
    if (auto* wo = dynamic_cast<WormFeature*>(&f)) {
      wo->face = item.element;
      return true;
    }
    if (auto* ww = dynamic_cast<WormWheelFeature*>(&f)) {
      ww->face = item.element;
      return true;
    }
    return false;
  case PickSlot::FlatPatternBody:
    if (auto* fp = dynamic_cast<FlatPatternFeature*>(&f);
        fp != nullptr && item.kind == SelectionItem::Kind::Body) {
      fp->bodyId = item.id;
      return true;
    }
    return false;
  case PickSlot::CornerReliefBody:
    if (auto* cr = dynamic_cast<CornerReliefFeature*>(&f);
        cr != nullptr && item.kind == SelectionItem::Kind::Body) {
      cr->bodyId = item.id;
      return true;
    }
    return false;
  case PickSlot::HemEdges:
    if (auto* hm = dynamic_cast<HemFeature*>(&f);
        hm != nullptr && item.kind == SelectionItem::Kind::Edge) {
      if (!hm->edges.empty() &&
          item.element.featureId != hm->edges.front().featureId)
        return false;
      return ToggleRefInList(hm->edges, item.element);
    }
    return false;
  case PickSlot::UnfoldBody:
    if (auto* uf = dynamic_cast<UnfoldFeature*>(&f);
        uf != nullptr && item.kind == SelectionItem::Kind::Body) {
      uf->bodyId = item.id;
      return true;
    }
    return false;
  case PickSlot::EdgeFlangeEdges:
    if (auto* ef = dynamic_cast<EdgeFlangeFeature*>(&f);
        ef != nullptr && item.kind == SelectionItem::Kind::Edge) {
      // Every picked edge has to be on ONE sheet body — a flange bends a
      // panel, not two parts at once — so the first pick names the body and
      // a later edge from elsewhere is refused rather than left to break
      // regeneration. The same rule Shell's open faces follow.
      if (!ef->edges.empty() &&
          item.element.featureId != ef->edges.front().featureId)
        return false;
      return ToggleRefInList(ef->edges, item.element);
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
      uint64_t sketchId = 0;
      if (auto* ext = dynamic_cast<ExtrudeFeature*>(&f)) {
        regions = &ext->regions;
        sketchId = ext->sketchId;
      } else if (auto* rev = dynamic_cast<RevolveFeature*>(&f)) {
        regions = &rev->regions;
        sketchId = rev->sketchId;
      }
      if (regions != nullptr && !ids.empty()) {
        // One click picks the loop's whole pattern/mirror family (a linked
        // pattern is ONE thing — see SketchFeature::SiblingRegions); a click
        // on any already-picked member removes the family again. Ctrl narrows
        // it to THIS region alone — the four-of-five-holes intent, which the
        // family toggle would otherwise make unreachable by clicking.
        std::vector<std::vector<uint32_t>> family = {ids};
        if (!ImGui::GetIO().KeyCtrl && state.document != nullptr)
          if (auto* sk = dynamic_cast<SketchFeature*>(
                  state.document->FindFeatureById(sketchId)))
            family = sk->SiblingRegions(ids);
        const bool present =
            std::find(regions->begin(), regions->end(), ids) != regions->end();
        for (const std::vector<uint32_t>& r : family) {
          const auto it = std::find(regions->begin(), regions->end(), r);
          if (present) {
            if (it != regions->end())
              regions->erase(it);
          } else if (it == regions->end()) {
            regions->push_back(r);
          }
        }
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
         "Click sketch regions to extrude (a pattern picks together, "
         "Ctrl = one region; again removes; none = all)",
         /*required=*/false}};
    if (ext->endMode == ExtrudeEnd::UpToFace) {
      specs.push_back({PickSlot::ExtrudeEndFace, "Target face", kFace, false,
                       "Pick the face the extrude ends at"});
    }
    return specs;
  }
  if (dynamic_cast<const RevolveFeature*>(&feature) != nullptr) {
    return {{PickSlot::RevolveRegions, "Profiles", kRegion, true,
             "Click sketch regions to revolve (a pattern picks together, "
             "Ctrl = one region; again removes; none = all)",
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
             "Pick the body to pattern, or a cut's tree row to pattern "
             "the hole it makes"}};
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
  if (dynamic_cast<const WormFeature*>(&feature) != nullptr) {
    return {{PickSlot::GearBlank, "Blank", kFace, false,
             "Pick the worm blank's cylindrical outside: its tip"}};
  }
  if (dynamic_cast<const WormWheelFeature*>(&feature) != nullptr) {
    return {{PickSlot::GearBlank, "Blank", kFace, false,
             "Pick the wheel blank's cylindrical rim"}};
  }
  if (dynamic_cast<const EdgeFlangeFeature*>(&feature) != nullptr) {
    return {{PickSlot::EdgeFlangeEdges, "Edges", kEdge, true,
             "Click STRAIGHT edges of a flat panel (click again to remove). "
             "Which of a rim's two edges you pick decides which way the wall "
             "folds"}};
  }
  if (dynamic_cast<const CornerReliefFeature*>(&feature) != nullptr) {
    return {{PickSlot::CornerReliefBody, "Body", kBody, false,
             "Pick the sheet body whose corners to relieve"}};
  }
  if (dynamic_cast<const HemFeature*>(&feature) != nullptr) {
    return {{PickSlot::HemEdges, "Edges", kEdge, true,
             "Click STRAIGHT edges of a flat panel to hem (click again to "
             "remove). Which of a rim's two edges decides which way it folds"}};
  }
  if (dynamic_cast<const UnfoldFeature*>(&feature) != nullptr) {
    return {{PickSlot::UnfoldBody, "Body", kBody, false,
             "Pick the sheet body whose bends to lay flat"}};
  }
  if (dynamic_cast<const FlatPatternFeature*>(&feature) != nullptr) {
    return {{PickSlot::FlatPatternBody, "Body", kBody, false,
             "Pick the sheet body to lay flat"}};
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
  case PickSlot::GearBlank: {
    if (auto* wo = dynamic_cast<const WormFeature*>(&feature))
      return !wo->face.name.empty();
    auto* ww = dynamic_cast<const WormWheelFeature*>(&feature);
    return ww != nullptr && !ww->face.name.empty();
  }
  case PickSlot::EdgeFlangeEdges: {
    auto* ef = dynamic_cast<const EdgeFlangeFeature*>(&feature);
    return ef != nullptr && !ef->edges.empty();
  }
  case PickSlot::CornerReliefBody: {
    auto* cr = dynamic_cast<const CornerReliefFeature*>(&feature);
    return cr != nullptr && cr->bodyId != 0;
  }
  case PickSlot::HemEdges: {
    auto* hm = dynamic_cast<const HemFeature*>(&feature);
    return hm != nullptr && !hm->edges.empty();
  }
  case PickSlot::UnfoldBody: {
    auto* uf = dynamic_cast<const UnfoldFeature*>(&feature);
    return uf != nullptr && uf->bodyId != 0;
  }
  case PickSlot::FlatPatternBody: {
    auto* fp = dynamic_cast<const FlatPatternFeature*>(&feature);
    return fp != nullptr && fp->bodyId != 0;
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

namespace {
// Each non-feature tool session, in one place. Adding one is a row here and
// nothing else; test_conventions asserts every k*Slot sentinel appears.
const ToolSession kToolSessions[] = {
    {kMateSessionSlot,
     [](const AppState& s) {
       return s.assembly != nullptr && s.mateTool.active;
     },
     [](AppState& s, const SelectionItem& i) { MateConnectorPicked(s, i); }},
    {kSectionToolSlot,
     [](const AppState& s) {
       return s.document != nullptr && s.sectionToolOpen;
     },
     [](AppState& s, const SelectionItem& i) { SectionFacePicked(s, i); }},
    // A CAM pick lives as long as the JOB does, not the part document: since
    // v73 a setup belongs to a job tab, where `document` is null by
    // definition. Asking the old question disarmed the pick in the same frame
    // it was armed — silently, which is the exact failure this table's own
    // comment was written about.
    {kCamToolSlot, [](const AppState& s) { return s.job != nullptr; },
     [](AppState& s, const SelectionItem& i) { CamFacePicked(s, i); }},
    {kFastenerToolSlot,
     [](const AppState& s) {
       return s.assembly != nullptr && s.fastenerTool.open;
     },
     [](AppState& s, const SelectionItem& i) { FastenerRimPicked(s, i); }},
    {kBearingToolSlot,
     [](const AppState& s) {
       return s.assembly != nullptr && s.bearingTool.open;
     },
     [](AppState& s, const SelectionItem& i) { BearingSeatPicked(s, i); }},
};
} // namespace

std::span<const ToolSession> ToolSessions() { return kToolSessions; }

const ToolSession* ToolSessionFor(uint64_t sentinel) {
  for (const ToolSession& t : kToolSessions)
    if (t.sentinel == sentinel)
      return &t;
  return nullptr;
}

void SlotPickClicked(AppState& state, const SelectionItem& item,
                     uint32_t meshId, uint32_t index) {
  if (!state.slotPick.Active())
    return;
  // One table, both questions (see ToolSession in the header). A session that
  // is not in it falls through to "is this a feature in the active document",
  // which on an assembly tab is never true.
  if (const ToolSession* session = ToolSessionFor(state.slotPick.featureId)) {
    session->consume(state, item);
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

// The Derived editor (docs/modeling/derived.md): which part (or, in an
// in-context edit, which neighbour) the copy is taken from, as one button
// per candidate — a combo gathers with an empty label and cannot be gated —
// plus the reference flag. Choosing a source TAKES the snapshot, so the
// preview shows the copy at once and Accept has something to commit.
void DrawDerivedParams(AppState& state, DerivedFeature& d, size_t index,
                       bool commit) {
  if (d.sourcePart == 0) {
    ImGui::TextDisabled("Source: none yet");
  } else {
    ImGui::Text("Source: %s (id %llu)", d.sourceName.c_str(),
                static_cast<unsigned long long>(d.sourcePart));
    const std::optional<bool> stale = DerivedStale(state, d);
    if (!stale.has_value())
      ImGui::TextDisabled("(source not loaded; freshness unknown)");
    else if (*stale)
      ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                         ICON_FA_TRIANGLE_EXCLAMATION
                         " Stale: the source has changed since this copy");
  }
  const std::vector<DerivedSource> candidates = DerivedSourceCandidates(state);
  if (candidates.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.40f, 1.0f),
                       ICON_FA_TRIANGLE_EXCLAMATION
                       " No other part to derive from — add one (File > New "
                       "Part) or open the assembly and edit this part in "
                       "context");
    ImGui::PopTextWrapPos();
  } else {
    ImGui::TextDisabled("Derive from:");
    for (const DerivedSource& c : candidates) {
      const std::string label =
          (c.occurrenceId ? ICON_FA_OBJECT_GROUP " " : ICON_FA_CUBE " ") +
          c.name + "###derive_src_" + std::to_string(c.slotId) + "_" +
          std::to_string(c.occurrenceId);
      const bool current =
          c.slotId == d.sourcePart &&
          (c.occurrenceId == 0 || d.position != glm::dvec3(0.0));
      if (ImGui::Selectable(label.c_str(), current)) {
        nlohmann::json before = DocumentIO::FeatureToJson(d);
        const std::string why = PointDerivedAt(state, d, c);
        if (why.empty()) {
          if (d.GetName() == "Derived")
            d.SetName(c.name + " (derived)");
          CommitDiscrete(state, d, before, index, commit);
        } else {
          state.sketchEdit.actionMsg = why;
        }
      }
    }
  }
  HandleEdit(state, d,
             ImGui::Checkbox("Reference (a ghost to draw on, not this "
                             "part's matter)",
                             &d.reference),
             index, commit);
  if (d.sourcePart != 0 && ImGui::Button("Update from source")) {
    if (const std::string why = UpdateDerived(state, d.GetId()); !why.empty())
      state.sketchEdit.actionMsg = why;
  }
}

// A template instance's editor (docs/modeling/templates.md#in-the-gui):
// which template (while it is a preview), whether the library has moved on,
// what it stands on, and one row per parameter -- a number (a literal or an
// expression over the part's variables), a string (its choices when the
// template lists them).
void DrawTemplateParams(AppState& state, TemplateFeature& tf, size_t index,
                        bool commit) {
  Document& doc = *state.document;
  if (!commit && !state.projectTemplates.empty()) {
    std::vector<const char*> names;
    int sel = 0;
    for (size_t i = 0; i < state.projectTemplates.size(); i++) {
      names.push_back(state.projectTemplates[i].name.c_str());
      if (state.projectTemplates[i].name == tf.templateName)
        sel = static_cast<int>(i);
    }
    if (LabeledCombo("Template", &sel, names.data(),
                     static_cast<int>(names.size()))) {
      const tmpl::Template& t = state.projectTemplates[sel];
      nlohmann::json params = nlohmann::json::object();
      for (const tmpl::Param& p : t.params)
        if (p.name != "on")
          params[p.name] = p.example;
      const nlohmann::json on = state.templateTool.on;
      try {
        tf.values = tmpl::ValuesFromRequest(t, params, &on);
        tf.templateName = t.name;
        tf.definition = t;
        tf.SetName(t.name);
        tf.ids.clear();
        tf.AssignIds([&doc] { return doc.AllocateId(); });
        state.templateTool.last = t.name;
      } catch (const std::exception& e) {
        state.sketchEdit.actionMsg = e.what();
      }
      doc.Regenerate();
      return; // the rows below are the old template's
    }
  } else {
    ImGui::Text("Template: %s", tf.templateName.c_str());
  }
  ImGui::TextDisabled("Revision %llu, %zu features",
                      static_cast<unsigned long long>(tf.definition.revision),
                      tf.definition.features.size());
  const std::optional<bool> changed = TemplateChanged(state, tf);
  if (!changed.has_value()) {
    ImGui::TextDisabled("(not in this project's library)");
  } else if (*changed) {
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                       ICON_FA_TRIANGLE_EXCLAMATION
                       " Template changed: the library's is not the one "
                       "this expands");
    if (commit && ImGui::Button("Update from template###tpl_update")) {
      if (const std::string why = UpdateTemplateInstance(state, tf.GetId(), {});
          !why.empty())
        state.sketchEdit.actionMsg = why;
      return; // the feature was replaced
    }
  }
  if (!tf.definition.comment.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tf.definition.comment.c_str());
    ImGui::PopTextWrapPos();
  }
  for (const tmpl::Param& p : tf.definition.params) {
    auto it = tf.values.find(p.name);
    if (it == tf.values.end())
      continue;
    tmpl::Value& v = it->second;
    ImGui::PushID(p.name.c_str());
    switch (p.kind) {
    case tmpl::ParamKind::Length:
    case tmpl::ParamKind::Angle:
    case tmpl::ParamKind::Count:
      ExprDim(state, tf, index, commit, p.name.c_str(), v.expr, v.number, -1e9);
      break;
    case tmpl::ParamKind::String: {
      if (!p.choices.empty()) {
        std::vector<const char*> items;
        int sel = 0;
        for (size_t i = 0; i < p.choices.size(); i++) {
          items.push_back(p.choices[i].c_str());
          if (p.choices[i] == v.text)
            sel = static_cast<int>(i);
        }
        const nlohmann::json before = DocumentIO::FeatureToJson(tf);
        if (LabeledCombo(p.name.c_str(), &sel, items.data(),
                         static_cast<int>(items.size()))) {
          v.text = p.choices[sel];
          CommitDiscrete(state, tf, before, index, commit);
        }
      } else {
        static char buf[128];
        static std::string active;
        const std::string key = std::to_string(tf.GetId()) + "/" + p.name;
        if (active != key || !ImGui::IsAnyItemActive())
          std::snprintf(buf, sizeof(buf), "%s", v.text.c_str());
        LeftLabel(p.name.c_str());
        ImGui::InputText("###tpl_text", buf, sizeof(buf));
        if (ImGui::IsItemActivated()) {
          active = key;
          state.editBaseline = DocumentIO::FeatureToJson(tf);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && buf[0] != 0 &&
            v.text != buf) {
          const nlohmann::json before = state.editBaseline;
          v.text = buf;
          CommitDiscrete(state, tf, before, index, commit);
        }
      }
      break;
    }
    case tmpl::ParamKind::Reference: {
      std::string label = v.ref.is_string() ? v.ref.get<std::string>() : "";
      if (v.ref.is_object()) {
        const Feature* f =
            doc.FindFeatureById(v.ref.value("feature_id", uint64_t{0}));
        label = (v.ref.contains("element")
                     ? "face " + v.ref.value("element", std::string{}) + " of "
                     : std::string{}) +
                (f ? f->GetName() : std::string("(missing)"));
      }
      LeftLabel(p.name.c_str());
      ImGui::TextUnformatted(label.c_str());
      break;
    }
    }
    if (!p.comment.empty() && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", p.comment.c_str());
    ImGui::PopID();
  }
  if (commit && ImGui::Button("Unlink (plain features)###tpl_unlink")) {
    if (const std::string why = UnlinkTemplate(state, tf.GetId()); !why.empty())
      state.sketchEdit.actionMsg = why;
  }
}

// The worm's and the wheel's rows (docs/modeling/worm-gears.md): the pair's
// numbers, with what the blank derived and what the pair does — its lead
// angle and whether it self-locks — read back from the last regeneration.
static void DrawWormRows(AppState& state, Feature& feature, size_t index,
                         bool commit) {
  auto* wo = dynamic_cast<WormFeature*>(&feature);
  auto* ww = dynamic_cast<WormWheelFeature*>(&feature);
  double& module = wo ? wo->module : ww->module;
  std::string& moduleExpr = wo ? wo->moduleExpr : ww->moduleExpr;
  int& starts = wo ? wo->starts : ww->starts;
  double& pressure = wo ? wo->pressureDeg : ww->pressureDeg;
  bool& right = wo ? wo->rightHanded : ww->rightHanded;
  const nlohmann::json rep = wo ? wo->ReportJson() : ww->ReportJson();

  ExprDim(state, feature, index, commit, "Module", moduleExpr, module);
  if (module <= 0.0 && rep.is_object())
    ImGui::TextDisabled("from the blank: %.3f", rep.at("module").get<double>());
  const auto count = [&](const char* label, int& v, int lo, int hi) {
    LeftLabel(label);
    const std::string id = std::string("##") + label;
    const bool changed = ExprInputInt(state, id.c_str(), &v);
    v = std::clamp(v, lo, hi);
    HandleEdit(state, feature, changed, index, commit);
  };
  const auto length = [&](const char* label, double& v, const char* zero) {
    LeftLabel(label);
    const std::string id = std::string("##") + label;
    HandleEdit(state, feature,
               ExprDragDouble(state, id.c_str(), &v, 0.01f, 0.0, 1e4,
                              v <= 0.0 ? zero : "%.3f"),
               index, commit);
  };
  if (ww)
    count("Teeth", ww->teeth, 8, 400);
  count("Starts", starts, 1, 8);
  HandleEdit(state, feature, DragDim(state, "Pressure angle", &pressure), index,
             commit);
  {
    nlohmann::json before = DocumentIO::FeatureToJson(feature);
    const char* hands[] = {"Right", "Left"};
    int hand = right ? 0 : 1;
    if (LabeledCombo("Hand", &hand, hands, 2)) {
      right = hand == 0;
      CommitDiscrete(state, feature, before, index, commit);
    }
  }
  if (wo) {
    length("Backlash", wo->backlash, "none");
    length("Length", wo->length, "full face");
  } else {
    length("Worm dia", ww->wormDiameter, "10 modules");
  }
  if (rep.is_object()) {
    if (ww)
      ImGui::TextDisabled("Centre distance %.3f, ratio %.0f:1, throat arc "
                          "%.0f deg",
                          rep.at("centre_distance").get<double>(),
                          rep.at("ratio").get<double>(),
                          rep.at("throat_arc_deg").get<double>());
    else
      ImGui::TextDisabled("Pitch dia %.3f, lead %.3f",
                          rep.at("pitch_diameter").get<double>(),
                          rep.at("lead").get<double>());
    const nlohmann::json& lk = rep.at("locking");
    ImGui::TextDisabled("Lead angle %.2f deg: %s dry, %s greased",
                        rep.at("lead_angle_deg").get<double>(),
                        lk.at(0).at("self_locking").get<bool>() ? "self-locking"
                                                                : "back-drives",
                        lk.at(1).at("self_locking").get<bool>()
                            ? "self-locking"
                            : "back-drives");
  }
  const ElementRef& face = wo ? wo->face : ww->face;
  PickSlotRow(state, feature, PickSlot::GearBlank,
              face.name.empty() ? "" : face.name);
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
    HandleEdit(state, feature,
               DragFloat3Row(state, "Position", &box->position.x), index,
               commit);
    // ExprDim, not DragDim: the "fx" button is what lets a variable drive
    // the dimension, and without it a primitive could not be parametric —
    // which meant a part built from primitives could not be configured.
    ExprDim(state, feature, index, commit, "Width", box->widthExpr, box->width);
    ExprDim(state, feature, index, commit, "Height", box->heightExpr,
            box->height);
    ExprDim(state, feature, index, commit, "Depth", box->depthExpr, box->depth);
  } else if (auto* sphere = dynamic_cast<SphereFeature*>(&feature)) {
    HandleEdit(state, feature,
               DragFloat3Row(state, "Center", &sphere->center.x), index,
               commit);
    ExprDim(state, feature, index, commit, "Radius", sphere->radiusExpr,
            sphere->radius);
  } else if (auto* cyl = dynamic_cast<CylinderFeature*>(&feature)) {
    HandleEdit(state, feature, DragFloat3Row(state, "Center", &cyl->center.x),
               index, commit);
    HandleEdit(state, feature,
               DragFloat3Row(state, "Direction", &cyl->direction.x), index,
               commit);
    ExprDim(state, feature, index, commit, "Radius", cyl->radiusExpr,
            cyl->radius);
    ExprDim(state, feature, index, commit, "Height", cyl->heightExpr,
            cyl->height);
  } else if (auto* import = dynamic_cast<ImportFeature*>(&feature)) {
    ImGui::TextDisabled("File: %s", import->path.c_str());
    // STL and BREP carry no unit (STEP declares its own), so which one the
    // file was drawn in is the user's to say: a metre mesh read as mm is a
    // part 1000x too small that otherwise looks fine.
    if (ImportFeature::TakesUnits(import->path)) {
      const std::vector<const char*> names = units::LengthNames();
      int sel = 0;
      for (size_t i = 0; i < names.size(); i++)
        if (std::string(names[i]) == units::ToString(import->lengthUnit))
          sel = static_cast<int>(i);
      const nlohmann::json before = DocumentIO::FeatureToJson(*import);
      if (LabeledCombo("File units", &sel, names.data(),
                       static_cast<int>(names.size())) &&
          units::LengthFromString(names[sel], import->lengthUnit))
        CommitDiscrete(state, feature, before, index, commit);
    }
  } else if (auto* d = dynamic_cast<DerivedFeature*>(&feature)) {
    DrawDerivedParams(state, *d, index, commit);
  } else if (auto* tf = dynamic_cast<TemplateFeature*>(&feature)) {
    DrawTemplateParams(state, *tf, index, commit);
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
      AlignCameraToSketch(state, sk->plane);
    }
  } else if (auto* dp = dynamic_cast<DatumPlaneFeature*>(&feature)) {
    DrawDatumPlaneParams(state, *dp, index, commit);
  } else if (auto* da = dynamic_cast<DatumAxisFeature*>(&feature)) {
    DrawDatumAxisParams(state, *da, index, commit);
  } else if (auto* fr = dynamic_cast<FrameFeature*>(&feature)) {
    DrawFrameParams(state, *fr, index, commit);
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
    // A twist is a blind or symmetric extrude's (ExtrudeFeature::
    // WhyIncomplete refuses the rest, and says so in the window).
    if (ext->endMode == ExtrudeEnd::Blind ||
        ext->endMode == ExtrudeEnd::Symmetric) {
      ExprDim(state, feature, index, commit, "Twist (deg)", ext->twistExpr,
              ext->twistDeg, /*minValue=*/-1e6); // either hand
      if (ext->twistDeg != 0.0 || !ext->twistExpr.empty())
        HandleEdit(state, feature,
                   ImGui::Checkbox("Herringbone", &ext->herringbone), index,
                   commit);
    }
    // A taper is a blind solid extrude's
    // (docs/modeling/README.md#tapered-extrudes); WhyIncomplete says so for
    // the rest.
    if (ext->endMode == ExtrudeEnd::Blind && !ext->thin) {
      ExprDim(state, feature, index, commit, "Taper (deg)", ext->taperExpr,
              ext->taperDeg, /*minValue=*/-89.9);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every wall leans in by this many degrees from the "
                          "extrude direction (negative leans out); 45 prints "
                          "the underside of a boss or a cam lobe");
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
    // What the hole is FOR (docs/modeling/holes-and-threads.md#hole). Chosen
    // from the vendored table's own list, so the picker cannot offer a size the
    // lookup would refuse — and picking one DERIVES the diameter, which is why
    // the diameter row goes read-only rather than staying an editable number
    // that the next regeneration would silently overwrite.
    {
      static std::vector<std::string> sizes; // the table is immutable
      if (sizes.empty()) {
        sizes.push_back("(plain diameter)");
        for (const std::string& sz : parts::ClearanceSizes(nullptr))
          sizes.push_back(sz);
      }
      std::vector<const char*> items;
      items.reserve(sizes.size());
      for (const std::string& sz : sizes)
        items.push_back(sz.c_str());
      int pick = 0;
      for (size_t i = 1; i < sizes.size(); i++)
        if (sizes[i] == ho->fitThread)
          pick = static_cast<int>(i);
      if (LabeledCombo("Drilled for", &pick, items.data(),
                       static_cast<int>(items.size()))) {
        nlohmann::json before = DocumentIO::FeatureToJson(*ho);
        ho->fitThread = pick == 0 ? std::string{} : sizes[pick];
        if (!ho->fitThread.empty() && ho->fitKind.empty())
          ho->fitKind = "normal"; // the fit a clearance hole is unless said
        CommitDiscrete(state, feature, before, index, commit);
      }
      if (!ho->fitThread.empty()) {
        const char* fits[] = {"close", "normal", "loose", "tapped"};
        int f = 1;
        for (int i = 0; i < 4; i++)
          if (ho->fitKind == fits[i])
            f = i;
        if (LabeledCombo("Fit", &f, fits, 4)) {
          nlohmann::json before = DocumentIO::FeatureToJson(*ho);
          ho->fitKind = fits[f];
          CommitDiscrete(state, feature, before, index, commit);
        }
      }
    }
    ImGui::BeginDisabled(!ho->fitThread.empty());
    ExprDim(state, feature, index, commit, "Diameter", ho->diameterExpr,
            ho->diameter);
    ImGui::EndDisabled();
    if (!ho->fitThread.empty() &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Derived from the fit — set 'Drilled for' back to a "
                        "plain diameter to type one");
    }
    HandleEdit(state, feature, ImGui::Checkbox("Through all", &ho->through),
               index, commit);
    // How a hole across the print direction prints
    // (docs/modeling/holes-and-threads.md#printed-holes).
    {
      const char* printedNames[] = {"Round", "Teardrop", "Teardrop, flat"};
      int pr = std::to_underlying(ho->printed);
      if (LabeledCombo("Printed", &pr, printedNames, 3)) {
        nlohmann::json before = DocumentIO::FeatureToJson(*ho);
        ho->printed = static_cast<HolePrinted>(pr);
        CommitDiscrete(state, feature, before, index, commit);
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A hole across the print direction: its roof "
                          "raised to a 45-degree point, or to a flat no "
                          "wider than the process bridges, so it prints "
                          "without sagging. Needs the part's print process");
    }
    if (!ho->through)
      ExprDim(state, feature, index, commit, "Depth", ho->depthExpr, ho->depth);
    // The floor's drill point and a tapped hole's thread depth, each 0 to
    // derive it (docs/modeling/holes-and-threads.md#hole).
    if (!ho->through) {
      HandleEdit(state, feature,
                 ExprInputDouble(state, "Drill point (deg)", &ho->pointAngleDeg,
                                 "%.1f"),
                 index, commit);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The point's included angle at the floor; 0 "
                          "derives it (now %.0f: %s), 180 is flat",
                          ho->PointAngleUsed(),
                          ho->fitThread.empty()
                              ? "a plain diameter keeps a flat floor"
                              : "a jobber drill's, as a fit's drill is");
    }
    if (ho->fitKind == "tapped" && !ho->fitThread.empty()) {
      ExprDim(state, feature, index, commit, "Thread depth",
              ho->threadDepthExpr, ho->threadDepth);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How deep it is threaded from the mouth; 0 derives "
                          "it (now %.2f: %s)",
                          ho->ThreadDepthUsed(),
                          ho->through ? "the whole bore"
                                      : "the depth less a tap's lead, 3 "
                                        "pitches");
    }
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
      HandleEdit(state, feature,
                 DragFloat3Row(state, "Origin", &cv->axisOrigin.x), index,
                 commit);
      HandleEdit(state, feature, DragFloat3Row(state, "Axis", &cv->axisDir.x),
                 index, commit);
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
        HandleEdit(state, feature, ExprInputInt(state, "##Degree", &cv->degree),
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
                   ExprDragFloat3(state, "##pt", &cv->points[p].x, 0.05f),
                   index, commit);
        ImGui::PopID();
      }
      if (ImGui::Button("Add point")) {
        nlohmann::json before = DocumentIO::FeatureToJson(*cv);
        const glm::dvec3 last =
            cv->points.empty() ? glm::dvec3(0.0) : cv->points.back();
        cv->points.push_back(last + glm::dvec3(1.0, 0.0, 0.0));
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
               ExprDragDouble(state, "##Wall", &pi->wallThickness, 0.02f, wmin,
                              wmax,
                              pi->wallThickness <= 0.0 ? "solid" : "%.3f"),
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
  } else if (dynamic_cast<WormFeature*>(&feature) != nullptr ||
             dynamic_cast<WormWheelFeature*>(&feature) != nullptr) {
    DrawWormRows(state, feature, index, commit);
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
    if (const auto row = threads::Lookup(th->designation)) {
      // The table's pitch is the only one a table thread takes. A stored
      // one that is not (a file written by hand) is the feature's error,
      // and shown as what it is -- never the table's number over another.
      LeftLabel("Pitch");
      if (threads::PitchError(*row, th->designation, th->pitch).empty()) {
        ImGui::Text("%.4g (from table)", th->pitch);
      } else {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f),
                           "%.4g -- %s is %.4g", th->pitch,
                           row->designation.c_str(), row->pitch);
        ImGui::SameLine();
        if (ImGui::SmallButton("Use the table's")) {
          nlohmann::json before = DocumentIO::FeatureToJson(*th);
          th->pitch = row->pitch;
          CommitDiscrete(state, feature, before, index, commit);
        }
      }
    } else {
      HandleEdit(state, feature, DragDim(state, "Pitch", &th->pitch), index,
                 commit);
      // A custom thread's flank angle: half the thread angle, 30 the ISO
      // profile, 45 one whose every flank prints without support
      // (docs/modeling/holes-and-threads.md#flank-angle). A table thread is
      // the standard's 30 and shows none.
      LeftLabel("Flank angle");
      HandleEdit(state, feature,
                 ExprDragDouble(state, "##Flank angle", &th->flankDeg, 0.25f,
                                threads::kMinFlankDeg, threads::kMaxFlankDeg,
                                "%.1f deg"),
                 index, commit);
    }
    {
      LeftLabel("Length");
      static const double lmin = 0.0, lmax = 1e6;
      HandleEdit(state, feature,
                 ExprDragDouble(state, "##Length", &th->length, 0.05f, lmin,
                                lmax, th->length <= 0.0 ? "full face" : "%.3f"),
                 index, commit);
    }
    // The class: the series' own classes for the side the face is on (the
    // overlay knows which once the face resolves), or none.
    if (const auto spec = threads::Lookup(th->designation)) {
      const bool external = !th->overlay.valid || th->overlay.external;
      const auto& classes = threads::Classes(spec->series, external);
      LeftLabel("Class");
      if (ImGui::BeginCombo("###ThreadClass", th->threadClass.empty()
                                                  ? "(none)"
                                                  : th->threadClass.c_str())) {
        if (ImGui::Selectable("(none)", th->threadClass.empty())) {
          nlohmann::json before = DocumentIO::FeatureToJson(*th);
          th->threadClass.clear();
          CommitDiscrete(state, feature, before, index, commit);
        }
        for (const std::string& c : classes) {
          if (ImGui::Selectable(c.c_str(), th->threadClass == c)) {
            nlohmann::json before = DocumentIO::FeatureToJson(*th);
            th->threadClass = c;
            CommitDiscrete(state, feature, before, index, commit);
          }
        }
        ImGui::EndCombo();
      }
    }
    {
      // Modelled cuts the real helical groove (slow on a long thread, which
      // is why cosmetic stays the default); the hand is the helix's.
      nlohmann::json before = DocumentIO::FeatureToJson(*th);
      if (ImGui::Checkbox("Modeled", &th->modeled))
        CommitDiscrete(state, feature, before, index, commit);
      const char* hands[] = {"Right", "Left"};
      int hand = th->rightHanded ? 0 : 1;
      if (LabeledCombo("Hand", &hand, hands, 2)) {
        th->rightHanded = hand == 0;
        CommitDiscrete(state, feature, before, index, commit);
      }
    }
    PickSlotRow(state, feature, PickSlot::ThreadFace,
                th->face.name.empty() ? "" : th->face.name);
    // On a printed part: what the print wants this thread modelled at, the
    // same report add_feature returns
    // (docs/tolerances/README.md#printed-parts).
    if (state.document != nullptr)
      if (const auto r = PrintedThreadFor(*state.document, *th)) {
        ImGui::TextWrapped("%s", r->Sentence().c_str());
        for (const std::string& n : r->notes)
          ImGui::TextDisabled("%s", n.c_str());
      }
  } else if (auto* rb = dynamic_cast<RibFeature*>(&feature)) {
    ExprDim(state, feature, index, commit, "Thickness", rb->thicknessExpr,
            rb->thickness);
    HandleEdit(state, feature, ImGui::Checkbox("Flip", &rb->flipDirection),
               index, commit);
    PickSlotRow(state, feature, PickSlot::RibBody,
                rb->bodyId == 0 ? "(nearest)"
                                : BodyRefLabel(state, rb->bodyId));
  } else if (auto* bf = dynamic_cast<BaseFlangeFeature*>(&feature)) {
    // No thickness row: a blank is cut from ONE sheet, so the gauge belongs
    // to the document and not to this feature. The Sheet Metal panel is where
    // it is changed.
    HandleEdit(state, feature, ImGui::Checkbox("Other side", &bf->reversed),
               index, commit);
  } else if (auto* ef = dynamic_cast<EdgeFlangeFeature*>(&feature)) {
    ExprDim(state, feature, index, commit, "Length", ef->lengthExpr,
            ef->length);
    const char* modes[] = {"Flat wall", "To outside mold line",
                           "To inside mold line"};
    int mode = std::to_underlying(ef->lengthMode);
    if (LabeledCombo("Length is", &mode, modes, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*ef);
      ef->lengthMode = static_cast<FlangeLengthMode>(mode);
      CommitDiscrete(state, feature, before, index, commit);
    }
    ExprDim(state, feature, index, commit, "Angle", ef->angleDegExpr,
            ef->angleDeg);
    // 0 means "the document's", which is itself the material's minimum — so
    // the field shows what it will USE rather than a bare zero.
    ExprDim(state, feature, index, commit, "Radius", ef->radiusExpr,
            ef->radius);
    const char* positions[] = {"Material inside", "Material outside",
                               "Bend outside"};
    int pos = std::to_underlying(ef->position);
    if (LabeledCombo("Position", &pos, positions, 3)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*ef);
      ef->position = static_cast<FlangePosition>(pos);
      CommitDiscrete(state, feature, before, index, commit);
    }
    HandleEdit(state, feature,
               ImGui::Checkbox("Close corners", &ef->closedCorners), index,
               commit);
    ExprDim(state, feature, index, commit, "Inset start", ef->offsetStartExpr,
            ef->offsetStart);
    ExprDim(state, feature, index, commit, "Inset end", ef->offsetEndExpr,
            ef->offsetEnd);
    PickSlotRow(
        state, feature, PickSlot::EdgeFlangeEdges,
        ef->edges.empty() ? "" : std::to_string(ef->edges.size()) + " picked");
    RefListRows(state, feature, index, commit, ef->edges);
  } else if (auto* nc = dynamic_cast<NormalCutFeature*>(&feature)) {
    DrawFeatureRefCombo(state, feature, "Profile", &nc->sketchId, index, commit,
                        /*allowNone=*/false, AcceptClosedSketch);
    ImGui::TextDisabled(nc->bodyId == 0 ? "Cuts the body it lands on"
                                        : "Cuts the named body");
  } else if (auto* cr = dynamic_cast<CornerReliefFeature*>(&feature)) {
    PickSlotRow(state, feature, PickSlot::CornerReliefBody,
                BodyRefLabel(state, cr->bodyId));
    const char* styles[] = {"Circular", "Rectangular"};
    int style = std::to_underlying(cr->style);
    if (LabeledCombo("Shape", &style, styles, 2)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*cr);
      cr->style = static_cast<CornerReliefStyle>(style);
      CommitDiscrete(state, feature, before, index, commit);
    }
    // 0 is "from the bend": radius plus thickness, what clears both bends.
    ExprDim(state, feature, index, commit, "Size", cr->sizeExpr, cr->size);
  } else if (auto* hm = dynamic_cast<HemFeature*>(&feature)) {
    ExprDim(state, feature, index, commit, "Return", hm->lengthExpr,
            hm->length);
    const char* kinds[] = {"Closed", "Open"};
    int kind = std::to_underlying(hm->kind);
    if (LabeledCombo("Kind", &kind, kinds, 2)) {
      nlohmann::json before = DocumentIO::FeatureToJson(*hm);
      hm->kind = static_cast<HemKind>(kind);
      CommitDiscrete(state, feature, before, index, commit);
    }
    if (hm->kind == HemKind::Open)
      ExprDim(state, feature, index, commit, "Gap", hm->gapExpr, hm->gap);
    HandleEdit(state, feature,
               ImGui::Checkbox("Outside the edge", &hm->outside), index,
               commit);
    // Where the hem stops short of each end of the edge: a hem running the
    // whole length of a pan's wall fuses to the wall beside it.
    ExprDim(state, feature, index, commit, "Inset start", hm->offsetStartExpr,
            hm->offsetStart);
    ExprDim(state, feature, index, commit, "Inset end", hm->offsetEndExpr,
            hm->offsetEnd);
    PickSlotRow(
        state, feature, PickSlot::HemEdges,
        hm->edges.empty() ? "" : std::to_string(hm->edges.size()) + " picked");
    RefListRows(state, feature, index, commit, hm->edges);
  } else if (auto* jg = dynamic_cast<JogFeature*>(&feature)) {
    // The line is not a pick in the viewport: it is the sketch drawn on the
    // panel, and the combo offers only sketches that hold exactly one.
    DrawFeatureRefCombo(state, feature, "Line", &jg->sketchId, index, commit,
                        /*allowNone=*/false, AcceptJogLine);
    ExprDim(state, feature, index, commit, "Height", jg->heightExpr,
            jg->height);
    ExprDim(state, feature, index, commit, "Angle", jg->angleDegExpr,
            jg->angleDeg);
    HandleEdit(state, feature, ImGui::Checkbox("Other side", &jg->flip), index,
               commit);
  } else if (auto* uf = dynamic_cast<UnfoldFeature*>(&feature)) {
    PickSlotRow(state, feature, PickSlot::UnfoldBody,
                BodyRefLabel(state, uf->bodyId));
    ImGui::TextDisabled(uf->bendFeatures.empty() ? "Every bend of the body"
                                                 : "The bends of %zu flange(s)",
                        uf->bendFeatures.size());
  } else if (auto* rf = dynamic_cast<RefoldFeature*>(&feature)) {
    // Which Unfold it closes is not a pick: it is the one still open, and a
    // Refold naming any other would fold up underneath a newer flat.
    const Feature* target = state.document->FindFeatureById(rf->unfoldId);
    ImGui::TextUnformatted(target != nullptr
                               ? ("Folds back: " + target->GetName()).c_str()
                               : "No open Unfold to fold back");
  } else if (auto* fp = dynamic_cast<FlatPatternFeature*>(&feature)) {
    PickSlotRow(state, feature, PickSlot::FlatPatternBody,
                BodyRefLabel(state, fp->bodyId));
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
      PlaneRefCombo(state, feature, sp->datumId, sp->basePlane, index, commit,
                    [&] { RecenterSplitOffset(state, *sp); });
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
