#pragma once

#include "core/ElementMap.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// CAM: milling setups, operations and the toolpaths they produce
// (docs/cam/README.md).
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
  // A single-profile thread mill (v145, docs/cam/operations.md#thread-mill):
  // a disc whose rim is a V of `tipAngleDeg` INCLUDED angle (60° for ISO
  // and unified threads) truncated by `tipFlat`, on a neck of
  // `neckDiameter` x `neckLength`. `diameter` is the cutting diameter, at
  // the flat; `fluteLength` is the head's axial length, the V's own width.
  ThreadMill,
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
  // How wide the tool is ABOVE its flutes, from the top of the flutes up to
  // the collar. Zero (the default) means nobody said, and the tool is taken
  // to be the cutter's width all the way up -- the assumption every check
  // made before this existed. Wider is the common small cutter: a 3 mm bit
  // on a 6 mm shank, a V-bit's body, a stepped downcut. Below its flute
  // length along a wall that part does not rub, it HITS: the cut is narrower
  // than the thing being driven into it. Narrower is a necked tool, made to
  // reach past its flutes (docs/cam/setup.md#reach).
  double shankDiameter = 0.0;
  // A thread mill's head (ToolType::ThreadMill, v145): the flat its V is
  // truncated to at the cutting diameter, and the neck the head sits on --
  // narrower than the head, so it passes inside the thread's crests, and
  // long enough to reach the thread's bottom. Read by nothing else.
  double tipFlat = 0.0;      // mm, axial width of the flat at the rim
  double neckDiameter = 0.0; // mm
  double neckLength = 0.0;   // mm, from the top of the head up
  bool operator==(const Tool&) const = default;
};

// How deep a tool reaches before something wider than what it cut follows it
// down: the flutes, and for a thread mill the neck above its head as well
// (docs/cam/setup.md#reach). The reach check and the shank probe read this.
[[nodiscard]] inline double ReachLength(const Tool& t) {
  return t.fluteLength + (t.type == ToolType::ThreadMill ? t.neckLength : 0.0);
}

// A thread mill's profile depth: how far its V stands out from its neck,
// (diameter - neck) / 2. The deepest thread it can cut, and the reason the
// neck clears the crests (docs/cam/operations.md#thread-mill).
[[nodiscard]] inline double ProfileDepth(const Tool& t) {
  return std::max(0.0, (t.diameter - t.neckDiameter) * 0.5);
}

// What a thread mill's shape is when nobody has said, from its cutting
// diameter (docs/cam/operations.md#the-thread-mill-cutter): a 60° V, a flat
// of a sixtieth of the diameter (0.1 mm on a 6 mm head, a common grind), a
// neck of 0.6 of it (a profile 0.2 of the diameter deep, which reaches the
// full depth of a thread whose tap drill is a little over the head), three
// diameters of neck, and the head's own axial length as its flute. Only the
// fields that are 0 are filled, so a measured number is never replaced;
// `angleStated` false also sets the 60° (a Tool's default angle is a V-bit's).
inline void FillThreadMillShape(Tool& t, bool angleStated) {
  if (!angleStated)
    t.tipAngleDeg = 60.0;
  if (t.tipFlat <= 0.0)
    t.tipFlat = t.diameter / 60.0;
  if (t.neckDiameter <= 0.0)
    t.neckDiameter = t.diameter * 0.6;
  if (t.neckLength <= 0.0)
    t.neckLength = t.diameter * 3.0;
  const double half = glm::radians(t.tipAngleDeg * 0.5);
  t.fluteLength = t.tipFlat + 2.0 * ProfileDepth(t) * std::tan(half);
}

