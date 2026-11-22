// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A sketch spline cut exactly (docs/sketching/README.md#a-spline-cut-exactly):
// knot insertion, the pieces a cut makes, and the derivatives a fillet's
// Newton step reads. Pure -- nothing here changes a sketch.

#include "core/features/SketchFeature.h"

#include <glm/glm.hpp>
#include <optional>
#include <vector>

namespace sketch {

// Inserts knot `u` once into the degree-`p` spline (knots `U`, poles `P`) by
// Boehm's rule: the curve does not change. `source`, when given, runs beside
// `P` naming the original pole each one is (-1 for a pole the insertion
// made), and is kept up to date. `u` may be an unclamped end of the domain.
void InsertKnot(std::vector<double>& U, std::vector<glm::dvec2>& P, int p,
                double u, std::vector<int>* source = nullptr);

// How two consecutive pieces of a cut spline meet: where the curve was
// C2 or smoother, G2 (a Coincident and a Curvature); C1, G1 (a Coincident
// and a Tangent); C0, a Coincident alone.
enum class PieceJoint { C0, G1, G2 };

// One piece of a cut spline: a storable (clamped-uniform) spline, and for
// each of its poles the original pole it is (-1 for a pole the cut made).
struct SplinePiece {
  BSpline spline;
  std::vector<int> source;
};

// The pieces cutting `s` at parameter `u` (in its domain [0, n-p], strictly
// inside) makes, in order along it. Over the span u lies in, each side is a
// Bezier; past the next knot, raised to full multiplicity, the whole spans
// are one clamped-uniform spline; a side inside one span is one Bezier, and
// a cut on a knot leaves two clamped-uniform splines. `before` pieces end at
// u, the rest start there. `joints[i]` joins pieces i and i+1. Every piece
// keeps `s`'s construction flag; ids are 0. Nullopt when `u` is not inside
// the domain or `s` has fewer than two poles.
struct SplineCut {
  std::vector<SplinePiece> pieces;
  std::vector<PieceJoint> joints;
  size_t before = 0; // pieces[0 .. before) lie before the cut
};
std::optional<SplineCut> CutSpline(const BSpline& s, double u);

// The parameter of the point of `s` nearest `p`: a sample per 1/64 of a span,
// then Newton on the distance.
double SplineParamNearest(const BSpline& s, glm::dvec2 p);

// The first and second derivatives of `s` with respect to its parameter at
// `u` (the hodograph, exact). Zero where the degree is too low.
glm::dvec2 BSplineDerivUV(const BSpline& s, double u);
glm::dvec2 BSplineDeriv2UV(const BSpline& s, double u);

// Whether `s` is closed (its first and last poles equal, three or more).
bool SplineClosed(const BSpline& s);

} // namespace sketch
