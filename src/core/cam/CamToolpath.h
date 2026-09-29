#pragma once

#include "core/Feature.h" // Body
#include "core/cam/CamAdaptive.h"
#include "core/cam/CamJob.h"
#include "core/cam/CamPicks.h" // FaceRegion, WallContour, SurfaceRegion — kept
#include "core/cam/CamSurface.h" // SurfaceCache, SurfaceRequest
// visible here: 28 files include this one      // Loop, Toolpath

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <vector>

// Toolpath generation for 2.5D milling (docs/cam/README.md).
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

// How a generate treats the one expensive step, adaptive clearing
// (docs/ui/long-operations.md#cam-toolpath-passes). With no context (the
// default) every pass runs inline, as it always did. With a cache, a pass whose
// key is in it is not run again — an undo, a re-open, an unrelated edit. With
// `defer` as well, a pass that is NOT cached is not run either: it is appended
// to `pending` for a worker, and the toolpath comes back `Toolpath::pending`.
// The caller owns the cache and the worker; this file only says what it
// would have computed.
struct GenerateContext {
  AdaptiveCache* cache = nullptr;
  bool defer = false;
  std::vector<AdaptiveRequest> pending; // out
  // ...and the other expensive step, which the comment above used to call
  // "the one". A finishing pass is MINUTES, not seconds, and it ran on the
  // frame until it was deferred too
  // (docs/ui/long-operations.md#cam-toolpath-passes). Deferred whole, per
  // OPERATION, because a finishing pass has no interior seam the way a stack of
  // adaptive levels does.
  SurfaceCache* surfaceCache = nullptr;
  // The caller's fingerprint of the geometry these toolpaths are cut from.
  // The core cannot take one cheaply; the UI already keeps exactly this in
  // `JobRenderVersion`. Zero means "do not cache surfacing" -- an honest
  // answer for a caller that has no such key, and the default, so nothing
  // silently reads a result cut from other geometry.
  uint64_t bodiesKey = 0;
  std::vector<SurfaceRequest> pendingSurface; // out
  // ...and a profile's projected outlines, the third
  // (docs/cam/README.md#background-work). Keyed on `bodiesKey` too, so zero
  // turns it off.
  OutlineCache* outlineCache = nullptr;
  std::vector<OutlineRequest> pendingOutline; // out
};

// One operation's toolpath. Never throws: a failure comes back as
// `Toolpath::error`, because a CAM job with one bad op should still show the
// other five.
[[nodiscard]] Toolpath Generate(const Setup& setup, const Operation& op,
                                const std::vector<Body>& bodies,
                                GenerateContext* ctx = nullptr);

// Every enabled operation, in order.
[[nodiscard]] std::vector<Toolpath> GenerateAll(const Setup& setup,
                                                const std::vector<Body>& bodies,
                                                GenerateContext* ctx = nullptr);

// The stock box for a setup, derived from the model bounds plus margins, IN
// SETUP SPACE (C9) — so an indexed job gets the box the machine will see.
// Pass model-space bodies; the framing happens here. Returns false when
// there is nothing to derive it from.
bool StockBounds(const Setup& setup, const std::vector<Body>& bodies,
                 glm::dvec3& min, glm::dvec3& max);

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
  // What is true of the op as derived and worth saying before anything is
  // cut: a drill's point in a flat-bottomed hole, say. Not refusals.
  std::vector<std::string> notes;
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
int RederiveDepths(Setup& setup, const std::vector<Body>& bodies,
                   const std::vector<const Setup*>& earlier = {});

// The setups of `all` cut before the one with id `setupId` -- the job's
// order is the order they are cut in.
[[nodiscard]] std::vector<const Setup*>
EarlierSetups(const std::vector<Setup>& all, uint64_t setupId);

