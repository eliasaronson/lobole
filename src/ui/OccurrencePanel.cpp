#include "ui/OccurrencePanel.h"
#include "ui/AppActions.h"
#include "ui/AssemblyActions.h"
#include "ui/DockLayout.h"    // PanelHeading
#include "ui/ExprInput.h"     // expression-capable numeric fields
#include "ui/FeatureEditor.h" // PickSlot, kMateSessionSlot (mate slot rows)
#include "ui/FileDialogs.h"
#include "ui/ListSelection.h"
#include "ui/PanelLayout.h" // LeftLabel / WrapRow (readable at dock width)
#include "ui/PanelTitles.h"
#include "ui/PreviewFeature.h" // BeginToolWindowFrame (mate session window)
#include "ui/TextFit.h"

#include "core/io/Serialization.h" // RelationKindToString

#include "imgui.h"

#include <glm/gtc/quaternion.hpp> // eulerAngles, quat(vec3)
#include <glm/trigonometric.hpp>  // degrees, radians

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace ui {

namespace {
std::string PartNameById(const AppState& state, uint64_t partId) {
  for (const PartSlot& s : state.parts)
    if (s.kind == DocKind::Part && s.id == partId)
      return s.name;
  return "(missing part)";
}

// A5: an occurrence references a part OR an assembly, so the panel resolves
// both — and says which, since "Gangway" placed as a unit behaves differently
// from a part (it is one row, one selection, one explode step).
std::string RefNameById(const AppState& state, const Occurrence& o) {
  const DocKind want =
      o.refKind == OccurrenceRef::Assembly ? DocKind::Assembly : DocKind::Part;
  for (const PartSlot& s : state.parts)
    if (s.kind == want && s.id == o.partId)
      return s.name;
  return o.refKind == OccurrenceRef::Assembly ? "(missing assembly)"
                                              : "(missing part)";
}

// The tab index of an occurrence's referenced document, for "Open".
std::optional<size_t> RefSlotIndex(const AppState& state, const Occurrence& o) {
  const DocKind want =
      o.refKind == OccurrenceRef::Assembly ? DocKind::Assembly : DocKind::Part;
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == want && state.parts[i].id == o.partId)
      return i;
  return std::nullopt;
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
// Relations (docs/assemblies/relations-plan.md): listed with their ratio
// and phase editable and a remove button. Made by RPC in this round — a
// relation names two mates and the panel selects one — so there is no
// creation tool here for the defaults gate to hold.
void DrawRelationsSection(AppState& state) {
  AssemblyDocument& asmDoc = *state.assembly;
  if (asmDoc.RelationCount() == 0)
    return;
  ImGui::SeparatorText("Relations");
  uint64_t remove = 0;
  for (Relation& r : asmDoc.GetRelations()) {
    ImGui::PushID(static_cast<int>(r.id));
    const Mate* a = asmDoc.FindMateById(r.mateA);
    const Mate* b = asmDoc.FindMateById(r.mateB);
    const std::string label =
        (r.name.empty() ? std::string(DocumentIO::RelationKindToString(r.kind))
                        : r.name) +
        "  [" + DocumentIO::RelationKindToString(r.kind) + "]";
    if (!r.error.empty()) {
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "%s", "(!)");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", r.error.c_str());
      ImGui::SameLine();
    }
    ImGui::TextUnformatted(label.c_str());
    if (a != nullptr && b != nullptr)
      ImGui::TextDisabled(
          "  %s -> %s",
          (a->name.empty() ? "mate " + std::to_string(a->id) : a->name).c_str(),
          (b->name.empty() ? "mate " + std::to_string(b->id) : b->name)
              .c_str());
    double ratio = r.ratio, phase = r.phase;
    ImGui::SetNextItemWidth(110.0f);
    if (ExprInputDouble(state, "ratio###rel_ratio", &ratio, "%.5g") &&
        ratio != 0.0 && ratio != r.ratio) {
      SnapshotAssembly(state);
      r.ratio = ratio;
      asmDoc.BumpVersion();
      SolveActiveAssembly(state);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ExprInputDouble(state, "phase###rel_phase", &phase, "%.5g") &&
        phase != r.phase) {
      SnapshotAssembly(state);
      r.phase = phase;
      asmDoc.BumpVersion();
      SolveActiveAssembly(state);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x###rel_remove"))
      remove = r.id;
    ImGui::PopID();
  }
  if (remove != 0) {
    SnapshotAssembly(state);
    asmDoc.RemoveRelation(remove);
    SolveActiveAssembly(state);
  }
}

void DrawMatesSection(AppState& state) {
  AssemblyDocument& asmDoc = *state.assembly;

  if (state.mateTool.active && state.mateTool.previewMateId)
    return; // the Tool window owns the mid-preview UI; hide the transient mate

  ImGui::SeparatorText("Mates");
  if (asmDoc.MateCount() == 0) {
    TextDisabledWrapped("None. Use the mate tools in the toolbar.");
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
    const bool changed =
        ExprDragFloat(state, label, &deg, 1.0f, -FLT_MAX, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (changed) {
      sel->rotation = glm::radians(deg);
      SolveActiveAssembly(state);
    }
  };
  auto offsetDof = [&](const char* label) {
    const bool changed = ExprDragFloat(state, label, &sel->offset, 0.1f,
                                       -FLT_MAX, FLT_MAX, "%.3f");
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
    const bool changed = ExprDragFloat2(state, "Slide (x,y)", dxy, 0.1f);
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
    const bool changed =
        ExprDragFloat3(state, "Rotation (deg)", &euler.x, 1.0f);
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
    // The id goes in the LABEL, not a PushID(int) scope: an integer scope is
    // not addressable as a path segment, so no test could ever select a view
    // (docs/ui/gui-coverage.md).
    const bool active = state.activeExplodedView == v.id;
    const std::string label = (v.name.empty() ? "(unnamed)" : v.name) +
                              "###explode_view_" + std::to_string(v.id);
    if (ImGui::Selectable(label.c_str(), active)) {
      state.activeExplodedView = v.id;
      state.syncedDocumentVersion = UINT64_MAX;
    }
  }
  if (ImGui::SmallButton("Add view###explode_add")) {
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
    LeftLabel("Name");
    if (ImGui::InputText("###explode_name", name, sizeof name,
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      SnapshotAssembly(state);
      sel->name = name;
      asmDoc.BumpVersion();
    }
    // Factor slider: cheap live path (matrix updates only); picking catches
    // up on the resync forced when the slider is released.
    LeftLabel("Factor");
    if (ImGui::SliderFloat("###explode_factor", &state.explodeFactor, 0.0f,
                           1.0f))
      ApplyExplodeTransforms(state);
    if (ImGui::IsItemDeactivatedAfterEdit())
      state.syncedDocumentVersion = UINT64_MAX;
    // SameLine does not wrap, so "Delete view" was drawn past the panel edge
    // at the width the dock ships at.
    WrapRow row;
    row.Next(ButtonWidth("Auto-explode"));
    if (ImGui::SmallButton(
            "Auto-explode###explode_auto")) { // radial seed, then drag
      if (AutoExplode(state, sel->id))
        state.syncedDocumentVersion = UINT64_MAX;
    }
    row.Next(ButtonWidth("Delete view"));
    if (ImGui::SmallButton("Delete view###explode_delete")) {
      SnapshotAssembly(state);
      asmDoc.RemoveExplodedView(sel->id);
      state.activeExplodedView = 0;
      state.syncedDocumentVersion = UINT64_MAX;
      asmDoc.BumpVersion();
    }
    TextDisabledWrapped("Drag the gizmo to set an occurrence's offset.");
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
// A mass a 40 px column can hold. The BOM printed raw grams — a 385 kg dock
// totalled "385169.06" — and an ImGui table cell CLIPS rather than elides, so
// what a person read was "385" under a header saying "Total g". A truncated
// number still looks like a number, which is the worst way for a bill of
// materials to be wrong: every unit mass on that screen was out by a factor
// of ten to a thousand while looking perfectly reasonable.
//
// Now core/Units: the g/kg switch this used to make per value is exactly
// what a document unit system replaces. A column of masses has to be
// comparable DOWN THE COLUMN, and "1.2 kg" over "900 g" is not.
std::string MassText(const AppState& state, double grams) {
  return units::FormatMass(grams, state.ActiveUnits());
}

// Full text on hover, for whatever the column still cannot hold. Kept
// alongside TextFit's own tooltip because a TABLE cell clips on its own,
// before any of our text lands — the column width is the table's business,
// not the string's.
void CellTooltip(const std::string& text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", text.c_str());
}

void DrawBomSection(AppState& state) {
  ImGui::Spacing();
  if (!ImGui::CollapsingHeader("Bill of materials###bom_header"))
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
  // Five columns in a 210 px dock gave each about 40 characters' worth of
  // pixels between them, so every cell clipped: three different parts all
  // read "Gang", "stainless_steel" read "stair", and the masses lost their
  // leading digits. The table ADAPTS the way the Variables one does — full
  // five columns when there is room, and two rows per part when there is not
  // (name and quantity, then material and total). Widen the dock to get the
  // table back.
  const float availW = ImGui::GetContentRegionAvail().x;
  const bool wide = availW >= ImGui::GetFontSize() * 34.0f;
  if (ImGui::BeginTable("###bom_table", wide ? 5 : 2,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp)) {
    // Narrow: the second column carries a MASS on the stacked row, so it
    // needs room for "385.17 kg", not just for "19".
    ImGui::TableSetupColumn("Part", ImGuiTableColumnFlags_WidthStretch,
                            wide ? 1.6f : 1.3f);
    ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthStretch,
                            wide ? 0.4f : 1.0f);
    if (wide) {
      ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthStretch,
                              1.2f);
      ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthStretch, 0.9f);
      ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch,
                              0.9f);
      ImGui::TableHeadersRow(); // "M… U… T…" helps nobody at the narrow width
    }
    for (const BomRow& r : cache.rows) {
      const std::string material = r.material.empty() ? "-" : r.material;
      const std::string unit = r.massKnown ? MassText(state, r.unitMassG) : "?";
      const std::string total =
          r.massKnown ? MassText(state, r.unitMassG * r.quantity) : "?";
      // Everything about the row, for the hover — the escape hatch for
      // whatever the column still cannot hold.
      const std::string full = r.partName + "  x" + std::to_string(r.quantity) +
                               "  " + material + "  " + unit + " each, " +
                               total + " total";
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.partName.c_str());
      CellTooltip(full);
      ImGui::TableNextColumn();
      ImGui::Text("%d", r.quantity);
      CellTooltip(full);
      if (wide) {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(material.c_str());
        CellTooltip(full);
        ImGui::TableNextColumn();
        if (r.massKnown)
          ImGui::TextUnformatted(unit.c_str());
        else
          ImGui::TextDisabled("?"); // no material/density assigned
        CellTooltip(full);
        ImGui::TableNextColumn();
        if (r.massKnown)
          ImGui::TextUnformatted(total.c_str());
        else
          ImGui::TextDisabled("?");
        CellTooltip(full);
      } else {
        // Second row: what the dropped columns were carrying.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", material.c_str());
        CellTooltip(full);
        ImGui::TableNextColumn();
        if (r.massKnown)
          ImGui::TextDisabled("%s", total.c_str());
        else
          ImGui::TextDisabled("?");
        CellTooltip(full);
      }
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Total");
    for (int c = 1; c < (wide ? 4 : 1); ++c)
      ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    if (cache.massKnown) {
      ImGui::TextUnformatted(MassText(state, cache.totalMassG).c_str());
      CellTooltip(MassText(state, cache.totalMassG));
    } else {
      ImGui::TextDisabled("?");
    }
    ImGui::EndTable();
  }
  // V2: name the reason for every "?" so a null total explains itself.
  for (const std::string& w : cache.warnings)
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "%s", w.c_str());
  const bool canSave = FileDialogsAvailable();
  ImGui::BeginDisabled(!canSave);
  if (ImGui::Button("Export CSV###bom_export"))
    ExportBomDialog(state);
  ImGui::EndDisabled();
  if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Needs a system file dialog (unavailable here) - use "
                      "export_bom over the script API");
  }

  // Every placed fastener, re-derived against the stack it is IN
  // (docs/parts/gui-plan.md, phase 4). Next to the BOM because that is where
  // a person looks at what the assembly is MADE of, and because the answer
  // has the same shape as the mass warnings above it.
  //
  // On demand rather than every frame: it walks the whole assembly along a
  // ray per fastener. And ON DEMAND rather than never — a length derived
  // from other documents' geometry cannot be a live binding (A2), so a stack
  // that moved under a screw has to be asked about. Until now it could only
  // be asked over the script API, which means in practice it was not.
  if (ImGui::Button("Check fasteners###bom_check_fasteners")) {
    state.fastenerChecks.clear();
    for (const FastenerCheck& c : CheckFasteners(state))
      state.fastenerChecks.push_back(c.part + " " + c.why);
    state.fastenerChecked = true;
  }
  if (state.fastenerChecked) {
    if (state.fastenerChecks.empty()) {
      ImGui::TextDisabled("Every fastener still fits its stack.");
    } else {
      for (const std::string& w : state.fastenerChecks)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "%s", w.c_str());
    }
  }
}

