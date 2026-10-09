// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

namespace ui {

// Draws the whole UI for one frame: menu bar, dockspace, panels, dialogs.
// Call between ImGui::NewFrame() and ImGui::Render().
void DrawUI(AppState& state);

} // namespace ui
