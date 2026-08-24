// Restoring a window to a screen that may no longer exist.
//
// The saving half is easy; this is the half that strands people. Unplug the
// monitor a window was on, or dock a laptop and undock it, and a naive
// restore puts the window somewhere no cursor can reach — with no way to
// move it, because the title bar is off-screen too. Pure and in core so it
// can be held against a synthetic monitor list with no display server
// (core/io/WindowGeometry.h).

#include <catch2/catch_test_macros.hpp>

#include "core/io/WindowGeometry.h"

using settings::FitToMonitors;
using settings::MonitorRect;
using settings::WindowGeometry;

namespace {
const std::vector<MonitorRect> kLaptop = {{0, 0, 1920, 1050}};
// A 4K panel to the RIGHT of the laptop, as a docked desk looks.
const std::vector<MonitorRect> kDocked = {{0, 0, 1920, 1050},
                                          {1920, 0, 3840, 2100}};
} // namespace

TEST_CASE("a first run takes a share of the screen, centred", "[window]") {
  const WindowGeometry g = FitToMonitors(WindowGeometry{}, kLaptop);
  CHECK(g.w == 1536); // 80% of 1920 — not a literal 1280, which is a third
  CHECK(g.h == 840);  // of a 1440p panel and most of a laptop's
  CHECK(g.x == (1920 - 1536) / 2);
  CHECK(g.y == (1050 - 840) / 2);
}

TEST_CASE("a remembered rect that still fits is left alone", "[window]") {
  WindowGeometry saved{100, 80, 1200, 800, false};
  CHECK(FitToMonitors(saved, kLaptop) == saved);
}

TEST_CASE("a maximised flag survives, and does not need a rect", "[window]") {
  WindowGeometry saved{100, 80, 1200, 800, true};
  const WindowGeometry g = FitToMonitors(saved, kLaptop);
  CHECK(g.maximized);
  // The un-maximised rect is what it restores DOWN to, so it is kept.
  CHECK(g.w == 1200);
}

TEST_CASE("a window on a monitor that is gone comes home", "[window]") {
  // Saved on the 4K panel, restored with only the laptop present. Without
  // this the window opens at x=2400 on a 1920-wide desktop: invisible, and
  // unreachable because its title bar is off-screen too.
  WindowGeometry saved{2400, 200, 1600, 1000, false};
  const WindowGeometry g = FitToMonitors(saved, kLaptop);
  CHECK(g.x >= 0);
  CHECK(g.x + g.w <= 1920);
  CHECK(g.y + g.h <= 1050);

  // ...and with the panel plugged back in, it is left exactly where it was.
  CHECK(FitToMonitors(saved, kDocked) == saved);
}

TEST_CASE("a shared EDGE is not being on a screen", "[window]") {
  // Right edge exactly on the laptop's left edge: zero pixels visible. An
  // overlap test written with >= would call this fine.
  WindowGeometry saved{-1200, 100, 1200, 800, false};
  const WindowGeometry g = FitToMonitors(saved, kLaptop);
  CHECK(g.x >= 0);
}

TEST_CASE("a rect too big for the screen it lands on is capped", "[window]") {
  // Saved from the 4K panel, restored onto the laptop. As stuck as an
  // off-screen window if it is not shrunk: the bottom and right are simply
  // unreachable.
  WindowGeometry saved{0, 0, 3200, 1900, false};
  const WindowGeometry g = FitToMonitors(saved, kLaptop);
  CHECK(g.w <= 1920);
  CHECK(g.h <= 1050);
  CHECK(g.x + g.w <= 1920);
  CHECK(g.y + g.h <= 1050);
}

TEST_CASE("no monitors at all is not a reason to move the window", "[window]") {
  // A headless X server can report none. Inventing a position for a screen
  // we cannot see is worse than trusting what we have.
  WindowGeometry saved{10, 20, 1200, 800, false};
  CHECK(FitToMonitors(saved, {}) == saved);
  const WindowGeometry fresh = FitToMonitors(WindowGeometry{}, {});
  CHECK(fresh.w == settings::kDefaultWidth);
  CHECK(fresh.h == settings::kDefaultHeight);
}
