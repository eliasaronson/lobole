// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/features/SketchFeature.h"

#include <glm/glm.hpp>

#include <map>
#include <string>
#include <vector>

// A 2D drawing read into a sketch: what the DXF and SVG readers share
// (docs/sketching/README.md#importing-dxf-and-svg). Each reader turns its file
// into sketch entities in millimetres, in the drawing's own frame (Y up), and
// says what it left out and where its unit came from. The entities are baked —
// ordinary, editable sketch geometry — so nothing links back to the file.
//
// Everything a reader produces goes through the affine below, which keeps a
// conic exact: an affine image of a circle is an ellipse, of an arc an
// elliptical arc, and a mirror reverses an arc into the CCW-always model.
namespace sketchimport {

// p -> lin * p + off.
struct Affine2 {
  glm::dmat2 lin{1.0};
  glm::dvec2 off{0.0};
  [[nodiscard]] glm::dvec2 Apply(glm::dvec2 p) const { return lin * p + off; }
  // (this ∘ inner): inner first, then this.
  [[nodiscard]] Affine2 Then(const Affine2& inner) const {
    return {lin * inner.lin, lin * inner.off + off};
  }
};

// The conic c + L (cos t, sin t) for t from t0 to t1, increasing (`full` for
// the whole curve), as the sketch entity that draws exactly it: a Circle or
// Arc when L is a rotation times a scale (with or without a mirror), else an
// Ellipse or EllipseArc. A mirror (det L < 0) reverses the sweep, since the
// model is CCW-always.
[[nodiscard]] sketch::Entity ConicEntity(glm::dvec2 c, const glm::dmat2& L,
                                         double t0, double t1, bool full);

// `e` mapped through `a`, exactly: points and spline poles move, a conic is
// re-derived through ConicEntity. Keeps the entity's construction flag.
[[nodiscard]] sketch::Entity TransformEntity(const sketch::Entity& e,
                                             const Affine2& a);

// A non-rational B-spline of any knot vector as clamped pieces the sketch
// stores exactly: itself when its knots are clamped-uniform and it has at
// most 255 poles, else one Bezier piece per knot span (knot insertion, which
// changes no point of the curve).
[[nodiscard]] std::vector<sketch::BSpline>
SplineToSketch(const std::vector<double>& knots,
               const std::vector<glm::dvec2>& poles, int degree);
// The curve itself at u (de Boor), for checking the pieces against.
[[nodiscard]] glm::dvec2 EvalSpline(const std::vector<double>& knots,
                                    const std::vector<glm::dvec2>& poles,
                                    int degree, double u);

// What a reader hands back.
struct Drawing {
  std::vector<sketch::Entity> entities; // mm, the drawing's frame, Y up
  // Left out, by kind ("TEXT", "text", "hidden"), with how many.
  std::map<std::string, int> notImported;
  // What the reader wants said: "3 stroked shapes imported as centrelines".
  std::vector<std::string> notes;
  std::string unit;       // "mm", "in", "px", ...
  std::string unitSource; // "$INSUNITS 1", "the default: none declared", ...
};

// The endpoints of a drawing as one point each: an endpoint within `tol` of
// an earlier one moves onto it, so a chain written with rounding closes. An
// arc's ends are where its geometry puts them, so lines and splines move to
// meet arcs, never the other way.
void SnapEndpoints(std::vector<sketch::Entity>& entities, double tol);

// Endpoints that join nothing within `tol` (construction excluded): why an
// outline will not extrude.
[[nodiscard]] std::vector<glm::dvec2>
OpenEnds(const std::vector<sketch::Entity>& entities, double tol);

// The box the entities span (conics by their extremes), for centring.
[[nodiscard]] bool Bounds(const std::vector<sketch::Entity>& entities,
                          glm::dvec2& lo, glm::dvec2& hi);

// The tolerance endpoints are joined at, the one the profile builder chains
// with (core/sketch/SketchProfile.cpp).
inline constexpr double kChainTol = 1e-4;

} // namespace sketchimport
