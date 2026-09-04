#include "core/parts/AxisStack.h"

#include "cad/CADKernel.h"

#include <BRepIntCurveSurface_Inter.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <glm/gtc/matrix_inverse.hpp>

namespace parts {

namespace {

// Where the ray crosses one body's faces, as distances along it, sorted.
//
// The intersector reports every face crossing, and a solid is entered and left
// in pairs — so the crossings for a plate with a hole THE RAY MISSES are two,
// and for a plate whose hole the ray goes down are zero. That is the property
// the caller reads a misaligned hole out of, and it is why this returns the
// crossings rather than a yes/no.
std::vector<double> Crossings(const TopoDS_Shape& shape, const gp_Pnt& origin,
                              const gp_Dir& direction, double maxDist) {
  std::vector<double> out;
  BRepIntCurveSurface_Inter inter;
  // 1e-6 is the tolerance every other geometric comparison in this codebase
  // uses for millimetres; a coarser one merges a thin washer's two faces.
  inter.Init(shape, gp_Lin(origin, direction), 1.0e-6);
  for (; inter.More(); inter.Next()) {
    const double w = inter.W();
    if (w < -1.0e-6 || w > maxDist)
      continue; // behind the rim, or past where we agreed to look
    out.push_back(w);
  }
  std::sort(out.begin(), out.end());
  // A tangent graze reports the same parameter twice and is not an entry: a
  // screw does not pass through the edge of a plate it merely touches.
  out.erase(
      std::unique(out.begin(), out.end(),
                  [](double a, double b) { return std::abs(a - b) < 1.0e-6; }),
      out.end());
  return out;
}

} // namespace

std::vector<StackSegment> TraverseAxis(const std::vector<PlacedBody>& placed,
                                       const glm::dvec3& origin,
                                       const glm::dvec3& direction,
                                       double holeRadius, double maxDist) {
  std::vector<StackSegment> out;
  const double len = glm::length(direction);
  if (len < 1e-12 || maxDist <= 0.0)
    return out;
  const glm::dvec3 dir = direction / len;

  // Four origins just outside the bore wall. Offset rather than on the axis
  // because the axis runs down the hole, where there is nothing to hit; four
  // rather than one because a hole near an edge has material on some sides
  // and not others (see the header).
  glm::dvec3 u =
      glm::abs(dir.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
  u = glm::normalize(glm::cross(dir, u));
  const glm::dvec3 v = glm::normalize(glm::cross(dir, u));
  const double offset = holeRadius > 1e-9 ? holeRadius * 1.05 + 0.05 : 0.0;
  const std::vector<glm::dvec3> origins = {
      origin + u * offset, origin - u * offset, origin + v * offset,
      origin - v * offset};

  for (const PlacedBody& p : placed) {
    if (p.body == nullptr || !p.body->shape.IsValid())
      continue;
    // The ray into the body's own frame, not the body into the world: these
    // placements are rigid, so the distances along the ray are identical and
    // this copies no geometry.
    const glm::dmat4 toLocal = glm::inverse(glm::dmat4(p.world));
    const glm::dvec3 localDir =
        glm::normalize(glm::dvec3(toLocal * glm::dvec4(dir, 0.0)));

    // The union of what the bundle finds: a body is entered where the FIRST
    // ray enters it and left where the LAST leaves, so a counterbore or a
    // chamfer on one side does not shorten the plate.
    double enter = 0.0, exit = 0.0;
    bool hit = false;
    for (const glm::dvec3& rayOrigin : origins) {
      const glm::dvec3 localOrigin =
          glm::dvec3(toLocal * glm::dvec4(rayOrigin, 1.0));
      const std::vector<double> hits =
          Crossings(p.body->shape.GetShape(),
                    gp_Pnt(localOrigin.x, localOrigin.y, localOrigin.z),
                    gp_Dir(localDir.x, localDir.y, localDir.z), maxDist);
      // Pairs: in, out, in, out. An odd count means this ray started INSIDE
      // the body, which a rim on a real hole cannot do; skip it rather than
      // invent an entry at zero.
      if (hits.size() < 2 || hits.size() % 2 != 0)
        continue;
      enter = hit ? std::min(enter, hits.front()) : hits.front();
      exit = hit ? std::max(exit, hits.back()) : hits.back();
      hit = true;
    }
    if (!hit || exit - enter <= 1.0e-6)
      continue;
    StackSegment seg;
    seg.part = p.part;
    seg.partId = p.partId;
    seg.occurrence = p.occurrence;
    seg.enter = enter;
    seg.exit = exit;
    out.push_back(seg);
  }
  std::sort(out.begin(), out.end(),
            [](const StackSegment& a, const StackSegment& b) {
              return a.enter < b.enter;
            });
  return out;
}

JointExtent JointEnd(const std::vector<StackSegment>& stack,
                     const std::vector<StackSegment>& onAxis, double truncate) {
  JointExtent e;
  // Anything on the axis stops the screw there, whatever the plates beside it
  // do. `onAxis` is already in ray order, so its first hit is the near one.
  e.blind = !onAxis.empty();
  if (e.blind) {
    e.end = onAxis.front().enter;
  } else {
    for (const StackSegment& seg : stack)
      e.end = std::max(e.end, seg.exit);
  }
  // A caller's own bound never reaches PAST where the axis stops: a far-end
  // pick or a nut says where the joint ends, and if the axis ended sooner
  // then the axis wins.
  if (truncate > 0.0 && truncate < e.end)
    e.end = truncate;
  return e;
}

const StackSegment* SegmentAt(const std::vector<StackSegment>& stack,
                              double distance) {
  // The LAST match, not the first, and the difference is not academic: where
  // two parts abut, the boundary distance is inside both of them to within
  // tolerance, and the one that matters is the DEEPER one — the part the axis
  // is entering, not the one it is leaving. Taking the first turned "the hole
  // in Skew is not lined up with this one" into "the axis runs into Off",
  // which sends a person to the wrong part (test_actions.cpp:1753).
  const StackSegment* found = nullptr;
  for (const StackSegment& seg : stack)
    if (distance >= seg.enter - 1e-6 && distance <= seg.exit + 1e-6)
      found = &seg;
  return found;
}

} // namespace parts
