// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"

#include <string>
#include <vector>

// Building flanges: a bend plus a wall, swept along a straight panel edge
// (docs/sheetmetal/README.md#features). One builder for every feature that
// bends a wall up from an edge — the edge flange, the hem — so they cannot
// disagree about where a bend line is or what a bend records.
namespace sheet {

// The local frame of a flangeable edge. `e` runs from `p0` to `p1`;
// `outward` is the rim's own outward normal, so it points away from the
// material along the panel; `up` is the outward normal of the PICKED panel
// face, which is what decides which way the wall folds — picking the edge on
// the far face of the same rim bends the other way.
struct EdgeFrame {
  gp_Pnt p0, p1;
  gp_Dir e, outward, up;
  double length = 0.0;
};

// Resolves a picked edge of `body` to its frame. Every refusal names what is
// wrong with the PICK — a curved edge, a corner where two rims meet, an edge
// of something that is not a sheet — because "flange failed" sends the caller
// looking at the numbers instead of the selection.
[[nodiscard]] bool ResolveEdgeFrame(const Body& body, const ElementRef& ref,
                                    double thickness, EdgeFrame& out,
                                    std::string& error);

// What one flange is, once its feature has turned its own parameters into
// geometry: the same for every edge the feature picked.
struct FlangeSpec {
  double radius = 0.0;  // inside, mm
  double angle = 0.0;   // radians, (0, pi]
  double wall = 0.0;    // flat wall past the bend, mm
  double setback = 0.0; // from the picked edge back to the tangent line, mm
  double offsetStart = 0.0, offsetEnd = 0.0; // insets from the edge's ends
  ReliefStyle relief = ReliefStyle::None;
  double reliefWidth = 0.0, reliefDepth = 0.0;
  // Where two of the picked edges meet at a corner and their flanges only
  // touch at a point (bent outside the panel), close the corner: extend each
  // past the corner as far as the other sticks out, then mitre them.
  bool closedCorners = false;
  // A hem rather than a bend: crushed flat, so exempt from the material's
  // minimum bend radius (the radius here is the gap, not a die).
  bool crushed = false;
};

// Builds one flange per picked edge onto the bodies they belong to and
// records a bend for each. Two flanges that meet at a corner and would
// otherwise overlap are mitred: each is trimmed back from the bisector plane
// of the corner by half of `miterGap`, and the trim is recorded on its bend.
// Returns the reason on failure (empty = done); on failure the bodies are
// untouched.
[[nodiscard]] std::string ApplyFlanges(std::vector<Body>& bodies,
                                       const std::vector<ElementRef>& edges,
                                       const FlangeSpec& spec,
                                       const SheetMetalRules& rules,
                                       uint64_t featureId, double miterGap);

} // namespace sheet
