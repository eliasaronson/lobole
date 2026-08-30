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

// The stock box for a setup, derived from the model bounds plus margins, IN
// SETUP SPACE (C9) — so an indexed job gets the box the machine will see.
// Pass model-space bodies; the framing happens here. Returns false when
// there is nothing to derive it from.
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

// NOTE: this one takes no Setup, so it cannot frame anything — the loops and
// the z come back in whatever space `bodies` are in. Callers working on an
// indexed setup pass BodiesInSetupFrame's output (C9).
[[nodiscard]] FaceRegion PickedFaceRegion(const std::vector<Body>& bodies,
                                          const ElementRef& ref,
                                          double deflection = 0.02);

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
  bool ok = false;
  std::string why; // when !ok: what to click instead, in words
};

[[nodiscard]] WallContour PickedWallContour(const std::vector<Body>& bodies,
                                            const ElementRef& ref,
                                            double deflection = 0.02);

// Whether `ref` names a vertical face — the question every caller asks before
// deciding which of the two picks above applies.
[[nodiscard]] bool IsWallFace(const std::vector<Body>& bodies,
                              const ElementRef& ref);

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

// `toolNumber` 0 = the setup's first tool. Every derived number is a fraction
// of the cutter, so an operation derived for one tool and cut with another
// carries the wrong stepdown.
[[nodiscard]] DerivedOp DeriveOperation(const Setup& setup,
                                        const std::vector<Body>& bodies,
                                        OpType type, const ElementRef& target,
                                        int toolNumber = 0);

// --- C9: the setup frame (indexed 4th axis) --------------------------------

// Whether this setup commands a rotary axis: the machine has one AND it is
// indexed off zero. The ONE question the toolpath and the post ask before
// putting an A word in a file — a rotated FIXTURE is not an answer to it.
[[nodiscard]] bool EmitsRotary(const Setup& setup);

// The other side of the same part: this fixture turned a half turn about the
// setup's own X, so what was against the bed faces the spindle. A flip job's
// second setup is DERIVED from its first — the alternative is typing an
// orientation twice and having the two disagree.
[[nodiscard]] glm::dquat FlipFrame(const glm::dquat& frame);

// Whether the part is clamped as modelled — the fixture frame is identity.
// Says nothing about `rotationA`: the two are separate claims.
[[nodiscard]] bool FrameIsSquare(const Setup& setup);

// Setup space -> model space: how the part is fixtured, then how far the
// table is indexed. The ONE place the two compose, so nothing downstream has
// to know there are two.
[[nodiscard]] glm::dmat3 SetupToModel(const Setup& setup);

// The bodies as this setup sees them: `SetupToModel` undone, so the setup's
// "up" is +Z again and the whole 2.5D pipeline — sectioning, face regions,
// stock bounds, the verifier — works unchanged on a part that is not modelled
// face-up. This is what the plan meant by generating toolpaths in SETUP
// SPACE: an orientation becomes a transform, not a rewrite.
//
// Element names survive: only the shape is transformed, so a machining
// selection still resolves.
[[nodiscard]] std::vector<Body> BodiesInSetupFrame(const Setup& setup,
                                                   const std::vector<Body>& in);

// The FIXTURING frame that lays `normal` under the spindle — the quaternion
// taking +Z to it. Any planar face has one; there is nothing to refuse.
//
// A normal fixes two of three rotational degrees of freedom; the spin about
// the spindle is still free, and `current` settles it: of all the frames that
// lay the face up, this returns the one nearest the frame already in hand.
// So clicking a face changes as little as possible, and clicking the same
// face twice is idempotent rather than a slow spiral.
[[nodiscard]] glm::dquat SolveFaceUp(const glm::dvec3& normal,
                                     const glm::dquat& current);

// The A that would turn `normal` to face the spindle, in degrees. Returns
// false when there is none: a face pointing along X can never face +Z under
// a rotation about X, and saying so beats rotating to the nearest wrong
// thing.
//
// This is the ROTARY path, and the restriction is real for a trunnion. For
// laying a part on the bed use `SolveFaceUp`, which has no such limit — a
// clamp can hold a part any way up.
[[nodiscard]] bool SolveIndexA(const glm::dvec3& normal, double& degrees);

// A point in setup space, back in model space — for DRAWING. Toolpaths,
// stock and ghosts are all computed where the machine works; the viewport
// shows the part where the user put it. Without this the backplot of an
// indexed job floats beside the model it belongs to.
[[nodiscard]] glm::dvec3 FromSetupSpace(const Setup& setup,
                                        const glm::dvec3& p);

// The rapid height for this job: clear of the stock, in absolute Z (which is
// what the toolpath emits). The default in the struct is a bare 5 mm, which
// sits INSIDE any stock taller than that — the verifier reported it as "rapid
// through material at line 11" the first time a setup was made by clicking,
// which is a broken cutter on a real machine.
[[nodiscard]] double DeriveSafeZ(const Setup& setup,
                                 const std::vector<Body>& bodies);

// Time spent CUTTING, from each move's own feed. Rapids are counted only when
// the setup states a rapid feed — inventing one would be a guess about the
// machine, and this number exists to be trusted.
[[nodiscard]] double CuttingMinutes(const Setup& setup,
                                    const std::vector<Toolpath>& paths);

} // namespace cam
