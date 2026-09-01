#include "ui/FastenerTool.h"

#include "core/parts/FastenerProfile.h"
#include "ui/AssemblyActions.h"
#include "ui/FeatureEditor.h"  // PickSlot, kFastenerToolSlot
#include "ui/PreviewFeature.h" // BeginToolWindowFrame

#include "imgui.h"

namespace ui {

namespace {

// The standards this verb offers, from the profile table rather than a list
// of its own: screws whose head stands proud of the surface.
//
// Countersunk is excluded on purpose and not silently — `FitFastenerToHole`
// refuses it by name, because the rim you pick on a plain bore is the bore's
// and the head would end up inside solid material. It comes back when a hole
// carries its countersink (docs/modeling/hole-standards.md). A nut and a
// washer are not here either: they do not pass through a hole, they are the
// far side of one, which is what the checkboxes below are.
std::vector<const parts::Profile*> OfferedStandards() {
  std::vector<const parts::Profile*> out;
  for (const parts::Profile& p : parts::AllProfiles())
    if (p.kind == parts::Kind::Screw && !p.lengthOverHead)
      out.push_back(&p);
  return out;
}

FastenerRequest RequestOf(const AppState::FastenerTool& tool, bool dryRun) {
  FastenerRequest r;
  r.standard = tool.standard;
  r.nut = tool.nut;
  r.washerUnderHead = tool.washerUnderHead;
  r.washerUnderNut = tool.washerUnderNut;
  r.dryRun = dryRun;
  return r;
}

} // namespace

void ArmFastenerRim(AppState& state) {
  state.slotPick = {kFastenerToolSlot, static_cast<int>(PickSlot::FastenerRim),
                    SelectionKindBit(SelectionItem::Kind::Edge),
                    /*multi=*/false,
                    "Pick the rim of the hole — the circular edge the head "
                    "sits against"};
}

void OpenFastenerTool(AppState& state) {
  state.fastenerTool = AppState::FastenerTool{};
  state.fastenerTool.open = true;
  // Opens ARMED: the pick is the whole verb, so asking for it immediately is
  // the honest first move rather than a window that waits to be understood.
  ArmFastenerRim(state);
}

void CloseFastenerTool(AppState& state) {
  state.fastenerTool.open = false;
  if (state.slotPick.featureId == kFastenerToolSlot)
    state.slotPick = {};
}

void RefreshFastenerFit(AppState& state) {
  AppState::FastenerTool& tool = state.fastenerTool;
  tool.ok = false;
  tool.summary.clear();
  tool.why.clear();
  if (!tool.rimElement.IsSet())
    return;
  const FastenerFit fit =
      FitFastenerToHole(state, RimPick{tool.rimOccurrence, tool.rimElement},
                        RequestOf(tool, /*dryRun=*/true));
  tool.ok = fit.ok;
  tool.summary = fit.summary;
  tool.why = fit.why;
}

void FastenerRimPicked(AppState& state, const SelectionItem& item) {
  if (item.kind != SelectionItem::Kind::Edge || !item.element.IsSet()) {
    // The pipeline only offers what the slot asked for, so this is a
    // belt-and-braces case; say why rather than swallowing the click (U10).
    state.slotPick.miss = "that is not a circular edge — pick the rim of the "
                          "hole, the edge the head sits against";
    return;
  }
  state.fastenerTool.rimOccurrence = item.id;
  state.fastenerTool.rimElement = item.element;
  state.slotPick = {}; // filled; the window now offers Place
  RefreshFastenerFit(state);
}

void DrawFastenerToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "Fastener");
  AppState::FastenerTool& tool = state.fastenerTool;

  if (state.assembly == nullptr) {
    ImGui::TextWrapped(
        "A fastener is a bought part placed in an ASSEMBLY. Switch to an "
        "assembly tab and pick the rim of the hole it passes through.");
    if (ImGui::Button("Done###fastener_done_notab"))
      CloseFastenerTool(state);
    ImGui::End();
    return;
  }

  const bool armed = state.slotPick.featureId == kFastenerToolSlot;
  const bool havePick = tool.rimElement.IsSet();

  ImGui::TextDisabled("Hole");
  if (ImGui::Button(havePick ? "Re-pick rim###fastener_pick"
                             : "Pick rim###fastener_pick"))
    ArmFastenerRim(state);
  ImGui::SameLine();
  if (armed)
    ImGui::TextDisabled("click a hole's rim…");
  else if (havePick)
    ImGui::Text("occurrence %llu",
                static_cast<unsigned long long>(tool.rimOccurrence));
  else
    ImGui::TextDisabled("nothing picked");

  ImGui::Separator();
  ImGui::TextDisabled("Screw");
  // BUTTONS, not a combo: an ImGui combo gathers with an empty label, so a
  // test (or an agent) cannot address the choice at all — which cost a round
  // in the CAM work-origin work. One stable id per standard.
  bool changed = false;
  const std::vector<const parts::Profile*> standards = OfferedStandards();
  for (size_t i = 0; i < standards.size(); i++) {
    if (i % 2 != 0)
      ImGui::SameLine();
    const parts::Profile& p = *standards[i];
    const bool active = tool.standard == p.name;
    ImGui::BeginDisabled(active);
    const std::string id = p.designation + "###fastener_std_" + p.name;
    if (ImGui::Button(id.c_str())) {
      tool.standard = p.name;
      changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", p.what.c_str());
  }

  ImGui::Separator();
  ImGui::TextDisabled("Joint");
  changed |= ImGui::Checkbox("Washer under head###fastener_washer_head",
                             &tool.washerUnderHead);
  bool wantNut = !tool.nut.empty();
  if (ImGui::Checkbox("Nut on the far side###fastener_nut", &wantNut)) {
    tool.nut = wantNut ? "iso4032" : "";
    if (!wantNut)
      tool.washerUnderNut = false;
    changed = true;
  }
  ImGui::BeginDisabled(!wantNut);
  changed |= ImGui::Checkbox("Washer under nut###fastener_washer_nut",
                             &tool.washerUnderNut);
  ImGui::EndDisabled();

  ImGui::Separator();
  // The decomposition, before the commit. This line is the product: a gap
  // that should not be there, or a plate that is not the one you thought,
  // shows up HERE rather than afterwards in a bill of materials.
  if (changed)
    RefreshFastenerFit(state);
  if (!havePick) {
    ImGui::TextDisabled("Pick the rim of a hole; the size and length follow "
                        "from it.");
  } else if (tool.ok) {
    ImGui::TextWrapped("%s", tool.summary.c_str());
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.25f, 1.0f));
    ImGui::TextWrapped("%s", tool.why.c_str());
    ImGui::PopStyleColor();
  }
  if (!state.slotPick.miss.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.25f, 1.0f));
    ImGui::TextWrapped("%s", state.slotPick.miss.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::Separator();
  ImGui::BeginDisabled(!tool.ok);
  if (ImGui::Button("Place###fastener_place")) {
    std::string error;
    const FastenerFit fit =
        FitFastenerToHole(state, RimPick{tool.rimOccurrence, tool.rimElement},
                          RequestOf(tool, /*dryRun=*/false));
    if (fit.ok) {
      tool.placed++;
      // Straight back to picking. A flange of eight bolts is eight clicks,
      // not eight trips to the toolbar — and the options carry over, because
      // the next hole in a pattern takes the same screw.
      tool.rimElement = {};
      tool.rimOccurrence = 0;
      tool.summary.clear();
      tool.ok = false;
      ArmFastenerRim(state);
    } else {
      tool.ok = false;
      tool.why = fit.why;
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Done###fastener_done"))
    CloseFastenerTool(state);
  if (tool.placed > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("%d placed", tool.placed);
  }

  ImGui::End();
}

} // namespace ui
