#pragma once

#include "core/ElementMap.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

struct Body;

// What a round seat IS, read off the part it is modelled on
// (docs/parts/README.md#bearings): a shaft journal or a housing bore, its
// diameter, and at each end whether a shoulder stops a bearing there, how
// far that shoulder reaches and what fillet runs into it.
//
// One reading for two jobs, so they cannot disagree: `fit_bearing` places a
// bearing against the shoulder it finds, and `check_seat` judges that same
// shoulder against the bearing's chamfer. Everything is in the PART's own
// coordinates; the caller places it.
namespace parts {

struct SeatEnd {
  // On the axis, in the plane a bearing's side face sits in: the shoulder
  // plane (past any fillet), or the end of the seat when nothing stops it.
  glm::dvec3 origin{0.0};
  // Unit, from that plane onto the seat — the way a bearing slides on.
  glm::dvec3 along{0.0, 0.0, 1.0};
  double length = 0.0; // how far the seat runs from `origin` along `along`
  bool shoulder = false;
  ElementRef shoulderFace;
  // The shoulder's reach: its OUTER diameter on a shaft, its INNER diameter
  // in a housing — the diameter a ring's side face bears against.
  double shoulderDiameter = 0.0;
  // The fillet between the seat and the shoulder; 0 for a sharp corner.
  double filletRadius = 0.0;
};

struct Seat {
  ElementRef face;
  bool shaft = false; // convex: a bearing's BORE goes on it
  double diameter = 0.0;
  glm::dvec3 axisPoint{0.0}, axisDir{0.0, 0.0, 1.0};
  // Both ends of a picked face; only the picked end of a picked rim.
  std::vector<SeatEnd> ends;
};

// A cylindrical face, or a circular edge on one (the rim at the end a bearing
// sits against). Refused, with `why`, for anything else.
[[nodiscard]] std::optional<Seat> SeatOf(const std::vector<Body>& bodies,
                                         const ElementRef& pick,
                                         std::string* why);

// Every named cylindrical face of `bodies` on the axis through `axisPoint`
// along `axisDir` (the part's own coordinates): the candidates for a ring's
// seat that no mate names. check_seat reads a bearing's second ring through
// these, because a one-solid bearing in a mechanism is mated to ONE seat
// (fastening it to both would lock the shaft to the housing).
[[nodiscard]] std::vector<ElementRef>
CoaxialCylinders(const std::vector<Body>& bodies, const glm::dvec3& axisPoint,
                 const glm::dvec3& axisDir);

} // namespace parts