// How the rotary is INSTALLED (docs/cam/rotary.md#how-the-rotary-is-installed).
// Inside the app the part's axis is setup +X, the chuck at its low end, and A
// turns right-handed about it; this says what that is on THIS machine, so the
// post writes it and the reader reads it back. Stated together or not at all
// -- and not stated, a setup held in the rotary does not post: guessing X is
// what would have run a first program across a part that lay along Y.
struct RotaryMount {
  char along = 0;    // 'x' | 'y': the machine axis it lies along; 0 = unstated
  int chuckEnd = -1; // the chuck at that axis's minus (-1) or plus (+1) end
  char word = 'A';   // the letter the controller drives it with: 'A' | 'B'
  // Which way the TOP of the part moves on a positive jog: an axis ('x' |
  // 'y', across `along`) and a sign. What a person sees at the machine, so
  // no handedness to reason about.
  char topAxis = 0;
  int topSign = 0;
  [[nodiscard]] bool Stated() const { return along != 0; }
  bool operator==(const RotaryMount&) const = default;
};

// What holds a part in the rotary: the chuck and the tailstock, as an
// envelope round the axis (docs/cam/rotary.md#chuck-and-tailstock). Every
// size 0 = not measured, and a conservative default stands in -- bigger than
// the hardware, never smaller -- which the verdict says.
struct RotaryHolding {
  double chuckDiameter = 0.0;    // the chuck body, mm (90)
  double gripLength = 0.0;       // how much stock the jaws hold, mm (15)
  bool hasTailstock = false;     // the shop has one to engage
  double centreLength = 0.0;     // the centre's point to its base, mm (25)
  double centreDiameter = 0.0;   // the centre at its base, mm (14)
  double tailBodyDiameter = 0.0; // the tailstock behind the centre, mm (60)
  double clearance = 0.0;        // kept from both, mm (3)
  bool operator==(const RotaryHolding&) const = default;
};

// The MACHINE, as one physical thing (docs/cam/setup.md#machine-profiles;
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
  // The machine's traverse rate, mm/min; 0 = not stated. It clocks rapids in
  // the time estimate, caps the axes on a turning cut (CamRotaryFeed.h) and is
  // the ceiling the feed check compares a tool's band against. Rapids are
  // always posted as G0, the controller's own rapid; nothing emits a capped
  // G1. Seeds Setup::rapidFeed.
  double rapidFeed = 0.0;
  // The A axis's top speed, in degrees per minute; 0 = not stated. A cut
  // that turns the part is held to it (CamRotaryFeed.h), and it is its own
  // number because it is its own motor: a 4th axis turns at a few thousand
  // degrees a minute on a router whose linear rapid is 1500 mm/min.
  double rotaryMaxFeed = 0.0;
  // How the rotary is installed, and what holds a part in it (v132).
  RotaryMount rotary;
  RotaryHolding holding;
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
  case ToolType::ThreadMill: {
    // The head's lower flank: flat out to the neck, then rising at half the
    // V's angle from the radial to the rim. A height map sees the head from
    // BELOW, so this is everything the carve can know of it -- the upper
    // flank and the crests it leaves stand in the column the carve clears,
    // which is why a thread is judged on its own terms
    // (docs/cam/simulation.md#threads).
    const double neck = t.neckDiameter * 0.5;
    return d <= neck ? 0.0
                     : (d - neck) * std::tan(glm::radians(t.tipAngleDeg * 0.5));
  }
  default:
    return 0.0;
  }
}

