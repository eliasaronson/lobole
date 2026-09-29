#pragma once

#include "core/cam/CamJob.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamVerify.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Cutting simulation — the stock as it stands PART WAY through the program
// (docs/cam/simulation.md).
//
// C5 already removes material exactly; it just runs the whole file at once and
// reports two numbers. This is the same removal with a cursor in front of it,
// so the block can be watched coming apart.
//
// One simulator, not two. FreeCAD ships a height-map verifier AND a separate
// depth-buffer CSG display that re-renders every swept segment each frame —
// the picture and the verdict there come from different engines and can
// disagree. Everything here runs the POSTED FILE through `CarveSweep`, the
// same function `Inspect` carves with, so the last frame of a playback IS the
// map the verdict was read from.
namespace cam {

// Where the cursor sits: part way through segment `seg`, which runs from
// `moves[seg].to` to `moves[seg+1].to`. Start is {0, 0}; the end is
// {Segments()-1, 1}.
struct SimCursor {
  size_t seg = 0;
  double t = 0.0; // fraction of that segment completed, [0,1]

  [[nodiscard]] bool operator<(const SimCursor& o) const {
    return seg != o.seg ? seg < o.seg : t < o.t;
  }
  [[nodiscard]] bool operator==(const SimCursor& o) const {
    return seg == o.seg && t == o.t;
  }
};

// The program laid out as something a slider can address.
//
// The axis is PATH DISTANCE, not wall-clock. Cutting time we can defend — it
// comes from each move's own feed, which the file states. A rapid traverse
// rate is a fact about the machine that the setup usually does not state, and
// inventing one would put a fabricated cycle time in front of the user. So
// distance drives the cursor (exact, and it needs no claim about anything),
// and time is a readout that says what it counted.
struct Timeline {
  GProgram program;
  // Cumulative, at the END of each segment. Parallel to the segment list, so
  // both are Segments() long.
  std::vector<double> distance; // mm travelled
  std::vector<double> minutes;  // cutting minutes, rapids only when stated
  double totalDistance = 0.0;
  double cuttingMinutes = 0.0;
  // Segments that begin a new operation / a new tool, in order. What "skip to
  // the next op" and "skip to the next tool" step between.
  std::vector<size_t> opStarts;
  std::vector<size_t> toolChanges;

  // Why this program cannot be simulated, when it cannot. Nothing sets it:
  // `BuildTimeline` accepts every program it parses, a rotary one included
  // (see `rotary` below), so it is always empty. The playback panel still
  // refuses to simulate when it is not.
  std::string unsupported;

  // The A axis moves in this program: the stock is a radius field about the
  // rotary axis, not a height map, and the playback turns the part.
  bool rotary = false;

  [[nodiscard]] size_t Segments() const {
    return program.moves.size() < 2 ? 0 : program.moves.size() - 1;
  }
  [[nodiscard]] bool Empty() const { return Segments() == 0; }
  [[nodiscard]] SimCursor End() const {
    return Empty() ? SimCursor{} : SimCursor{Segments() - 1, 1.0};
  }

  // Where the tool is, in SETUP space (the file's own frame).
  [[nodiscard]] glm::dvec3 PositionAt(SimCursor at) const;
  // The rotary axis at the cursor, degrees -- interpolated along a segment
  // exactly as the position is. Zero for a file that states no A.
  [[nodiscard]] double AngleAt(SimCursor at) const;
  // The move being executed at the cursor — the tool, the feed, the op, and
  // the G-code line to point a readout at. Null only when the timeline is
  // empty.
  [[nodiscard]] const GMove* MoveAt(SimCursor at) const;

  // The two directions of the slider.
  [[nodiscard]] double DistanceAt(SimCursor at) const;
  [[nodiscard]] SimCursor CursorAtDistance(double mm) const;
  [[nodiscard]] double MinutesAt(SimCursor at) const;

