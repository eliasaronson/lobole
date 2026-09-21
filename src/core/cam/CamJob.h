#pragma once

#include "core/ElementMap.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// CAM: milling setups, operations and the toolpaths they produce
// (docs/cam/plan.md).
//
// A Setup is a document-level object, like fem::Study — not a Feature. It
// does not change the model; it describes how to cut it. Toolpaths are
// generated IN SETUP SPACE and posted through the setup's frame, which is the
// seam that keeps an indexed 4th axis from being a rewrite: a rotary setup is
// the same object with a rotated frame and an A word in the post.
namespace cam {

// --- tools ----------------------------------------------------------------

// What the end of the tool looks like. Everything before R15 assumed a flat
// end mill — a fair default, and wrong for the two tools a chamfer or a
// roundover needs. The shape is not cosmetic: it decides what the verifier
// thinks was removed, and it is what an edge-follow operation reads to know
// whether it is cutting a bevel or an arc.
enum class ToolType {
  Flat,  // square end mill
  Ball,  // ball nose; tip radius = diameter/2
  VBit,  // engraving/chamfer cone, `tipAngleDeg` INCLUDED angle
  Drill, // twist drill, `tipAngleDeg` point angle (118° typical)
};

struct Tool {
  int number = 1; // T-word; also how a post addresses it
  std::string name;
  double diameter = 6.0; // mm
  ToolType type = ToolType::Flat;
  // Included angle at the point, for the two conical types. 90° is the
  // common chamfer bit; a drill is usually 118°.
  double tipAngleDeg = 90.0;
  int flutes = 2;
  double fluteLength = 20.0; // mm, the usable depth
  // Feeds and speeds are NOT computed from a chip-load formula: this app
  // does not know the machine's rigidity, and a wrong number here breaks a
  // tool. They are what the user actually runs.
  double feedCut = 300.0;    // mm/min in XY
  double feedPlunge = 100.0; // mm/min in Z
  double spindleRpm = 10000.0;
  // What THIS cutter wants per tooth, in mm, when somebody knows -- a tool
  // vendor's chart for this cutter in the material this setup cuts. Zero (the
  // default) means nobody said, and the check falls back to a scaling rule off
  // the diameter (CamFeeds.h). Evidence beats a rule, and the finding cites
  // which one it used, because a limit nobody can trace is a limit nobody can
  // argue with.
  //
  // On the TOOL rather than in a table of our own because there is no open
  // standard to vendor here: fastener geometry is ISO because parts must
  // interchange, while cutting data is empirical, machine-dependent and
  // published as vendor copyright. So we ship a conservative rule and let the
  // real numbers be typed in beside the feed they belong to -- and a machine's
  // tool library carries them into every new setup for free.
  double chiploadMin = 0.0;
  double chiploadMax = 0.0;
  // The rest of the assembly, above the flutes: how far the cutter sticks out
  // of the collet in total, and how wide the thing it sticks out of is. Both
  // zero (the default) means nobody said, and nothing is checked -- the same
  // rule as `Machine::travel`, because a limit nobody typed is not a limit.
  //
  // Two numbers and not a holder model. This is what a person can read off
  // the machine with a rule: the tip-to-collet-face length they set when they
  // tightened the nut, and the widest part of the nut. A real ER32 nut is a
  // stepped cone and an ISO30 holder is a taper, and modelling either would
  // ask the operator for a drawing they do not have in order to answer a
  // question a cylinder already answers conservatively.
  double overallLength = 0.0;  // mm, tip to the collar's underside
  double collarDiameter = 0.0; // mm, the collet nut / holder, at its widest
  bool operator==(const Tool&) const = default;
};

// The MACHINE, as one physical thing (docs/cam/job-document.md, round two;
// core/cam/CamMachine.h for what is asked about it). Post dialect, arc
// fitting and the rotary axis lived on `Setup` because there was nowhere
// else; they are claims about the machine in the shop, and a setup EMBEDS a
// copy of the machine it was set up on, named by `name`. Saved globally, one
// file per machine beside settings.json (Serialization.h).
struct Machine {
  std::string name; // the key a job names it by; "" = the job's own, unnamed
  // The work area, in machine coordinates. Zero-size means "not stated", and
  // nothing is checked against it -- a box nobody typed is not a limit.
  glm::dvec3 travelMin{0.0, 0.0, 0.0};
  glm::dvec3 travelMax{0.0, 0.0, 0.0};
  std::string post = "mach3"; // post id; "linuxcnc" planned
  // Whether the post fits G2/G3 arcs (C7). A controller that mishandles arcs
  // mishandles them every session.
  bool fitArcs = true;
  // Whether the machine has an A axis at all. False means the rotary controls
  // are not offered and no file can command an axis the shop does not own.
  bool hasRotaryAxis = false;
  // Seeds Setup::rapidFeed; 0 = G0. Kept per job as well, because a capped
  // rapid is a per-job choice on the controllers that need one (Kiri:Moto
  // keeps it on the process, not the device).
  double rapidFeed = 0.0;
  // The A axis's top speed, in degrees per minute; 0 = not stated. A cut
  // that turns the part is held to it (CamRotaryFeed.h), and it is its own
  // number because it is its own motor: a 4th axis turns at a few thousand
  // degrees a minute on a router whose linear rapid is 1500 mm/min.
  double rotaryMaxFeed = 0.0;
  // The tool library a new setup's table is copied from (M4). Copied, never
  // linked: FreeCAD's answer, and the one the per-setup table already gives.
  std::vector<Tool> library;

