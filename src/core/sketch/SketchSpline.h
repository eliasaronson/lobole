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

// How the two pieces of a cut spline meet, as the curve did there: Smooth
// (a Coincident and a SplineJoint: C2 from degree 3, C1 at degree 2) where
// the spline with the cut's knot inserted once is that smooth; G1 (a
// Coincident and a Tangent) at a cubic's double knot, and mid-span at degree
// 1, where the pieces are collinear; C0, a Coincident alone, at a knot of
// full multiplicity (a polyline's corner).
enum class PieceJoint { C0, G1, Smooth };

// One piece of a cut spline: a storable spline, and for
// each of its poles the original pole it is (-1 for a pole the cut made).
struct SplinePiece {
  BSpline spline;
  std::vector<int> source;
};

// The two pieces cutting `s` at parameter `u` (strictly inside its domain,
// SplineDomain) makes, in order along it: `u` inserted to multiplicity p
// (fewer on an existing knot, snapped to within 1e-9 of a span) and the
// poles divided there, each piece keeping the parent's knots on its side,
// not renormalised (an empty vector when they come out clamped-uniform), so
// the two read the curve in one parameter. `before` is 1. `joints[0]` joins
// them. Every piece keeps `s`'s construction flag; ids are 0. Nullopt when
// `u` is not inside the domain or `s` has fewer than two poles.
struct SplineCut {
  std::vector<SplinePiece> pieces;
  std::vector<PieceJoint> joints;
  size_t before = 0; // pieces[0 .. before) lie before the cut
};
std::optional<SplineCut> CutSpline(const BSpline& s, double u);

// Whether the knot vector `U` of degree `p` is clamped with evenly spaced
// single interior knots: an affine image of the clamped-uniform vector, so
// the curve is the same stored with no knots at all.
bool UniformUpToScale(const std::vector<double>& U, int p);

// The clamped spline over [b0, b1] of the degree-`p` spline (U, P), where
// each of b0 and b1 is an end of the domain or a knot of multiplicity at
// least p: its poles and its knots (the parent's between b0 and b1, not
// renormalised). `src`, when given, runs beside `P` and is sliced into
// `srcOut`.
BSpline SliceSpline(const std::vector<double>& U,
                    const std::vector<glm::dvec2>& P, int p, double b0,
                    double b1, const std::vector<int>* src = nullptr,
                    std::vector<int>* srcOut = nullptr);

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
