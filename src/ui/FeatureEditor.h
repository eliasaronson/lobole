#pragma once

#include "ui/AppState.h"

class Feature;

namespace ui {

// Draws editable widgets for a feature's parameters (used by both the
// Properties panel and the preview dialog). When commit is true, edits are
// pushed as undoable ModifyFeatureCommands at the given feature index; when
// false (a pending preview), edits only mutate the feature + regenerate.
void DrawFeatureParamWidgets(AppState& state, Feature& feature, size_t index,
                             bool commit);

// Dialog pick slots (U3): which reference field of the edited feature an
// armed SlotPickState fills. One global enum — slots are feature-specific
// but ids need only be unique. Only append: tests address rows as
// "###slot<value>".
enum class PickSlot : int {
  ExtrudeEndFace, // up-to-face target (Face)
  BooleanTarget,  // Body
  BooleanTool,    // Body
  PatternSource,  // Body
  ShellBody,      // Body
  ShellOpenFaces, // Face, multi
  DraftFaces,     // Face, multi
  ExtrudeRegions, // U2: sketch regions, multi
  RevolveRegions, // U2: sketch regions, multi
  RibBody,        // Body (0 = nearest overlapping)
  ThreadFace,     // a cylindrical Face
  BlendEdges,     // U7: fillet/chamfer edge collection (Edge, multi)
};

// A tool's pick slots, declaratively (U7): the ordered list drives arming
// (kinds/prompt come from here, not the draw site), viewport routing, and —
// U7b — seeding from a pre-existing selection. Order = seed priority.
struct SlotSpec {
  PickSlot slot;
  const char* label;  // slot row label ("Target", "Edges")
  uint32_t kinds;     // SelectionItem::Kind bitmask accepted
  bool multi;         // toggle-list slot; stays armed after a pick
  const char* prompt; // status pill while armed
};

// The slots the feature exposes IN ITS CURRENT STATE (e.g. the extrude
// end-face slot exists only in Up-to-face mode). Empty for features without
// pick slots.
std::vector<SlotSpec> SlotSpecsFor(const Feature& feature);

// Arms `slot` on the feature (kinds/prompt from SlotSpecsFor). Returns false
// when the feature doesn't currently expose that slot.
bool ArmSlot(AppState& state, const Feature& feature, PickSlot slot);

// The fillet/chamfer whose BlendEdges slot is armed, or 0 — the blend pick
// overlay and the selection overlay's channel handoff key off this.
uint64_t ArmedBlendFeatureId(const AppState& state);

// The selection pipeline routes a resolved pick to the armed slot here:
// writes the feature's reference (one undoable edit when the feature is
// committed; live mutation while previewing) and disarms single slots.
// meshId/index are the render handles the pick resolved from (the blend
// edge remap anchors on them; 0 otherwise).
void SlotPickClicked(AppState& state, const SelectionItem& item,
                     uint32_t meshId = 0, uint32_t index = 0);

} // namespace ui