  [[nodiscard]] bool HasWorkArea() const {
    return travelMax.x > travelMin.x && travelMax.y > travelMin.y &&
           travelMax.z > travelMin.z;
  }
  bool operator==(const Machine&) const = default;
};

// How far ABOVE the tool's lowest point its cutting surface sits, `d` mm out
// from the axis. Zero everywhere for a flat mill (the old assumption, kept
// exactly); the bowl of a ball; the cone of a V-bit or a drill point. The
// verifier's material removal and the edge-follow geometry both come from
// this one function, so the two cannot disagree about the tool's shape.
[[nodiscard]] inline double TipHeight(const Tool& t, double d) {
  const double r = t.diameter * 0.5;
  d = std::min(std::abs(d), r);
  switch (t.type) {
  case ToolType::Ball:
    return r - std::sqrt(std::max(0.0, r * r - d * d));
  case ToolType::VBit:
  case ToolType::Drill: {
    // Half the included angle, measured from the axis.
    const double half = glm::radians(t.tipAngleDeg * 0.5);
    const double tanHalf = std::tan(half);
    return tanHalf > 1e-9 ? d / tanHalf : 0.0;
  }
  default:
    return 0.0;
  }
}

// Does this tool state a holder at all? Both numbers, and a collar wider than
// the cutter -- a collar no wider than the flutes can foul nothing the flutes
// did not already reach, and the reach check (CamFeeds.h) owns that question.
[[nodiscard]] inline bool HasHolder(const Tool& t) {
  return t.overallLength > 0.0 && t.collarDiameter > t.diameter;
}

// Would material at radial distance `d` from the axis, standing `h` mm above
// the tool's tip, hit the collar?
//
// Deliberately blind inside the cutter's own radius. There the tool is the
// tool: material in that column is either cut (and the height map records
// that it was) or it is a plunge deeper than the cutter is long, which
// `CamFeeds` already states at the SETUP, before a file exists. Reporting it
// twice, in two different vocabularies, is how a check stops being read.
[[nodiscard]] inline bool FoulsHolder(const Tool& t, double d, double h) {
  return HasHolder(t) && d > t.diameter * 0.5 && d <= t.collarDiameter * 0.5 &&
         h > t.overallLength;
}

// What an operation MACHINES: an element, and which part of the job it is on.
//
// The qualifier is not decoration. Two parts in one job can easily own the
// same feature id and the same element name, so a bare ElementRef stops being
// an answer the moment a job holds more than one part. It names the PART, not
// a placement: "machine this pocket" is a statement about the bracket, and it
// is cut wherever that bracket sits.
//
// FreeCAD reached the same shape from the other end -- its operation Base is
// an (object, subelement) pair, and Path/Op/Base.py:520 MIGRATES from the
// scope-limited link it shipped first (docs/cam/job-document.md).
struct Target {
  uint64_t jobPartId = 0;
  ElementRef ref;

