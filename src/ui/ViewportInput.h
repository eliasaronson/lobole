// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

namespace ui {

// 3D-viewport interaction (the standard CAD bindings), read from ImGui input
// state
// once per frame:
//   Left click          select face (Ctrl+click also aligns the camera)
//   Middle drag         rotate | +Ctrl pan | +Shift zoom | +Alt roll
//   Scroll              zoom (toward the cursor; Preferences > Navigation)
//   Arrows              rotate (Shift = 90°) | +Ctrl pan | +Alt roll
//   Z / Shift+Z         zoom out / in
//   (hover              face pre-highlight)
// Routing through ImGui instead of GLFW callbacks gates it against the UI
// automatically and lets the test engine (or any automation) drive the
// viewport exactly like a human.
void HandleViewportInput(AppState& state);

} // namespace ui