// The top of the material over `op`'s region, in `setup`'s frame, when the
// job's `earlier` setups have already cut: an indexed setup after a TURNING
// one starts on the turned part, whose top over the op is the turned
// envelope -- the model's largest radius along the op's span plus what the
// turning left on it -- not the square bar's top. `freshTop` (the stock's)
// where no earlier setup turned this span, or where the frames do not share
// the turning axis. Never above `freshTop`
// (docs/cam/operations.md#derivation--what-a-click-means).
[[nodiscard]] double StockTopOver(const Setup& setup,
                                  const std::vector<const Setup*>& earlier,
                                  const std::vector<Body>& rawBodies,
                                  const Operation& op, double freshTop);
// Whether a derived operation of this type starts at the top of the stock
// (and so from `StockTopOver`) -- a clearing, a contour, a drill or a 3-axis
// surface pass -- rather than at a height of the model's own, like an edge
// break's edge or a facing's part top.
[[nodiscard]] bool TopFromStock(OpType type);

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
// `earlier`: the job's setups cut before this one (`EarlierSetups`). A top
// derived from the stock is taken from what they left (`StockTopOver`).
[[nodiscard]] DerivedOp
DeriveOperation(const Setup& setup, const std::vector<Body>& bodies,
                OpType type, const Target& target, int toolNumber = 0,
                const std::vector<const Setup*>& earlier = {});

// A rotary ROUGH (docs/cam/rotary.md#layers-rest-and-the-rough-verb): the
// rotary finish's derivation and refusals, then layers of half the cutter from
// the blank inward and a stock to leave of DefaultFinishStock(tool), so the
// finish that follows takes an even skin. The rotary finish deliberately
// derives no stepdown -- a finish is not a choice about the stock -- and this
// is the verb that makes that choice.
// A 3-axis ROUGH: the whole-part raster finish's derivation, then layers of
// half the cutter from the stock top and a skin of DefaultFinishStock(tool)
// (docs/cam/operations.md#rough-a-surface-and-rest-machining).
[[nodiscard]] DerivedOp DeriveSurfaceRough(const Setup& setup,
                                           const std::vector<Body>& bodies,
                                           int toolNumber = 0);
// A REST pass: the same kind of pass as the setup's last surface finish, over
// the same region, with tool `toolNumber` and `restFromTool` = the finish's
// tool, so it cuts only where that larger cutter could not reach. Refused with
// no finish to follow, or a tool that is not smaller.
// What the CAM panel's "Add operation" adds to this setup: a rotary finish
// when the setup already cuts on the 4th axis, the whole-part cut-out
// otherwise. Read from what the setup HOLDS, not from the machine alone -- a
// plate clamped to the bed of a 4th-axis machine is still a 3-axis job
// (docs/cam/operations.md#derivation--what-a-click-means).
[[nodiscard]] OpType AddOperationType(const Setup& setup);

[[nodiscard]] DerivedOp DeriveRest(const Setup& setup,
                                   const std::vector<Body>& bodies,
                                   int toolNumber = 0);
[[nodiscard]] DerivedOp DeriveRotaryRough(const Setup& setup,
                                          const std::vector<Body>& bodies,
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
  // Where this part's material actually IS, in setup space: the box the
  // check already had to compute to answer the question above. Reported
  // because "does it fit" and "where is it" are the same measurement, and a
  // caller laying a sheet out had to reconstruct this one part at a time --
  // shrink-wrap the stock, disable every other placement, read the stock box
  // back, put it all back.
  glm::dvec3 lo{0.0};
  glm::dvec3 hi{0.0};
  // Placement ids whose footprint this one runs into. Two parts in one sheet
  // occupying the same rectangle is scrap, and it is invisible in a backplot
  // that draws both.
  std::vector<uint64_t> overlaps;
};

