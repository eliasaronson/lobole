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
#include <string>
#include <vector>

namespace units {
struct System;
}

namespace sketch {

// The curves Split takes: a line, an arc, a circle, an ellipse, an
// elliptical arc, a spline.
bool Splittable(const Entity& e);

// The length of a curve Split takes (an ellipse's and a spline's measured
// along a fine polyline); 0 for anything else.
double CurveLength(const Entity& e);

// The point of the curve `e` nearest `p` (an open curve's end when `p` is
// past it), and how far it is. Nullopt for a point or a label.
struct NearestPoint {
  glm::dvec2 at{0.0};
  double distance = 0.0;
};
std::optional<NearestPoint> NearestOnCurve(const Entity& e, glm::dvec2 p);

// True when `p` is an end of the open curve `e` (within `tol`), or the seam
// of a closed spline, where a cut splits nothing. A circle and an ellipse
// have no end.
bool AtCurveEnd(const Entity& e, glm::dvec2 p, double tol = 1e-4);

// Every point where `other` crosses the curve `e`, inside both. Against a
// line, an arc or a circle, the other curve is sampled along its own
// parameter and each sign change of its distance refined by bisection (a
// line and a circle in closed form); two curves neither of which is one of
// those cross where their fine polylines do, refined by Newton on both
// parameters. A touch with no sign change is no crossing. Empty for a
// point or a label.
std::vector<glm::dvec2> Crossings(const Entity& e, const Entity& other);

// The parameter of the point `p` (on it) along an ellipse, an elliptical arc
// (its ellipse's parameter, inside the sweep) or a spline. Nullopt for
// anything else.
std::optional<double> CurveParam(const Entity& e, glm::dvec2 p);

// The cuts a curve takes: two for a circle or an ellipse, one otherwise (a
// closed spline's seam is its other cut).
size_t CutsFor(const Entity& e);

// The pieces cutting `e` at `cuts` (points on it) makes, in order along it: a
// line, an arc or an elliptical arc at one cut gives two, a circle or an
// ellipse at two gives two arcs, the first from cuts[0] counter-clockwise to
// cuts[1], and a spline at one cut gives the pieces sketch::CutSpline makes
// (SketchSpline.h). The first keeps `e`'s id -- of a spline's, the longest
// does; every piece keeps its construction flag; the others have id 0.
// Empty when the count is wrong for the curve.
std::vector<Entity> SplitPieces(const Entity& e,
                                const std::vector<glm::dvec2>& cuts);

// The index, among a cut spline's pieces, of the one that keeps its id: the
// longest.
size_t KeeperOf(const std::vector<BSpline>& pieces);

// The unit tangent of curve `e` at the point `p` on it (along its
// parameter); +U for a point or a label.
glm::dvec2 TangentAt(const Entity& e, glm::dvec2 p);

// The word a sketch file spells constraint kind `k` in ("point_on_line").
const char* ConstraintWord(ConstraintKind k);

// Constraint kind `k` as a person reads it ("point on curve", "horizontal
// distance").
const char* ConstraintPhrase(ConstraintKind k);

// What a spline's cut drops, said the way a person sees it: a dimension by
// its kind and value in `sys`, the rest by kind, and where it sat --
// "distance 2.24 mm on the spline's 3rd pole", "tangent at the end of the
// spline the cut removes", "the expression on the spline's 4th pole's x".
// No ids: a script reads those off the CutDrop.
std::string DescribeCutDrop(const SketchFeature::CutDrop& d,
                            const units::System& sys);

} // namespace sketch
