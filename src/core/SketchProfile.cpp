#include "core/SketchProfile.h"

#include "Logger.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <GC_MakeSegment.hxx>
#include <GeomAbs_CurveType.hxx>
#include <ShapeFix_Wire.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Elips.hxx>
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

// A point on a circle at UV angle `ang`.
glm::vec2 OnCircle(const glm::vec2& c, double r, double ang) {
  return {c.x + static_cast<float>(r * std::cos(ang)),
          c.y + static_cast<float>(r * std::sin(ang))};
}

// Tessellates one loop edge into UV points in traversal order (a → b). A line
// is its two endpoints; an arc is sampled along its CCW span, then oriented so
// the first point matches `a`.
std::vector<glm::vec2> TessellateLoopEdge(const LoopEdge& e) {
  if (!e.arc)
    return {e.a, e.b};
  const double sweep = e.angEnd - e.angStart; // normalized positive
  int segs = std::max(1, static_cast<int>(std::lround(kCircleSegments * sweep /
                                                      (2.0 * M_PI))));
  std::vector<glm::vec2> pts;
  pts.reserve(segs + 1);
  for (int i = 0; i <= segs; i++)
    pts.push_back(OnCircle(e.center, e.radius,
                           e.angStart + sweep * i / segs)); // canonical CCW
  if (!pts.empty() && glm::length(pts.front() - e.a) >
                          glm::length(pts.back() - e.a))
    std::reverse(pts.begin(), pts.end()); // orient to traversal a → b
  return pts;
}
} // namespace

double Loop::SignedArea() const {
  if (circle) {
    return M_PI * circle->radius * circle->radius;
  }
  if (ellipse) {
    return M_PI * ellipse->rMajor * ellipse->rMinor;
  }
  // Concatenate the boundary as a polyline (drop each edge's duplicated start),
  // then shoelace. Arcs are tessellated so curved area is counted.
  std::vector<glm::vec2> poly;
  for (const auto& e : edges) {
    std::vector<glm::vec2> seg = TessellateLoopEdge(e);
    for (size_t i = (poly.empty() ? 0 : 1); i < seg.size(); i++)
      poly.push_back(seg[i]);
  }
  if (poly.size() >= 2 && Near(poly.front(), poly.back()))
    poly.pop_back();
  double area = 0.0;
  const size_t n = poly.size();
  for (size_t i = 0; i < n; i++) {
    const glm::vec2& p0 = poly[i];
    const glm::vec2& p1 = poly[(i + 1) % n];
    area += static_cast<double>(p0.x) * p1.y - static_cast<double>(p1.x) * p0.y;
  }
  return area * 0.5;
}

std::vector<Loop> BuildLoops(const std::vector<Entity>& entities) {
  std::vector<Loop> loops;

  // Collect chainable edges (lines + arcs); circles are loops already.
  std::vector<LoopEdge> freeEdges;
  for (const auto& entity : entities) {
    if (IsConstruction(entity))
      continue; // reference geometry: never contributes to a profile
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, Circle>) {
            Loop loop;
            loop.circle = e;
            loops.push_back(loop);
          } else if constexpr (std::is_same_v<T, Ellipse>) {
            Loop loop;
            loop.ellipse = e;
            loops.push_back(loop);
          } else if constexpr (std::is_same_v<T, Line>) {
            freeEdges.push_back({false, e.a, e.b});
          } else if constexpr (std::is_same_v<T, Arc>) {
            double s = e.startAngle, en = e.endAngle;
            while (en <= s)
              en += 2.0 * M_PI; // CCW-always
            freeEdges.push_back({true, ArcPointUV(e, 1), ArcPointUV(e, 2),
                                 e.center, e.radius, s, en});
          }
          // Point contributes nothing to a profile
        },
        entity);
  }

  // Chain edges into closed loops by endpoint proximity. Reversing an edge to
  // fit the walk only swaps its traversal endpoints; the arc's canonical span
  // is untouched, so its geometry survives.
  std::vector<bool> used(freeEdges.size(), false);
  for (size_t start = 0; start < freeEdges.size(); start++) {
    if (used[start])
      continue;

    std::vector<LoopEdge> chain = {freeEdges[start]};
    used[start] = true;
    glm::vec2 tail = chain.front().b;

    bool extended = true;
    while (extended) {
      extended = false;
      for (size_t i = 0; i < freeEdges.size(); i++) {
        if (used[i])
          continue;
        LoopEdge cand = freeEdges[i];
        if (Near(cand.a, tail)) {
          // already oriented a → b
        } else if (Near(cand.b, tail)) {
          std::swap(cand.a, cand.b); // traverse it backwards
        } else {
          continue;
        }
        tail = cand.b;
        chain.push_back(cand);
        used[i] = true;
        extended = true;
        break;
      }
    }

    // Closed if the walk returns to the first edge's start.
    if (chain.size() >= 2 && Near(chain.front().a, tail)) {
      Loop loop;
      loop.edges = std::move(chain);
      loops.push_back(loop);
    } else {
      LOG_WARN("Sketch profile: discarding open chain of {} edges",
               chain.size());
    }
  }

  return loops;
}

