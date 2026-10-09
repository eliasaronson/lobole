// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

// The drawing sheet: rendering it, its input handling, and the
// sheet/views/dimensions panel.
//
// Split out of ui/AppActions.h, which had grown to ~180 declarations spanning
// every domain in the app and is included by 40 translation units — so every
// panel and RPC module paid for all of it. See
// docs/architecture.md#the-ui-actions-layer.
namespace ui {

// The sheet settings + title-block editor for an active drawing tab (drawn
// where the feature tree is for a part). Defined in DrawingPanel.cpp. The view
// list and dimension tools arrive in later drawings milestones.
void DrawSheetPanel(AppState& state);

// The Add view tool window (the toolbar's Add view): source, configuration,
// orientation and scale.
void DrawAddViewToolWindow(AppState& state);

// Renders the active drawing's sheet (border, title block, and later views/
// dims) into the viewport through the 2D sheet camera. Defined in
// DrawingPanel.cpp; called from the frame loop when a drawing tab is active.
void DrawSheet(AppState& state);

// 2D sheet pan/zoom (scroll-to-cursor zoom, middle-drag pan) — the drawing-tab
// replacement for the orbit-camera viewport input. Defined in DrawingPanel.cpp.
void HandleSheetInput(AppState& state);

} // namespace ui
