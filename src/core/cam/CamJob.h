#pragma once

#include "core/ElementMap.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

#include <cstdint>
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
};

// Which side of a contour the tool runs on. For a pocket this is implicit
// (inside); for a profile it is the difference between a part and a hole.
enum class Side { Outside, Inside, On };

struct Operation {
  uint64_t id = 0;
  std::string name;
  OpType type = OpType::Profile;
  int toolNumber = 1;

  // What to machine. Stable element references (toponaming), so a model edit
  // that moves a face does not orphan the job — the reason to have CAM in
  // the CAD rather than downstream of a STEP export.
  std::vector<ElementRef> targets;

  // Depth, in setup space. topZ is where the cut starts (usually the stock
  // top), bottomZ where it ends. stepdown 0 = one pass.
  double topZ = 0.0;
  double bottomZ = -1.0;
  double stepdown = 0.0;
  double stockToLeave = 0.0;

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
  // A hole is not an island: there is nothing there to avoid. Setting this
  // clears the outline and passes straight over the openings. FreeCAD spells
  // the same idea `UseOutline` ("uses the outline of the base geometry",
  // Path/Op/PocketShape.py) and implements it the same way — keep the outer
  // wire, drop the rest.
  bool useOutline = false;

  // Pocket/Face only: fraction of the tool diameter to step over per pass.
  // EdgeRound reads it too: it is what sets the cusp between arc passes.
  double stepover = 0.45;

  // EdgeRound only: the size of the break, in mm — the radius of the round
  // for a ball, the horizontal width of the bevel for a V-bit. 0 = derive it
  // from the tool and the wall (defaults rule).
  double edgeSize = 0.0;

  bool enabled = true;
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
enum class OriginAt { StockMin, StockCentre, ModelMin };

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
  ElementRef topFace;
  // The MACHINE's rotary index: rotation about X in degrees, emitted as an A
  // word. 0 for every 3-axis job — INCLUDING one whose frame is rotated.
  double rotationA = 0.0;
  // Whether the machine has an A axis at all. A property of the MACHINE, like
  // `post` and `fitArcs`, so it persists. False means the rotary controls are
  // not offered and no file can command an axis the shop does not own.
  bool hasRotaryAxis = false;

  // Whether the post fits G2/G3 arcs (C7). A property of the MACHINE, so it
  // lives on the setup and persists — a controller that mishandles arcs
  // mishandles them every session.
  bool fitArcs = true;
  std::string post = "mach3"; // post id; "linuxcnc" planned
  double safeZ = 5.0;         // rapid height above the stock top (mm)
  double rapidFeed = 0.0;     // 0 = G0; some machines want a capped G1

  std::vector<Tool> tools;
  std::vector<Operation> operations;

  [[nodiscard]] const Tool* FindTool(int number) const {
    for (const Tool& t : tools)
      if (t.number == number)
        return &t;
    return nullptr;
  }
};

// --- toolpaths ------------------------------------------------------------

// One machine move. Deliberately carries an optional A even though nothing
// emits one yet: the plan's rule is that v1 must not assume three axes in
// the toolpath representation or the post interface.
struct Move {
  enum class Kind { Rapid, Cut, Plunge };
  Kind kind = Kind::Cut;
  glm::dvec3 to{0.0};
  bool hasA = false;
  double a = 0.0;
  double feed = 0.0; // mm/min; 0 = rapid
};

struct Toolpath {
  uint64_t operationId = 0;
  int toolNumber = 1;
  std::vector<Move> moves;
  std::string error; // non-empty = this op produced nothing, and why
  // Non-empty when the operation was cut by a DIFFERENT strategy than its
  // type says — adaptive falling back to concentric clearing. Silence about
  // that is a job whose feeds were chosen for the strategy it did not use.
  std::string strategy;

  [[nodiscard]] bool Empty() const { return moves.empty(); }
  // Total distance travelled while cutting, in mm — the cheap sanity check a
  // test can assert against a known contour.
  [[nodiscard]] double CutLength() const;
  // Lowest Z reached. A toolpath that dives below the stock bottom is a bug
  // worth catching in a test rather than in aluminium.
  [[nodiscard]] double MinZ() const;
};

} // namespace cam
