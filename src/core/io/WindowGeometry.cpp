// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/WindowGeometry.h"

#include <algorithm>

namespace settings {

namespace {

// Do the two rects share actual area? A shared EDGE is not an overlap: a
// window whose right edge touches the screen's left edge is entirely
// off-screen and no more reachable for it.
bool Overlaps(const WindowGeometry& g, const MonitorRect& m) {
  return g.x < m.x + m.w && g.x + g.w > m.x && g.y < m.y + m.h &&
         g.y + g.h > m.y;
}

// The monitor a geometry sits most of itself on.
const MonitorRect* BestMonitor(const WindowGeometry& g,
                               const std::vector<MonitorRect>& monitors) {
  const MonitorRect* best = nullptr;
  long bestArea = 0;
  for (const MonitorRect& m : monitors) {
    if (!Overlaps(g, m))
      continue;
    const long w = std::min(g.x + g.w, m.x + m.w) - std::max(g.x, m.x);
    const long h = std::min(g.y + g.h, m.y + m.h) - std::max(g.y, m.y);
    const long area = w * h;
    if (area > bestArea) {
      bestArea = area;
      best = &m;
    }
  }
  return best;
}

WindowGeometry Centred(const MonitorRect& m) {
  WindowGeometry g;
  g.w = std::max(kMinWidth, static_cast<int>(m.w * kDefaultFraction));
  g.h = std::max(kMinHeight, static_cast<int>(m.h * kDefaultFraction));
  g.w = std::min(g.w, m.w);
  g.h = std::min(g.h, m.h);
  g.x = m.x + (m.w - g.w) / 2;
  g.y = m.y + (m.h - g.h) / 2;
  return g;
}

} // namespace

WindowGeometry FitToMonitors(const WindowGeometry& saved,
                             const std::vector<MonitorRect>& monitors) {
  if (monitors.empty()) {
    // Nothing to check against — a headless X server can report no monitors.
    // Trust what we have rather than invent a position for a screen we
    // cannot see.
    if (saved.Remembered())
      return saved;
    WindowGeometry g;
    g.w = kDefaultWidth;
    g.h = kDefaultHeight;
    return g;
  }

  if (!saved.Remembered())
    return Centred(monitors.front());

  const MonitorRect* m = BestMonitor(saved, monitors);
  if (m == nullptr)
    return Centred(monitors.front()); // the monitor it was on is gone

  WindowGeometry g = saved;
  // Cap to the monitor BEFORE nudging: a window restored from a 4K screen
  // onto a 1080p one is as stuck as an off-screen one, and shrinking it
  // moves where "inside" is.
  g.w = std::clamp(g.w, std::min(kMinWidth, m->w), m->w);
  g.h = std::clamp(g.h, std::min(kMinHeight, m->h), m->h);
  g.x = std::clamp(g.x, m->x, m->x + m->w - g.w);
  g.y = std::clamp(g.y, m->y, m->y + m->h - g.h);
  return g;
}

} // namespace settings
