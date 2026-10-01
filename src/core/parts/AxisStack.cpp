#include "core/parts/AxisStack.h"

#include "cad/CADKernel.h"
#include "core/Measure.h"

#include <BRepClass3d_SolidClassifier.hxx>

#include <BRepIntCurveSurface_Inter.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>
#include <optional>

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

  // The BOX first. `placed` is every body of every leaf in the assembly —
  // 300-odd on an engine — and a bolt's axis crosses three or four of them.
  // Without this each fit paid four ray casts AND a solid classifier for
  // every body in the model: fit_fastener was 747 ms a call, 21% of that
  // engine's whole build. Same prune, same reason, as the hover pick
  // (docs/rendering/README.md#picking). The bounds are memoized on CADShape,
  // and the ray is already in the body's local frame, so this is a slab test
  // against numbers already computed.
  const auto reaches = [&](const glm::dvec3& localOrigin,
                           const glm::dvec3& localDir, const glm::dvec3& lo,
                           const glm::dvec3& hi) {
    double tMin = 0.0, tMax = maxDist;
    for (int a = 0; a < 3; ++a) {
      const double d = localDir[a];
      if (std::abs(d) < 1e-12) {
        if (localOrigin[a] < lo[a] || localOrigin[a] > hi[a])
          return false;
        continue;
      }
      double t0 = (lo[a] - localOrigin[a]) / d;
      double t1 = (hi[a] - localOrigin[a]) / d;
      if (t0 > t1)
        std::swap(t0, t1);
      tMin = std::max(tMin, t0);
      tMax = std::min(tMax, t1);
      if (tMin > tMax)
        return false;
    }
    return true;
  };

  for (const PlacedBody& p : placed) {
    if (p.body == nullptr || !p.body->shape.IsValid())
      continue;
    // The ray into the body's own frame, not the body into the world: these
    // placements are rigid, so the distances along the ray are identical and
    // this copies no geometry.
    const glm::dmat4 toLocal = glm::inverse(p.world);
    const glm::dvec3 localDir =
        glm::normalize(glm::dvec3(toLocal * glm::dvec4(dir, 0.0)));

    // The union of what the bundle finds: a body is entered where the FIRST
    // ray enters it and left where the LAST leaves, so a counterbore or a
    // chamfer on one side does not shorten the plate.
    glm::dvec3 lo(0.0), hi(0.0);
    p.body->shape.GetBoundingBox(lo, hi);
    // A hair of slack: the bundle sits a hole-radius off the axis and a
    // conservative box is already generous, but a ray grazing a face at the
    // box boundary must not be pruned.
    lo -= glm::dvec3(1e-3);
    hi += glm::dvec3(1e-3);

    // Every interval of MATERIAL the bundle finds, not one span from the
    // first entry to the last exit. A folded part is crossed twice by one
    // axis — a cart's side panel has a flange under the firebox and another
    // 650 mm below it at the floor — and reporting that as one segment made
    // the whole cart read as solid between them: the nut on the top flange
    // was called "buried in Cart side left", and a check that cries wolf on
    // a correct model is worse than no check.
    std::vector<std::pair<double, double>> spans;
    // One classifier per BODY, not per ray: constructing it explores the
    // whole solid (BRepClass3d_SolidExplorer::InitShape was 11.5% of the
    // engine build), and the four rays classify against the same shape.
    std::optional<BRepClass3d_SolidClassifier> classifier;
    for (const glm::dvec3& rayOrigin : origins) {
      const glm::dvec3 localOrigin =
          glm::dvec3(toLocal * glm::dvec4(rayOrigin, 1.0));
      if (!reaches(localOrigin, localDir, lo, hi))
        continue; // this ray cannot meet the body at all
      const std::vector<double> hits =
          Crossings(p.body->shape.GetShape(),
                    gp_Pnt(localOrigin.x, localOrigin.y, localOrigin.z),
                    gp_Dir(localDir.x, localDir.y, localDir.z), maxDist);
      if (hits.empty())
        continue;
      // Tangencies and coincident face boundaries need not alternate in/out.
      // Classify the intervals instead of discarding an odd crossing count:
      // discarding one hid blind floors and selected screws through them.
      if (!classifier)
        classifier.emplace(p.body->shape.GetShape());
      double previous = 0.0;
      for (size_t i = 0; i <= hits.size(); ++i) {
        const double next = i < hits.size() ? hits[i] : maxDist;
        if (next - previous > 1e-6) {
          const glm::dvec3 mid =
              localOrigin + localDir * ((previous + next) / 2);
          classifier->Perform(gp_Pnt(mid.x, mid.y, mid.z), 1e-6);
          if (classifier->State() == TopAbs_IN)
            spans.emplace_back(previous, next);
        }
        previous = next;
      }
    }
    if (spans.empty())
      continue;
    // The four rays see the same plate four times, and a chamfer or a
    // counterbore on one side makes one of them start late: intervals that
    // overlap or abut are the same piece of material and merge, which is the
    // union the comment above asks for — bounded to that piece.
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<double, double>> merged;
    for (const auto& span : spans) {
      if (!merged.empty() && span.first <= merged.back().second + 1.0e-3)
        merged.back().second = std::max(merged.back().second, span.second);
      else
        merged.push_back(span);
    }
    for (const auto& span : merged) {
      if (span.second - span.first <= 1.0e-6)
        continue;
      StackSegment seg;
      seg.part = p.part;
      seg.partId = p.partId;
      seg.occurrence = p.occurrence;
      seg.enter = span.first;
      seg.exit = span.second;
      out.push_back(seg);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const StackSegment& a, const StackSegment& b) {
              return a.enter < b.enter;
            });
  return out;
}