  [[nodiscard]] bool IsSet() const { return ref.IsSet(); }
  [[nodiscard]] bool operator==(const Target& o) const {
    return jobPartId == o.jobPartId && ref.featureId == o.ref.featureId &&
           ref.name == o.ref.name;
  }
};

// --- operations -----------------------------------------------------------

enum class OpType {
  Profile, // follow a contour, tool outside/inside by its radius
  Pocket,  // clear a closed region, islands respected
  Face,    // clear the top of the stock
  Drill,   // plunge at points
  // C8: clear a region at a roughly constant angle of engagement instead of
  // in concentric rings, so the whole depth can be taken in one pass. Needs
  // the LGPL libarea shared library; without it the pocket path is used.
  Adaptive,
  // R15: follow a wall's TOP EDGE and break it — a chamfer with a V-bit, a
  // roundover with a ball nose. The tool decides which; a flat mill is
  // refused rather than run as a square step nobody asked for.
  EdgeRound,
  // Finishing passes (docs/cam/surfacing.md). Neither is a stack of levels:
  // the Z varies continuously WITHIN a pass, which is what the 2.5D pipeline
  // cannot express and why these are two more types rather than a flag on an
  // existing one.
  //
  // Two and not one, because they answer opposite questions -- a raster is
  // good on shallow surfaces and poor on steep ones, a waterline the reverse
  // -- and because a single op cannot honestly carry both an XY fraction and
  // a Z step under one name.
  SurfaceParallel,
  SurfaceWaterline,
  // 4-axis finishing: parallel passes ALONG the rotary axis, the part turning
  // between them, or one spiral with A turning while the tool cuts -- by the
  // op's `rotaryPattern` (docs/cam/rotary-plan.md,
  // docs/cam/rotary-continuous-plan.md).
  SurfaceRotary,
  // Not a type: the sentinel every table is asserted against. Without it a
  // value appended here with no row in CamNames.cpp saved as "profile" and
  // reloaded as a full-depth contour round the part -- the silent
  // substitution that file exists to abolish, in the one direction it did not
  // cover. Switches carry an explicit `case Count:` that refuses, so a new
  // type cannot quietly inherit someone else's branch either.
  Count,
};

// Which side of a contour the tool runs on. For a pocket this is implicit
// (inside); for a profile it is the difference between a part and a hole.
enum class Side { Outside, Inside, On };

// Corner relief on profiles and pockets (docs/cam/router-round.md, R4). A
// round cutter leaves a radius in every inside corner of the material; a
// joint that has to take a square tenon needs the corner cleared. Dogbone:
// out along the corner's bisector and back, the tool's RADIUS -- FreeCAD's
// nominal, and the only length that clears the corner (Kiri:Moto's fifth of
// a diameter does not). T-bone: the same length along the longer of the two
// walls, where a joint hides it. Off by default: a relief is a decision about
// the joint, not a fact about the part.
enum class CornerRelief { None, Dogbone, TBone };

// How a rotary finish covers the part (docs/cam/rotary-continuous-plan.md).
// Parallel: passes ALONG the axis, each at one A, the part turning between
// them -- a rotary raster. Spiral: one helix from end to end, A turning WHILE
// the tool cuts, one revolution per stepover -- continuous 4-axis.
enum class RotaryPattern { Parallel, Spiral };

struct Operation {
  uint64_t id = 0;
  std::string name;
  OpType type = OpType::Profile;
  int toolNumber = 1;