void DrawOccurrencePanel(AppState& state) {
  ImGui::Begin(kPanelModel);
  PanelHeading(kPanelModel);
  if (!state.assembly) {
    ImGui::End();
    return;
  }
  AssemblyDocument& asmDoc = *state.assembly;

  ImGui::SeparatorText("Occurrences");
  if (asmDoc.OccurrenceCount() == 0)
    TextDisabledWrapped("Empty - use Insert part on the toolbar.");

  // The rows as listed — what a Shift-click range spans (ListSelection.h).
  std::vector<uint64_t> occOrder;
  for (const Occurrence& o : asmDoc.GetOccurrences())
    occOrder.push_back(o.id);
  for (const Occurrence& o : asmDoc.GetOccurrences()) {
    // Ids in the LABELS, not a PushID(int) scope: an integer scope is not
    // addressable as a path segment, so the assembly's primary list could
    // only be reached by a "**" wildcard crawl that depends on the row's
    // cosmetic decorations ("  [assembly]  (fixed)") to match.
    const std::string suffix = std::to_string(o.id);
    bool vis = o.visible;
    if (ImGui::Checkbox(("###occ_vis_" + suffix).c_str(), &vis)) {
      SnapshotAssembly(state);
      asmDoc.FindById(o.id)->visible = vis;
      asmDoc.BumpVersion();
    }
    ImGui::SameLine();
    const std::string label =
        (o.name.empty() ? RefNameById(state, o) : o.name) +
        (o.refKind == OccurrenceRef::Assembly ? "  [assembly]" : "") +
        (o.fixed ? "  (fixed)" : "") + "###occ_" + suffix;
    // Ctrl-click adds and removes, Shift-click ranges, a plain click
    // replaces — the one list gesture (ListSelection.h). Own kind only: a
    // selected mate stays, as the old separate fields let it.
    if (SelectableEllipsis(label, IsOccurrenceSelected(state, o.id)))
      ListRowClicked(state, SelectionItem::Kind::Occurrence, o.id, occOrder,
                     /*ownKindOnly=*/true);

    // Right-click the row to go to what it references: the shortcut for the
    // "Open"/"Edit" button below, which needs the row selected first and only
    // shows for the selected one. Opening a row that is not the selected one
    // would be a silent selection change, so the menu selects it too.
    if (ImGui::BeginPopupContextItem(("###occ_ctx_" + suffix).c_str())) {
      // Right-clicking INSIDE a multi-selection keeps it — collapsing to the
      // clicked row is how you lose a five-part selection by aiming at it.
      ListRowContext(state, SelectionItem::Kind::Occurrence, o.id,
                     /*ownKindOnly=*/true);
      const bool isSub = o.refKind == OccurrenceRef::Assembly;
      const std::string item =
          std::string("Open ") + (isSub ? "assembly" : "part") + "###occ_open";
      const std::optional<size_t> slot = RefSlotIndex(state, o);
      if (ImGui::MenuItem(item.c_str(), nullptr, false, slot.has_value())) {
        SetActivePart(state, *slot);
        ImGui::EndPopup();
        ImGui::End();
        return; // the panel is now drawing a different document
      }
      // The reference dangles (its tab was closed/deleted). Say so rather
      // than offering an Open that would quietly do nothing.
      if (!slot && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", isSub ? "That assembly is not in this project"
                                      : "That part is not in this project");
      }
      // Delete belongs HERE, on the thing being deleted. It existed only as a
      // button in the "Selected occurrence" editor below the list, which you
      // reach by selecting a row first and which is off the bottom of a short
      // panel on any real assembly — so the answer to "how do I remove this"
      // was invisible, and the tab bar's Delete part refused because of the
      // very occurrences nobody could find. Reported from use.
      // Detach: a model is allowed to be wrong on purpose, and the pose is
      // derived only while the attachment stands.
      if (o.attachedTo != 0 &&
          ImGui::MenuItem("Detach from its part###occ_detach")) {
        SnapshotAssembly(state);
        asmDoc.FindById(o.id)->attachedTo = 0;
        asmDoc.BumpVersion();
        ImGui::EndPopup();
        ImGui::End();
        return;
      }
      ImGui::Separator();
      const size_t picked = SelectedOccurrenceIds(state).size();
      const std::string del =
          DeleteLabel(picked, "occurrence") + "###occ_delete";
      if (ImGui::MenuItem(del.c_str())) {
        DeleteSelection(state);
        ImGui::EndPopup();
        ImGui::End();
        return; // the list we are walking just changed under us
      }
      ImGui::EndPopup();
    }
  }

  Occurrence* sel = SelectedOccurrenceId(state)
                        ? asmDoc.FindById(SelectedOccurrenceId(state))
                        : nullptr;
  if (sel) {
    ImGui::SeparatorText("Selected occurrence");
    const bool isSub = sel->refKind == OccurrenceRef::Assembly;
    ImGui::Text("%s: %s", isSub ? "Assembly" : "Part",
                RefNameById(state, *sel).c_str());
    // v1 does not expand a sub-assembly in place: its own tab IS the editor,
    // and in-context editing there already works.
    if (const auto slot = RefSlotIndex(state, *sel)) {
      ImGui::SameLine();
      if (ImGui::SmallButton(isSub ? "Open###open_ref" : "Edit###open_ref"))
        SetActivePart(state, *slot);
    }

    char name[128];
    std::snprintf(name, sizeof(name), "%s", sel->name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      SnapshotAssembly(state);
      sel->name = name;
      asmDoc.BumpVersion();
    }

    glm::vec3 pos = sel->position;
    const bool posChanged = ExprDragFloat3(state, "Position", &pos.x, 0.1f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state); // one snapshot per drag session
    if (posChanged) {
      sel->position = pos;
      asmDoc.BumpVersion();
    }

    glm::vec3 euler = glm::degrees(glm::eulerAngles(sel->orientation));
    const bool rotChanged =
        ExprDragFloat3(state, "Rotation (deg)", &euler.x, 1.0f);
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
    const size_t picked = SelectedOccurrenceIds(state).size();
    const std::string del = DeleteLabel(picked, "occurrence");
    if (ImGui::Button(del.c_str())) {
      DeleteSelection(state);
      ImGui::End();
      return; // `sel` pointed into the list that just shrank
    }
  }

  DrawMatesSection(state);
  DrawRelationsSection(state);
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
    if (c.anchor.frameId != 0) { // v86: the mate as designed, by name
      std::string fname = "frame " + std::to_string(c.anchor.frameId);
      if (const Occurrence* o = asmDoc.FindById(c.occurrenceId))
        if (const Document* part = PartDocById(state, o->partId))
          for (const auto& f : part->GetFeatures())
            if (f->GetId() == c.anchor.frameId)
              fname = f->GetName();
      s += " @ " + fname;
    } else if (c.anchor.snapEdge.IsSet())
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
