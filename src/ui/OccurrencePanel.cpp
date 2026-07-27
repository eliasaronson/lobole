#include "ui/AppActions.h"
#include "ui/FeatureEditor.h" // PickSlot, kMateSessionSlot (mate slot rows)
#include "ui/FileDialogs.h"
#include "ui/PreviewFeature.h" // BeginToolWindowFrame (mate session window)
#include "ui/PropertiesPanel.h" // DrawMeasureSection (shared readout)

#include "imgui.h"

#include <glm/gtc/quaternion.hpp> // eulerAngles, quat(vec3)
#include <glm/trigonometric.hpp>  // degrees, radians

#include <algorithm>
#include <cstring>
#include <string>

namespace ui {

namespace {
std::string PartNameById(const AppState& state, uint64_t partId) {
  for (const PartSlot& s : state.parts)
    if (s.kind == DocKind::Part && s.id == partId)
      return s.name;
  return "(missing part)";
}

const char* MateTypeLabel(MateType t) {
  switch (t) {
  case MateType::Fastened:
    return "fastened";
  case MateType::Revolute:
    return "revolute";
  case MateType::Slider:
    return "slider";
  case MateType::Cylindrical:
    return "cylindrical";
  case MateType::Planar:
    return "planar";
  case MateType::Ball:
    return "ball";
  }
  return "?";
}

// The name of the occurrence an id refers to (its label, else its part name).
std::string OccLabel(const AppState& state, const AssemblyDocument& asmDoc,
                     uint64_t occId) {
  const Occurrence* o = asmDoc.FindById(occId);
  if (!o)
    return "(missing)";
  return o->name.empty() ? PartNameById(state, o->partId) : o->name;
}

// The mate list and its selected-mate editor. The create/edit session's
// controls (connector slot rows, flip, accept/cancel) live in the floating
// Tool window (DrawMateToolWindow below, U7c).
void DrawMatesSection(AppState& state) {
  AssemblyDocument& asmDoc = *state.assembly;

  if (state.mateTool.active && state.mateTool.previewMateId)
    return; // the Tool window owns the mid-preview UI; hide the transient mate

  ImGui::SeparatorText("Mates");
  if (asmDoc.MateCount() == 0) {
    ImGui::TextDisabled("None. Use the mate tools in the toolbar.");
    return;
  }

  for (const Mate& mate : asmDoc.GetMates()) {
    ImGui::PushID(static_cast<int>(mate.id));
    const std::string label =
        (mate.name.empty() ? std::string(MateTypeLabel(mate.type))
                           : mate.name) +
        "  [" + MateTypeLabel(mate.type) + "]";
    if (!mate.error.empty()) {
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "%s", "(!)");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", mate.error.c_str());
      ImGui::SameLine();
    }
    if (ImGui::Selectable(label.c_str(), SelectedMateId(state) == mate.id))
      SelectMate(state, mate.id);
    ImGui::PopID();
  }

  Mate* sel = SelectedMateId(state) ? asmDoc.FindMateById(SelectedMateId(state))
                                    : nullptr;
  if (!sel)
    return;

  ImGui::SeparatorText("Selected mate");
  ImGui::Text("%s -> %s", OccLabel(state, asmDoc, sel->a.occurrenceId).c_str(),
              OccLabel(state, asmDoc, sel->b.occurrenceId).c_str());