  // What to machine. Stable element references (toponaming), so a model edit
  // that moves a face does not orphan the job — the reason to have CAM in
  // the CAD rather than downstream of a STEP export.
  std::vector<Target> targets;

  // Depth, in setup space. topZ is where the cut starts (usually the stock
  // top), bottomZ where it ends. stepdown 0 = one pass.
  double topZ = 0.0;
  double bottomZ = -1.0;
  double stepdown = 0.0;
  double stockToLeave = 0.0;
  // Rough then finish (R6): a second tool that takes the wall in ONE
  // full-depth pass after this operation has roughed it, leaving
  // `finishStock` behind for it. One operation with two tools rather than
  // two operations that must agree about a boundary. 0 = no finishing pass.
  int finishTool = 0;
  double finishStock = 0.0; // derived: the roughing tool's diameter / 10
  // Profile only (R5): leave the wall on a tangent arc of the tool's radius
  // at the end of every pass instead of stopping against it and lifting --
  // the stop is the dwell mark. The ramp is the entry, so there is no lead
  // in: the tool arrives at the wall descending, already moving along it.
  bool leadArcs = false;
  // Profile, cutting INSIDE a level face: leave the BORES up to this
  // diameter alone -- they are drilled. 0, the default, cuts every opening.
  //
  // It exists because "cut the openings in this face" and "drill the holes in
  // this face" are both true of the same face: on a carbon frame the slots
  // are milled and the M2/M3 fastener holes are DRILLED, and without a word
  // for it one operation cuts what the other has already taken out -- minutes
  // of a 2 mm cutter orbiting air in a hole that is already gone.
  //
  // A bore and not a width, because width cannot say it: this frame's
  // narrowest MILLED opening is a 2.2 mm slot and its widest DRILLED hole is
  // 3.2 mm across, so any threshold that keeps the slot keeps the holes too.
  // What separates them is that one is a hole. It is the caller's statement
  // of intent rather than a guess the app can make -- a hole drilled
  // undersize and finished to size is the same pick and the opposite answer.
  double drilledBores = 0.0;
  // Profile and pocket only (R4): what to do at the inside corners of the
  // material. Applied to the wall pass -- the loop one tool radius off the
  // wall -- at every sharp corner where the cutter cannot reach.
  CornerRelief cornerRelief = CornerRelief::None;
  // Drill only (docs/cam/router-round.md, R3): retract fully between pecks
  // of this depth so the chips clear -- the G83 reading, as plain moves,
  // since neither GRBL nor our post speaks canned cycles. 0 = one plunge.
  // Derived: the tool's diameter once the hole is deeper than three of them.
  double peck = 0.0;

  Side side = Side::Outside;
  bool climb = true;

  // Profile only: leave uncut tabs so the part does not break free on the
  // last pass and get thrown by the cutter.
  int tabCount = 0;
  double tabWidth = 6.0;
  double tabHeight = 1.5;

  // Clear the face's OUTLINE, ignoring the openings inside it. A pocket takes
  // its boundary from the picked face's wires: the outer one is where to
  // stop, and every inner one is an island to go around. That is right for
  // material standing in the pocket and wrong for a HOLE — the cutter detours
  // around thin air, and where a hole nearly touches the pocket wall it
  // leaves an uncut sliver too narrow for the tool to return to.
  //
  // A hole is not an island: there is nothing there to avoid, and a hole
  // NARROWER than the cutter is the case this exists for — the tool bridges
  // it and keeps cutting the floor either side. Setting this passes straight
  // over the openings.
  //
  // It does NOT pass over material. A boss standing up from the floor has the
  // same kind of inner wire and the opposite meaning, and driving over one is
  // a full-width cut at full depth into metal nobody asked to remove. The
  // generator tells them apart by sectioning just above the floor: an inner
  // wire with something standing in it is kept whatever this says. FreeCAD's
  // `UseOutline` (Path/Op/PocketShape.py) is the same idea and does not draw
  // that distinction — it takes `Part.Face(element.OuterWire)` and loses the
  // bosses with the holes.
  bool useOutline = false;

