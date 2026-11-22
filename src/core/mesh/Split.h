// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What a region grown from a picked point found when one surface does not
// explain it (docs/modeling/README.md#two-surfaces): two surfaces of the
// kind asked, side by side or stepping -- a stepped bore, a lip, two faces
// a few tenths apart -- or, for a plane, a strip along the crown of a round.
// GrowFrom refuses both, naming what is there, rather than reporting a fit
// that averages them.

#include "core/mesh/Fit.h"
#include "core/mesh/TriMesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mesh {

// The split is looked for only when the region's rms is past this share of
// the scan's noise tolerance: that tolerance is about six sigma of a
// vertex's noise (ScanTolerance), so a third of it is twice the noise's
// sigma -- something besides noise is in the region.
inline constexpr double kSplitAbove = 1.0 / 3.0;
// Each of the two holds at least this share of the region's vertices.
inline constexpr double kSplitShare = 0.15;
// They stand further apart than this many times the larger one's scatter.
inline constexpr double kSplitApart = 4.0;
// One smooth surface -- a quadric for planes, a radius varying evenly along
// the axis for cylinders -- explains the region as well when its rms is
// within this factor of the two surfaces': a crowned plate or a taper.
inline constexpr double kSplitSmooth = 1.5;
// A plane whose normals spread past this, or that covers less than
// kCrownCover of its own bounding rectangle while it bows past the noise,
// is a strip along the crown of a round.
inline constexpr double kCrownSpreadDeg = 10.0;
inline constexpr double kCrownCover = 0.15;

// What SplitTwo found.
struct TwoSurfaces {
  // The region's rms was past kSplitAbove of the noise, and the kind is one
  // the split is defined for (a plane or a cylinder): the split was tried.
  bool examined = false;
  // It is two surfaces: every test below held.
  bool two = false;
  // The two, each refitted over its own vertices: for cylinders `a` is the
  // smaller radius; for planes the lower along `a`'s normal.
  FitResult a, b;
  double shareA = 0.0, shareB = 0.0; // of the region's vertices
  double scatter = 0.0;   // the larger robust sigma of each from its own (mm)
  double apart = 0.0;     // the smaller median distance of each from the other
  double twoRms = 0.0;    // every vertex from its own surface (mm)
  double smoothRms = 0.0; // every vertex from the one smooth surface (mm)
  // Cylinders: whether they step along the axis, and where: `stepAt` mm
  // along `axis` (largest component positive) from `from`, the axis point
  // at the region's end.
  bool steps = false;
  glm::dvec3 from{0.0}, axis{0.0, 0.0, 1.0};
  double stepAt = 0.0;
  // Planes: how far apart they are at `a`'s middle, and the angle between
  // them (rad); `throughA` and `throughB` the middle of each.
  double offset = 0.0, angle = 0.0;
  glm::dvec3 throughA{0.0}, throughB{0.0};
  // The region's triangles by the surface most of their corners are on.
  std::vector<uint32_t> trianglesA, trianglesB;

  // "two cylinders here: R4.68 mm and R5.18 mm, stepping at 12.4 mm along
  // the axis [..] from [..]; click one, or give radius_mm": the refusal.
  [[nodiscard]] std::string Describe() const;
};

// Whether `region` (triangles of `m`), fitted by `fit`, is two surfaces of
// `fit`'s kind: split in two by distance to two models (from several
// starts: the residual's sign, and the region cut across at its quartiles),
// each refitted until the split stops changing, then judged -- both hold a
// real share, they stand further apart than their scatter, and one smooth
// surface does not explain the region as well. `noiseMm` is the scan's
// noise tolerance (ScanTolerance), what kSplitAbove is a share of.
TwoSurfaces SplitTwo(const TriMesh& m, const std::vector<uint32_t>& region,
                     const FitResult& fit, double noiseMm);

// How a plane region bows: the angle its normals turn across it, from the
// quadric through its vertices (so a scan's noisy facets do not count),
// and the share of its own bounding rectangle it covers.
struct PlaneBow {
  double spreadRad = 0.0;
  double cover = 1.0;
  [[nodiscard]] bool Crown(bool bowsPastNoise) const;
};
// `extent` is the region's used points' spans along their principal
// directions (Region::extent), `area` its area.
PlaneBow BowOf(const TriMesh& m, const std::vector<uint32_t>& region,
               const FitResult& plane, const glm::dvec2& extent, double area);

} // namespace mesh
