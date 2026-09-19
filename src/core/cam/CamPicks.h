#pragma once

#include "core/Feature.h" // Body, ElementRef
#include "core/cam/CamJob.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

// What a CLICK on the model means to CAM: a face, and the region or contour
// it implies.
//
// Split out of CamToolpath.h, which had grown to five subjects in one header
// and was reached by 28 files -- `ui/AppState.h` among them, and that one is
// the compile-time bottleneck this project already has a review about. It
// needs `FaceRegion` and nothing else in there.
//
// CamToolpath.h still includes this, so nothing that used to see these
// declarations has to change to keep seeing them.
namespace cam {

// --- picking a face, and what it implies (docs/cam/gui.md G2) -------------

// A planar horizontal face of the model, as the region it bounds: the outer
// wire plus any inner wires (islands standing in a pocket). This is what a
// click on a pocket floor MEANS — the boundary to clear and the depth to
// clear it to — so it lives in the engine where a headless test can reach it.
struct FaceRegion {
  std::vector<Loop> loops; // outer (hole=false) first, then islands
  double z = 0.0;          // the face's own height, in setup space
  bool ok = false;
  std::string why; // when !ok: what to click instead, in words
};

// NOTE: this one takes no Setup, so it cannot frame anything — the loops and
// the z come back in whatever space `bodies` are in. Callers working on an
// indexed setup pass BodiesInSetupFrame's output (C9).
// `jobPartId` names WHICH part of the job the pick is on, and 0 means any.
// Feature ids are per document, so two parts on one sheet share them freely:
// without the qualifier the first body with that id answers, and the pick
// lands on the wrong part (or, when the name does not resolve there, reads as
// "that face is gone from the model"). It is the same qualifier a Target
// carries -- pass `target.jobPartId`.
[[nodiscard]] FaceRegion PickedFaceRegion(const std::vector<Body>& bodies,
                                          const ElementRef& ref,
                                          double deflection = 0.02,
                                          uint64_t jobPartId = 0);

// What a click on a WALL means. An opening that goes all the way through has
// no floor to click — a through slot's only faces are vertical — so a job
// that can only target level faces cannot machine one at all. Clicking the
// wall the cutter should follow says everything the operation needs: the
// contour is the section loop that wall belongs to, the depths are the wall's
// own extent, and whether it is an opening or the part's own perimeter is the
// difference between cutting inside the contour and outside it.
struct WallContour {
  Loop loop;            // the contour, in setup space
  double topZ = 0.0;    // the wall's own top …
  double bottomZ = 0.0; // … and bottom
  bool opening = false; // an inner boundary: cut INSIDE it
  // Where on the section the pick landed. `loop` is the contour AT THE WALL'S
  // OWN MID-HEIGHT, and an outline that changes with depth (a bore opening
  // onto a side face notches it at its own height and nowhere else) makes
  // that one answer wrong everywhere else — so a caller working down the
  // depths re-resolves the contour per pass, and this is what it resolves by.
  glm::dvec2 at{0.0, 0.0};
  bool ok = false;
  std::string why; // when !ok: what to click instead, in words
};

[[nodiscard]] WallContour PickedWallContour(const std::vector<Body>& bodies,
                                            const ElementRef& ref,
                                            double deflection = 0.02,
                                            uint64_t jobPartId = 0);

// The outline a cutter may FOLLOW at each of `passes`, which is not the
// part's cross-section there.
//
// A flat end mill at depth z removes the whole column above it at its
// footprint, so it may only travel where every height between `topZ` and z is
// waste. The governing shape is therefore the part's PROJECTION over
// [z, topZ] — the union of its sections — and the two differ exactly where a
// feature opens part-way down: a bore that opens onto a side face notches the
// section at its own height and nowhere else, and a profile that follows that
// notch cuts the bore's own depth plus the solid above and below it
// (docs/known-bugs.md, the front_wall gouge).
//
// Accumulated from `topZ` DOWNWARD, which is why it is a walk rather than a
// function of z. `topZ` and not the stock top: an operation's top is the
// declaration that everything above it is already clear, which is what lets
// an opening starting below the stock top be profiled at all.
//
// Inside and outside need no separate rule. The result carries its holes, so
// an outside cut follows its outer boundaries and an opening follows the hole
// it was picked in — the sweepable region is the complement of this one,
// either way. Returns one entry per pass, in the same order.
[[nodiscard]] std::vector<std::vector<Loop>>
ProjectedOutlines(const std::vector<Body>& bodies, double topZ,
                  const std::vector<double>& passes, double deflection = 0.02);

// What a click on a face of ANY orientation means for a finishing pass: the
// region to stay inside, as its SILHOUETTE in XY (docs/cam/surfacing.md).
//
// The two picks above both refuse curvature on purpose -- one wants a level
// face, the other a dead-vertical one -- and between them they cover the 2.5D
// question "what is the outline at this level". A surfacing pass asks a
// different one, and a curved or tilted face cannot answer it the same way: it
// has no single depth to machine to, which is exactly what PickedFaceRegion
// says when it refuses one.
//
// So the face contributes its OUTLINE and nothing else. The depths come from
// the cutter-location surface of the whole model (CamDropCutter.h), because a
// pass that took its Z from the picked face alone would gouge whatever stands
// beside it. "Finish within this face" is the honest reading of the verb.
//
// Being a B-Rep app is worth spending here. Kiri:Moto needs a dihedral-angle
// flood fill with a user-tunable threshold to turn a clicked triangle into a
// face; a B-Rep face IS that answer, with no threshold to get wrong.
struct SurfaceRegion {
  std::vector<Loop> loops; // outer (hole=false) first, then any holes
  double minZ = 0.0;       // the face's own Z extent, for reporting
  double maxZ = 0.0;
  bool ok = false;
  std::string why; // when !ok: what to click instead, in words
};

[[nodiscard]] SurfaceRegion PickedSurfaceRegion(const std::vector<Body>& bodies,
                                                const ElementRef& ref,
                                                double deflection = 0.02,
                                                uint64_t jobPartId = 0);

// The silhouette of every body in XY, for the no-pick form of the verb --
// "finish the whole part", which mirrors how Face and "cut out the whole
// part" already need no pick.
[[nodiscard]] SurfaceRegion WholePartSilhouette(const std::vector<Body>& bodies,
                                                double deflection = 0.02);

// Whether `ref` names a vertical face — the question every caller asks before
// deciding which of the two picks above applies.
[[nodiscard]] bool IsWallFace(const std::vector<Body>& bodies,
                              const ElementRef& ref, uint64_t jobPartId = 0);

} // namespace cam
