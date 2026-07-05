#pragma once

#include "ui/AppState.h"

namespace ui {

// Submits the workbench dockspace (3D scene visible through the passthru
// central node) and builds the default layout the first time there is no
// saved one. Call after the main menu bar, before any panels.
void BeginDockspace(AppState& state);

} // namespace ui
