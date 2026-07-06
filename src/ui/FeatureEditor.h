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

} // namespace ui
