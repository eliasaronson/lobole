// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The measures a blend is sized against, at a picked edge and each of its
// two faces (Measure.h: DihedralAngleAt, WidthAcrossEdgeAt,
// ConcaveRadiusAt). core/BlendFit.cpp turns them into bounds;
// query_geometry answers them on their own.

#include "core/MeasureBlend.h"

#include <BndLib_Add2dCurve.hxx>
#include <Bnd_Box2d.hxx>

#include <BRepAdaptor_Surface.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <Geom2dAPI_InterCurveCurve.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom2d_Line.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir2d.hxx>
#include <gp_Lin2d.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <unordered_map>

namespace Measure {

namespace {

// The two faces either side of an edge, as the blend builder takes them;
// their indices are element-map face indices.
struct EdgeSides {
  TopoDS_Edge edge;
  TopoDS_Face face[2];
  int faceIndex[2] = {-1, -1};
};

// The edge as `face` holds it (its orientation in that face's wire), so the
// p-curve is the one of that face.
TopoDS_Edge EdgeInFace(const TopoDS_Edge& edge, const TopoDS_Face& face) {
  for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next())
    if (ex.Current().IsSame(edge))
      return TopoDS::Edge(ex.Current());
  return edge;
}

// Where the edge is, at fraction `t` of its parameter range, on `face`: the
// UV point, the UV direction pointing into the face square to the edge, the
// 3-D point, the outward normal, and the 3-D direction into the face.
struct Station {
  gp_Pnt2d uv;
  gp_Dir2d into; // UV
  gp_Pnt p;
  gp_Dir normal; // outward
  gp_Dir into3;  // tangent to the face, square to the edge, into it
};

// `inside` says whether a UV point is in the face (its own classifier).
using Inside = std::function<bool(const gp_Pnt2d&)>;

std::optional<Station> StationOn(const TopoDS_Edge& anyEdge,
                                 const TopoDS_Face& face,
                                 const BRepAdaptor_Surface& surf, double t,
                                 const Inside& inside) {
  const TopoDS_Edge edge = EdgeInFace(anyEdge, face);
  double f = 0.0, l = 0.0;
  const Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(edge, face, f, l);
  if (pc.IsNull())
    return std::nullopt;
  const double u = f + (l - f) * t;
  gp_Pnt2d uv;
  gp_Vec2d d1;
  pc->D1(u, uv, d1);
  if (d1.Magnitude() < 1e-12)
    return std::nullopt;
  BRepLProp_SLProps props(surf, uv.X(), uv.Y(), 1, 1e-9);
  if (!props.IsNormalDefined())
    return std::nullopt;
  Station s;
  s.uv = uv;
  s.p = props.Value();
  s.normal = props.Normal();
  if (face.Orientation() == TopAbs_REVERSED)
    s.normal.Reverse();
  // The edge's 3-D tangent, then the direction in the face square to it,
  // expressed back in UV through the first derivatives.
  const gp_Vec du = props.D1U(), dv = props.D1V();
  const gp_Vec tangent = du * d1.X() + dv * d1.Y();
  gp_Vec into3 = gp_Vec(s.normal).Crossed(tangent);
  if (into3.Magnitude() < 1e-12)
    return std::nullopt;
  // Solve du*a + dv*b = into3 in the least-squares sense.
  const double e = du.Dot(du), ff = du.Dot(dv), g = dv.Dot(dv);
  const double det = e * g - ff * ff;
  if (std::abs(det) < 1e-18)
    return std::nullopt;
  const double r1 = du.Dot(into3), r2 = dv.Dot(into3);
  gp_Vec2d into((g * r1 - ff * r2) / det, (e * r2 - ff * r1) / det);
  if (into.Magnitude() < 1e-12)
    return std::nullopt;
  // Which side the face is on: ask its own classifier a step inside, rather
  // than reason from the wire's and the face's orientations.
  const double uSpan = std::abs(surf.LastUParameter() - surf.FirstUParameter());
  const double vSpan = std::abs(surf.LastVParameter() - surf.FirstVParameter());
  double step =
      1e-4 * std::max(1.0, std::min(std::isfinite(uSpan) ? uSpan : 1.0,
                                    std::isfinite(vSpan) ? vSpan : 1.0));
  into.Normalize();
  bool decided = false;
  for (int tries = 0; tries < 3 && !decided; tries++, step *= 10.0) {
    const gp_Pnt2d ahead(uv.X() + into.X() * step, uv.Y() + into.Y() * step);
    const gp_Pnt2d behind(uv.X() - into.X() * step, uv.Y() - into.Y() * step);
    const bool inA = inside(ahead), inB = inside(behind);
    if (inA != inB) {
      if (inB) {
        into.Reverse();
        into3.Reverse();
      }
      decided = true;
    }
  }
  if (!decided)
    return std::nullopt;
  s.into = gp_Dir2d(into);
  s.into3 = gp_Dir(into3);
  return s;
}

constexpr double kStations[] = {0.1, 0.3, 0.5, 0.7, 0.9};

// One boundary curve of a face in its UV, with its box: a ray across a
// face with a hundred holes in it skips the rims it cannot reach.
struct Boundary {
  Handle(Geom2d_Curve) curve;
  int index = -1;
  Bnd_Box2d box;
};

// The distance along the ray (origin o, unit direction d) at which it
// enters `box`, or infinity when it misses it.
double RayEntersAt(const gp_Pnt2d& o, const gp_Dir2d& d, const Bnd_Box2d& box) {
  if (box.IsVoid())
    return std::numeric_limits<double>::infinity();
  double x0, y0, x1, y1;
  box.Get(x0, y0, x1, y1);
  double t0 = -std::numeric_limits<double>::infinity();
  double t1 = std::numeric_limits<double>::infinity();
  const double org[2] = {o.X(), o.Y()}, dir[2] = {d.X(), d.Y()};
  const double lo[2] = {x0, y0}, hi[2] = {x1, y1};
  for (int k = 0; k < 2; k++) {
    if (std::abs(dir[k]) < 1e-15) {
      if (org[k] < lo[k] || org[k] > hi[k])
        return std::numeric_limits<double>::infinity();
      continue;
    }
    double a = (lo[k] - org[k]) / dir[k], b = (hi[k] - org[k]) / dir[k];
    if (a > b)
      std::swap(a, b);
    t0 = std::max(t0, a);
    t1 = std::min(t1, b);
  }
  if (t1 < t0 || t1 < 0.0)
    return std::numeric_limits<double>::infinity();
  return std::max(t0, 0.0);
}

} // namespace