  // Pocket/Face only: fraction of the tool diameter to step over per pass.
  // EdgeRound reads it too: it is what sets the cusp between arc passes.
  double stepover = 0.45;

  // EdgeRound only: the size of the break, in mm — the radius of the round
  // for a ball, the horizontal width of the bevel for a V-bit. 0 = derive it
  // from the tool and the wall (defaults rule).
  double edgeSize = 0.0;

  // --- surfacing (v118) ---------------------------------------------------
  //
  // Three quantities that a single "stepover" would have to carry, and the
  // reason each has its own name. `stepover` above is the spacing ACROSS a
  // raster's passes, as a fraction of tool diameter, which it already was.
  // These two are millimetres and say so, because the surveyed tools all
  // spell several different things "step over" -- FreeCAD has three ops whose
  // StepOver means a percentage, a percentage that is NOT the waterline Z
  // step, and a distance per revolution.

  // SurfaceParallel only: the raster direction, degrees anticlockwise from
  // +X. Zero runs the passes along X.
  double cutAngleDeg = 0.0;
  // Both surfacing types: spacing ALONG a pass, where the cutter-location
  // surface is read, in mm. 0 = derive it from the CL grid, which is the
  // finest answer that surface can give.
  double sampleStep = 0.0;
  // SurfaceWaterline only: the Z between laps, in mm. NOT a stepover: an XY
  // fraction of tool diameter cannot express how far apart constant-Z laps
  // sit, which is the conflation this field exists to avoid. 0 = derive it
  // from the tool and the cusp.
  double zStep = 0.0;

  // SurfaceRotary only (v123): parallel passes or one spiral. The same
  // `stepover` serves both, and means the spacing between neighbouring cuts
  // in each: across the passes for parallel, the axial advance per turn for
  // the spiral. `stepdown`, when set, cuts the rotary pass in LAYERS from the
  // blank inward, each held off the part by the next ring of stock.
  RotaryPattern rotaryPattern = RotaryPattern::Parallel;

  bool enabled = true;