// The feed for a cutting move that may descend
// (docs/cam/operations.md#depth-passes-and-entries). A plunge feed exists to
// limit how fast the tool goes DOWN; on a ramp or a helix most of the move is
// sideways, so what the plunge feed governs is only the vertical component:
// min(feedCut, feedPlunge / sin(angle)). A 3 degree ramp at 1200 mm/min comes
// down at 63 mm/min, and running it at the plunge feed instead cut at a third
// of the chip load -- rubbing, in MDF -- for minutes of every job; a small
// hole, whose ramp fills its whole lap, was cut almost entirely that way. Level
// or rising: the cutting feed. Straight down: the plunge feed. pycam's rule;
// FreeCAD ramps at the cutting feed.
[[nodiscard]] inline double EntryFeed(const Tool& t, const glm::dvec3& from,
                                      const glm::dvec3& to) {
  const double drop = from.z - to.z;
  if (drop <= 1e-9)
    return t.feedCut;
  const double along = glm::length(to - from);
  return std::min(t.feedCut, t.feedPlunge * along / drop);
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
// `cam::ReachCheck` states (or refuses) when the op or its tool is edited,
// before a file exists. Reporting it
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
// scope-limited link it shipped first (docs/cam/README.md#the-model).
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
  // Finishing passes (docs/cam/operations.md#3-axis-surfacing). Neither is a
  // stack of levels: the Z varies continuously WITHIN a pass, which is what the
  // 2.5D pipeline cannot express and why these are two more types rather than a
  // flag on an existing one.
  //
  // Two and not one, because they answer opposite questions -- a raster is
  // good on shallow surfaces and poor on steep ones, a waterline the reverse
  // -- and because a single op cannot honestly carry both an XY fraction and
  // a Z step under one name.
  SurfaceParallel,
  SurfaceWaterline,
  // 4-axis finishing: parallel passes ALONG the rotary axis, the part turning
  // between them, or one spiral with A turning while the tool cuts -- by the
  // op's `rotaryPattern` (docs/cam/rotary.md#patterns-rotary_pattern).
  SurfaceRotary,
  // Follow SKETCH curves ON the curve, down from the surface the sketch lies
  // on (v138, docs/cam/operations.md#engrave): a label, a line, an arc. The
  // one type whose target is not a face of a body.
  Engrave,
  // Mill a thread (v145, docs/cam/operations.md#thread-mill): a helix of
  // half-turn arcs, Z advancing half a pitch each, in the bore (or round the
  // boss) a Thread feature -- or a tapped Hole -- is on. Driven by the thread,
  // not by typed numbers: its size, pitch, hand, side and class.
  ThreadMill,
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

// Corner relief on profiles and pockets (docs/cam/operations.md#profile). A
// round cutter leaves a radius in every inside corner of the material; a
// joint that has to take a square tenon needs the corner cleared. Dogbone:
// out along the corner's bisector and back, the tool's RADIUS -- FreeCAD's
// nominal, and the only length that clears the corner (Kiri:Moto's fifth of
// a diameter does not). T-bone: the same length along the longer of the two
// walls, where a joint hides it. Off by default: a relief is a decision about
// the joint, not a fact about the part.
enum class CornerRelief { None, Dogbone, TBone };

// Cutter radius compensation in the controller
// (docs/cam/post.md#cutter-compensation). Off: the file is the tool centre and
// nothing else, as always. Wear: the SAME tool-centre path, posted under
// G41/G42 with the tool's D register, so the controller's tool table holds
// only the wear -- 0 cuts what was verified, and a measured bore is corrected
// at the machine without re-posting. The other convention ("in control": the
// part contour posted, the table holding the whole radius) is deliberately
// not offered: its file is not the cut the verifier reads, and an empty table
// cuts every wall a radius into the part.
enum class CutterComp { Off, Wear };

// How a rotary finish covers the part
// (docs/cam/rotary.md#patterns-rotary_pattern). Parallel: passes ALONG the
// axis, each at one A, the part turning between them -- a rotary raster.
// Spiral: one helix from end to end, A turning WHILE the tool cuts, one
// revolution per stepover -- continuous 4-axis. Helical: passes that FOLLOW a
// twist, helices at the part's own lead stepped round one from the next
// (docs/cam/rotary.md#patterns-rotary_pattern) -- continuous too.
enum class RotaryPattern { Parallel, Spiral, Helical };

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
  // Rest machining (v125,
  // docs/cam/operations.md#rough-a-surface-and-rest-machining): the number of
  // the LARGER cutter that finished before this one. A surface op with it set
  // cuts only where that cutter could not reach -- where this tool's tip goes
  // measurably deeper than its did. 0 = cut the whole region.
  int restFromTool = 0;
  // Profile only (R5): leave the wall on a tangent arc of the tool's radius
  // at the end of every pass instead of stopping against it and lifting --
  // the stop is the dwell mark. The ramp is the entry, so there is no lead
  // in: the tool arrives at the wall descending, already moving along it.
  bool leadArcs = false;
  // Profile, pocket and drill (a bore), v158: post the finishing wall passes
  // under cutter compensation (docs/cam/post.md#cutter-compensation). On a
  // rough-then-finish op, the finishing toolpath only.
  CutterComp cutterComp = CutterComp::Off;
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
  // Drill only (docs/cam/operations.md#drill): retract fully between pecks
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
  // SurfaceRotary, helical only (v124): the axial advance per turn of a
  // pass, in mm, signed for the hand (positive: theta grows with x). 0 =
  // derive it from the part's own twist (DeriveRotaryTwist).
  double leadMm = 0.0;

  // Cut at each of these rotary indices, degrees about X (v136): the
  // crenels of a rook are ONE operation at 0/90/180/270, not four setups
  // that each repeat it at one angle. Generated once per angle in that
  // angle's frame and joined by a retract; every move carries its A. Empty =
  // the setup's own `rotationA`. Needs a machine with a rotary axis; not on
  // a surface_rotary, which turns the part as it cuts.
  std::vector<double> indexA;

  // Engrave only (v138, docs/cam/operations.md#engrave): the groove's width
  // AT THE SURFACE, mm, or its depth below it, mm -- one statement, not two.
  // A V-bit's depth IS its width (TipHeight(tool, width / 2)); a flat or a
  // ball takes a depth only. 0 = not stated.
  double engraveWidth = 0.0;
  double engraveDepth = 0.0;

  // ThreadMill only (v145): how many radial passes take the thread's depth,
  // each removing the same area of it. 0 = derive it from the cutter: the
  // first pass takes at most half its profile depth
  // (docs/cam/operations.md#thread-mill). `climb` and `leadArcs` are the
  // direction and the semicircle entry and exit.
  int threadPasses = 0;

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
// (docs/cam/setup.md#stock).
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
// two ways to tilt a part is one too many
// (docs/cam/setup.md#parts-and-placements).
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
  // (docs/cam/setup.md#fixturing-frame-and-rotary-index).
  std::optional<glm::dquat> frame;
};

