// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "imgui.h"

#include <glm/glm.hpp>

#include <string>

namespace ui {

struct AppState;

// A window that floats over the 3D viewport: the tool window, the CAM
// Generate window, the BOM, the CAM transport
// (docs/ui/panels.md#floating-windows).
//
// Each of those placed itself its own way, and they disagreed. The tool window
// re-anchored when the viewport moved, clamped itself inside the app and drew
// a shadow; the others anchored once on the appearing frame, did none of that,
// and two of them anchored at exactly the tool window's corner, so they opened
// on top of it. This is the tool window's placement, once:
//
//  - anchored to a corner of the viewport, and RE-anchored when that corner
//    moves (docking a panel, closing the terminal) or the window reappears --
//    two frames, one to learn an auto-resized window's size and one to place
//    it; a window not dragged keeps that corner as its size changes; a drag
//    sticks until the anchor moves again;
//  - an auto-resized window that reappears is also SIZED afresh, child
//    windows included, measured over two hidden frames: the tool window is
//    shared by every session, and otherwise kept the last session's width;
//  - clamped inside the app window every frame, BEFORE Begin, from the size
//    Begin will give it (moved after Begin, its content is laid out where it
//    was, and a full-width child widens the window by the move every frame);
//  - the elevated border and the soft shadow that say "above the model";
//  - no docking, no collapsing, no saved settings, so a stale imgui.ini
//    cannot park it off screen.

enum class FloatAnchor {
  TopRight,        // the tool window's corner (and the tool window only)
  BelowToolWindow, // top right, but under the tool window while it is open
  BottomCenter,    // a media transport
};

// Where the window was anchored and when it was last drawn. One per window,
// kept by its caller.
struct FloatingPlacement {
  glm::vec2 anchor{-1.0f, -1.0f};
  int anchorFrames = 0;
  int lastFrame = -10;
};

// Begins the window (the caller ALWAYS calls ImGui::End, as for Begin).
// Returns what ImGui::Begin returned.
bool BeginFloatingWindow(AppState& state, const std::string& title, bool* open,
                         FloatingPlacement& placement, FloatAnchor anchor,
                         ImGuiWindowFlags flags = 0, float bgAlpha = 0.96f);

// Records the CURRENT ImGui window (call it between Begin and End) as an
// obstacle the camera fit keeps the model out from under: its rect, in the
// main viewport's space, goes into `state.viewportObstaclesNext`
// (docs/ui/README.md#viewport). BeginFloatingWindow calls it for every
// floating window but the one anchored TopRight -- the tool window, which
// the fit counts only while the session state says a session is open
// (AppState::toolWindowRect).
void RecordViewportObstacle(AppState& state);

} // namespace ui
