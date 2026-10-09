// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h" // Body
#include "core/cam/CamJob.h"
#include "core/cam/CamVerify.h" // HeightMap

#include <glm/glm.hpp>

#include <string>
#include <vector>

// The CUTTER-LOCATION surface: for each (x, y), the lowest Z at which this
// tool can sit without cutting into the model
// (docs/cam/operations.md#3-axis-surfacing).
//
// Everything in the 2.5D pipeline answers "what is the part's outline at this
// level". A finishing pass asks the other question -- "how low may the tool go
// HERE" -- and it is the only thing 3-axis surfacing needs that the app does
// not already have.
//
// The model surface dilated by the tool's own profile:
//
//   z_CL(x,y) = max over cells p within the tool's radius of
//                 ( z_surface(p) - TipHeight(tool, d(p)) )
//
// where d(p) is the distance to the NEAREST POINT of the square of material
// the sample p stands for, not to its centre: conservative by up to half a
// cell, and never into the part (docs/cam/operations.md#3-axis-surfacing).
//
// `TipHeight` is the same function `CarveSweep` verifies with, so the surface
// the generator plans against and the shape the checker removes cannot
// disagree about the cutter. opencamlib calls it `MillingCutter::height` and
// uses it for the VERTEX-drop case only, paying for facet and edge drop with a
// three-scalar offset-surface descriptor and a closed form per cutter. A grid
// dilation buys vertex and facet accuracy without either; measured at sample
// centres it lost precision on sharp convex edges -- most of a millimetre of
// gouge under a ball's steep rim -- which the nearest-point distance above
// turns into at most half a cell of material left instead.
namespace cam {

// What the drop cutter was asked for, and what it could not do.
struct CLSurface {
  HeightMap z;     // the cutter-location height, on its own grid
  bool ok = false; //
  std::string why; // when !ok: in words

  // Cells the tool can never reach, because nothing of the model lies under
  // them. They hold `floorZ` rather than a height, and a pass must not cut
  // to them -- it lifts. Counted so a caller can tell "the region is off the
  // part" from "the region is fine".
  size_t unreached = 0;
};

// How finely to drop-cut, and where.
struct DropRequest {
  // The box to cover, in the frame below. Only x/y are read; the z range is
  // the model's own.
  glm::dvec2 min{0.0, 0.0};
  glm::dvec2 max{0.0, 0.0};

  // Cell size, mm. Zero derives it from the tool and `cusp` -- the plan's
  // defaults rule, since a literal here is right for one cutter and wrong for
  // every other. Never coarser than the scallop the pass is allowed to leave,
  // or the surface cannot represent the thing it is planning.
  double res = 0.0;

  // The scallop a finish pass may leave, mm. Also the source of `res` when
  // that is zero. Zero takes 2% of the tool radius, which is what
  // `DerivedStepover` already solves the ball-nose cusp equation against.
  double cusp = 0.0;

  // Everything below this is off the part: a cell no model surface covers
  // gets this height and is counted in `unreached`.
  double floorZ = 0.0;

  // The rotation taking MODEL space to the frame the tool drops in. Identity
  // for every 3-axis job.
  //
  // It exists now, unused, for the same reason `Move` has carried an optional
  // A word since C9: rotary drop-cutting IS drop-cutting in a rotated frame,
  // and a drop cutter that hard-codes world -Z as the tool axis is the
  // difference between reusing this file and writing a second one. FreeCAD's
  // continuous-rotary layer cost 28% of its 3-axis surfacing precisely
  // because its generators took plain arrays and no world assumptions
  // (docs/cam/operations.md#3-axis-surfacing).
  glm::dquat frame{1.0, 0.0, 0.0, 0.0};
};

// Resolve `res` and `cusp` against the tool, so a caller can report what it
// is about to do and a test can assert the derivation without running one.
[[nodiscard]] double DerivedCusp(const Tool& tool, double requested);
[[nodiscard]] double DerivedDropRes(const Tool& tool, double requestedRes,
                                    double cusp);

// Drop `tool` over `bodies` across the request's box. Never throws: a failure
// comes back as `!ok` with a sentence, like every other CAM entry point.
//
// The bodies are meshed DETACHED at a tolerance derived from `res`: a
// triangulation rides on the shared TShape, so meshing a Body here at the
// fineness a finish pass needs would re-tessellate the document behind the
// renderer's back (see CADShape::GenerateMeshDetached).
[[nodiscard]] CLSurface DropCut(const std::vector<Body>& bodies,
                                const Tool& tool, const DropRequest& req);

} // namespace cam