// What HOLDS the stock: a clamp, a vise, a fixture plate -- an ordinary part
// document, placed on the bed of one setup
// (docs/cam/setup.md#workholding-fixtures).
//
// Not a Placement and not a JobPart: those are WORKPIECES, which get stock,
// targets and toolpaths. A fixture gets none of that; it is only somewhere
// the tool must never go, so it can never grow the stock box or become a
// surface a gouge is measured against.
//
// BED space, not model space. The setup's `frame` turns the PART to lie on
// the bed and a vise does not turn with it, so a fixture's model is read as
// bed coordinates, Z up, then spun and moved (FixtureTransform). It belongs to
// a setup because re-clamping between setups is what a second setup is.
//
// Or a KEEP-OUT BOX, with no part at all: "there is a clamp here, this big"
// is most of what a first job needs, and a part tab per toe clamp is the
// friction that would leave the feature unused.
// Which edge of the stock's top a seated fixture reaches in over.
enum class StockEdge : uint8_t { MinusX, PlusX, MinusY, PlusY };

// A fixture that RESTS ON the stock rather than standing at a bed position:
// one face of it on the stock's top, reaching in over one edge
// (docs/cam/setup.md#seats). A face-to-face relation, stored and
// re-solved against the stock as it is now -- so a clamp placed on a board
// stays on the board when the stock grows, where a bed position floats.
// Every number is measured from the stock, not the bed.
struct FixtureSeat {
  ElementRef face; // the fixture's face that rests on the stock top
  StockEdge edge = StockEdge::MinusX;
  double alongMm = 0.0; // from the edge's low end to the face's centre
  double overMm = 0.0;  // how far the face reaches in past the edge
  double spinDeg = 0.0; // turned from pointing straight in
};

struct Fixture {
  uint64_t id = 0;          // from the job's one id counter
  uint64_t sourceDocId = 0; // the part slot it is a copy of; 0 = a box
  glm::dvec3 box{0.0};      // a keep-out box's size, mm, corner at its origin
  std::string name;         // display; a part owns its own name
  glm::dvec3 pos{0.0};      // setup space, mm
  double rotZdeg = 0.0;     // spin about +Z, degrees
  bool enabled = true;      // off = drawn, not checked
  // When set, `pos` and `rotZdeg` are the pose it last solved to, kept so a
  // reader that cannot solve still has somewhere sane to draw it; the seat
  // is the truth (ui::FixtureBedTransform re-solves it on every read).
  std::optional<FixtureSeat> seat;