struct BlendProbe::Impl {
  TopoDS_Shape shape;
  TopTools_IndexedMapOfShape edges, faces;
  TopTools_IndexedDataMapOfShapeListOfShape parents;
  std::unordered_map<int, std::optional<EdgeSides>> sides;
  std::unordered_map<int, std::optional<double>> dihedral;
  std::unordered_map<int, std::unique_ptr<BRepAdaptor_Surface>> adaptors;
  std::unordered_map<int, std::vector<Boundary>> boundaries;
  std::unordered_map<int, std::unique_ptr<BRepTopAdaptor_FClass2d>> classes;

  // The face's own classifier, built once: a face with a hundred holes
  // costs a pass over every wire to build one.
  Inside InsideOf(int face) {
    auto& c = classes[face];
    if (!c)
      c = std::make_unique<BRepTopAdaptor_FClass2d>(
          TopoDS::Face(faces(face + 1)), 1e-9);
    BRepTopAdaptor_FClass2d* cls = c.get();
    return [cls](const gp_Pnt2d& p) { return cls->Perform(p) == TopAbs_IN; };
  }

  std::optional<Station> At(const EdgeSides& es, int k, double t) {
    return StationOn(es.edge, es.face[k], Adaptor(es.faceIndex[k]), t,
                     InsideOf(es.faceIndex[k]));
  }

  const BRepAdaptor_Surface& Adaptor(int face) {
    auto& a = adaptors[face];
    if (!a)
      a = std::make_unique<BRepAdaptor_Surface>(TopoDS::Face(faces(face + 1)));
    return *a;
  }

