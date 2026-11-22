// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The geometry of a sketch Split (docs/sketching/README.md#split): where a
// curve is crossed, the point of it nearest a cursor, and the pieces a cut
// makes. Pure -- nothing here changes a sketch; SketchFeature::SplitEntity
// applies the answer.

#include "core/features/SketchFeature.h"

#include <glm/glm.hpp>
#include <optional>
#include <vector>

namespace sketch {

// The curves Split takes: a line, an arc, a circle.
bool Splittable(const Entity& e);

// The length of a line, an arc or a circle; 0 for anything else.
double CurveLength(const Entity& e);

// The point of the line, arc or circle `e` nearest `p` (an arc's end when
// `p` is off its sweep), and how far it is. Nullopt for anything else.
struct NearestPoint {
  glm::dvec2 at{0.0};
  double distance = 0.0;
};
std::optional<NearestPoint> NearestOnCurve(const Entity& e, glm::dvec2 p);

// True when `p` is an end of the line or arc `e` (within `tol`), where a cut
// splits nothing. A circle has no end.
bool AtCurveEnd(const Entity& e, glm::dvec2 p, double tol = 1e-4);

// Every point where `other` crosses the line, arc or circle `e`, inside both:
// `other` is sampled along its own parameter and each sign change of its
// distance to `e` refined by bisection, so any curve (line, circle, arc,
// ellipse, elliptical arc, spline) crosses. A touch with no sign change is
// no crossing. Empty for a point, a label, or `e` not splittable.
std::vector<glm::dvec2> Crossings(const Entity& e, const Entity& other);

// The pieces cutting `e` at `cuts` (points on it) makes, in order along it: a
// line or an arc at one cut gives two, a circle at two gives two arcs, the
// first from cuts[0] counter-clockwise to cuts[1]. The first keeps `e`'s id;
// every piece keeps its construction flag; the others have id 0. Empty when
// the count is wrong for the curve.
std::vector<Entity> SplitPieces(const Entity& e,
                                const std::vector<glm::dvec2>& cuts);

} // namespace sketch
