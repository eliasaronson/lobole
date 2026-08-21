#pragma once

#include "core/Feature.h" // Body
#include "core/cam/CamJob.h"

#include <glm/glm.hpp>

#include <vector>

// Toolpath generation for 2.5D milling (docs/cam/plan.md).
//
// The pipeline, and where each piece comes from:
//
//   bodies --SectionCaps--> planar cross-section at Z   (already ours, I3)
//          --wire walk-->   ordered 2D loops
//          --Clipper2-->    offset / clear              (Boost-licensed dep)
//          --link-->        moves with lead-in, plunge and retract
//
// Everything here is headless and unit-testable: bodies in, moves out, no
// ImGui and no file writing. Posting those moves to a dialect is CamPost.
namespace cam {

// A closed contour on a Z plane, in setup space. `hole` marks a loop that
// bounds a void rather than material — the distinction Clipper needs to
// respect islands in a pocket.
struct Loop {
  std::vector<glm::dvec2> pts;
  bool hole = false;
};

// The cross-section of `bodies` at height `z`, as ordered closed loops.
// `deflection` is the chord tolerance for curved edges (mm) — the same knob
// tessellation uses, and the reason a circular pocket comes out as a polygon
// rather than an arc until arc-fitting lands.
[[nodiscard]] std::vector<Loop> SectionLoops(const std::vector<Body>& bodies,
                                             double z,
                                             double deflection = 0.02);

// The Z levels a stepped operation visits: topZ - stepdown, … , bottomZ, with
// the last pass landing exactly on bottomZ (a short final pass is correct; a
// pass 0.02 mm deeper than asked is a scrapped part).
[[nodiscard]] std::vector<double> DepthPasses(const Operation& op);

// One operation's toolpath. Never throws: a failure comes back as
// `Toolpath::error`, because a CAM job with one bad op should still show the
// other five.
[[nodiscard]] Toolpath Generate(const Setup& setup, const Operation& op,
                                const std::vector<Body>& bodies);

// Every enabled operation, in order.
[[nodiscard]] std::vector<Toolpath>
GenerateAll(const Setup& setup, const std::vector<Body>& bodies);

// The stock box for a setup, derived from the model bounds plus margins.
// Returns false when there is nothing to derive it from.
bool StockBounds(const Setup& setup, const std::vector<Body>& bodies,
                 glm::dvec3& min, glm::dvec3& max);

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

[[nodiscard]] FaceRegion PickedFaceRegion(const std::vector<Body>& bodies,
                                          const ElementRef& ref,
                                          double deflection = 0.02);

// The operation a verb produces from a picked face. Every depth here is
// derived — from the face, the stock and the tool — which is the whole point:
// the `2 -> -0` failure the panel used to allow cannot be expressed.
//
// `target` may be unset for the verbs that need no pick (Face the top, and
// "cut out the whole part").
struct DerivedOp {
  bool ok = false;
  std::string why; // when !ok: why this verb cannot apply here
  Operation op;    // id left 0 — the caller owns id allocation
};

[[nodiscard]] DerivedOp DeriveOperation(const Setup& setup,
                                        const std::vector<Body>& bodies,
                                        OpType type, const ElementRef& target);

// Time spent CUTTING, from each move's own feed. Rapids are counted only when
// the setup states a rapid feed — inventing one would be a guess about the
// machine, and this number exists to be trusted.
[[nodiscard]] double CuttingMinutes(const Setup& setup,
                                    const std::vector<Toolpath>& paths);

} // namespace cam
