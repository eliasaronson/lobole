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

} // namespace parts
