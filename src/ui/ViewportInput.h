// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

namespace ui {

// 3D-viewport interaction, read from ImGui input state once per frame:
//   Left click          select (Ctrl: multi; Shift+click aligns the camera)
//   Navigation drags    whatever Preferences > Navigation binds: by default
//                       middle/right/Alt+left rotate, +Ctrl pan, +Shift
//                       zoom, Alt+middle roll (core/io/MouseNav.h)
//   Right click         an occurrence's menu (a click, not a drag)
//   Scroll              zoom (toward the cursor; Preferences > Navigation)
//   Arrows              rotate (Shift = 90°) | +Ctrl pan | +Alt roll
//   (hover              face pre-highlight)
// Routing through ImGui instead of GLFW callbacks gates it against the UI
// automatically and lets the test engine (or any automation) drive the
// viewport exactly like a human.
void HandleViewportInput(AppState& state);

// The navigation gestures the preferences hold now, as a table (rebuilt only
// when the rows change). The viewport, the drawing sheet and the controls
// cheat-sheet all ask this one.
const mousenav::NavMap& CurrentNavMap(const AppState& state);

} // namespace ui
