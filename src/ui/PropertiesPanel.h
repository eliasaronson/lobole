#pragma once

#include "ui/AppState.h"

namespace ui {

// "Properties" panel: editor for the selected feature, plus camera and
// face-selection info.
void DrawPropertiesPanel(AppState& state);
// The measure tool's list + readout block, drawn inside whichever panel hosts
// it (part Properties panel; assembly Occurrence panel). No-op unless armed.
void DrawMeasureSection(AppState& state);

} // namespace ui