  const std::optional<EdgeSides>& Sides(int edgeIndex) {
    if (const auto it = sides.find(edgeIndex); it != sides.end())
      return it->second;
    auto& out = sides[edgeIndex];
    if (edgeIndex < 0 || edgeIndex + 1 > edges.Extent())
      return out;
    EdgeSides es;
    es.edge = TopoDS::Edge(edges(edgeIndex + 1));
    if (BRep_Tool::Degenerated(es.edge))
      return out;
    const int at = parents.FindIndex(es.edge);
    if (at == 0)
      return out;
    int n = 0;
    for (const TopoDS_Shape& f : parents(at)) {
      if (n == 1 && f.IsSame(es.face[0]))
        continue;
      if (n == 2)
        return out; // more than two faces: not a manifold edge
      es.face[n++] = TopoDS::Face(f);
    }
    if (n != 2)
      return out;
    for (int k = 0; k < 2; k++)
      es.faceIndex[k] = faces.FindIndex(es.face[k]) - 1;
    if (es.faceIndex[0] < 0 || es.faceIndex[1] < 0)
      return out;
    out = es;
    return out;
  }

  const std::vector<Boundary>& BoundaryOf(int face) {
    if (const auto it = boundaries.find(face); it != boundaries.end())
      return it->second;
    auto& out = boundaries[face];
    const TopoDS_Face& f = TopoDS::Face(faces(face + 1));
    for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next()) {
      const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
      if (BRep_Tool::Degenerated(e) || BRep_Tool::IsClosed(e, f))
        continue; // a seam is not where the face ends
      double a = 0.0, b = 0.0;
      const Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(e, f, a, b);
      if (pc.IsNull())
        continue;
      Boundary bd;
      bd.curve = new Geom2d_TrimmedCurve(pc, a, b);
      bd.index = edges.FindIndex(e) - 1;
      BndLib_Add2dCurve::Add(bd.curve, 1e-7, bd.box);
      out.push_back(std::move(bd));
    }
    return out;
  }

  // Which side of face k of `es` the edge is convex on.
  std::optional<bool> Convex(const EdgeSides& es, int k) {
    const auto me = At(es, k, 0.5);
    const auto other = At(es, 1 - k, 0.5);
    if (!me || !other)
      return std::nullopt;
    return me->into3.Dot(other->normal) < 0.0;
  }
};

BlendProbe::BlendProbe(const Body& body) : m(std::make_unique<Impl>()) {
  m->shape = body.shape.GetShape();
  if (m->shape.IsNull())
    return;
  TopExp::MapShapes(m->shape, TopAbs_EDGE, m->edges);
  TopExp::MapShapes(m->shape, TopAbs_FACE, m->faces);
  TopExp::MapShapesAndAncestors(m->shape, TopAbs_EDGE, TopAbs_FACE, m->parents);
}

BlendProbe::~BlendProbe() = default;

std::optional<std::array<int, 2>> BlendProbe::Faces(int edge) {
  try {
    const auto& es = m->Sides(edge);
    if (!es)
      return std::nullopt;
    return std::array<int, 2>{es->faceIndex[0], es->faceIndex[1]};
  } catch (const Standard_Failure&) {
    return std::nullopt;
  }
}

std::optional<double> BlendProbe::Dihedral(int edge) {
  if (const auto it = m->dihedral.find(edge); it != m->dihedral.end())
    return it->second;
  auto& out = m->dihedral[edge];
  try {
    const auto& es = m->Sides(edge);
    if (!es)
      return out;
    const auto a = m->At(*es, 0, 0.5);
    const auto b = m->At(*es, 1, 0.5);
    if (!a || !b)
      return out;
    const double between = a->normal.Angle(b->normal); // [0, pi]
    // Convex where face a runs away from the edge below face b's plane.
    const bool convex = a->into3.Dot(b->normal) < 0.0;
    out = convex ? std::numbers::pi - between : std::numbers::pi + between;
  } catch (const Standard_Failure&) {
    out.reset();
  }
  return out;
}

