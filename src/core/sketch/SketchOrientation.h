// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The orientations a sketch's dimensions leave open, read from geometry
// alone (no PlaneGCS): what a value edit must not turn over
// (docs/sketching/README.md#solving). A triangle of unsigned distances and an
// arc's sweep have no constraint that knows their side, so one solver jump
// can land on the mirror image or the complement arc; the solver compares
// these before and after.

#include "core/features/SketchFeature.h"

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace sketch {

struct Orientation {
  enum class Kind { Triangle, ArcSweep };
  Kind kind = Kind::Triangle;
  std::array<PointRef, 3> corners{}; // Triangle: one point of each corner
  uint32_t arc = 0;                  // ArcSweep: the arc's entity id
  std::vector<size_t> constraints;   // the constraints that set it, ascending
};

// True for a point the solver may not move (datum, Fixed, projected,
// linked-copy output, expression-driven). A triangle with every corner held
// and an arc with every point held cannot turn, so they are not listed.
using PointHeld = std::function<bool(const PointRef&)>;

// Triangles: the points Coincident joins are one corner; a Distance between
// two corners is an edge, as is the pair of any two held corners (their
// distance is fixed); every triangle of edges with a free corner is one
// orientation, its constraints the distances on its edges. Capped (a few
// hundred triangles, a work limit on the search) so a large sketch costs
// nothing noticeable. Arcs: every arc with a point the solver moves, its
// constraints the dimensions on the arc and on its ends and centre.
std::vector<Orientation>
FindOrientations(const std::vector<Entity>& entities,
                 const std::vector<Constraint>& constraints,
                 const PointHeld& held = {});
inline std::vector<Orientation> FindOrientations(const SketchFeature& sketch,
                                                 const PointHeld& held = {}) {
  return FindOrientations(sketch.entities, sketch.constraints, held);
}

// Each orientation's sign in `entities`: +1 / -1, or 0 where it is not
// clear -- a triangle whose height is within 1/1000 of its longest side, an
// arc whose sweep is within 1e-3 rad of a half turn (solver noise decides
// the sign there, so nothing is refused for it) -- or its geometry missing.
std::vector<int> OrientationSigns(const std::vector<Orientation>& orientations,
                                  const std::vector<Entity>& entities);

// The value a dimension has in the given geometry, with the solver's sign
// conventions: Distance, DistancePL, Radius, Diameter and ArcLength (r x
// sweep) unsigned; DistanceX/Y
// b − a; Angle the turn from line a to line b, radians in (−π, π]. nullopt for
// a constraint that is not a dimension or whose refs do not resolve.
std::optional<double> MeasureDimension(const std::vector<Entity>& entities,
                                       const Constraint& constraint);
std::optional<double> MeasureDimension(const SketchFeature& sketch,
                                       size_t constraint);

} // namespace sketch
