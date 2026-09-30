#include "core/parts/BearingSeat.h"

#include "core/Feature.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Torus.hxx>

#include <algorithm>
#include <cmath>
#include <limits>

namespace parts {

namespace {

glm::dvec3 V(const gp_XYZ& p) { return {p.X(), p.Y(), p.Z()}; }

// The ElementMap index conventions (faces in explorer order, edges in
// MapShapes order) — Measure's, restated because its resolver is private.
struct Resolved {
  const Body* body = nullptr;
  TopoDS_Shape shape;
  bool face = false;
};
Resolved Resolve(const std::vector<Body>& bodies, const ElementRef& ref) {
  Resolved out;
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || body.reference)
      continue;
    const TopoDS_Shape& whole = body.shape.GetShape();
    if (whole.IsNull())
      continue;
    if (const auto index = body.elementMap.Resolve(ref.name)) {
      int i = 0;
      for (TopExp_Explorer ex(whole, TopAbs_FACE); ex.More(); ex.Next(), i++)
        if (i == *index) {
          out = {&body, ex.Current(), true};
          return out;
        }
    }
    if (const auto index = body.elementMap.ResolveEdge(ref.name)) {
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(whole, TopAbs_EDGE, edges);
      if (*index >= 0 && *index < edges.Extent()) {
        out = {&body, edges(*index + 1), false};
        return out;
      }
    }
  }
  return out;
}

// The name a face of `body` carries, for handing back as a reference.
ElementRef NameOf(const Body& body, const TopoDS_Shape& face) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++)
    if (ex.Current().IsSame(face)) {
      if (const std::string* n = body.elementMap.NameOf(i))
        return {body.createdBy, *n};
      break;
    }
  return {};
}

struct Axis {
  glm::dvec3 p, d;
  double T(const glm::dvec3& x) const { return glm::dot(x - p, d); }
  double Radial(const glm::dvec3& x) const {
    const glm::dvec3 v = x - p;
    return glm::length(v - glm::dot(v, d) * d);
  }
};

bool Coaxial(const Axis& a, const gp_Ax1& other) {
  const glm::dvec3 d = V(other.Direction().XYZ());
  return std::abs(std::abs(glm::dot(d, a.d)) - 1.0) < 1e-6 &&
         a.Radial(V(other.Location().XYZ())) < 1e-4;
}

// The radial reach of a face: its nearest and farthest points from the axis,
// sampled along its boundary.
std::pair<double, double> RadialExtent(const TopoDS_Face& face,
                                       const Axis& axis) {
  double lo = std::numeric_limits<double>::max(), hi = 0.0;
  for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
    const BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
    constexpr int kSamples = 16;
    for (int i = 0; i <= kSamples; i++) {
      const double u = c.FirstParameter() +
                       (c.LastParameter() - c.FirstParameter()) * i / kSamples;
      const double r = axis.Radial(V(c.Value(u).XYZ()));
      lo = std::min(lo, r);
      hi = std::max(hi, r);
    }
  }
  return {lo, hi};
}

// A plane square to the axis, and where along it.
std::optional<double> SquarePlaneAt(const TopoDS_Face& face, const Axis& axis) {
  const BRepAdaptor_Surface s(face);
  if (s.GetType() != GeomAbs_Plane)
    return std::nullopt;
  const gp_Pln pln = s.Plane();
  const glm::dvec3 n = V(pln.Axis().Direction().XYZ());
  if (std::abs(std::abs(glm::dot(n, axis.d)) - 1.0) > 1e-6)
    return std::nullopt;
  return axis.T(V(pln.Location().XYZ()));
}

// Whether a plane at a seat's end is a shoulder, and how far it reaches: on
// a shaft it must stand OUT past the journal, in a housing IN past the bore.
// A shaft's own end face reaches inward and is no shoulder.
bool ShoulderReach(const TopoDS_Face& plane, const Axis& axis, bool shaft,
                   double radius, double& diameter) {
  const auto [lo, hi] = RadialExtent(plane, axis);
  if (shaft && hi > radius + 1e-6) {
    diameter = 2.0 * hi;
    return true;
  }
  if (!shaft && lo < radius - 1e-6) {
    diameter = 2.0 * lo;
    return true;
  }
  return false;
}

} // namespace

