#pragma once

#include "ui/AppState.h"

namespace ui {

// The frame-time overlay
// (docs/rendering/frame-pacing.md#the-frame-time-instrument). Draws nothing
// unless `state.perfHudOpen`, which nothing in the UI turns on: it is opened by
// --perf-hud, by Ctrl+Shift+F, or by the frame_stats RPC. The chord is
// `view.perf_hud` in the command table, flagged `hidden` so it stays out of the
// menus, Preferences and the shortcuts window.
void DrawPerfHud(AppState& state);

} // namespace ui
