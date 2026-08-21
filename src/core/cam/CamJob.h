#pragma once

#include "core/ElementMap.h"

#include <glm/glm.hpp>

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

struct Tool {
  int number = 1; // T-word; also how a post addresses it
  std::string name;
  double diameter = 6.0; // mm
  int flutes = 2;
  double fluteLength = 20.0; // mm, the usable depth
  // Feeds and speeds are NOT computed from a chip-load formula: this app
  // does not know the machine's rigidity, and a wrong number here breaks a
  // tool. They are what the user actually runs.
  double feedCut = 300.0;    // mm/min in XY
  double feedPlunge = 100.0; // mm/min in Z
  double spindleRpm = 10000.0;
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

  // Pocket/Face only: fraction of the tool diameter to step over per pass.
  double stepover = 0.45;

  bool enabled = true;
};

// --- the setup ------------------------------------------------------------

// Stock: what is in the vise before the first cut. Derived from the model's
// bounding box plus margins rather than typed in absolute terms, so it stays
// right when the model changes (defaults rule — docs/ui/u10-*).
struct Stock {
  glm::dvec3 margin{2.0, 2.0, 1.0}; // added around the model bbox (mm)
  double bottomMargin = 1.0;        // below the model, for the sacrificial pass
};

struct Setup {
  uint64_t id = 0;
  std::string name;

  Stock stock;
  // Work origin and orientation. The post emits coordinates in this frame;
  // rotating it is how an INDEXED 4th axis works (deferred, but the shape is
  // here so it lands as data rather than as a redesign).
  glm::dvec3 origin{0.0, 0.0, 0.0};
  // Rotation about X in degrees — the A axis of a typical 4th-axis mill. 0
  // for every 3-axis job, and nothing emits it yet.
  double rotationA = 0.0;

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

  [[nodiscard]] bool Empty() const { return moves.empty(); }
  // Total distance travelled while cutting, in mm — the cheap sanity check a
  // test can assert against a known contour.
  [[nodiscard]] double CutLength() const;
  // Lowest Z reached. A toolpath that dives below the stock bottom is a bug
  // worth catching in a test rather than in aluminium.
  [[nodiscard]] double MinZ() const;
};

} // namespace cam