std::optional<Seat> SeatOf(const std::vector<Body>& bodies,
                           const ElementRef& pick, std::string* why) {
  const auto fail = [&](const std::string& message) -> std::optional<Seat> {
    if (why)
      *why = message;
    return std::nullopt;
  };
  const Resolved r = Resolve(bodies, pick);
  if (r.body == nullptr || r.shape.IsNull())
    return fail("\"" + pick.name +
                "\" does not resolve on that part — pick the seat again");

  try {
    const TopoDS_Shape& whole = r.body->shape.GetShape();
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(whole, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    const auto facesOf = [&](const TopoDS_Shape& edge) {
      std::vector<TopoDS_Face> out;
      const int i = edgeFaces.FindIndex(edge);
      if (i == 0)
        return out;
      for (const TopoDS_Shape& f : edgeFaces(i))
        if (std::none_of(out.begin(), out.end(),
                         [&](const TopoDS_Face& g) { return g.IsSame(f); }))
          out.push_back(TopoDS::Face(f));
      return out;
    };

    // The seat face, and — for a rim — the end it names.
    TopoDS_Face seatFace;
    std::optional<double> pickedEndRadius;
    TopoDS_Edge pickedEdge;
    if (r.face) {
      seatFace = TopoDS::Face(r.shape);
      if (BRepAdaptor_Surface(seatFace).GetType() != GeomAbs_Cylinder)
        return fail("a bearing seats on a ROUND face — a shaft journal or a "
                    "housing bore. Pick the cylinder, or the rim where it "
                    "meets the shoulder.");
    } else {
      pickedEdge = TopoDS::Edge(r.shape);
      const BRepAdaptor_Curve c(pickedEdge);
      if (c.GetType() != GeomAbs_Circle)
        return fail("that edge is not round — pick the rim where the seat "
                    "meets its shoulder, or the seat face itself");
      const gp_Circ circ = c.Circle();
      for (const TopoDS_Face& f : facesOf(pickedEdge)) {
        const BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Cylinder)
          continue;
        const Axis ax{V(circ.Axis().Location().XYZ()),
                      V(circ.Axis().Direction().XYZ())};
        if (std::abs(s.Cylinder().Radius() - circ.Radius()) < 1e-6 &&
            Coaxial(ax, s.Cylinder().Axis())) {
          seatFace = f;
          break;
        }
      }
      if (seatFace.IsNull())
        return fail("that rim is not the end of a round seat — no cylinder "
                    "of its diameter meets it. Pick the rim where the "
                    "journal (or the bore) meets its shoulder, not the "
                    "shoulder's outer edge.");
      pickedEndRadius = circ.Radius();
    }

    const BRepAdaptor_Surface surface(seatFace);
    const gp_Cylinder cyl = surface.Cylinder();
    const double radius = cyl.Radius();
    Axis axis{V(cyl.Axis().Location().XYZ()),
              glm::normalize(V(cyl.Axis().Direction().XYZ()))};

    Seat seat;
    seat.face = NameOf(*r.body, seatFace);
    seat.diameter = 2.0 * radius;
    seat.axisPoint = axis.p;
    seat.axisDir = axis.d;

    // Convex or concave, from the face's own outward normal: material inside
    // the cylinder is a shaft.
    {
      const double u =
          0.5 * (surface.FirstUParameter() + surface.LastUParameter());
      const double v =
          0.5 * (surface.FirstVParameter() + surface.LastVParameter());
      BRepLProp_SLProps props(surface, u, v, 1, 1e-7);
      if (!props.IsNormalDefined())
        return fail("could not tell which side of that seat is material");
      glm::dvec3 n = V(props.Normal().XYZ());
      if (seatFace.Orientation() == TopAbs_REVERSED)
        n = -n;
      const glm::dvec3 pnt = V(props.Value().XYZ());
      const glm::dvec3 rv =
          pnt - axis.p - glm::dot(pnt - axis.p, axis.d) * axis.d;
      seat.shaft = glm::dot(n, rv) > 0.0;
    }

    // The seat's two ends, as the circular edges of its own radius.
    struct End {
      double t;
      TopoDS_Edge edge;
    };
    std::vector<End> ends;
    for (TopExp_Explorer ex(seatFace, TopAbs_EDGE); ex.More(); ex.Next()) {
      const TopoDS_Edge e = TopoDS::Edge(ex.Current());
      const BRepAdaptor_Curve c(e);
      if (c.GetType() != GeomAbs_Circle ||
          std::abs(c.Circle().Radius() - radius) > 1e-6)
        continue;
      const double t = axis.T(V(c.Circle().Location().XYZ()));
      if (std::none_of(ends.begin(), ends.end(),
                       [&](const End& x) { return std::abs(x.t - t) < 1e-6; }))
        ends.push_back({t, e});
    }
    if (ends.empty())
      return fail("that seat has no round end to sit a bearing against");
    double tMin = ends.front().t, tMax = ends.front().t;
    for (const End& e : ends) {
      tMin = std::min(tMin, e.t);
      tMax = std::max(tMax, e.t);
    }

    for (const End& end : ends) {
      if (pickedEndRadius) {
        const BRepAdaptor_Curve pc(pickedEdge);
        const double t = axis.T(V(pc.Circle().Location().XYZ()));
        if (std::abs(t - end.t) > 1e-6)
          continue;
      }
      SeatEnd out;
      const double otherEnd = std::abs(end.t - tMin) < 1e-6 ? tMax : tMin;
      const double sign = otherEnd > end.t ? 1.0 : -1.0;
      out.along = axis.d * sign;
      double tOrigin = end.t;
      // What lies past this end: a shoulder plane, a fillet into one, or
      // nothing that stops a bearing (a chamfer, the shaft's end face).
      for (const TopoDS_Face& next : facesOf(end.edge)) {
        if (next.IsSame(seatFace))
          continue;
        const BRepAdaptor_Surface ns(next);
        if (const auto at = SquarePlaneAt(next, axis)) {
          double dia = 0.0;
          if (ShoulderReach(next, axis, seat.shaft, radius, dia)) {
            out.shoulder = true;
            out.shoulderDiameter = dia;
            out.shoulderFace = NameOf(*r.body, next);
            tOrigin = *at;
          }
        } else if (ns.GetType() == GeomAbs_Torus &&
                   Coaxial(axis, ns.Torus().Axis())) {
          // A fillet: the shoulder is the plane on its far side.
          const double rf = ns.Torus().MinorRadius();
          for (TopExp_Explorer fe(next, TopAbs_EDGE); fe.More(); fe.Next()) {
            for (const TopoDS_Face& beyond : facesOf(fe.Current())) {
              if (beyond.IsSame(next) || beyond.IsSame(seatFace))
                continue;
              const auto at = SquarePlaneAt(beyond, axis);
              double dia = 0.0;
              if (at && ShoulderReach(beyond, axis, seat.shaft, radius, dia)) {
                out.shoulder = true;
                out.shoulderDiameter = dia;
                out.shoulderFace = NameOf(*r.body, beyond);
                out.filletRadius = rf;
                tOrigin = *at;
              }
            }
          }
        }
        if (out.shoulder)
          break;
      }
      out.origin = axis.p + axis.d * tOrigin;
      out.length = std::abs(otherEnd - tOrigin);
      seat.ends.push_back(out);
    }
    if (seat.ends.empty())
      return fail("that rim is not an end of its seat");
    return seat;
  } catch (const Standard_Failure& failure) {
    return fail(std::string("could not read that seat (") +
                failure.GetMessageString() + ")");
  }
}

} // namespace parts