  // Every member, compared without anyone listing them: what lets the
  // round-trip gate (test_cam_capability.cpp) notice a field the serializer
  // forgot, including one added after the gate was written.
  bool operator==(const Operation&) const = default;
};

// In what ORDER the operations cut.
//
// With one part, authoring order IS cutting order and nobody needs this.
// With four, the natural authoring order is per part, and the post emits a
// tool change on every change of tool number -- so a four-part sheet
// authored the obvious way changes tools four times per tool instead of
// once. FreeCAD makes this a job property for the same reason
// (`OrderOutputBy`, and build_postlist_by_tool says "to minimize tool
// changes" in its own docstring); BambuStudio reaches it from the
// 3D-printing side with a per-plate print sequence.
enum class OrderOutputBy {
  // The order they were written in. The default, and byte-for-byte what this
  // app posted before the field existed.
  Authored,
  // Group by tool number: one tool change per tool.
  Tool,
  // Finish each part before starting the next -- what you want when a part
  // may break free and you would rather lose one than four.
  Part,
};

// --- the setup ------------------------------------------------------------

// Stock: what is in the vise before the first cut. Derived from the model's
// bounding box plus margins rather than typed in absolute terms, so it stays
// right when the model changes (defaults rule — docs/ui/u10-*).
// How the workpiece is described. Relative is the default and the right one
// for a part cut from oversize material; Fixed is the sheet you actually own,
// which a shrink-wrap around one part cannot express
// (docs/cam/stock-and-origin.md W3).
enum class StockMode { Relative, Fixed };

struct Stock {
  StockMode mode = StockMode::Relative;
  glm::dvec3 margin{2.0, 2.0, 1.0}; // Relative: added around the model bbox
  double bottomMargin = 1.0;        // below the model, for the sacrificial pass
  // Fixed: the workpiece as it is, in mm, and where the part sits inside it —
  // 0 = the part's low face against the stock's low face, 1 = high against
  // high, 0.5 = centred. Z defaults to 1: sheet goods are referenced from
  // their top, which is the face you surface and touch off on.
  glm::dvec3 size{0.0, 0.0, 0.0};
  glm::dvec3 place{0.5, 0.5, 1.0};
};

// Where X0 Y0 Z0 is: the corner the operator touches off on. Derived from the
// stock or the model, never a typed coordinate — `origin{0,0,0}` was a
// hard-coded literal standing where the defaults rule wants a derivation, and
// it put zero at the MODEL origin, which for a generated or imported part is
// wherever the source document happened to leave it.
//
// A picked VERTEX is the obvious fourth option and is deferred: ElementMap
// has no vertex kind, so it needs plumbing this change does not.
enum class OriginAt { StockMin, StockCentre, ModelMin, StockMax };

// WHERE a part sits in the stock, for this setup.
//
// In-plane only -- a position on the bed and a spin under the spindle --
// because anything out of plane is what the fixturing frame already says, and
// two ways to tilt a part is the mistake docs/cam/setup-frame.md just finished
// paying for.
//
// SEVERAL placements may name one `jobPartId`: that is four of the same
// bracket nested in one sheet, which is how every slicer models it
// (PrusaSlicer's ModelObject + N ModelInstance). The generator does not cut
// repeats yet and says so rather than quietly cutting one.
struct Placement {
  uint64_t id = 0;
  uint64_t jobPartId = 0;
  glm::dvec2 pos{0.0, 0.0}; // setup space, mm
  double rotZdeg = 0.0;
  // On the sheet, not cut this run (PrusaSlicer's ModelInstance::printable).
  bool enabled = true;

  // How THIS part is clamped, when it is clamped differently from the rest of
  // the setup. Unset means "as the setup says" -- which is the whole answer
  // for a job whose parts all lie the same way, and keeps `Setup::frame` the
  // single-part answer it has always been.
  //
  // It exists because fixturing is a claim about A PART IN A FIXTURE, and a
  // sheet job is the case that proves it: a speaker cabinet's baffle, sides
  // and back are each modelled in their assembly orientation and all cut from
  // one board, so they need three different frames and one stock. Optional
  // rather than defaulted, because a default of identity would silently
  // un-clamp a part the moment you gave it a position
  // (docs/cam/job-document.md; the split it refines is setup-frame.md's).
  std::optional<glm::dquat> frame;
};

struct Setup {
  uint64_t id = 0;
  std::string name;

  Stock stock;
  // Work origin, in setup space: every coordinate the POST writes is relative
  // to it. Derived — see `OriginAt` and `RecomputeOrigin`, which is what keeps
  // it right when the stock, the fixture or the picked face moves. Read it,
  // do not assign it.
  glm::dvec3 origin{0.0, 0.0, 0.0};
  OriginAt originAt = OriginAt::StockMin;

  // WHERE THE MACHINE IS ON THE SHEET, in setup space, measured from the
  // stock's near corner.
  //
  // The reach box has always been travel-sized with its corner AT the stock's
  // -- fine for a sheet that fits the bed, and an assumption for one that does
  // not. A 1122 mm baffle on a 900 mm router is slid through and clamped
  // wherever the work is, so nailing the box to the near corner is a GUESS,
  // and it was one with teeth: it decided what got clipped, and it moved when
  // the part was turned round, so spinning a board end-for-end made seven
  // operations fall off a machine that could reach every one of them.
  //
  // Zero (the default) is exactly the old behaviour, so nothing that fits
  // changes. `DeriveReachOffset` computes the offset that brings the cutting
  // inside the window, which is what an overhanging sheet actually needs.
  glm::dvec2 reachOffset{0.0, 0.0};

