#pragma once

#include "ui/AppState.h"

namespace ui {

// "Sketch Tools" floating toolbar, shown only while in sketch mode.
void DrawSketchToolbar(AppState& state);

// Extrude/Revolve the active sketch: exits sketch mode and opens the
// feature preview. Default mode is Add once a body exists (New Body on the
// first solid). Shared by the Sketch panel buttons and Shift+E / Shift+R.
void BeginSketchExtrude(AppState& state);
void BeginSketchRevolve(AppState& state);

} // namespace ui