namespace {

// Builds an OCCT wire for one loop in world space on the given plane. Every
// wire is passed through ShapeFix_Wire (reorder / reconnect / close) so greedy
// edge order and small endpoint gaps — especially where arcs meet lines — are
// repaired before face building (FreeCAD Sketch::toShape does the same).
TopoDS_Wire MakeLoopWire(const SketchPlane& plane, const Loop& loop) {
  const glm::vec3 n3 = plane.GetNormal();
  const gp_Dir normal(n3.x, n3.y, n3.z);
  auto toPnt = [&](const glm::vec2& uv) {
    glm::vec3 w = plane.PlaneToWorld(uv);
    return gp_Pnt(w.x, w.y, w.z);
  };

  BRepBuilderAPI_MakeWire wire;
  if (loop.circle) {
    glm::vec3 u = plane.GetUAxis();
    gp_Ax2 axis(toPnt(loop.circle->center), normal, gp_Dir(u.x, u.y, u.z));
    gp_Circ circ(axis, loop.circle->radius);
    wire.Add(BRepBuilderAPI_MakeEdge(circ).Edge());
  } else if (loop.ellipse) {
    const Ellipse& el = *loop.ellipse;
    // Major-axis direction: +U rotated by `rotation` within the plane.
    glm::vec3 u = plane.GetUAxis(), v = plane.GetVAxis();
    const double c = std::cos(el.rotation), s = std::sin(el.rotation);
    glm::vec3 major = u * static_cast<float>(c) + v * static_cast<float>(s);
    gp_Ax2 axis(toPnt(el.center), normal, gp_Dir(major.x, major.y, major.z));
    gp_Elips ell(axis, el.rMajor, el.rMinor);
    wire.Add(BRepBuilderAPI_MakeEdge(ell).Edge());
  } else {
    for (const auto& e : loop.edges) {
      if (e.arc) {
        // Build the canonical CCW arc (angStart → angEnd) as a trimmed gp_Circ;
        // its in-loop orientation is fixed up by ShapeFix_Wire below.
        glm::vec3 u = plane.GetUAxis();
        gp_Ax2 axis(toPnt(e.center), normal, gp_Dir(u.x, u.y, u.z));
        gp_Circ circ(axis, e.radius);
        gp_Pnt s = toPnt(OnCircle(e.center, e.radius, e.angStart));
        gp_Pnt en = toPnt(OnCircle(e.center, e.radius, e.angEnd));
        wire.Add(BRepBuilderAPI_MakeEdge(circ, s, en).Edge());
      } else {
        Handle(Geom_TrimmedCurve) seg =
            GC_MakeSegment(toPnt(e.a), toPnt(e.b)).Value();
        wire.Add(BRepBuilderAPI_MakeEdge(seg).Edge());
      }
    }
  }
  if (!wire.IsDone())
    return TopoDS_Wire();

  ShapeFix_Wire fix;
  fix.Load(wire.Wire());
  fix.SetPrecision(1e-4);
  fix.FixReorder();
  fix.FixConnected();
  fix.FixClosed();
  return fix.Wire();
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
        } else if constexpr (std::is_same_v<T, Arc>) {
          double s = e.startAngle, en = e.endAngle;
          while (en <= s)
            en += 2.0 * M_PI; // CCW-always
          const double sweep = en - s;
          int segs = std::max(2, static_cast<int>(std::lround(
                                     kCircleSegments * sweep / (2.0 * M_PI))));
          points.reserve(segs + 1);
          for (int i = 0; i <= segs; i++) {
            double a = s + sweep * i / segs;
            points.push_back(
                {e.center.x + static_cast<float>(e.radius * std::cos(a)),
                 e.center.y + static_cast<float>(e.radius * std::sin(a))});
          }
        } else if constexpr (std::is_same_v<T, Ellipse>) {
          points.reserve(kCircleSegments + 1);
          for (int i = 0; i <= kCircleSegments; i++)
            points.push_back(
                EllipsePointUV(e, 2.0 * M_PI * i / kCircleSegments));
        }
        // Point: no polyline
      },
      entity);
  return points;
}