  char name[128];
  std::snprintf(name, sizeof(name), "%s", sel->name.c_str());
  if (ImGui::InputText("Name##mate", name, sizeof(name),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    SnapshotAssembly(state);
    sel->name = name;
    asmDoc.BumpVersion();
  }

  if (ImGui::Checkbox("Flip##mate", &sel->flipped)) {
    SnapshotAssembly(state);
    SolveActiveAssembly(state);
  }

  // The DOF controls: which fields are live depends on the mate type. Editing
  // them IS moving the mechanism numerically (the pre-gizmo stand-in). Each
  // snapshots once per drag (IsItemActivated) and re-solves on change.
  auto angleDof = [&](const char* label) {
    float deg = glm::degrees(sel->rotation);
    const bool changed = ImGui::DragFloat(label, &deg, 1.0f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed) {
      sel->rotation = glm::radians(deg);
      SolveActiveAssembly(state);
    }
  };
  auto offsetDof = [&](const char* label) {
    const bool changed = ImGui::DragFloat(label, &sel->offset, 0.1f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed)
      SolveActiveAssembly(state);
  };
  switch (sel->type) {
  case MateType::Revolute:
    angleDof("Angle (deg)");
    break;
  case MateType::Slider:
    offsetDof("Offset");
    break;
  case MateType::Cylindrical:
    angleDof("Angle (deg)");
    offsetDof("Offset");
    break;
  case MateType::Planar: {
    float dxy[2] = {sel->dx, sel->dy};
    const bool changed = ImGui::DragFloat2("Slide (x,y)", dxy, 0.1f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed) {
      sel->dx = dxy[0];
      sel->dy = dxy[1];
      SolveActiveAssembly(state);
    }
    angleDof("Spin (deg)");
    break;
  }
  case MateType::Ball: {
    glm::vec3 euler = glm::degrees(glm::eulerAngles(sel->ballRotation));
    const bool changed = ImGui::DragFloat3("Rotation (deg)", &euler.x, 1.0f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed) {
      sel->ballRotation = glm::quat(glm::radians(euler));
      SolveActiveAssembly(state);
    }
    break;
  }
  case MateType::Fastened:
    break; // no DOF
  }

  if (!sel->error.empty())
    ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "%s",
                       sel->error.c_str());

  // Re-pick a connector face (U7c): opens the mate session on this mate —
  // click a connector row in the Tool window, then pick in the viewport.
  if (ImGui::Button("Edit connectors…"))
    StartMateEdit(state, sel->id);

  if (ImGui::Button("Delete mate")) {
    const uint64_t id = sel->id;
    SnapshotAssembly(state);
    asmDoc.RemoveMate(id);
    SelectMate(state, 0);
    SolveActiveAssembly(state);
  }
}
} // namespace