  // The next boundary at or after `at`, for the skip buttons. Returns End()
  // when there is none left.
  [[nodiscard]] SimCursor NextOp(SimCursor at) const;
  [[nodiscard]] SimCursor NextToolChange(SimCursor at) const;
};

// `rapidFeed` is the setup's: zero leaves rapids out of the clock entirely,
// which is what `CuttingMinutes` does and for the same reason.
//
// `model` is how the controller reads F on a move that turns while it cuts
// (CamGcode.h): the file's F on such a move was scaled for it, so the clock
// divides the controller's length by it. It changes nothing for a file whose
// A never moves in a cut.
[[nodiscard]] Timeline
BuildTimeline(GProgram program, double rapidFeed,
              RotaryFeedModel model = RotaryFeedModel::Combined);

// The stock at a cursor.
//
// Forward is incremental — a segment's swept bbox is a handful of cells.
// BACKWARD RESTARTS FROM THE STARTING BLOCK, because a height map is lossy: a
// cell that has been lowered cannot say what it used to be. That is a
// property of the model, not an oversight; if scrubbing a long program back
// ever measures too slow, the fix is to keep snapshots every N segments,
// which is a cache and not a redesign.
class StockSim {
public:
  // Arms the simulation over `pristine`, the stock this setup starts from
  // (`StartingStock`: the fresh block, less what the job's earlier setups
  // removed). The cursor starts at the beginning, with nothing of THIS
  // setup's program cut.
  void Begin(const Timeline& timeline, const Setup& setup, HeightMap pristine);
  void Clear();

  // Moves the cursor, carving (or re-carving) whatever lies between. Clamped
  // to the timeline.
  void SeekTo(SimCursor to);

  [[nodiscard]] bool Ready() const { return !m_pristine.z.empty(); }
  [[nodiscard]] const HeightMap& Stock() const { return m_stock; }
  [[nodiscard]] const HeightMap& Pristine() const { return m_pristine; }
  // How much material has come off at this cursor, mm^3. ONE number, from
  // the same grid everything else here reads, so a headless caller (or a
  // test) can assert that a simulation did something without owning a
  // renderer.
  [[nodiscard]] double RemovedVolume() const;
  [[nodiscard]] SimCursor At() const { return m_at; }
  // Bumped whenever the stock changed, so a mesher can tell "nothing moved"
  // from "the same cursor by coincidence" without comparing the whole grid.
  [[nodiscard]] uint64_t Revision() const { return m_revision; }

private:
  void Carve(SimCursor from, SimCursor to);

  const Timeline* m_timeline = nullptr;
  Setup m_setup; // copied: playback outlives an edit to the panel's setup
  HeightMap m_pristine;
  HeightMap m_stock;
  SimCursor m_at{0, 0.0};
  uint64_t m_revision = 0;
};

// The stock at a cursor, for a program whose part TURNS
// (docs/cam/rotary.md#the-rotary-picture).
//
// The twin of `StockSim` over a radius field, carving with the same
// `CarveSweepRotary` the verifier uses -- which is what makes the last frame
// of a rotary playback the stock `InspectRotary` reads its verdict from.
//
// One thing the planar class only anticipates, this one needs: SNAPSHOTS.
// Backward still cannot un-cut, and the planar header says what to do when
// replaying from the blank measures too slow. For the rolling pin it does --
// 644,160 moves at 65 us is a 42-second replay from the end. A snapshot is
// kept every `KeyframeEvery()` segments on the way forward, and a backward
// seek restarts from the nearest one behind it.
class RotaryStockSim {
public:
  void Begin(const Timeline& timeline, const Setup& setup, RadiusMap pristine);
  void Clear();
  void SeekTo(SimCursor to);

  [[nodiscard]] bool Ready() const { return !m_pristine.r.empty(); }
  [[nodiscard]] const RadiusMap& Stock() const { return m_stock; }
  [[nodiscard]] const RadiusMap& Pristine() const { return m_pristine; }
  [[nodiscard]] SimCursor At() const { return m_at; }
  [[nodiscard]] uint64_t Revision() const { return m_revision; }
  // mm^3 off, from the same grid: a cell's area is its axial size by its arc,
  // and the volume a cell lost is the annulus between the two radii.
  [[nodiscard]] double RemovedVolume() const;
  // How many segments between snapshots. DERIVED from a memory budget and the
  // grid's size, not typed: a literal count is right for one part and either
  // wasteful or useless for every other.
  [[nodiscard]] size_t KeyframeEvery() const { return m_every; }
  [[nodiscard]] size_t Keyframes() const { return m_keys.size(); }

private:
  void Carve(SimCursor from, SimCursor to);

  const Timeline* m_timeline = nullptr;
  Setup m_setup;
  RadiusMap m_pristine;
  RadiusMap m_stock;
  SimCursor m_at{0, 0.0};
  uint64_t m_revision = 0;
  size_t m_every = 0;
  // m_keys[k] is the stock with every segment before k * m_every carved.
  std::vector<std::vector<float>> m_keys;
};

} // namespace cam