  // How the part is FIXTURED: the rotation taking setup space to model space.
  // A part is modelled where it belongs in the assembly — a speaker baffle
  // stands upright — and clamping it flat on the bed is a statement about the
  // job, not about the model. Saying it here is what lets one part be cut on
  // several sides without a rotated copy per side (docs/cam/setup-frame.md).
  //
  // It says NOTHING about the machine. `rotationA` below is the other half of
  // what used to be one number, and the conflation is what made laying a
  // panel flat post `G0 A90` at a 3-axis router.
  glm::dquat frame{1.0, 0.0, 0.0, 0.0};
  // The face Z zero was taken from. On the SETUP, not on the session: a job
  // reopened from its own file used to say "Z zero is not set yet" about an
  // origin it had saved and was using, because the panel asked a field that
  // lives as long as the window does. It is also per-setup, which the session
  // field was not — two setups in one document shared one answer between
  // them (docs/cam/dogfood-round.md). Empty = nobody has picked one; the
  // origin is then whatever it defaults to.
  Target topFace;
  // The MACHINE's rotary index: rotation about X in degrees, emitted as an A
  // word. 0 for every 3-axis job — INCLUDING one whose frame is rotated.
  // Needs `machine.hasRotaryAxis`: no file can carry an A word for an axis
  // the shop does not own.
  double rotationA = 0.0;

  // The machine this setup was set up on: the post, arc fitting, the rotary
  // axis, the work area. An EMBEDDED COPY of a globally stored profile, named
  // by `machine.name` (docs/cam/job-document.md, round two); `post`,
  // `fitArcs` and `hasRotaryAxis` used to be fields here, which is how every
  // new job re-answered "which machine". Unnamed = the job's own.
  Machine machine;
  double safeZ = 5.0;     // rapid height above the stock top (mm)
  double rapidFeed = 0.0; // 0 = G0; some machines want a capped G1

  // Where each part sits in the stock. Empty is a job that has parts but has
  // not been laid out -- everything at the origin, which is the honest
  // reading of "nobody has said".
  std::vector<Placement> placements;

  // In what order the operations cut, and whether the post writes one file
  // per group. Properties of the JOB, unlike the machine above.
  OrderOutputBy order = OrderOutputBy::Authored;
  // One program per tool (docs/cam/router-round.md, R2): most hobby
  // controllers cannot take a tool change mid-file, so the operator runs
  // T1's file, changes the tool, runs T2's. Forces tool order, since a file
  // per tool that is not in tool order is a contradiction.
  bool splitOutput = false;

  std::vector<Tool> tools;
  std::vector<Operation> operations;

  [[nodiscard]] const Placement* FindPlacement(uint64_t id) const {
    for (const Placement& p : placements)
      if (p.id == id)
        return &p;
    return nullptr;
  }

