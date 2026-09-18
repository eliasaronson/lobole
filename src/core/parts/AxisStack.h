#pragma once

#include "core/Feature.h" // Body

#include <glm/glm.hpp>

#include <string>
#include <vector>

// What an axis passes through (docs/parts/plan.md, phase 3).
//
// A fastener's length is not typed, it is derived — from the stack the hole
// runs through. That means asking a question no other part of this codebase
// asks: starting at a hole's rim and travelling down its axis, what solids do
// I enter and leave, and where?
//
// The answer is deliberately a DECOMPOSITION rather than a single number,
// because the awkward cases are the point. A gap between two plates is either
// an unmodelled gasket or the floating mount plate that cost a dogfooding
// round, and nothing in the geometry distinguishes them — so the caller is
// handed both plates and the gap between them and says so in its summary,
// rather than quietly adding 3 mm to a screw. Reporting the shape of the stack
// is what makes a mistake visible; guessing at it is what hides one.
namespace parts {

// One solid the axis passes through, in ray order. `enter` and `exit` are
// distances along the ray from its origin, in millimetres.
struct StackSegment {
  std::string part; // display name, for the summary
  uint64_t partId = 0;
  // Which PLACEMENT of that part, so a caller can attach something to the one
  // it actually passed through. `partId` cannot answer that: a stack of two
  // identical plates is one part twice.
  uint64_t occurrence = 0;
  double enter = 0.0;
  double exit = 0.0;
  [[nodiscard]] double thickness() const { return exit - enter; }
};

// One placed body to traverse: its shape and where the assembly puts it.
struct PlacedBody {
  const Body* body = nullptr;
  glm::mat4 world{1.0f};
  std::string part;
  uint64_t partId = 0;
  uint64_t occurrence = 0;
};

// Every solid the fastener passes through, ordered by distance from `origin`
// along `direction`, for at most `maxDist`.
//
// `holeRadius` is the crux, and the first version of this got it wrong. A ray
// straight down the hole's axis hits NOTHING — the hole is a void, and every
// plate the screw goes through has one on that line, by construction. The
// material a screw spans is beside the hole, not on it.
//
// So the probe is a small BUNDLE of rays parallel to the axis, offset just
// outside the bore wall, and each body's segment is the union of what they
// find. Four of them rather than one, because a hole near a part's edge or
// through a boss can have material on some sides and not others, and a single
// offset ray would report a plate as absent because it happened to point at
// the thin side.
//
// A plate whose hole does NOT line up still shows up, at its full thickness —
// the bundle finds material where the bore should have been. That is what lets
// the caller say "the hole in Bracket is off this axis" rather than silently
// producing a bolt through solid metal.
//
// The rays are transformed into each body's local frame rather than each body
// into world space: the placements are rigid, so the distances are the same
// either way and this copies no geometry.
[[nodiscard]] std::vector<StackSegment>
TraverseAxis(const std::vector<PlacedBody>& placed, const glm::dvec3& origin,
             const glm::dvec3& direction, double holeRadius, double maxDist);

// WHERE THE JOINT ENDS — the one answer both deriving a fastener and checking
// one have to agree on.
//
// They did not. `fit_fastener` stopped at the floor of a blind hole, which is
// what a grip is; `check_fasteners` re-derived from the probed stack alone and
// counted the material BELOW the hole as more joint. An 80 mm block with a 25
// mm blind hole under a 20 mm plate came back as a 45 mm grip from one and an
// 80 mm stack from the other, with nothing happening in between, so every
// blind-hole joint in a model read as too short by exactly the material under
// its hole (docs/agent-scale-followup-plan.md, W1).
//
// Both callers already hold both traversals; only one of them was using the
// second. Taking the answer from here means they agree by construction rather
// than by two pieces of code being written carefully.
//
// `stack` is the PROBED traversal (the bundle offset outside the bore: plate
// thicknesses). `onAxis` is the traversal straight down the axis, which finds
// only what actually STOPS a screw — a blind hole's floor, or a plate whose
// hole does not line up. `truncate` is a caller's own bound (a far-end pick,
// or the far face of the joint's own nut), or 0 for none.
// `threadEnd` is where the THREAD a screw of this size engages comes to an
// end, or 0 for none: a screw in a tapped hole is held by that thread, and
// what the axis meets past it belongs to somebody else. Without it a tapped
// BOSS — a bore that runs out into open air with the far wall of the part
// beyond it — re-derived as a much longer joint ending in that wall, and a
// correct screw was reported as having no thread in a part it does not touch
// (docs/grill-friction-plan.md).
struct JointExtent {
  double end = 0.0;   // how far down the axis the joint reaches, in mm
  bool blind = false; // it stopped ON material, not in daylight
};
[[nodiscard]] JointExtent JointEnd(const std::vector<StackSegment>& stack,
                                   const std::vector<StackSegment>& onAxis,
                                   double truncate, double threadEnd = 0.0);

// Where the first thread of `designation` on this axis ENDS, measured from
// `origin` along `dir`, or 0 when the axis enters none. Internal threads
// only: an external one is the screw's own.
[[nodiscard]] double ThreadEndAlong(const std::vector<PlacedBody>& placed,
                                    const glm::dvec3& origin,
                                    const glm::dvec3& dir,
                                    const std::string& designation);

// The segment a distance down the axis lands in, or null when it falls in a
// gap between two of them (which is a real answer: something solid sits on
// the axis clear of every plate the hole passes through).
[[nodiscard]] const StackSegment*
SegmentAt(const std::vector<StackSegment>& stack, double distance);

} // namespace parts
