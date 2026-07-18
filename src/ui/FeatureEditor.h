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
// but ids need only be unique.
enum class PickSlot : int {
  ExtrudeEndFace,   // up-to-face target (Face)
  BooleanTarget,    // Body
  BooleanTool,      // Body
  PatternSource,    // Body
  ShellBody,        // Body
  ShellOpenFaces,   // Face, multi
  DraftFaces,       // Face, multi
  ExtrudeRegions,   // U2: sketch regions, multi
  RevolveRegions,   // U2: sketch regions, multi
};

// The selection pipeline routes a resolved pick to the armed slot here:
// writes the feature's reference (one undoable edit when the feature is
// committed; live mutation while previewing) and disarms single slots.
void SlotPickClicked(AppState& state, const SelectionItem& item);

} // namespace ui
