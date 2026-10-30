// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A section into a sketch (docs/sketching/README.md#a-section-into-a-sketch):
// where the sketch's plane cuts the bodies, as chained polylines in the
// plane's UV (the true cut: a mesh body's slice exactly, a B-Rep body's
// section curves to a deflection far under the tolerance), and those
// polylines refitted as sketch entities -- lines, lines and arcs, or
// B-splines -- within a stated tolerance, with the largest distance from the
// true cut to what was drawn.

#include "core/features/SketchFeature.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

struct Body;

namespace sketch {

// The fits project_section accepts, in the order the refusal names them.
inline constexpr const char* kSectionFits = "polyline, arcs, spline";
const char* ToString(SectionFit fit);
// Nullopt for anything else; the caller refuses naming kSectionFits.
std::optional<SectionFit> SectionFitFromString(const std::string& name);

// One chain of the cut, in the plane's UV. A closed one does not repeat its
// first point.
struct CutChain {
  std::vector<glm::dvec2> points;
  bool closed = false;
};

struct SectionCut {
  std::vector<CutChain> chains;
  // The cut bodies' bounding diagonal (mm): what a default tolerance is
  // derived from.
  double diagonal = 0.0;
  // A B-Rep section that OpenCASCADE could not compute, named.
  std::string error;
};

// The plane cutting `bodies` (reference bodies skipped). A mesh body is cut by
// mesh::Slice; a B-Rep body by a non-destructive BRepAlgoAPI_Section, its
// curves discretised to `deflection` mm and chained into wires.
SectionCut CutBodies(const std::vector<const Body*>& bodies,
                     const SketchPlane& plane, double deflection);

// The default tolerance for a cut of bodies whose bounding diagonal is
// `diagonal`: kSectionToleranceShare of it, 0.1 mm on a 100 mm scan.
inline constexpr double kSectionToleranceShare = 1.0e-3;
inline double DefaultSectionTolerance(double diagonal) {
  return diagonal * kSectionToleranceShare;
}

struct FittedSection {
  std::vector<Entity> entities; // construction is left false; ids 0
  // The largest distance from a point of the true cut to what was drawn
  // (mm).
  double deviation = 0.0;
};

// Each chain refitted within `tolerance` mm:
// - polyline: Douglas-Peucker, one line per kept span;
// - arcs: the longest line or three-point arc from each point that keeps
//   every point it spans within the tolerance, greedily;
// - spline: clamped uniform cubic B-splines (the sketch's own), least squares
//   with the ends pinned, as many poles as the tolerance needs; a chain past
//   255 poles (a sketch spline's limit) is split into pieces that meet, and
//   a closed chain is always two (an extrusion of a lone closed spline
//   weighs wrong: docs/known-bugs.md).
FittedSection FitSection(const std::vector<CutChain>& chains, SectionFit fit,
                         double tolerance);

// One chain as B-splines within `tolerance` (the spline fit above), the
// pieces joined end to end; `deviation` is the largest distance from a
// point to them. What an offset of a spline outline is turned back into
// sketch entities by, too.
std::vector<BSpline> FitSplines(const std::vector<glm::dvec2>& points,
                                bool closed, double tolerance,
                                double* deviation = nullptr);

} // namespace sketch