// What is arranged badly but not wrongly: the answers that must not stop a
// job. A gap narrower than the cutter is the one this exists for -- it is a
// gouge, but only for the tools that go there, and only cam_verify would
// otherwise catch it, after the file is posted. One sentence each, empty when
// the sheet is fine.
[[nodiscard]] std::vector<std::string>
LayoutWarnings(const Setup& setup, const std::vector<Body>& bodies);

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
// The job against the MACHINE's work area (round two, M2): what the spindle
// has to REACH -- the placed parts' extent per axis -- against the travel's.
// Not the stock's: a sheet longer than the bed is normal on an open-ended
// router, slid through and cut in tiles, and the sheet hanging off the
// machine is the operator's business (MachineOverhang says so, as a note).
// A WARNING, not a refusal: a part longer than the bed is cut as far as the
// machine reaches, and the rest is a second setup with the sheet moved
// along -- refusing the whole job refused the part the user wanted the
// first metre of. Extents only -- where the operator touches off inside the
// travel is not the job's to know -- and only when the machine states a work
// area at all. Empty when it fits or nothing is stated; otherwise the axis
// and the shortfall, in words. The bed is never grown to fit.
[[nodiscard]] std::string MachineWarning(const Setup& setup,
                                         const std::vector<Body>& bodies);

// Where to put the machine's reach window so the CUTTING falls inside it
// (Setup::reachOffset). A sheet longer than the bed is slid through and
// clamped where the work is; this works out where that is, from the toolpaths
// rather than from the sheet. `ok` false with `why` in words when no position
// helps -- the cutting itself is wider than the travel, which is genuinely two
// setups and says so. Zero along an axis where the stock fits the travel: the
// window is centred there already.
struct DerivedReach {
  glm::dvec2 offset{0.0, 0.0};
  bool ok = false;
  std::string why;
};
[[nodiscard]] DerivedReach DeriveReachOffset(const Setup& setup,
                                             const std::vector<Body>& bodies);
// The stock against the travel, when the parts themselves fit: empty, or a
// sentence saying which way the sheet hangs off the bed.
[[nodiscard]] std::string MachineOverhang(const Setup& setup,
                                          const std::vector<Body>& bodies);

// Where the machine's travel sits in SETUP space: a box the size of the
// travel, its floor at the stock's bottom, centred on the stock along an axis
// where the stock fits and at its near corner along one where it is longer,
// plus reachOffset -- how a sheet is loaded, and the one anchoring the viewport
// draws and the clip below cuts to. False when the machine states no work area
// or there is no stock. `bodies` unframed; the Framed form takes them framed.
[[nodiscard]] bool MachineBounds(const Setup& setup,
                                 const std::vector<Body>& bodies,
                                 glm::dvec3& lo, glm::dvec3& hi);
[[nodiscard]] bool MachineBoundsFramed(const Setup& setup,
                                       const std::vector<Body>& framed,
                                       glm::dvec3& lo, glm::dvec3& hi);

// Cut what fits: every move clipped to the box in X and Y. A cut that runs
// out of the box is cut to the boundary and the tool retracts to `safeZ`; one
// that comes back in re-enters at the boundary at safe height and plunges
// (at `plungeFeed`) back to the depth it was at. What was left out is
// counted in Toolpath::clippedMm. GenerateAll applies this when the machine
// states a work area, so the backplot, the simulation, the verifier and the
// file all show the same shortened cut.
void ClipToMachine(Toolpath& tp, const glm::dvec3& lo, const glm::dvec3& hi,
                   double safeZ, double plungeFeed);

// One operation's toolpaths: one, or two when it roughs with one tool and
// finishes with another (R6, Operation::finishTool). GenerateAll walks this.
[[nodiscard]] std::vector<Toolpath> GenerateOp(const Setup& setup,
                                               const Operation& op,
                                               const std::vector<Body>& bodies,
                                               GenerateContext* ctx = nullptr);
[[nodiscard]] std::vector<Toolpath>
GenerateOpFramed(const Setup& setup, const Operation& op,
                 const std::vector<Body>& framed,
                 GenerateContext* ctx = nullptr);

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
// ...for one job part, which may be clamped differently from the rest of the
// setup (v76). `jobPartId` 0 asks about the setup's own frame.
[[nodiscard]] bool FrameIsSquare(const Setup& setup, uint64_t jobPartId);

// How a given part is clamped in this setup: its placement's frame if it has
// one, the setup's otherwise. The ONE place that question is answered.
[[nodiscard]] glm::dquat FixtureFrame(const Setup& setup, uint64_t jobPartId);

