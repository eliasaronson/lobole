#pragma once

#include "ui/AppState.h"

namespace ui {

// Submits the workbench dockspace (3D scene visible through the passthru
// central node) and builds the default layout the first time there is no
// saved one. Call after the main menu bar, before any panels.
void BeginDockspace(AppState& state);

// Height of the bottom part-tabs bar. The dockspace reserves this strip so
// docked panels end above the bar instead of running beneath it.
float PartTabsBarHeight();

} // namespace ui
