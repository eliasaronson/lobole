#pragma once

#include "ui/AppState.h"

namespace ui {

// Draws the icon toolbar row. Called inside the dockspace host window, above
// the DockSpace (see DockLayout).
void DrawToolbar(AppState& state);

// The background-import progress modal. Drawn by DockLayout at host-window
// level rather than by a toolbar: the toolbar shown depends on the active
// tab, and an import CHANGES the active tab while it runs (see the comment on
// the definition).
void DrawImportModal(AppState& state);

} // namespace ui
