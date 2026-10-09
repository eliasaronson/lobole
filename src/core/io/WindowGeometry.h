// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vector>

// Where the main window was, and where it can actually go now.
//
// The window used to be created with a literal `glfwCreateWindow(1280, 720)`,
// every launch, whatever the user had last time — which on a CAD app people
// run maximised on a large panel is the most-felt of the chrome papercuts.
//
// The half that needs saying out loud is the RESTORE. A saved rect is not
// automatically a usable one: unplug the monitor it was on, or dock a laptop
// and undock it, and the window comes back somewhere no cursor can reach,
// with no way to move it. So a remembered rect is checked against the
// monitors that exist NOW, and dropped if it does not land on one — the same
// reasoning the Preferences window gives for centring rather than
// remembering its own position (SettingsWindow.cpp).
//
// Pure, and in core, so designer_tests can hold it with a synthetic monitor
// list and no display server at all.
namespace settings {

// A monitor's WORK area (what is left after panels, docks and taskbars), in
// virtual-desktop coordinates. Not the video mode: a window sized to the
// video mode hides behind the taskbar.
struct MonitorRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct WindowGeometry {
  int x = 0;
  int y = 0;
  int w = 0; // 0 = nothing has been remembered yet
  int h = 0;
  bool maximized = false;

  bool Remembered() const { return w > 0 && h > 0; }
  bool operator==(const WindowGeometry& o) const {
    return x == o.x && y == o.y && w == o.w && h == o.h &&
           maximized == o.maximized;
  }
  bool operator!=(const WindowGeometry& o) const { return !(*this == o); }
};

// The geometry to actually open with.
//
// `saved` empty (a first run) => a default: kDefaultFraction of the first
// monitor's work area, centred on it. A percentage rather than a literal
// 1280x720, which is a third of a 1440p panel and most of a laptop's.
//
// `saved` present but landing on no current monitor => the same default.
// "Landing on" means a real overlap, not a shared edge: a window one pixel
// onto the screen is not recoverable by a person.
//
// `saved` present and usable => itself, with its size capped to the monitor
// it is on (a 4K-sized window restored onto a 1080p screen is as stuck as an
// off-screen one) and nudged back inside if the cap moved it out.
//
// No monitors at all (a headless X server can report none) => `saved`
// unchanged if it has a size, else a bare kDefaultWidth x kDefaultHeight.
WindowGeometry FitToMonitors(const WindowGeometry& saved,
                             const std::vector<MonitorRect>& monitors);

inline constexpr double kDefaultFraction = 0.8;
inline constexpr int kDefaultWidth = 1280;
inline constexpr int kDefaultHeight = 720;
// Small enough to be a mistake rather than a preference. A window narrower
// than this cannot show the dock and the viewport at once.
inline constexpr int kMinWidth = 640;
inline constexpr int kMinHeight = 400;

} // namespace settings