// The occurrence list + properties editor, drawn in the "Model" panel (where a
// part tab shows its feature tree) when an assembly tab is active. Edits
// snapshot for undo; a drag is one undo step (snapshot on IsItemActivated).
// Exploded views (A4): persistent named views; the ACTIVE one + factor are
// transient display state. Activating (or sliding the factor) never touches
// the document — the offset composes into rendered transforms only.
void DrawExplodedSection(AppState& state) {
  AssemblyDocument& asmDoc = *state.assembly;
  ImGui::SeparatorText("Exploded views");

  // "None" + one selectable per view; the active one drives the display.
  if (ImGui::Selectable("None##explode", state.activeExplodedView == 0)) {
    state.activeExplodedView = 0;
    state.syncedDocumentVersion = UINT64_MAX; // display change: force resync
  }
  uint64_t deleteId = 0;
  for (ExplodedView& v : asmDoc.GetExplodedViews()) {
    ImGui::PushID(static_cast<int>(v.id));
    const bool active = state.activeExplodedView == v.id;
    if (ImGui::Selectable(v.name.empty() ? "(unnamed)" : v.name.c_str(),
                          active)) {
      state.activeExplodedView = v.id;
      state.syncedDocumentVersion = UINT64_MAX;
    }
    ImGui::PopID();
  }
  if (ImGui::SmallButton("Add view")) {
    SnapshotAssembly(state);
    ExplodedView v;
    v.name = "Exploded " + std::to_string(asmDoc.GetExplodedViews().size() + 1);
    state.activeExplodedView = asmDoc.AddExplodedView(std::move(v));
    state.syncedDocumentVersion = UINT64_MAX;
  }

  if (ExplodedView* sel =
          asmDoc.FindExplodedViewById(state.activeExplodedView)) {
    char name[128];
    snprintf(name, sizeof name, "%s", sel->name.c_str());
    if (ImGui::InputText("Name##explode", name, sizeof name,
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      SnapshotAssembly(state);
      sel->name = name;
      asmDoc.BumpVersion();
    }
    // Factor slider: cheap live path (matrix updates only); picking catches
    // up on the resync forced when the slider is released.
    if (ImGui::SliderFloat("Factor", &state.explodeFactor, 0.0f, 1.0f))
      ApplyExplodeTransforms(state);
    if (ImGui::IsItemDeactivatedAfterEdit())
      state.syncedDocumentVersion = UINT64_MAX;
    if (ImGui::SmallButton("Auto-explode")) { // radial seed, then drag to taste
      if (AutoExplode(state, sel->id))
        state.syncedDocumentVersion = UINT64_MAX;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete view")) {
      SnapshotAssembly(state);
      asmDoc.RemoveExplodedView(sel->id);
      state.activeExplodedView = 0;
      state.syncedDocumentVersion = UINT64_MAX;
      asmDoc.BumpVersion();
    }
    ImGui::TextDisabled("Drag the gizmo to set an occurrence's offset.");
  }
}

// Bill of materials (A4): derived rows per referenced part. The mass math
// (OCC volume properties per body) is not per-frame cheap, so the table is
// Assembly configurations (C4): named variants that suppress occurrences.
// The active selector switches the shown variant; the active config's editor
// is a per-occurrence suppression checklist.
void DrawConfigsSection(AppState& state) {
  AssemblyDocument& asmDoc = *state.assembly;
  ImGui::SeparatorText("Configurations");
  const uint64_t active = asmDoc.ActiveConfiguration();

  if (ImGui::Selectable("Base (all shown)", active == 0)) {
    SnapshotAssembly(state);
    asmDoc.SetActiveConfiguration(0);
  }
  for (const Configuration& c : asmDoc.GetConfigurations()) {
    ImGui::PushID(1000 + static_cast<int>(c.id));
    if (ImGui::Selectable(c.name.c_str(), active == c.id)) {
      SnapshotAssembly(state);
      asmDoc.SetActiveConfiguration(c.id);
    }
    ImGui::PopID();
  }
  if (ImGui::Button("+ Add configuration")) {
    SnapshotAssembly(state);
    const uint64_t id = asmDoc.AddConfiguration().id;
    asmDoc.SetActiveConfiguration(id);
  }

  Configuration* cfg = asmDoc.FindConfiguration(active);
  if (!cfg)
    return; // base

  char nm[128];
  std::snprintf(nm, sizeof(nm), "%s", cfg->name.c_str());
  if (ImGui::InputText("Config name", nm, sizeof(nm),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    SnapshotAssembly(state);
    asmDoc.FindConfiguration(active)->name = nm;
    asmDoc.BumpVersion();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Delete##cfg")) {
    SnapshotAssembly(state);
    asmDoc.RemoveConfiguration(active);
    return;
  }
  ImGui::TextUnformatted("Suppressed occurrences");
  for (const Occurrence& o : asmDoc.GetOccurrences()) {
    bool sup = cfg->Suppresses(o.id);
    ImGui::PushID(2000 + static_cast<int>(o.id));
    if (ImGui::Checkbox(OccLabel(state, asmDoc, o.id).c_str(), &sup)) {
      SnapshotAssembly(state);
      auto& v = asmDoc.FindConfiguration(active)->suppressedFeatures;
      v.erase(std::remove(v.begin(), v.end(), o.id), v.end());
      if (sup)
        v.push_back(o.id);
      asmDoc.BumpVersion();
    }
    ImGui::PopID();
  }
}

// cached and recomputed only when the assembly or a referenced part changed.
void DrawBomSection(AppState& state) {
  ImGui::Spacing();
  if (!ImGui::CollapsingHeader("Bill of materials"))
    return;

  static BomTable cache;
  static const void* cachedAsm = nullptr;
  static uint64_t cachedKey = 0;
  // While an assembly tab is active every part doc is stashed in its slot, so
  // this folds all referenced-part versions plus the assembly's own.
  uint64_t key = state.assembly->Version();
  for (const PartSlot& s : state.parts)
    if (s.kind == DocKind::Part && s.doc)
      key = key * 1315423911ull + s.doc->Version();
  if (cachedAsm != static_cast<const void*>(state.assembly.get()) ||
      cachedKey != key) {
    cache = ComputeBom(state);
    cachedAsm = state.assembly.get();
    cachedKey = key;
  }

  if (cache.rows.empty()) {
    ImGui::TextDisabled("No occurrences.");
    return;
  }
  if (ImGui::BeginTable("bom", 5,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Part");
    ImGui::TableSetupColumn("Qty");
    ImGui::TableSetupColumn("Material");
    ImGui::TableSetupColumn("Unit g");
    ImGui::TableSetupColumn("Total g");
    ImGui::TableHeadersRow();
    for (const BomRow& r : cache.rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.partName.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%d", r.quantity);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.material.empty() ? "-" : r.material.c_str());
      ImGui::TableNextColumn();
      if (r.massKnown)
        ImGui::Text("%.2f", r.unitMassG);
      else
        ImGui::TextDisabled("?"); // no material/density assigned
      ImGui::TableNextColumn();
      if (r.massKnown)
        ImGui::Text("%.2f", r.unitMassG * r.quantity);
      else
        ImGui::TextDisabled("?");
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Total");
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    if (cache.massKnown)
      ImGui::Text("%.2f", cache.totalMassG);
    else
      ImGui::TextDisabled("?");
    ImGui::EndTable();
  }
  if (ImGui::Button("Export CSV"))
    ExportBomDialog(state);
}

void DrawOccurrencePanel(AppState& state) {
  ImGui::Begin("Model");
  if (!state.assembly) {
    ImGui::End();
    return;
  }
  AssemblyDocument& asmDoc = *state.assembly;

  ImGui::SeparatorText("Occurrences");
  if (asmDoc.OccurrenceCount() == 0)
    ImGui::TextDisabled("Empty. Place parts with add_occurrence.");

  for (const Occurrence& o : asmDoc.GetOccurrences()) {
    ImGui::PushID(static_cast<int>(o.id));
    bool vis = o.visible;
    if (ImGui::Checkbox("##vis", &vis)) {
      SnapshotAssembly(state);
      asmDoc.FindById(o.id)->visible = vis;
      asmDoc.BumpVersion();
    }
    ImGui::SameLine();
    const std::string label =
        (o.name.empty() ? PartNameById(state, o.partId) : o.name) +
        (o.fixed ? "  (fixed)" : "");
    if (ImGui::Selectable(label.c_str(), SelectedOccurrenceId(state) == o.id))
      SelectOccurrence(state, o.id);
    ImGui::PopID();
  }

  Occurrence* sel = SelectedOccurrenceId(state)
                        ? asmDoc.FindById(SelectedOccurrenceId(state))
                        : nullptr;
  if (sel) {
    ImGui::SeparatorText("Selected occurrence");
    ImGui::Text("Part: %s", PartNameById(state, sel->partId).c_str());

    char name[128];
    std::snprintf(name, sizeof(name), "%s", sel->name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      SnapshotAssembly(state);
      sel->name = name;
      asmDoc.BumpVersion();
    }

    glm::vec3 pos = sel->position;
    const bool posChanged = ImGui::DragFloat3("Position", &pos.x, 0.1f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state); // one snapshot per drag session
    if (posChanged) {
      sel->position = pos;
      asmDoc.BumpVersion();
    }

    glm::vec3 euler = glm::degrees(glm::eulerAngles(sel->orientation));
    const bool rotChanged = ImGui::DragFloat3("Rotation (deg)", &euler.x, 1.0f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (rotChanged) {
      sel->orientation = glm::quat(glm::radians(euler));
      asmDoc.BumpVersion();
    }

    bool fixed = sel->fixed;
    if (ImGui::Checkbox("Fixed (grounded)", &fixed)) {
      SnapshotAssembly(state);
      sel->fixed = fixed;
      asmDoc.BumpVersion();
    }

    if (ImGui::Button("Edit in context")) { // A4: part tab, assembly ghosted
      EnterInContextEdit(state, sel->id);
      ImGui::End();
      return; // the panel just switched to a part tab
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete occurrence")) {
      const uint64_t id = sel->id;
      SnapshotAssembly(state);
      asmDoc.RemoveOccurrence(id);
      SelectOccurrence(state, 0);
      SolveActiveAssembly(state); // a mate referencing it now dangles/frees
    }
  }

  DrawMeasureSection(state);
  DrawMatesSection(state);
  DrawConfigsSection(state);
  DrawExplodedSection(state);
  DrawBomSection(state);

  ImGui::End();
}

// The mate session's Tool-window contents (U7c): two connector slot rows
// (same "###slotN" convention as the feature dialogs), then the preview's
// flip/error/accept/cancel — or a Done button for a re-pick session.
void DrawMateToolWindow(AppState& state) {
  auto& tool = state.mateTool;
  AssemblyDocument& asmDoc = *state.assembly;
  Mate* editM =
      tool.editMateId ? asmDoc.FindMateById(tool.editMateId) : nullptr;
  Mate* pm =
      tool.previewMateId ? asmDoc.FindMateById(tool.previewMateId) : nullptr;
  if (tool.editMateId != 0 && editM == nullptr) {
    CancelMateTool(state); // the edited mate vanished (undo/delete)
    return;
  }

  BeginToolWindowFrame(state, editM != nullptr
                                  ? std::string("Edit Mate")
                                  : std::string(MateTypeLabel(tool.type)));

  // A connector as a slot-row value ("Bracket-2", "+ snapped edge").
  const auto connLabel = [&](const Connector& c, bool have) -> std::string {
    if (!have || c.occurrenceId == 0)
      return "";
    std::string s = OccLabel(state, asmDoc, c.occurrenceId);
    if (c.anchor.snapEdge.IsSet())
      s += " (snapped)";
    return s;
  };
  Connector a{}, b{};
  bool haveA = false, haveB = false;
  if (editM != nullptr) {
    a = editM->a;
    b = editM->b;
    haveA = haveB = true;
  } else if (pm != nullptr) {
    a = pm->a;
    b = pm->b;
    haveA = haveB = true;
  } else {
    a = tool.first;
    haveA = tool.haveFirst;
  }

  // Slot rows: click to (re)arm; the armed row shows in the accent color.
  // Mid-preview re-picking is locked (Cancel first) — except in edit mode,
  // where re-picking IS the session.
  const bool rowsLive = editM != nullptr || pm == nullptr;
  const auto slotRow = [&](PickSlot slot, const char* label,
                           const std::string& value) {
    const bool armed = state.slotPick.featureId == kMateSessionSlot &&
                       state.slotPick.slot == static_cast<int>(slot);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
    if (armed) {
      const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImVec4(accent.x, accent.y, accent.z, 0.35f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(accent.x, accent.y, accent.z, 0.55f));
    }
    const std::string text =
        (value.empty() ? std::string(armed ? "(pick in viewport)" : "(none)")
                       : value) +
        "###slot" + std::to_string(static_cast<int>(slot));
    if (ImGui::Button(text.c_str(), ImVec2(ImGui::GetFontSize() * 14.0f, 0)) &&
        rowsLive) {
      if (armed)
        state.slotPick = {};
      else
        ArmMateConnector(state, slot == PickSlot::MateConnectorB);
    }
    if (armed)
      ImGui::PopStyleColor(2);
  };
  slotRow(PickSlot::MateConnectorA, "Connector A", connLabel(a, haveA));
  slotRow(PickSlot::MateConnectorB, "Connector B", connLabel(b, haveB));

  if (!tool.message.empty())
    ImGui::TextDisabled("%s", tool.message.c_str());

  Mate* live = editM != nullptr ? editM : pm;
  if (live != nullptr) {
    if (ImGui::Checkbox("Flip", &live->flipped)) {
      if (editM != nullptr)
        SnapshotAssembly(state);
      SolveActiveAssembly(state);
    }
    if (!live->error.empty())
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "%s",
                         live->error.c_str());
  }

  ImGui::Separator();
  bool accept = false, cancel = false;
  if (editM != nullptr) {
    if (ImGui::Button("Done"))
      accept = true; // edits were applied live (each re-pick = one undo step)
  } else {
    ImGui::BeginDisabled(pm == nullptr);
    if (ImGui::Button("Accept"))
      accept = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      cancel = true;
  }
  // Enter accepts / Escape flows through ViewportInput's cascade (slot
  // disarm first, then session cancel).
  if (!ImGui::IsAnyItemActive() && pm != nullptr &&
      (ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false))) {
    accept = true;
  }
  ImGui::End();

  if (accept)
    AcceptMateTool(state);
  else if (cancel)
    CancelMateTool(state);
}

} // namespace ui
