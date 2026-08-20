#pragma once

#include "ui/AppState.h"

namespace ui {

// The frame-time overlay (docs/rendering/frame-timing.md). Draws nothing
// unless `state.perfHudOpen`, which nothing in the UI turns on: it is opened
// by --perf-hud, by Ctrl+Shift+F, or by the frame_stats RPC. Also handles
// that shortcut, so the one deliberately-undocumented chord lives beside the
// window it opens rather than in the menu bar's table.
void DrawPerfHud(AppState& state);

} // namespace ui
