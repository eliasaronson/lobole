#include "core/SketchProfile.h"

#include "Logger.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GC_MakeSegment.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>

namespace sketch {

namespace {
constexpr double kChainTol = 1e-4;
constexpr int kCircleSegments = 32;

// True if two UV points are within the chaining tolerance.
bool Near(const glm::vec2& a, const glm::vec2& b) {
  return glm::length(a - b) < kChainTol;
}
} // namespace

double Loop::SignedArea() const {
  if (circle) {
    return M_PI * circle->radius * circle->radius;
  }
  double area = 0.0;
  const size_t n = polygon.size();
  for (size_t i = 0; i < n; i++) {
    const glm::vec2& p0 = polygon[i];
    const glm::vec2& p1 = polygon[(i + 1) % n];
    area += static_cast<double>(p0.x) * p1.y - static_cast<double>(p1.x) * p0.y;
  }
  return area * 0.5;
}

std::vector<Loop> BuildLoops(const std::vector<Entity>& entities) {
  std::vector<Loop> loops;

  // Collect free line segments to chain; circles are loops already.
  std::vector<std::pair<glm::vec2, glm::vec2>> segments;

  for (const auto& entity : entities) {
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, Circle>) {
            Loop loop;
            loop.circle = e;
            loops.push_back(loop);
          } else if constexpr (std::is_same_v<T, Line>) {
            segments.emplace_back(e.a, e.b);
          }
          // Point contributes nothing to a profile
        },
        entity);
  }

  // Chain remaining line segments into closed loops by endpoint proximity.
  std::vector<bool> used(segments.size(), false);
  for (size_t start = 0; start < segments.size(); start++) {
    if (used[start])
      continue;

    std::vector<glm::vec2> chain = {segments[start].first,
                                    segments[start].second};
    used[start] = true;
    glm::vec2 tail = segments[start].second;

    bool extended = true;
    while (extended) {
      extended = false;
      for (size_t i = 0; i < segments.size(); i++) {
        if (used[i])
          continue;
        if (Near(segments[i].first, tail)) {
          tail = segments[i].second;
        } else if (Near(segments[i].second, tail)) {
          tail = segments[i].first;
        } else {
          continue;
        }
        chain.push_back(tail);
        used[i] = true;
        extended = true;
        break;
      }
    }

    // Closed if the chain returns to its start
    if (chain.size() >= 4 && Near(chain.front(), chain.back())) {
      chain.pop_back(); // drop the duplicated closing point
      Loop loop;
      loop.polygon = std::move(chain);
      loops.push_back(loop);
    } else {
      LOG_WARN("Sketch profile: discarding open chain of {} points",
               chain.size());
    }
  }

  return loops;
}

namespace {

// Builds an OCCT wire for one loop in world space on the given plane.
TopoDS_Wire MakeLoopWire(const SketchPlane& plane, const Loop& loop) {
  auto toPnt = [&](const glm::vec2& uv) {
    glm::vec3 w = plane.PlaneToWorld(uv);
    return gp_Pnt(w.x, w.y, w.z);
  };

  if (loop.circle) {
    glm::vec3 c = plane.PlaneToWorld(loop.circle->center);
    glm::vec3 n = plane.GetNormal();
    glm::vec3 u = plane.GetUAxis();
    gp_Ax2 axis(gp_Pnt(c.x, c.y, c.z), gp_Dir(n.x, n.y, n.z),
                gp_Dir(u.x, u.y, u.z));
    gp_Circ circ(axis, loop.circle->radius);
    BRepBuilderAPI_MakeEdge edge(circ);
    BRepBuilderAPI_MakeWire wire(edge.Edge());
    return wire.Wire();
  }

  BRepBuilderAPI_MakeWire wire;
  const size_t n = loop.polygon.size();
  for (size_t i = 0; i < n; i++) {
    gp_Pnt a = toPnt(loop.polygon[i]);
    gp_Pnt b = toPnt(loop.polygon[(i + 1) % n]);
    Handle(Geom_TrimmedCurve) seg = GC_MakeSegment(a, b).Value();
    wire.Add(BRepBuilderAPI_MakeEdge(seg).Edge());
  }
  return wire.Wire();
}

} // namespace

CADShape BuildPrism(const SketchPlane& plane, const std::vector<Loop>& loops,
                    double distance, bool reversed) {
  if (loops.empty() || std::abs(distance) < 1e-9) {
    return CADShape();
  }

  try {
    // Largest-area loop is the outer boundary; the rest are holes.
    size_t outer = 0;
    double outerArea = std::abs(loops[0].SignedArea());
    for (size_t i = 1; i < loops.size(); i++) {
      double area = std::abs(loops[i].SignedArea());
      if (area > outerArea) {
        outerArea = area;
        outer = i;
      }
    }

    glm::vec3 o = plane.GetOrigin();
    glm::vec3 nrm = plane.GetNormal();
    glm::vec3 u = plane.GetUAxis();
    gp_Ax3 ax3(gp_Pnt(o.x, o.y, o.z), gp_Dir(nrm.x, nrm.y, nrm.z),
               gp_Dir(u.x, u.y, u.z));
    gp_Pln pln(ax3);

    BRepBuilderAPI_MakeFace faceMaker(pln, MakeLoopWire(plane, loops[outer]));
    for (size_t i = 0; i < loops.size(); i++) {
      if (i == outer)
        continue;
      TopoDS_Wire hole = MakeLoopWire(plane, loops[i]);
      faceMaker.Add(TopoDS::Wire(hole.Reversed()));
    }
    if (!faceMaker.IsDone()) {
      LOG_ERROR("Sketch profile: face construction failed");
      return CADShape();
    }

    const double d = reversed ? -distance : distance;
    gp_Vec extrusion(nrm.x * d, nrm.y * d, nrm.z * d);
    BRepPrimAPI_MakePrism prism(faceMaker.Face(), extrusion);
    if (!prism.IsDone()) {
      LOG_ERROR("Sketch profile: prism construction failed");
      return CADShape();
    }
    return CADShape(prism.Shape());
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Sketch profile: OCCT error: {}", e.GetMessageString());
    return CADShape();
  }
}

std::vector<glm::vec2> TessellateEntity(const Entity& entity) {
  std::vector<glm::vec2> points;
  std::visit(
      [&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, Line>) {
          points = {e.a, e.b};
        } else if constexpr (std::is_same_v<T, Circle>) {
          points.reserve(kCircleSegments + 1);
          for (int i = 0; i <= kCircleSegments; i++) {
            double a = 2.0 * M_PI * i / kCircleSegments;
            points.push_back(
                {e.center.x + static_cast<float>(e.radius * std::cos(a)),
                 e.center.y + static_cast<float>(e.radius * std::sin(a))});
          }
        }
        // Point: no polyline
      },
      entity);
  return points;
}

} // namespace sketch