  [[nodiscard]] bool IsBox() const { return sourceDocId == 0; }
};

// bed = T(pos) * Rz(rotZ) * model.
[[nodiscard]] inline glm::dmat4 FixtureTransform(const Fixture& f) {
  const double a = glm::radians(f.rotZdeg);
  glm::dmat4 m(1.0);
  m[0][0] = std::cos(a);
  m[0][1] = std::sin(a);
  m[1][0] = -std::sin(a);
  m[1][1] = std::cos(a);
  m[3] = glm::dvec4(f.pos, 1.0);
  return m;
}

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
  // default window: centred on the stock along an axis where it fits the
  // travel, at its near corner along one where it is longer
  // (docs/cam/setup.md#the-machines-work-area).
  //
  // The reach box has always been travel-sized with its corner AT the stock's
  // -- fine for a sheet that fits the bed, and an assumption for one that does
  // not. A 1122 mm baffle on a 900 mm router is slid through and clamped
  // wherever the work is, so nailing the box to the near corner is a GUESS,
  // and it was one with teeth: it decided what got clipped, and it moved when
  // the part was turned round, so spinning a board end-for-end made seven
  // operations fall off a machine that could reach every one of them.
  //
  // Zero (the default) is that default window. `DeriveReachOffset` computes
  // the offset that brings the cutting inside it, which is what an
  // overhanging sheet actually needs; on an axis that fits it is zero.
  glm::dvec2 reachOffset{0.0, 0.0};

  // How the part is FIXTURED: the rotation taking setup space to model space.
  // A part is modelled where it belongs in the assembly — a speaker baffle
  // stands upright — and clamping it flat on the bed is a statement about the
  // job, not about the model. Saying it here is what lets one part be cut on
  // several sides without a rotated copy per side
  // (docs/cam/setup.md#fixturing-frame-and-rotary-index).
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
  // them (docs/cam/setup.md#work-origin). Empty = nobody has picked one; the
  // origin is then whatever it defaults to.
  Target topFace;
  // The MACHINE's rotary index: rotation about X in degrees, emitted as an A
  // word. 0 for every 3-axis job — INCLUDING one whose frame is rotated.
  // Needs `machine.hasRotaryAxis`: no file can carry an A word for an axis
  // the shop does not own.
  double rotationA = 0.0;
  // Whether this setup's part is held IN THE ROTARY (v132). Derived when not
  // stated: a rotary op, an op with its own index list or a non-zero index
  // says so. An indexed setup at
  // A = 0 -- flats on a turned part -- is in the chuck all the same, and
  // says so itself (docs/cam/rotary.md#how-the-rotary-is-installed).
  enum class HeldIn { Derived, Bed, Rotary };
  HeldIn heldIn = HeldIn::Derived;
  // How much of the stock the jaws hold, mm; 0 = the machine's.
  double gripLength = 0.0;
  // The tailstock: -1 = engaged when the machine has one, 0 off, 1 on.
  int tailstock = -1;
  // The chuck: -1 / 1 = the part's stock is gripped at its low end (the
  // rotary has a chuck, always), 0 = held some other way nothing here models
  // -- a collet on a spigot beyond the stock, between centres with a dog.
  int chuck = -1;

  // The machine this setup was set up on: the post, arc fitting, the rotary
  // axis, the work area. An EMBEDDED COPY of a globally stored profile, named
  // by `machine.name` (docs/cam/setup.md#machine-profiles); `post`,
  // `fitArcs` and `hasRotaryAxis` used to be fields here, which is how every
  // new job re-answered "which machine". Unnamed = the job's own.
  Machine machine;
  double safeZ = 5.0;     // rapid height above the stock top (mm)
  double rapidFeed = 0.0; // traverse rate, mm/min, 0 = not stated (Machine)

  // Where each part sits in the stock. Empty is a job that has parts but has
  // not been laid out -- everything at the origin, which is the honest
  // reading of "nobody has said".
  std::vector<Placement> placements;

  // What holds the stock down in this setup (Fixture, above).
  std::vector<Fixture> fixtures;

  // In what order the operations cut, and whether the post writes one file
  // per group. Properties of the JOB, unlike the machine above.
  OrderOutputBy order = OrderOutputBy::Authored;
  // One program per tool (docs/cam/post.md#files): most hobby
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

  [[nodiscard]] const Fixture* FindFixture(uint64_t id) const {
    for (const Fixture& f : fixtures)
      if (f.id == id)
        return &f;
    return nullptr;
  }
  [[nodiscard]] Fixture* FindFixture(uint64_t id) {
    for (Fixture& f : fixtures)
      if (f.id == id)
        return &f;
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

// One machine move. Carries an optional A, written by the post as an A word.
// An indexed setup stamps its constant `rotationA` on every move of every
// toolpath, an operation cut at several indices (`Operation::indexA`) stamps
// each piece with its own angle, and a continuous rotary pass turns A while
// it cuts (CamStrategies.cpp, CamSurfaceStrategies.cpp). The parser keeps a
// modal A, and a file whose A moves is verified and played on a radius map
// (docs/cam/rotary.md#verifying-a-turning-part).
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
  // (docs/ui/long-operations.md#cam-toolpath-passes).
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
  // Where the path cut INTO the surface on purpose along a line: an
  // engraving's stroke (docs/cam/operations.md#engrave), as the tool
  // centre's polyline in setup XY, the height its tip ran at, the surface it
  // was cut from and the tool that cut it. The verifier excuses a cut down
  // to that NOMINAL groove -- the stroke swept by the tool's own shape,
  // TipHeight -- and nothing under it or beside it: a relief excuses any
  // depth within its bite, a groove only its own shape.
  struct Groove {
    std::vector<glm::dvec2> pts;
    double tipZ = 0.0;
    double topZ = 0.0;
    Tool tool;
  };
  std::vector<Groove> grooves;
  // Moves that are ONE exact arc, for the post to write as one G2/G3 --
  // helical when Z changes along it (docs/cam/post.md#arcs-and-line-merging).
  // Moves [first, last] are chords of the arc that starts where move
  // first - 1 ends, round `centre` (setup XY), Z linear in the angle. The
  // moves stay the polyline everything else reads; the post checks each run
  // still is one before trusting it (a clip or a merge may have moved them)
  // and writes the chords when it is not.
  struct Arc {
    size_t first = 0, last = 0;
    glm::dvec2 centre{0.0};
    bool clockwise = false;
  };
  std::vector<Arc> arcs;
  // A thread this path cuts (docs/cam/simulation.md#threads), as the
  // generator aimed it: the verifier cannot see a flank that faces sideways
  // in a height map, so it judges the thread from the posted file against
  // this -- the helix's pitch and hand, the diameter its tip reached against
  // the class's band, the span of full thread, the bore it runs in -- and
  // excuses the carve inside the thread's radius down to its lowest tip.
  struct Thread {
    uint64_t op = 0;        // the operation, as the file's `(op N)` names it
    glm::dvec2 centre{0.0}; // setup XY
    bool internal = true;
    bool rightHand = true;
    double pitch = 0.0;      // mm
    std::string designation; // "3/8-16 UNC"
    std::string threadClass; // the class aimed at
    bool classStated = true; // false: the thread named none, the default used
    double pdMin = 0.0, pdMax = 0.0; // the class's pitch-diameter band
    double pdAim = 0.0;              // what the last pass is set to cut
    double boreDia = 0.0;        // the face: the tap drill's bore, or the boss
    double zLo = 0.0, zHi = 0.0; // the threaded span, setup Z
    bool through = false;        // open below: overtravel past the last turn
    // The span of FULL thread the ends allow: all of [zLo, zHi] where the
    // helix may run past them, less what a blind floor keeps the head from
    // (its V is `fluteLength` tall) or a thread that stops mid-bore.
    double fullLo = 0.0, fullHi = 0.0;
    Tool tool;
  };
  std::vector<Thread> threads;
  // A toleranced round wall this path aimed at the middle of its band
  // (docs/cam/operations.md#tolerance-bands): the wall's centre and side,
  // its nominal diameter, the band, what the path aimed at and the wall's
  // span. The verifier measures the diameter the posted file cut there and
  // judges it against the band, and the post states the band in its header.
  struct Fit {
    uint64_t op = 0;        // the operation, as the file's `(op N)` names it
    glm::dvec2 centre{0.0}; // setup XY
    bool internal = true;   // a bore; false: a boss
    double nominal = 0.0;   // mm, the modelled diameter
    double lower = 0.0;     // mm, the band's deviations on the diameter
    double upper = 0.0;
    double aim = 0.0;            // mm, the diameter the path cuts
    double zLo = 0.0, zHi = 0.0; // the wall's span, setup Z
    std::string label;           // "K7", "+0.05/-0.02"
    // A DISTANCE band instead (docs/cam/operations.md#tolerance-bands): a
    // level floor or shoulder at `faceZ` whose distance from the level
    // reference face at `refZ` carries the band. `nominal`, the band and
    // `aim` are that distance; `internal` says removing metal lengthens it
    // (the floor is below its reference). `lo`/`hi` are the floor's extent,
    // setup XY, and `centre` its middle.
    bool depth = false;
    double faceZ = 0.0, refZ = 0.0;
    glm::dvec2 lo{0.0}, hi{0.0};
    [[nodiscard]] double Min() const { return nominal + lower; }
    [[nodiscard]] double Max() const { return nominal + upper; }
    // The floor's Z at a distance `d` from the reference.
    [[nodiscard]] double ZAt(double d) const {
      return faceZ < refZ ? refZ - d : refZ + d;
    }
  };
  std::vector<Fit> fits;
  // A pass that follows a finished WALL (docs/cam/post.md#cutter-compensation),
  // declared by the strategy that cut it: moves [first, last] are the pass
  // with the tool down, starting where move first - 1 ends, the material on
  // the right of travel when `materialRight`. Read only when the operation
  // asks for cutter compensation, which turns each into a `Comp`.
  struct Wall {
    size_t first = 0, last = 0;
    bool materialRight = true;
  };
  std::vector<Wall> walls;
  // A run the controller compensates: G41 (left) or G42 before move `first`,
  // the lead-in line; G40 before move `last`, the lead-out line. Everything
  // between is the lead-in arc, the wall pass and the lead-out arc. The moves
  // are the tool centre with no wear -- the cut the verifier reads.
  struct Comp {
    size_t first = 0, last = 0;
    bool left = true;
    // The wear (on the diameter, mm) the run was checked to compensate either
    // way: a tenth of the cutter.
    double checkedWear = 0.0;
  };
  std::vector<Comp> comps;
  // The finishing half of a rough-then-finish operation (R6): the same
  // operationId as the roughing toolpath, the finishing tool's number.
  bool finish = false;
  // Millimetres of cutting that lay beyond the machine's travel and were
  // left out of this path (ClipToMachine). The verdict says so; a controller
  // never truncates for you -- it alarms on the line, or hits the stop.
  double clippedMm = 0.0;
  // Cutting left out because it would reach into the chuck or the tailstock
  // (CamMount.h HoldingEnvelope): its own number, not the travel's.
  double holdingClippedMm = 0.0;
  // The targets this operation could NOT follow, one sentence each. An
  // operation with several targets is generated once per target
  // (GenerateFramed), and one unusable pick must not throw away the four good
  // ones -- so this is not `error`, which means the operation produced nothing
  // at all and makes the post skip it entirely.
  std::vector<std::string> skipped;

  [[nodiscard]] bool Empty() const { return moves.empty(); }
  // Total distance travelled while cutting, in mm — the cheap sanity check a
  // test can assert against a known contour. The TIP's path (SegmentLength):
  // a cut that turns A counts the arc it turns under the tool.
  [[nodiscard]] double CutLength() const;
  // Lowest Z reached. A toolpath that dives below the stock bottom is a bug
  // worth catching in a test rather than in aluminium.
  [[nodiscard]] double MinZ() const;
};

} // namespace cam
