#pragma once

#include "core/cam/CamJob.h" // Loop, Move, Operation, Tool, Setup

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

// Adaptive clearing (docs/cam/operations.md#pocket-and-adaptive).
//
// Kresimir Tusek's `Adaptive2d`, vendored at deps/libarea/ from FreeCAD
// b2da06bf. Concentric offsetting takes the full width of the cutter on the
// first ring of every pocket; adaptive keeps the ANGLE OF ENGAGEMENT roughly
// constant instead, which is what lets a small spindle take the whole depth
// in one pass. It is the one algorithm in this workstream nobody should
// reinvent.
//
// LICENCE BOUNDARY. libarea is LGPL-2.1-or-later and this application is not
// (today AGPL-3.0, and that is a decision worth keeping open). The LGPL code
// is therefore built as its own shared library — `designer_adaptive`, see
// CMakeLists — and reached only through this header, whose declarations
// carry no LGPL code into the caller. THIS FILE is the whole surface: nothing
// else in Lobole includes `Adaptive.hpp`, and nothing should.
//
// Its public interface makes that cheap, which is the reason this is
// vendoring rather than porting: `Execute()` speaks millimetre doubles in and
// (motion type, polyline) pairs out. The Clipper1 it uses internally never
// crosses the boundary.
namespace cam {

// A cleared region's toolpath, in the same Move vocabulary as everything
// else. `error` is non-empty when Adaptive2d gave up, in the words the panel
// prints — it reports seven distinct failures and a boolean would waste them.
struct AdaptiveResult {
  std::vector<Move> moves;
  std::string error;
  bool available = false; // false = this build has no adaptive clearing
};

// Clears `region` (outer loops plus island holes, in setup space, each with
// `hole` false and wound as Clipper winds them) from `fromZ` down to `z` —
// the helix entry is planned by the library, the descent to it is ours. A
// loop with `hole` TRUE is area already clear: air beyond the stock round a
// part cleared round (docs/cam/operations.md#clear-round), which is not
// cut. Reads exactly the inputs `AdaptiveKey` hashes and
// nothing else; `test_conventions` holds the two to the same list.
[[nodiscard]] AdaptiveResult
AdaptiveClear(const Setup& setup, const Operation& op, const Tool& tool,
              const std::vector<Loop>& region, double z, double fromZ);

[[nodiscard]] bool AdaptiveAvailable();

// --- Off the frame (docs/ui/long-operations.md#cam-toolpath-passes) ----------
//
// One pass of adaptive clearing costs seconds and libarea's `Execute` has no
// interruption points, so the pass is the unit of background work. What a
// pass needs is COPIED into a request — plain structs, no OCC shape, nothing
// that another thread could be mutating — and what it produced is keyed by a
// hash of exactly those inputs. A content key rather than "operation 7 of
// tab 4" is what makes the result safe to land on any tab, after any undo,
// and after the parameters have moved on (then it is simply an entry nobody
// reads).

// Everything one pass reads, and nothing it does not.
struct AdaptiveRequest {
  uint64_t key = 0;
  double safeZ = 0.0;     // the Setup fields a pass reads: safe Z...
  double rapidFeed = 0.0; // ...and the traverse rate its links run at
  Operation op;
  Tool tool;
  std::vector<Loop> region;
  double z = 0.0;
  double fromZ = 0.0;
};

// The key for one pass: a hash over the seven scalars and the loops. Kept
// beside the reads in CamAdaptive.cpp, and gated: every `setup.`/`op.`/
// `tool.` field that file reads must appear in this function, or a new read
// would silently miss the cache's invalidation — which is the one drift this
// design is exposed to and which is invisible in review.
[[nodiscard]] uint64_t AdaptiveKey(const Setup& setup, const Operation& op,
                                   const Tool& tool,
                                   const std::vector<Loop>& region, double z,
                                   double fromZ);

[[nodiscard]] AdaptiveRequest
MakeAdaptiveRequest(const Setup& setup, const Operation& op, const Tool& tool,
                    const std::vector<Loop>& region, double z, double fromZ);

// The same clearing, from a request — the form a worker thread calls.
[[nodiscard]] AdaptiveResult AdaptiveClear(const AdaptiveRequest& request);

// Results by key. Bounded by entries AND by moves (a baffle pocket is 26k
// moves; two hundred of those is memory a CAD app should not keep). Eviction
// is oldest-in first; `hits`/`misses` are for tests and the perf HUD.
class AdaptiveCache {
public:
  [[nodiscard]] const AdaptiveResult* Find(uint64_t key);
  void Put(uint64_t key, AdaptiveResult result);
  [[nodiscard]] bool Contains(uint64_t key) const;
  [[nodiscard]] size_t Size() const { return m_entries.size(); }
  void Clear();
  size_t hits = 0;
  size_t misses = 0;

private:
  static constexpr size_t kMaxEntries = 256;
  static constexpr size_t kMaxMoves = 2'000'000;
  std::unordered_map<uint64_t, AdaptiveResult> m_entries;
  std::deque<uint64_t> m_order; // insertion order, for eviction
  size_t m_moves = 0;
};

} // namespace cam
