#pragma once

#include "ui/AppState.h"

namespace ui {

// Draws the icon toolbar row. Called inside the dockspace host window, above
// the DockSpace (see DockLayout).
void DrawToolbar(AppState& state);

} // namespace ui
