#pragma once

#include "ui/AppState.h"

// The assembly panel (occurrences, mates, exploded views, BOM)
// and the mate tool window.
//
// Split out of ui/AppActions.h, which had grown to ~180 declarations spanning
// every domain in the app and is included by 40 translation units — so every
// panel and RPC module paid for all of it. See
// docs/architecture-review-2026-08-24.md.
namespace ui {

// The mate session's Tool-window contents (connector slot rows, flip/error,
// accept/cancel). Lives with the other mate UI in OccurrencePanel.cpp;
// called from DrawToolWindow.
void DrawMateToolWindow(AppState& state);

// The occurrence list + properties editor for an active assembly tab (drawn
// where the feature tree is for a part). Defined in OccurrencePanel.cpp.
void DrawOccurrencePanel(AppState& state);

} // namespace ui
