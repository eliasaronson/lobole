#pragma once

#include "core/Feature.h" // Body
#include "core/cam/CamJob.h"

#include <glm/glm.hpp>

#include <string>
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
                                            double deflection = 0.02);

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

// Where the workpiece is, in setup space, when the stock is FIXED: the model
// box positioned inside the typed size by `Stock::place`. Relative mode is the
// model box plus margins, as it always was. One function answers "what is the
// workpiece" for safe Z, derived depths, the verifier, the simulator and the
// plan view, which is why a second stock mode is not a rewrite.

// Empty when this setup has a workpiece it can cut; otherwise why not, in
// words. A fixed stock smaller than the part is the case worth naming: the
// job is not merely wrong, it does not exist.
[[nodiscard]] std::string StockRefusal(const Setup& setup,
                                       const std::vector<Body>& bodies);

// Put `setup.origin` where `setup.originAt` says, in setup space. The Z comes
// from the picked top face when there is one — that is what "Pick top face"
// has always promised — and from the stock top otherwise, because that is the
// face an operator touches off on.
//
// Call it after ANYTHING that moves the box: the stock, the fixture frame, the
// top-face pick, the model. A stale origin is a file whose every coordinate is
// offset by the amount the box moved.
[[nodiscard]] bool RecomputeOrigin(Setup& setup,
                                   const std::vector<Body>& bodies);

// Re-derive every operation's DEPTHS in the setup's current frame, returning
// how many moved. `topZ`/`bottomZ` are heights in setup space, so re-clamping
// the part leaves them measuring from a box that has moved — a cut that was
// through the panel becomes a cut through air, or through the spoilboard.
//
// Only operations that name a target are touched: those are the ones whose
// numbers were derived from the model in the first place, and there is
// nothing to re-derive the others from.
int RederiveDepths(Setup& setup, const std::vector<Body>& bodies);

// How far to step over per pass, as a fraction of the cutter's diameter.
//
// A flat mill leaves a flat floor whatever the stepover, so the rule of thumb
// stands: 45% of the cutter. A BALL NOSE does not — between two passes it
// leaves a cusp, and the height of that cusp is what decides the stepover, not
// a number of millimetres. Solving `cusp = r - sqrt(r^2 - (s/2)^2)` for the
// step gives `s = 2*sqrt(2*r*cusp - cusp^2)`.
//
// The cusp target is 2% of the tool's RADIUS — derived from the cutter like
// every other default here, so it stays right for a 1 mm ball and a 12 mm one.
// On a 6 mm ball that is 0.06 mm of ridge and a 1.7 mm step, against the 2.7
// a flat mill would take: slower, and the floor is the floor that was asked
// for. Before this a ball nose cleared a pocket on exactly the flat mill's
// path and left 160 mm2 of scallop that only the verifier ever mentioned.
[[nodiscard]] double DerivedStepover(const Tool& tool);

// Half the cutter per pass: the rule of thumb every 2.5D job starts from,
// expressed in the TOOL so it stays right for a 1 mm cutter and a 12 mm one.
// One derivation, three callers — the verbs, a retool, and the panel.
[[nodiscard]] inline double DerivedStepdown(const Tool& tool) {
  return tool.diameter * 0.5;
}

// Point `op` at a different cutter. Every derived number is a fraction of the
// cutter, so a stepdown still sitting at the OLD tool's derived value follows
// the new one: a 6 mm end mill's 3 mm pass handed to a 3 mm cutter is a
// full-diameter plunge. A stepdown the user typed is theirs, and is kept.
//
// Refuses a tool the setup does not have rather than falling back to tool 1 —
// the substitute cuts perfectly well, at the wrong feeds, with the wrong
// cutter.
[[nodiscard]] bool Retool(const Setup& setup, Operation& op, int toolNumber,
                          std::string& why);

// Move an operation to `toIndex`. Order is load-bearing: `GenerateAll` and the
// post both walk the vector, so it decides the cutting sequence and where the
// tool changes — and their `M0` pauses — fall.
[[nodiscard]] bool MoveOperation(Setup& setup, uint64_t opId, size_t toIndex);

// `toolNumber` 0 = the setup's first tool. Every derived number is a fraction
// of the cutter, so an operation derived for one tool and cut with another
// carries the wrong stepdown.
[[nodiscard]] DerivedOp DeriveOperation(const Setup& setup,
                                        const std::vector<Body>& bodies,
                                        OpType type, const Target& target,
                                        int toolNumber = 0);

// Where a placed part sits with respect to the stock. Three states, not two:
// a part hanging over the sheet edge and a part that is not on the sheet at
// all are different mistakes, and the second is usually "I forgot to place
// it". PrusaSlicer draws the same distinction
// (ModelInstancePVS_{Inside,Partly_Outside,Fully_Outside}, set by
// Print::validate()), and so does BambuStudio per plate.
enum class Fit { Inside, PartlyOutside, FullyOutside };

struct PlacementFit {
  uint64_t placementId = 0;
  uint64_t jobPartId = 0;
  Fit fit = Fit::Inside;
  // Placement ids whose footprint this one runs into. Two parts in one sheet
  // occupying the same rectangle is scrap, and it is invisible in a backplot
  // that draws both.
  std::vector<uint64_t> overlaps;
};

// Check the ARRANGEMENT, at the moment someone asks -- not while they drag.
// A sheet is laid out THROUGH states like these; blocking the drag makes the
// intermediate ones impossible to pass through, which is why both slicers
// compute a state and let you move freely. The refusal belongs at the post,
// where the mistake becomes G-code.
//
// `Fit` is only ever Inside for relative stock, which is derived FROM the
// parts: nothing can be outside a box drawn around it.
[[nodiscard]] std::vector<PlacementFit>
CheckPlacements(const Setup& setup, const std::vector<Body>& bodies);

// The operations of `setup` in the order they should CUT, as indices into
// `setup.operations`. Authoring order is what the user typed; this is what
// the machine does, and the two stop being the same thing the moment a job
// holds more than one part.
//
// Stable within a group, so the authored order still decides everything the
// grouping does not.
[[nodiscard]] std::vector<size_t> CuttingOrder(const Setup& setup);

// Why this setup cannot be cut as laid out, or "" when it can. Separate from
// StockRefusal, which is about the workpiece: this is about the ARRANGEMENT.
//
// Three things, each of which would otherwise produce a file that looks
// right:
//
//   - a part placed TWICE. The data model holds it (a placement per copy, the
//     way every slicer models instances) and the generator does not cut it
//     yet: a region resolves to the first body that answers, so the second
//     copy is silently skipped;
//   - a part off the sheet, wholly or partly -- named separately, because
//     "hanging over the edge" and "not on it at all" are different mistakes;
//   - two parts occupying the same rectangle, which a backplot drawing both
//     cannot show you.
[[nodiscard]] std::string PlacementRefusal(const Setup& setup,
                                           const std::vector<Body>& bodies);

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

// `FromSetupSpace` USED to live here: a point in setup space mapped back to
// model space, for drawing. It is gone. A job tab draws its parts LAID OUT
// (v73/J6) -- the viewport IS setup space -- so mapping back undid exactly
// the transform the viewport had just applied and floated the backplot, the
// tool glyph and the depth label beside the part they belong to. Anything
// drawn over a job is drawn in setup space, full stop.

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