namespace {
// Converts one OCCT edge (line or circle) back to a plane-UV sketch entity.
std::optional<Entity> EdgeToSketchEntity(const TopoDS_Edge& edge,
                                         const SketchPlane& plane) {
  BRepAdaptor_Curve curve(edge);
  auto toUV = [&](const gp_Pnt& p) {
    return plane.WorldToPlane(glm::vec3(p.X(), p.Y(), p.Z()));
  };
  const double f = curve.FirstParameter(), l = curve.LastParameter();
  const glm::vec2 uv1 = toUV(curve.Value(f)), uv2 = toUV(curve.Value(l));
  if (curve.GetType() == GeomAbs_Line) {
    if (glm::length(uv2 - uv1) < 1e-7f)
      return std::nullopt; // degenerate
    return Line{uv1, uv2};
  }
  if (curve.GetType() == GeomAbs_Circle) {
    const gp_Circ circ = curve.Circle();
    const glm::vec2 c = toUV(circ.Location());
    const double r = circ.Radius();
    const glm::vec2 mid = toUV(curve.Value(0.5 * (f + l)));
    auto ang = [&](glm::vec2 p) {
      return std::atan2(p.y - c.y, p.x - c.x);
    };
    auto wrap = [](double x) {
      while (x < 0)
        x += 2.0 * M_PI;
      while (x >= 2.0 * M_PI)
        x -= 2.0 * M_PI;
      return x;
    };
    const double a1 = ang(uv1), a2 = ang(uv2), am = ang(mid);
    const double sweep = wrap(a2 - a1);       // CCW a1 → a2
    if (wrap(am - a1) <= sweep)               // midpoint lies on that span
      return Arc{c, r, a1, a1 + sweep};
    return Arc{c, r, a2, a2 + (2.0 * M_PI - sweep)}; // the other way (CCW)
  }
  return std::nullopt; // other curve types not represented in sketches
}
} // namespace

std::vector<Entity> OffsetLoop(const SketchPlane& plane,
                               const std::vector<Entity>& chain, double distance,
                               bool joinArc) {
  std::vector<Entity> out;
  if (std::abs(distance) < 1e-9)
    return out;
  auto loops = BuildLoops(chain);
  if (loops.size() != 1)
    return out; // need exactly one closed loop
  TopoDS_Wire wire = MakeLoopWire(plane, loops[0]);
  if (wire.IsNull())
    return out;
  try {
    BRepOffsetAPI_MakeOffset mk(
        wire, joinArc ? GeomAbs_Arc : GeomAbs_Intersection);
    mk.Perform(distance);
    if (!mk.IsDone())
      return out;
    for (TopExp_Explorer ex(mk.Shape(), TopAbs_EDGE); ex.More(); ex.Next())
      if (auto e = EdgeToSketchEntity(TopoDS::Edge(ex.Current()), plane))
        out.push_back(*e);
  } catch (const Standard_Failure& e) {
    LOG_ERROR("Sketch offset: OCCT error: {}", e.GetMessageString());
    out.clear();
  }
  return out;
}

std::optional<float> SegmentIntersect(glm::vec2 a1, glm::vec2 b1, glm::vec2 a2,
                                      glm::vec2 b2) {
  const glm::vec2 r = b1 - a1, s = b2 - a2;
  auto cross = [](glm::vec2 u, glm::vec2 v) { return u.x * v.y - u.y * v.x; };
  const float rxs = cross(r, s);
  if (std::abs(rxs) < 1e-9f)
    return std::nullopt; // parallel / collinear
  const glm::vec2 qp = a2 - a1;
  const float t = cross(qp, s) / rxs; // param along [a1,b1]
  const float u = cross(qp, r) / rxs; // param along [a2,b2]
  const float eps = 1e-6f;
  if (t < -eps || t > 1.0f + eps || u < -eps || u > 1.0f + eps)
    return std::nullopt;
  return glm::clamp(t, 0.0f, 1.0f);
}

} // namespace sketch