// Setup space -> model space: how the part is fixtured, then how far the
// table is indexed. The ONE place the two compose, so nothing downstream has
// to know there are two.
[[nodiscard]] glm::dmat3 SetupToModel(const Setup& setup);
// ...for one part. Two parts in a sheet can be clamped differently, so this
// is the composition that actually applies to a body; the argument-less form
// is it with `jobPartId` 0.
[[nodiscard]] glm::dmat3 SetupToModel(const Setup& setup, uint64_t jobPartId);

// The ONE transform from model space to this setup's space for a body of
// part `jobPartId`: the part's fixture frame undone, then where it sits in
// the stock (docs/cam/setup.md#parts-and-placements). `BodiesInSetupFrame`
// applies it; the verifier maps another setup's cut through it
// (docs/cam/simulation.md#leftover-another-setup-removes).
[[nodiscard]] glm::dmat4 BodyToSetup(const Setup& setup, uint64_t jobPartId);

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

// Everything `BodiesInSetupFrame` reads from the setup, hashed: the fixture
// frame, the index and every placement. A cache keyed on the caller's
// geometry fingerprint alone hands one framing's result to another -- two
// setups of one job, or one operation cut at four indices, share that
// fingerprint and not their outlines.
[[nodiscard]] uint64_t FramingKey(const Setup& setup);

// The outward normal of a planar face, or false when the ref names nothing
// resolvable or something curved. Engine-side because both doors need it --
// the pick in the panel and `lay_face_up` over the RPC -- and a geometry
// question answered twice is a geometry question answered two ways.
//
// `jobPartId` says WHICH part of the job the ref belongs to, and 0 means any.
// Two parts in one job easily share a feature id -- each document numbers its
// own features from 1 -- so without it the first body with that id answers,
// and a sheet holding a mid plate and two camera plates could not lay the
// second plate up at all (the name resolved on the wrong part). It is the
// same qualifier an operation target carries, for the same reason.
[[nodiscard]] bool FaceNormalOf(const std::vector<Body>& bodies,
                                const ElementRef& ref, glm::vec3& normal,
                                uint64_t jobPartId = 0);

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
// ...and clear of every FIXTURE too (bodies in setup space): a toe clamp
// standing above the stock puts every rapid at the stock's safe Z through it
// (docs/cam/setup.md#safe-z). The same clearance rule, over the taller of
// the two.
[[nodiscard]] double DeriveSafeZ(const Setup& setup,
                                 const std::vector<Body>& bodies,
                                 const std::vector<Body>& fixtureBodies);

// Where a fixture goes, derived from the stock and the fixture's own size --
// never a typed position. A side puts it on the bed (its bottom at the
// stock's), against that face of the stock, centred along it; Centre puts its
// footprint on the stock's, bottom at the stock's bottom (a vise, which
// SURROUNDS the stock); Top lays it on the stock's top face, straddling the
// -X edge, which is where a toe clamp's arm belongs. `fixtureModel` is the
// fixture's bodies in its OWN coordinates; `rotZdeg` the spin it is placed
// with.
enum class FixtureSide { MinusX, PlusX, MinusY, PlusY, Centre, Top };
[[nodiscard]] bool ParseFixtureSide(const std::string& text, FixtureSide& out);
[[nodiscard]] std::string FixtureSideName(FixtureSide side);
// The placement a NEW fixture takes: Centre when it is wider than the stock
// in X or Y (it goes round it), otherwise the next free side, -X, +X, -Y, +Y
// and round again.
[[nodiscard]] FixtureSide
DefaultFixtureSide(const Setup& setup, const std::vector<Body>& bodies,
                   const std::vector<Body>& fixtureModel, double rotZdeg);
// A keep-out box's size when nobody has said: a toe clamp's worth, from the
// stock -- a quarter of its footprint each way, a quarter taller than it.
[[nodiscard]] glm::dvec3 DefaultKeepOutBox(const Setup& setup,
                                           const std::vector<Body>& bodies);