std::optional<WidthAcross> BlendProbe::Width(int edge, int faceIndex) {
  try {
    const auto& es = m->Sides(edge);
    if (!es)
      return std::nullopt;
    const int k = es->faceIndex[0] == faceIndex   ? 0
                  : es->faceIndex[1] == faceIndex ? 1
                                                  : -1;
    if (k < 0)
      return std::nullopt;
    const BRepAdaptor_Surface& surf = m->Adaptor(faceIndex);
    const GeomAbs_SurfaceType type = surf.GetType();
    if (type != GeomAbs_Plane && type != GeomAbs_Cylinder &&
        type != GeomAbs_Cone)
      return std::nullopt;
    const std::vector<Boundary>& bounds = m->BoundaryOf(faceIndex);
    std::optional<WidthAcross> least;
    for (const double t : kStations) {
      const auto st = m->At(*es, k, t);
      if (!st)
        continue;
      // Straight in 3-D only where the surface is: any direction on a plane,
      // the ruling on a cylinder or a cone (along v). Along those the UV
      // parameter is the length, so a box further than the best is skipped.
      if (type != GeomAbs_Plane && std::abs(st->into.X()) > 1e-6)
        continue;
      const Handle(Geom2d_Line) ray = new Geom2d_Line(st->uv, st->into);
      double best = std::numeric_limits<double>::infinity();
      int bestEdge = -1;
      // Nearest box first, and none past the nearest hit.
      std::vector<std::pair<double, const Boundary*>> order;
      for (const Boundary& b : bounds)
        if (const double at = RayEntersAt(st->uv, st->into, b.box);
            std::isfinite(at))
          order.emplace_back(at, &b);
      std::sort(order.begin(), order.end(),
                [](const auto& x, const auto& y) { return x.first < y.first; });
      for (const auto& [entry, bp] : order) {
        if (entry > best + 1e-6)
          break;
        const Boundary& b = *bp;
        Geom2dAPI_InterCurveCurve hits(ray, b.curve, 1e-9);
        for (int i = 1; i <= hits.NbPoints(); i++) {
          const gp_Pnt2d q = hits.Point(i);
          const double s = gp_Vec2d(st->uv, q).Dot(gp_Vec2d(st->into));
          if (s <= 1e-7)
            continue; // the edge itself, or behind it
          const double d = st->p.Distance(surf.Value(q.X(), q.Y()));
          if (d > 1e-6 && d < best) {
            best = d;
            bestEdge = b.index;
          }
        }
      }
      if (bestEdge >= 0 && (!least || best < least->width))
        least = WidthAcross{best, bestEdge};
    }
    return least;
  } catch (const Standard_Failure&) {
    return std::nullopt;
  }
}

std::optional<double> BlendProbe::ConcaveRadius(int edge, int faceIndex) {
  try {
    const auto& es = m->Sides(edge);
    if (!es)
      return std::nullopt;
    const int k = es->faceIndex[0] == faceIndex   ? 0
                  : es->faceIndex[1] == faceIndex ? 1
                                                  : -1;
    if (k < 0)
      return std::nullopt;
    const auto convex = m->Convex(*es, k);
    const auto me = m->At(*es, k, 0.5);
    if (!convex || !me)
      return std::nullopt;
    // A round on a convex edge sits in the material, one on a concave edge
    // in the air: the face curves toward it where it bends to that side.
    BRepLProp_SLProps props(m->Adaptor(faceIndex), me->uv.X(), me->uv.Y(), 2,
                            1e-9);
    if (!props.IsCurvatureDefined())
      return std::nullopt;
    // OpenCASCADE's curvatures are positive where the surface bends toward
    // its normal; the face's outward normal is that one or its reverse.
    const double sign =
        es->face[k].Orientation() == TopAbs_REVERSED ? -1.0 : 1.0;
    const double towardBlend = *convex ? -sign : sign;
    const double kappa = std::max(towardBlend * props.MaxCurvature(),
                                  towardBlend * props.MinCurvature());
    if (!(kappa > 1e-9))
      return std::nullopt; // flat, or curving away from the blend
    return 1.0 / kappa;
  } catch (const Standard_Failure&) {
    return std::nullopt;
  }
}

std::optional<std::array<int, 2>> FacesOfEdgeAt(const Body& body,
                                                int edgeIndex) {
  return BlendProbe(body).Faces(edgeIndex);
}

std::optional<double> DihedralAngleAt(const Body& body, int edgeIndex) {
  return BlendProbe(body).Dihedral(edgeIndex);
}

std::optional<WidthAcross> WidthAcrossEdgeAt(const Body& body, int edgeIndex,
                                             int faceIndex) {
  return BlendProbe(body).Width(edgeIndex, faceIndex);
}

std::optional<double> ConcaveRadiusAt(const Body& body, int edgeIndex,
                                      int faceIndex) {
  return BlendProbe(body).ConcaveRadius(edgeIndex, faceIndex);
}

} // namespace Measure
