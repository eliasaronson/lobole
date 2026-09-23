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
//    it; a drag sticks until the anchor moves again;
//  - clamped inside the app window every frame;
//  - the elevated border and the soft shadow that say "above the model";
//  - no docking, no collapsing, no saved settings, so a stale imgui.ini
//    cannot park it off screen.

enum class FloatAnchor {
  TopRight,        // the tool window's corner
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

} // namespace ui