// Which derived placement `pos` is, for a row to say where the fixture sits;
// empty when it has been moved off every one of them.
[[nodiscard]] std::string
FixturePlaceName(const Setup& setup, const std::vector<Body>& bodies,
                 const std::vector<Body>& fixtureModel, const Fixture& f);
// False when there is no stock to place against, or the fixture has no
// geometry.
[[nodiscard]] bool PlaceFixture(const Setup& setup,
                                const std::vector<Body>& bodies,
                                const std::vector<Body>& fixtureModel,
                                double rotZdeg, FixtureSide side,
                                glm::dvec3& pos);

// --- seats (docs/cam/setup.md#seats) ------------------------------
// Built with CAM off too (CamSetup.cpp): fixtures are drawn in any build.
[[nodiscard]] bool ParseStockEdge(const std::string& text, StockEdge& out);
[[nodiscard]] std::string StockEdgeName(StockEdge edge);
// The pose that puts `seat.face` of the fixture (its bodies in their OWN
// coordinates) on the top of the stock box [stockLo, stockHi]: the face's
// plane on the top, its nose pointing in square to `seat.edge` (plus
// `spinDeg`), its innermost point `overMm` in past the edge and its centre
// `alongMm` from the edge's low end. False, with `why`, when the face is
// gone, is not planar, or does not face down -- a fixture turns about Z
// only, so a face that does not face down cannot rest on anything.
[[nodiscard]] bool SolveFixtureSeat(const glm::dvec3& stockLo,
                                    const glm::dvec3& stockHi,
                                    const std::vector<Body>& fixtureModel,
                                    const FixtureSeat& seat, glm::dvec3& pos,
                                    double& rotZdeg, std::string* why);
// The inverse: the seat that `face` resting at bed pose (pos, rotZdeg) IS --
// the edge its nose points in across, the spin left over, and how far over
// and along it sits. Solving the result gives the pose back. For a fixture
// dragged onto the stock (docs/cam/gui.md#gizmos): the seat comes
// from where it was put.
[[nodiscard]] bool SeatFromPose(const glm::dvec3& stockLo,
                                const glm::dvec3& stockHi,
                                const std::vector<Body>& fixtureModel,
                                const ElementRef& face, const glm::dvec3& pos,
                                double rotZdeg, FixtureSeat& seat,
                                std::string* why);
// The flat faces of `bodies` square to X, Y and Z, by axis: where each lies on
// that axis and which way it faces. What a dragged fixture snaps with -- its
// real faces, so a step clamp's pad, bar and stud undersides are three
// candidates, where its bounding box would offer one.
struct AxisFacePlane {
  double at = 0.0;
  int facing = 1;  // +1 faces +axis, -1 faces -axis
  size_t body = 0; // index into `bodies`
  int face = 0;    // explorer index within that body
};
[[nodiscard]] std::array<std::vector<AxisFacePlane>, 3>
AxisFacePlanes(const std::vector<Body>& bodies);
// The named, flat, downward face of the fixture -- its bodies in their OWN
// coordinates, placed at (pos, rotZdeg) -- that lies ON the stock's top and
// over it: the largest by overlap, or unset when none does. A fixture put
// down there rests on that face (docs/cam/gui.md#gizmos).
[[nodiscard]] ElementRef FaceOnStockTop(const glm::dvec3& stockLo,
                                        const glm::dvec3& stockHi,
                                        const std::vector<Body>& fixtureModel,
                                        const glm::dvec3& pos, double rotZdeg);
// Why `face` (in the fixture's own model) cannot be a seat, or empty.
[[nodiscard]] std::string SeatFaceRefusal(const std::vector<Body>& fixtureModel,
                                          const ElementRef& face);

// Time spent CUTTING, from each move's own feed. Rapids are counted only when
// the setup states a rapid feed — inventing one would be a guess about the
// machine, and this number exists to be trusted.
[[nodiscard]] double CuttingMinutes(const Setup& setup,
                                    const std::vector<Toolpath>& paths);

} // namespace cam