  [[nodiscard]] const Tool* FindTool(int number) const {
    for (const Tool& t : tools)
      if (t.number == number)
        return &t;
    return nullptr;
  }
};

// --- toolpaths ------------------------------------------------------------

// One machine move. Carries an optional A: the plan's rule is that v1 must
// not assume three axes in the toolpath representation or the post interface.
//
// Since C9 something DOES emit one -- an indexed setup stamps its constant
// `rotationA` on every move of every toolpath (CamStrategies.cpp) and the
// post writes the A word. Everything downstream of the post is still three
// axes: the G-code parser drops an A word, and the height map the verifier
// and the simulation share cannot represent a part that turns.
struct Move {
  enum class Kind { Rapid, Cut, Plunge };
  Kind kind = Kind::Cut;
  glm::dvec3 to{0.0};
  bool hasA = false;
  double a = 0.0;
  double feed = 0.0; // mm/min; 0 = rapid
};

// What a roughing pass leaves for the finishing tool (R6), derived from the
// roughing cutter: a tenth of its diameter -- enough to take the roughing
// marks off, little enough that the finishing tool takes it in one pass.
[[nodiscard]] inline double DefaultFinishStock(const Tool& rough) {
  return rough.diameter * 0.1;
}

// A closed contour on a Z plane, in setup space. `hole` marks a loop that
// bounds a void rather than material — the distinction Clipper needs to
// respect islands in a pocket.
struct Loop {
  std::vector<glm::dvec2> pts;
  bool hole = false;
  // The diameter of the bore this loop IS, or 0 when it is not one: a wire of
  // exactly one circular edge. Exact, and not a guess from the polygon --
  // "is this opening a drilled hole?" is a question about the model, and a
  // 2.2 mm slot is narrower than a 3.2 mm bolt hole, so no measurement of
  // WIDTH can tell them apart. Filled by PickedFaceRegion; sectioned loops
  // leave it 0.
  double boreDiameter = 0.0;
};

struct Toolpath {
  uint64_t operationId = 0;
  int toolNumber = 1;
  std::vector<Move> moves;
  std::string error; // non-empty = this op produced nothing, and why
  // Generated with `GenerateContext::defer` and at least one adaptive pass
  // not yet in the cache: the moves are whatever the caller chose to keep
  // (the previous toolpath, or nothing) and nothing downstream may treat
  // them as the job — post, verify and simulate refuse while any is pending
  // (docs/ui/long-operations.md, phase 1).
  bool pending = false;
  // Non-empty when the operation was cut by a DIFFERENT strategy than its
  // type says — adaptive falling back to concentric clearing. Silence about
  // that is a job whose feeds were chosen for the strategy it did not use.
  std::string strategy;
  // Where the path deliberately cut INTO the model: a corner relief's bite,
  // as the path corner it was taken from and the radius the bite fills. The
  // verifier is told, so a dogbone reads as "corner relieved" and not as the
  // gouge it would otherwise be (R4).
  struct Relief {
    glm::dvec2 at{0.0};
    double radius = 0.0;
  };
  std::vector<Relief> reliefs;
  // Where the path deliberately did NOT cut: a tab, as the point on the
  // contour it sits on, the radius of the pillar it leaves (half the tab
  // width plus the tool's, because a round cutter passing either side is
  // what shapes it) and the height its top stands at. The verifier is told,
  // so a tab reads as "asked for" rather than as material left standing --
  // and unlike a relief this needs the HEIGHT, because a relief excuses a
  // cut that went too deep and a tab excuses one that did not happen: excuse
  // it by position alone and a genuinely uncut slab above the tab is excused
  // with it.
  struct Tab {
    glm::dvec2 at{0.0};
    double radius = 0.0;
    double topZ = 0.0;
  };
  std::vector<Tab> tabs;
  // The finishing half of a rough-then-finish operation (R6): the same
  // operationId as the roughing toolpath, the finishing tool's number.
  bool finish = false;
  // Millimetres of cutting that lay beyond the machine's travel and were
  // left out of this path (ClipToMachine). The verdict says so; a controller
  // never truncates for you -- it alarms on the line, or hits the stop.
  double clippedMm = 0.0;
  // The targets this operation could NOT follow, one sentence each. An
  // operation with several targets is generated once per target
  // (GenerateFramed), and one unusable pick must not throw away the four good
  // ones -- so this is not `error`, which means the operation produced nothing
  // at all and makes the post skip it entirely.
  std::vector<std::string> skipped;

  [[nodiscard]] bool Empty() const { return moves.empty(); }
  // Total distance travelled while cutting, in mm — the cheap sanity check a
  // test can assert against a known contour.
  [[nodiscard]] double CutLength() const;
  // Lowest Z reached. A toolpath that dives below the stock bottom is a bug
  // worth catching in a test rather than in aluminium.
  [[nodiscard]] double MinZ() const;
};

} // namespace cam
