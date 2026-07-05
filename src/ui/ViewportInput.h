#pragma once

#include "ui/AppState.h"

namespace ui {

// 3D-viewport mouse interaction, read from ImGui input state once per frame:
//   Left click        select face (Ctrl+click also aligns the camera)
//   Right drag        orbit
//   Middle drag       pan
//   Scroll            zoom
//   (hover            face pre-highlight)
// Routing through ImGui instead of GLFW callbacks gates it against the UI
// automatically and lets the test engine (or any automation) drive the
// viewport exactly like a human.
void HandleViewportInput(AppState& state);

} // namespace ui
