// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The geometry of a sketch fillet (docs/sketching/README.md#fillet): which
// corner a click means, and the tangent arc that rounds it. Pure -- nothing
// here changes a sketch; SketchFeature::FilletCorner applies the answer.

#include "core/features/SketchFeature.h"

#include <expected>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace sketch {

// One curve end at a point: a line's 0/1, an (elliptical) arc's 1/2, an open
// spline's first or last pole. `leaving` is the unit direction the curve
// leaves the point in (its tangent there, pointing along the curve), and
// `angle` that direction's angle, in [0, 2π).
struct CurveEnd {
  uint32_t entity = 0;
  uint8_t which = 0;
  glm::dvec2 leaving{0.0};
  double angle = 0.0;
};

// The ends `e` has, by `which`: a line's 0/1, an (elliptical) arc's 1/2, an
// open spline's first and last pole. None for anything else.
std::vector<uint8_t> EndsOf(const Entity& e);

// Every curve end within `tol` of `vertex`, sorted by `angle`. Points,
// labels, circles, ellipses and closed splines have no end and are not
// listed.
std::vector<CurveEnd> CurveEndsAt(const std::vector<Entity>& entities,
                                  glm::dvec2 vertex, double tol = 1e-4);

// The corner a cursor at `cursor` means at `vertex`: the two curve ends
// either side of the sector that holds it (b counter-clockwise from a).
// Within `onVertex` of the vertex the cursor names no sector, which is fine
// where two curves end (the corner is the side under half a turn) and
// refused where more do. Refused, naming why: no curve or one curve ends
// there, the cursor is on a vertex more than two end at, the sector is wider
// than half a turn, or the two curves leave in one direction.
struct Corner {
  CurveEnd a, b;
  glm::dvec2 vertex{0.0};
};
std::expected<Corner, std::string> CornerAt(const std::vector<Entity>& entities,
                                            glm::dvec2 vertex,
                                            glm::dvec2 cursor, double onVertex,
                                            double tol = 1e-4);

// The ends two curves share: each pair (one end of each, by `which`) within
// `tol` of each other, and where. Only lines', arcs', elliptical arcs' and
// open splines' ends count.
struct SharedEnd {
  uint8_t w1 = 0, w2 = 0;
  glm::dvec2 at{0.0};
};
std::vector<SharedEnd> SharedEnds(const Entity& a, const Entity& b,
                                  double tol = 1e-4);

// The fillet of radius `radius` between curve `c1` at its end `w1` and `c2`
// at `w2`, the two ends meeting at the corner. Lines and arcs: the centre is
// where the curves' offsets by the radius toward the inside of the corner
// meet, nearest the corner (closed form). `trimmed1/2` are the curves cut
// back to their tangent points t1/t2, and `arc` runs from one tangent point
// to the other on the side facing the corner (`arcStartsAtT1` says which is
// its start).
struct FilletGeometry {
  glm::dvec2 corner{0.0}, center{0.0}, t1{0.0}, t2{0.0};
  Arc arc{};
  bool arcStartsAtT1 = true;
  Entity trimmed1, trimmed2;
};
// Why a fillet was refused. `tooLarge`: the curves take a fillet, just not
// this big one (the caller can search for the largest that fits).
struct FilletRefusal {
  std::string why;
  bool tooLarge = false;
};
std::expected<FilletGeometry, FilletRefusal>
FilletOfCurves(const Entity& c1, uint8_t w1, const Entity& c2, uint8_t w2,
               double radius);

// The largest radius at most `upTo` that FilletOfCurves accepts, found by
// bisection and rounded DOWN to three significant figures, so the number a
// message prints is one that fits. 0 when none does.
double LargestFillet(const Entity& c1, uint8_t w1, const Entity& c2, uint8_t w2,
                     double upTo);

} // namespace sketch