double ThreadEndAlong(const std::vector<PlacedBody>& placed,
                      const glm::dvec3& origin, const glm::dvec3& dir,
                      const std::string& designation) {
  const double len = glm::length(dir);
  if (len < 1e-12)
    return 0.0;
  const glm::dvec3 axis = dir / len;
  double first = 0.0;
  for (const PlacedBody& p : placed) {
    if (p.body == nullptr)
      continue;
    for (const Measure::ThreadSpan& span :
         Measure::ThreadSpansOf(*p.body, p.world)) {
      if (span.external || span.spec.designation != designation)
        continue;
      // Coaxial with this screw, and on its line: a thread beside the axis
      // is a different hole in the same part.
      if (std::abs(std::abs(glm::dot(span.axis, axis)) - 1.0) > 1e-6)
        continue;
      const glm::dvec3 delta = span.start - origin;
      const double at = glm::dot(delta, axis);
      if (glm::length(delta - at * axis) > 1e-3)
        continue;
      const double other =
          at + (glm::dot(span.axis, axis) > 0 ? span.length : -span.length);
      const double end = std::max(at, other);
      if (end <= 1e-6)
        continue; // behind the head
      if (first <= 0.0 || end < first)
        first = end;
    }
  }
  return first;
}

JointExtent JointEnd(const std::vector<StackSegment>& stack,
                     const std::vector<StackSegment>& onAxis, double truncate,
                     double threadEnd, double maxSpan) {
  // A gap no screw of this size can span ends the joint before it: what lies
  // beyond is somebody else's part, however it lines up.
  if (maxSpan > 0.0) {
    double reached = -1.0;
    for (const StackSegment& seg : stack) {
      if (reached >= 0.0 && seg.enter - reached > maxSpan) {
        std::vector<StackSegment> nearStack, nearAxis;
        for (const StackSegment& s : stack)
          if (s.enter < reached + 1e-6)
            nearStack.push_back(s);
        for (const StackSegment& s : onAxis)
          if (s.enter < reached + 1e-6)
            nearAxis.push_back(s);
        return JointEnd(nearStack, nearAxis, truncate, threadEnd, 0.0);
      }
      reached = std::max(reached, seg.exit);
    }
  }
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
  // A THREAD ends the joint the way a nut does, and leaves it blind: what
  // holds a screw in a tapped hole is the thread, not something it reaches
  // through to. Applied before the caller's own bound, which may still be
  // nearer (a nut on a stud through a threaded boss).
  if (threadEnd > 0.0 && (!e.blind || threadEnd < e.end)) {
    e.end = e.blind ? std::min(e.end, threadEnd) : threadEnd;
    e.blind = true;
  }
  // A caller's own bound never reaches PAST where the axis stops: a far-end
  // pick or a nut says where the joint ends, and if the axis ended sooner
  // then the axis wins.
  //
  // And a joint that ends BEFORE whatever the axis runs into is not blind:
  // the far end is out in the open, so a nut can sit on it. Reading `blind`
  // off the axis alone refused the nut on every through-bolt whose line
  // happened to carry on into another part — an arm bolt with its nyloc on
  // the mid plate, refused because the top plate is 15 mm further up.
  //
  // AT the floor is not before it. Picking the bottom rim of a tapped hole is
  // how a caller says "the joint ends here", and that pick lands on the same
  // plane the on-axis ray found, to a rounding error. Read as a truncation it
  // turned a blind joint into a through one, and a through one is sized UP to
  // a stocked length: the grill's burner screws came back M5x14 in a 13.5 mm
  // hole, bottoming out on the floor and clamping nothing.
  if (truncate > 0.0 && truncate < e.end - 1e-3) {
    e.end = truncate;
    e.blind = false;
  }
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
