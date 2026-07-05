#pragma once

#include "ui/AppState.h"

namespace ui {

// Draws the whole UI for one frame: menu bar, dockspace, panels, dialogs.
// Call between ImGui::NewFrame() and ImGui::Render().
void DrawUI(AppState& state);

} // namespace ui
