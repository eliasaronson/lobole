// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>

#include <atomic>
#include <expected>
#include <string>
#include <vector>

struct Body;

namespace Measure {

// A seal is a contact that closes all the way round an axis
// (docs/inspect/README.md#seals). Seal() finds where two placed body sets
// meet -- where they overlap (a gasket pressed) and where faces lie on faces
// (a skirt on a shoulder) -- cuts that contact with a half-plane through the
// axis every 360/samples degrees, and follows each trace round: a ring is a
// path of traces, each touching the next, that comes back to the trace it
// left. A thread's flank contact goes round too, but comes back a pitch
// along: it is not a ring.
struct SealRing {
  double leastWidthMm = 0.0;        // the narrowest the ring's trace gets
  double atDeg = 0.0;               // where, about the axis
  double leastInterferenceMm = 0.0; // its thinnest overlap; 0 = touching
  double radiusMm = 0.0;            // its trace's mean distance from the axis
  glm::dvec3 centre{0.0};           // on the axis, level with its trace
};

struct SealArc {
  double fromDeg = 0.0, toDeg = 0.0;
};

// Angles are about the axis from world X (world Y for an axis near X),
// right-handed about axisDir.
struct SealReport {
  glm::dvec3 axisPoint{0.0}, axisDir{0.0, 0.0, 1.0};
  std::string axisFrom;          // which geometry gave the axis
  int samples = 0;               // half-planes, evenly round
  double touchTolMm = 0.0;       // faces nearer than this lie on each other
  double contactAreaMm2 = 0.0;   // where faces lie on faces
  double overlapVolumeMm3 = 0.0; // where the sets overlap
  std::vector<SealRing> rings;   // the closed ones, widest first
  std::vector<SealArc> open;     // where no trace is at all
  // Every half-plane has a trace, but no path closes: a helix.
  bool roundButOpen = false;
};

// The seal between body sets A and B, each placed by its matrix. An error
// when either side is empty, nothing touches, no axis can be found, or the
// kernel fails. `cancel` is polled between half-planes.
std::expected<SealReport, std::string>
Seal(const std::vector<Body>& bodiesA, const glm::dmat4& xfA,
     const std::vector<Body>& bodiesB, const glm::dmat4& xfB, int samples = 720,
     double touchTolMm = 1e-3, const std::atomic<bool>* cancel = nullptr);

} // namespace Measure
