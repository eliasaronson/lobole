// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/BlendFit.h"

#include "cad/Integrate.h"

#include "core/Measure.h"
#include "core/MeasureBlend.h"
#include "core/features/FilletFeature.h" // EdgeBlendOp

#include "core/ElementHistory.h" // EdgeBlendContours

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepLProp_CLProps.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <BndLib_Add2dCurve.hxx>
#include <Bnd_Box.hxx>
#include <Bnd_Box2d.hxx>
#include <ElCLib.hxx>
#include <ElSLib.hxx>
#include <GCE2d_MakeSegment.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <Geom2dAPI_InterCurveCurve.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec2d.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <tuple>
#include <unordered_map>

namespace fit {

namespace {

std::atomic<std::uint64_t> g_built{0};

std::string NameOrIndex(const std::string* n, const char* kind, int index) {
  return n != nullptr ? *n : std::string(kind) + std::to_string(index);
}

// A round's setback per mm of radius where the faces meet at `theta` (the
// solid's interior angle); none at a tangent or concave edge, where a disc's
// rim bounds nothing.
std::optional<double> ConvexSetbackPerMm(double theta) {
  const double turn = std::numbers::pi - theta;
  if (turn < 1e-4)
    return std::nullopt;
  return std::tan(0.5 * turn);
}

std::string RimWhy(const std::string& edge, double radius) {
  return "edge '" + edge + "' is a circle of radius " + Mm(radius) + " mm";
}

} // namespace

std::string Mm(double v) {
  std::string s = std::format("{:.6g}", v);
  return s;
}

double RoundDown3(double v) {
  if (!(v > 0.0) || !std::isfinite(v))
    return 0.0;
  const int e = static_cast<int>(std::floor(std::log10(v)));
  const int k = 2 - e; // decimals kept
  // Scaled by an exact power of ten, so the division gives the double
  // nearest the decimal: 749 / 100 prints 7.49.
  if (k >= 0) {
    const double p = std::pow(10.0, k);
    return std::floor(v * p * (1.0 + 1e-12)) / p;
  }
  const double p = std::pow(10.0, -k);
  return std::floor(v / p * (1.0 + 1e-12)) * p;
}

std::vector<Bound> BlendBounds(const Body& body, const std::vector<int>& edges,
                               EdgeBlendOp op) {
  const bool chamfer = op == EdgeBlendOp::Chamfer;
  Measure::BlendProbe probe(body);
  const char* blends = chamfer ? "chamfers" : "rounds";
  const char* aBlend = chamfer ? "a chamfer" : "a round";
  // How much of each face a blend of size 1 takes: its distance for a
  // chamfer, r / tan(theta/2) of a round where the faces meet at theta.
  const auto setbackPerMm = [&](int edge) -> std::optional<double> {
    if (chamfer)
      return 1.0;
    const auto theta = probe.Dihedral(edge);
    if (!theta)
      return std::nullopt;
    const double turn = std::abs(std::numbers::pi - *theta);
    if (turn < 1e-4)
      return std::nullopt; // tangent: nothing to round
    return std::tan(0.5 * turn);
  };
  const auto faceName = [&](int f) {
    return NameOrIndex(body.elementMap.NameOf(f), "face", f);
  };
  const auto edgeName = [&](int e) {
    return NameOrIndex(body.elementMap.EdgeNameOf(e), "edge", e);
  };
  // The refusing bounds first: of two equal, Least keeps the one before.
  std::vector<Bound> all =
      RefusingBounds(body, edges, op, std::numeric_limits<double>::infinity());
  const auto offer = [&](BoundKind kind, double size, std::string why,
                         std::string element) {
    if (!(size > 0.0) || !std::isfinite(size))
      return;
    all.push_back(Bound{size, std::move(why), std::move(element), kind});
  };
  for (const int e : edges) {
    const auto faces = probe.Faces(e);
    const auto k = setbackPerMm(e);
    if (!faces || !k)
      continue;
    const bool convex =
        chamfer ? true : probe.Dihedral(e).value_or(0.0) < std::numbers::pi;
    for (const int f : *faces) {
      if (const auto w = probe.Width(e, f)) {
        const bool farPicked =
            std::find(edges.begin(), edges.end(), w->endsAt) != edges.end();
        const auto info = w->endsAt == e && convex
                              ? Measure::DescribeEdgeAt(body, e)
                              : std::nullopt;
        if (info && info->hasRadius) {
          // Across a disc to the same rim: the circle bounds it.
          offer(BoundKind::Rim, w->width / (2.0 * *k),
                RimWhy(edgeName(e), info->radius), edgeName(e));
        } else if (farPicked) {
          const double kFar =
              w->endsAt == e ? *k : setbackPerMm(w->endsAt).value_or(0.0);
          const double size = w->width / (*k + kFar);
          offer(BoundKind::Between, size,
                "face '" + faceName(f) + "' is " + Mm(w->width) +
                    " mm wide between the two picked edges, so the " + blends +
                    " meet at " + Mm(size) + " mm",
                faceName(f));
        } else {
          const double size = w->width / *k;
          offer(BoundKind::Width, size,
                "face '" + faceName(f) + "' runs " + Mm(w->width) +
                    " mm from edge '" + edgeName(e) + "', so " + aBlend +
                    " of " + Mm(size) + " mm takes all of it",
                faceName(f));
        }
      }
      if (!chamfer)
        if (const auto rc = probe.ConcaveRadius(e, f))
          offer(BoundKind::Curvature, *rc,
                "face '" + faceName(f) + "' curves with a radius of " +
                    Mm(*rc) + " mm toward the round",
                faceName(f));
    }
  }
  return all;
}

namespace {

// What the refusing bounds look at: the faces either side of an edge, their
// kinds, and which way each runs from it. Analytic geometry only, so it
// costs a fraction of a build.
class Look {
public:
  explicit Look(const Body& body) : m_body(body) {
    const TopoDS_Shape& s = body.shape.GetShape();
    TopExp::MapShapes(s, TopAbs_EDGE, m_edges);
    TopExp::MapShapes(s, TopAbs_FACE, m_faces);
  }

  int EdgeCount() const { return m_edges.Extent(); }
  const TopoDS_Edge& Edge(int e) const { return TopoDS::Edge(m_edges(e + 1)); }
  const TopoDS_Face& Face(int f) const { return TopoDS::Face(m_faces(f + 1)); }
  std::string EdgeName(int e) const {
    return NameOrIndex(m_body.elementMap.EdgeNameOf(e), "edge", e);
  }
  std::string FaceName(int f) const {
    return NameOrIndex(m_body.elementMap.NameOf(f), "face", f);
  }

  // The two faces of an edge, as face indices; none on a seam, a free or a
  // non-manifold edge.
  std::optional<std::array<int, 2>> Faces(int e) {
    if (!m_parents) {
      m_parents.emplace();
      TopExp::MapShapesAndAncestors(m_body.shape.GetShape(), TopAbs_EDGE,
                                    TopAbs_FACE, *m_parents);
    }
    const int at = m_parents->FindIndex(Edge(e));
    if (at == 0)
      return std::nullopt;
    std::array<int, 2> out{-1, -1};
    int n = 0;
    for (const TopoDS_Shape& f : m_parents->FindFromIndex(at)) {
      const int fi = m_faces.FindIndex(f) - 1;
      if (fi < 0 || (n > 0 && out[0] == fi))
        return std::nullopt;
      if (n == 2)
        return std::nullopt;
      out[n++] = fi;
    }
    if (n != 2)
      return std::nullopt;
    return out;
  }

  // Whether edge `e` is on the outer wire of face `f`: a plane bounded by a
  // circle lies inside it there, outside it where the circle is a hole.
  bool OnOuterWire(int f, int e) {
    auto it = m_outer.find(f);
    if (it == m_outer.end())
      it = m_outer.emplace(f, BRepTools::OuterWire(Face(f))).first;
    for (TopExp_Explorer x(it->second, TopAbs_EDGE); x.More(); x.Next())
      if (x.Current().IsSame(Edge(e)))
        return true;
    return false;
  }

  // A plane bounded by this edge alone: a disc to its own rim.
  bool IsDisc(int f) const {
    int n = 0;
    for (TopExp_Explorer x(Face(f), TopAbs_EDGE); x.More(); x.Next())
      n++;
    return n == 1;
  }

  // Whether the open segment a-b on planar face `f` lies in the face: its
  // middle inside, and no boundary of the face crossing it.
  bool SegmentInFace(int f, const gp_Pnt& a, const gp_Pnt& b) {
    const TopoDS_Face& face = Face(f);
    const gp_Pln pl = BRepAdaptor_Surface(face, false).Plane();
    double ua = 0, va = 0, ub = 0, vb = 0;
    ElSLib::Parameters(pl, a, ua, va);
    ElSLib::Parameters(pl, b, ub, vb);
    const gp_Pnt2d A(ua, va), B(ub, vb);
    const double len = A.Distance(B);
    if (len < 1e-9)
      return false;
    BRepClass_FaceClassifier inside;
    inside.Perform(face, gp_Pnt2d(0.5 * (ua + ub), 0.5 * (va + vb)), 1e-7);
    if (inside.State() != TopAbs_IN)
      return false;
    // Its ends are on the two edges: a hair short of them.
    const gp_Vec2d d = gp_Vec2d(A, B) / len;
    const double eps = 1e-6 * len + 1e-7;
    const gp_Pnt2d A2 = A.Translated(d * eps), B2 = B.Translated(-d * eps);
    const Handle(Geom2d_TrimmedCurve) seg = GCE2d_MakeSegment(A2, B2).Value();
    Bnd_Box2d box;
    box.Add(A2);
    box.Add(B2);
    box.Enlarge(1e-7);
    for (const auto& [curve, cbox] : Boundaries(f)) {
      if (cbox.IsOut(box))
        continue;
      Geom2dAPI_InterCurveCurve x(seg, curve, 1e-9);
      if (x.NbPoints() > 0 || x.NbSegments() > 0)
        return false;
    }
    return true;
  }

private:
  using Boundary = std::pair<Handle(Geom2d_Curve), Bnd_Box2d>;
  const std::vector<Boundary>& Boundaries(int f) {
    auto it = m_boundaries.find(f);
    if (it != m_boundaries.end())
      return it->second;
    std::vector<Boundary> out;
    const TopoDS_Face& face = Face(f);
    for (TopExp_Explorer x(face, TopAbs_EDGE); x.More(); x.Next()) {
      double first = 0, last = 0;
      const Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(
          TopoDS::Edge(x.Current()), face, first, last);
      if (pc.IsNull())
        continue;
      Handle(Geom2d_Curve) trimmed = new Geom2d_TrimmedCurve(pc, first, last);
      Bnd_Box2d box;
      BndLib_Add2dCurve::Add(trimmed, 1e-7, box);
      out.emplace_back(trimmed, box);
    }
    return m_boundaries.emplace(f, std::move(out)).first->second;
  }

  const Body& m_body;
  TopTools_IndexedMapOfShape m_edges, m_faces;
  std::optional<TopTools_IndexedDataMapOfShapeListOfShape> m_parents;
  std::unordered_map<int, TopoDS_Wire> m_outer;
  std::unordered_map<int, std::vector<Boundary>> m_boundaries;
};

// The direction a coaxial cylinder or cone runs from its rim `circ` at `p`,
// along its ruling, or none for any other face.
std::optional<gp_Dir> IntoRevolved(const TopoDS_Face& face, const gp_Circ& circ,
                                   const gp_Pnt& p) {
  const BRepAdaptor_Surface s(face, false);
  const double tol = 1e-7 * std::max(1.0, circ.Radius());
  const auto coaxial = [&](const gp_Ax3& a) {
    return a.Direction().IsParallel(circ.Axis().Direction(), 1e-9) &&
           gp_Lin(a.Axis()).Distance(circ.Location()) < tol;
  };
  double u = 0.0, v = 0.0;
  gp_Pnt q;
  gp_Vec du, dv;
  if (s.GetType() == GeomAbs_Cylinder) {
    const gp_Cylinder c = s.Cylinder();
    if (!coaxial(c.Position()))
      return std::nullopt;
    ElSLib::Parameters(c, p, u, v);
    ElSLib::CylinderD1(u, v, c.Position(), c.Radius(), q, du, dv);
  } else if (s.GetType() == GeomAbs_Cone) {
    const gp_Cone c = s.Cone();
    if (!coaxial(c.Position()))
      return std::nullopt;
    ElSLib::Parameters(c, p, u, v);
    ElSLib::ConeD1(u, v, c.Position(), c.RefRadius(), c.SemiAngle(), q, du, dv);
  } else {
    return std::nullopt;
  }
  double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  if (!(v1 - v0 > 1e-9) || dv.Magnitude() < 1e-12)
    return std::nullopt;
  // The rim is one end of the face's range along the ruling; the face runs
  // toward the other.
  const double toLow = std::abs(v - v0), toHigh = std::abs(v - v1);
  if (std::min(toLow, toHigh) > 1e-6 * std::max(1.0, v1 - v0))
    return std::nullopt;
  return gp_Dir(toLow < toHigh ? dv : dv.Reversed());
}

// A blend's setback on a face per mm of its size, where the face runs from
// the edge along `intoFace` and the other along `intoOther`; none at a
// tangent edge.
std::optional<double> SetbackPerMm(EdgeBlendOp op, const gp_Dir& intoFace,
                                   const gp_Dir& intoOther) {
  const double psi = intoFace.Angle(intoOther);
  if (psi < 1e-4 || std::numbers::pi - psi < 1e-4)
    return std::nullopt;
  if (op == EdgeBlendOp::Chamfer)
    return 1.0;
  return std::tan(0.5 * (std::numbers::pi - psi));
}

// The builder's contours for these edges, kept for the last few inputs: the
// look before every build asks them, and the copy the builder is handed
// cost a tenth of the build of 104 edges of the plate of 100 bores, again at
// each regeneration of a feature whose input has not changed (a size
// dragged). Each entry holds its shape, so a match is that shape.
std::vector<topo::BlendContour> ContoursOf(const CADShape& body,
                                           const std::vector<int>& edges,
                                           EdgeBlendOp op) {
  struct Entry {
    TopoDS_Shape shape;
    std::vector<int> edges;
    EdgeBlendOp op;
    std::vector<topo::BlendContour> contours;
  };
  static std::mutex lock;
  // Never destroyed: shapes released after OpenCASCADE's own statics at
  // exit would be freed into a torn-down allocator.
  static auto& kept = *new std::deque<Entry>();
  constexpr size_t kKept = 4;
  {
    const std::lock_guard<std::mutex> hold(lock);
    for (const Entry& k : kept)
      if (k.op == op && k.edges == edges && k.shape.IsEqual(body.GetShape()))
        return k.contours;
  }
  std::vector<topo::BlendContour> contours =
      topo::EdgeBlendContours(body, edges, op);
  const std::lock_guard<std::mutex> hold(lock);
  kept.push_front({body.GetShape(), edges, op, contours});
  if (kept.size() > kKept)
    kept.pop_back();
  return contours;
}

const char* BlendsWord(EdgeBlendOp op) {
  return op == EdgeBlendOp::Chamfer ? "chamfers" : "rounds";
}

} // namespace

std::vector<Bound> RimBounds(const Body& body, const std::vector<int>& edges,
                             EdgeBlendOp op) {
  std::vector<Bound> out;
  if (body.shape.GetShape().IsNull())
    return out;
  Look look(body);
  for (const int e : edges) {
    if (e < 0 || e + 1 > look.EdgeCount())
      continue;
    const TopoDS_Edge& edge = look.Edge(e);
    if (BRep_Tool::Degenerated(edge) || !BRep_Tool::IsClosed(edge))
      continue;
    const BRepAdaptor_Curve curve(edge);
    if (curve.GetType() != GeomAbs_Circle)
      continue;
    const auto faces = look.Faces(e);
    if (!faces)
      continue;
    const gp_Circ circ = curve.Circle();
    const double R = circ.Radius();
    const gp_Pnt p = curve.Value(curve.FirstParameter());
    const gp_Dir radial(gp_Vec(circ.Location(), p));
    const gp_Dir axis = circ.Axis().Direction();
    // Each face's direction from the edge in the meridian: (radial, axial).
    std::array<std::pair<double, double>, 2> u{};
    bool meridian = true;
    for (int i = 0; i < 2 && meridian; i++) {
      const int f = (*faces)[i];
      const TopoDS_Face& face = look.Face(f);
      if (BRepAdaptor_Surface(face, false).GetType() == GeomAbs_Plane) {
        u[i] = {look.OnOuterWire(f, e) ? -1.0 : 1.0, 0.0};
      } else if (const auto d = IntoRevolved(face, circ, p)) {
        u[i] = {d->Dot(radial), d->Dot(axis)};
      } else {
        meridian = false;
      }
    }
    const std::string name = look.EdgeName(e);
    std::optional<double> size;
    std::string why = RimWhy(name, R);
    if (meridian) {
      const double phi = std::acos(std::clamp(
          u[0].first * u[1].first + u[0].second * u[1].second, -1.0, 1.0));
      if (phi < 1e-4 || std::numbers::pi - phi < 1e-4)
        continue; // tangent: nothing to blend
      // A chamfer takes its distance of each face, a round r / tan(phi/2).
      const double k =
          op == EdgeBlendOp::Chamfer ? 1.0 : 1.0 / std::tan(0.5 * phi);
      // A face leaning toward the axis reaches it R / -u_radial from the
      // rim, and no setback runs past that: the blend's edge on it would
      // be a circle of negative radius.
      for (int i = 0; i < 2; i++) {
        const double radialPart = u[i].first;
        if (!(radialPart < -1e-9))
          continue;
        const double reach = R / -radialPart;
        if (size && reach / k >= *size)
          continue;
        size = reach / k;
        why = RimWhy(name, R);
        if (std::abs(reach - R) > 1e-9 * R || std::abs(k - 1.0) > 1e-9)
          why += ", and face '" + look.FaceName((*faces)[i]) + "' runs " +
                 Mm(reach) + " mm from it to its axis, so " +
                 (op == EdgeBlendOp::Chamfer ? "a chamfer" : "a round") +
                 " of " + Mm(*size) + " mm takes all of it";
      }
    } else {
      // A disc to its own rim beside a face of another kind: the setback
      // across the disc is at most its radius.
      for (const int f : *faces)
        if (BRepAdaptor_Surface(look.Face(f), false).GetType() ==
                GeomAbs_Plane &&
            look.IsDisc(f)) {
          double k = 1.0; // a chamfer takes its distance
          if (op != EdgeBlendOp::Chamfer) {
            const auto theta = Measure::DihedralAngleAt(body, e);
            const auto perMm =
                theta ? ConvexSetbackPerMm(*theta) : std::nullopt;
            if (!perMm)
              break;
            k = *perMm;
          }
          size = R / k;
          break;
        }
    }
    if (!size || !(*size > 0.0) || !std::isfinite(*size))
      continue;
    out.push_back(Bound{*size, std::move(why), name, BoundKind::Rim});
  }
  return out;
}

std::optional<Bound> MeetBound(const Body& body, const std::vector<int>& edges,
                               EdgeBlendOp op, double below) {
  if (body.shape.GetShape().IsNull() || edges.empty() || !(below > 0.0))
    return std::nullopt;
  Look look(body);
  // Every edge the blend takes in: the builder's contours, the picked edges
  // and their tangent chains. A picked edge it takes into none (its faces
  // meet tangentially) blends nothing, and is the edges' objection to make.
  std::vector<int> blended;
  for (const topo::BlendContour& c : ContoursOf(body.shape, edges, op))
    blended.insert(blended.end(), c.edges.begin(), c.edges.end());
  std::sort(blended.begin(), blended.end());
  blended.erase(std::unique(blended.begin(), blended.end()), blended.end());

  // Each blended line or circle on a plane, with its setback per mm toward
  // either side of it in that plane.
  struct OnPlane {
    int edge = -1;
    bool line = false;
    gp_Lin lin;
    gp_Circ circ;
    double t0 = 0.0, t1 = 0.0; // the edge's parameter range
    bool closed = false;
    gp_Dir side;                // a line's: the plane's normal across it
    std::optional<double> k[2]; // toward +side (a circle's: outward), -side
    double kMax = 0.0;
    Bnd_Box box;
  };
  std::map<int, std::vector<OnPlane>> byFace;
  for (const int e : blended) {
    if (e < 0 || e + 1 > look.EdgeCount())
      continue;
    const TopoDS_Edge& edge = look.Edge(e);
    if (BRep_Tool::Degenerated(edge))
      continue;
    const auto faces = look.Faces(e);
    if (!faces)
      continue;
    const BRepAdaptor_Curve curve(edge);
    const bool line = curve.GetType() == GeomAbs_Line;
    if (!line && curve.GetType() != GeomAbs_Circle)
      continue;
    const double t0 = curve.FirstParameter(), t1 = curve.LastParameter();
    const gp_Pnt mid = curve.Value(0.5 * (t0 + t1));
    for (int i = 0; i < 2; i++) {
      const int f = (*faces)[i], g = (*faces)[1 - i];
      const BRepAdaptor_Surface fs(look.Face(f), false);
      if (fs.GetType() != GeomAbs_Plane)
        continue;
      OnPlane r;
      r.edge = e;
      r.line = line;
      r.t0 = t0;
      r.t1 = t1;
      gp_Dir out1, out2; // the directions in the plane across the edge
      std::optional<gp_Dir> intoOther;
      if (line) {
        r.lin = curve.Line();
        r.side =
            gp_Dir(fs.Plane().Axis().Direction().Crossed(r.lin.Direction()));
        out1 = r.side;
        out2 = r.side.Reversed();
        // The other face must be a plane: then the setback is the same all
        // along the edge. Which way it runs: its own classifier, a step in.
        const TopoDS_Face& other = look.Face(g);
        const BRepAdaptor_Surface gs(other, false);
        if (gs.GetType() != GeomAbs_Plane)
          continue;
        const gp_Dir across(
            gs.Plane().Axis().Direction().Crossed(r.lin.Direction()));
        const double step = 1e-4 * std::max(1.0, std::abs(t1 - t0));
        const auto in = [&](const gp_Dir& d) {
          BRepClass_FaceClassifier c(other, mid.Translated(gp_Vec(d) * step),
                                     1e-7);
          return c.State() == TopAbs_IN;
        };
        const bool plus = in(across), minus = in(across.Reversed());
        if (plus == minus)
          continue;
        intoOther = plus ? across : across.Reversed();
      } else {
        r.circ = curve.Circle();
        r.closed = BRep_Tool::IsClosed(edge);
        out1 = gp_Dir(gp_Vec(r.circ.Location(), mid));
        out2 = out1.Reversed();
        intoOther = IntoRevolved(look.Face(g), r.circ, mid);
        if (!intoOther)
          continue;
      }
      r.k[0] = SetbackPerMm(op, out1, *intoOther);
      r.k[1] = SetbackPerMm(op, out2, *intoOther);
      r.kMax = std::max(r.k[0].value_or(0.0), r.k[1].value_or(0.0));
      if (!(r.kMax > 0.0))
        continue;
      BRepBndLib::Add(edge, r.box, false);
      byFace[f].push_back(std::move(r));
    }
  }

  struct Candidate {
    double size = 0.0, gap = 0.0;
    int face = -1, a = -1, b = -1;
    gp_Pnt pa, pb;
  };
  std::vector<Candidate> candidates;
  const auto inside = [](const OnPlane& r, const gp_Pnt& p) {
    if (r.line) {
      const double t = ElCLib::Parameter(r.lin, p);
      const double m = 1e-6 * std::abs(r.t1 - r.t0);
      return t > r.t0 + m && t < r.t1 - m;
    }
    if (r.closed)
      return true;
    // Into the edge's own range, which may start below 0 or end past 2pi.
    double a = ElCLib::Parameter(r.circ, p); // [0, 2pi)
    const double m = 1e-6, turn = 2.0 * std::numbers::pi;
    while (a - turn >= r.t0)
      a -= turn;
    while (a < r.t0)
      a += turn;
    return a > r.t0 + m && a < r.t1 - m;
  };
  // Setback per mm of `r` at `p`, the blend's edge on the face moving off
  // `r` along `t`; none where `r` curves away under it (a concave-side
  // circle the setback `share` of the gap would pass the centre of).
  const auto setback = [](const OnPlane& r, const gp_Pnt& p,
                          const gp_Vec& t) -> std::optional<double> {
    if (r.line)
      return r.k[t.Dot(gp_Vec(r.side)) > 0.0 ? 0 : 1];
    const gp_Vec outward(r.circ.Location(), p);
    return r.k[t.Dot(outward) > 0.0 ? 0 : 1];
  };
  for (const auto& [f, rs] : byFace) {
    const gp_Dir normal =
        BRepAdaptor_Surface(look.Face(f), false).Plane().Axis().Direction();
    for (size_t i = 0; i < rs.size(); i++)
      for (size_t j = i + 1; j < rs.size(); j++) {
        const OnPlane& A = rs[i];
        const OnPlane& B = rs[j];
        if (A.edge == B.edge)
          continue;
        if (std::isfinite(below) &&
            A.box.Distance(B.box) >= below * (A.kMax + B.kMax))
          continue;
        // Segments square to both: along the line of centres of two
        // circles, square to a line through a circle's centre, between
        // two parallel lines where they overlap.
        std::vector<std::pair<gp_Pnt, gp_Pnt>> segs;
        if (!A.line && !B.line) {
          const gp_Vec cc(A.circ.Location(), B.circ.Location());
          gp_Dir d;
          if (cc.Magnitude() > 1e-9) {
            d = gp_Dir(cc);
          } else { // concentric: any radius both arcs reach
            d = gp_Dir(gp_Vec(A.circ.Location(),
                              ElCLib::Value(0.5 * (A.t0 + A.t1), A.circ)));
          }
          for (const double sa : {1.0, -1.0})
            for (const double sb : {1.0, -1.0})
              segs.emplace_back(A.circ.Location().Translated(
                                    gp_Vec(d) * (sa * A.circ.Radius())),
                                B.circ.Location().Translated(
                                    gp_Vec(d) * (sb * B.circ.Radius())));
        } else if (A.line != B.line) {
          const OnPlane& C = A.line ? B : A;
          const OnPlane& L = A.line ? A : B;
          const gp_Pnt foot =
              ElCLib::Value(ElCLib::Parameter(L.lin, C.circ.Location()), L.lin);
          gp_Vec n(C.circ.Location(), foot);
          if (n.Magnitude() < 1e-9)
            n = gp_Vec(L.side);
          n.Normalize();
          for (const double s : {1.0, -1.0}) {
            const gp_Pnt pc =
                C.circ.Location().Translated(n * (s * C.circ.Radius()));
            if (A.line)
              segs.emplace_back(foot, pc);
            else
              segs.emplace_back(pc, foot);
          }
        } else if (A.lin.Direction().IsParallel(B.lin.Direction(), 1e-9)) {
          const auto tb = [&](double t) {
            return ElCLib::Parameter(A.lin, ElCLib::Value(t, B.lin));
          };
          const double b0 = std::min(tb(B.t0), tb(B.t1));
          const double b1 = std::max(tb(B.t0), tb(B.t1));
          const double lo = std::max(A.t0, b0), hi = std::min(A.t1, b1);
          if (hi > lo) {
            const gp_Pnt pa = ElCLib::Value(0.5 * (lo + hi), A.lin);
            segs.emplace_back(
                pa, ElCLib::Value(ElCLib::Parameter(B.lin, pa), B.lin));
          }
        }
        for (const auto& [pa, pb] : segs) {
          const gp_Vec ab(pa, pb);
          const double gap = ab.Magnitude();
          if (gap < 1e-7 || std::abs(ab.Dot(gp_Vec(normal))) > 1e-6 * gap)
            continue;
          if (!inside(A, pa) || !inside(B, pb))
            continue;
          const gp_Vec t = ab / gap;
          const auto ka = setback(A, pa, t), kb = setback(B, pb, -t);
          if (!ka || !kb)
            continue;
          const double size = gap / (*ka + *kb);
          // A circle the setback runs into the middle of curves away from
          // under the blend: not this bound's to say.
          const auto regular = [&](const OnPlane& r, const gp_Pnt& p,
                                   const gp_Vec& dir, double k) {
            return r.line || dir.Dot(gp_Vec(r.circ.Location(), p)) > 0.0 ||
                   size * k < r.circ.Radius() * (1.0 - 1e-6);
          };
          if (!regular(A, pa, t, *ka) || !regular(B, pb, -t, *kb))
            continue;
          if (size < below && std::isfinite(size))
            candidates.push_back({size, gap, f, A.edge, B.edge, pa, pb});
        }
      }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& x, const Candidate& y) {
              // Ties by the edges, so the sentence names the same pair
              // whatever the size asked filtered out.
              return std::tie(x.size, x.face, x.a, x.b) <
                     std::tie(y.size, y.face, y.a, y.b);
            });
  // From the least up, the first whose segment is in the face: a few, so a
  // face whose nearest pairs are all blocked costs a bounded look.
  constexpr size_t kChecks = 32;
  for (size_t i = 0; i < candidates.size() && i < kChecks; i++) {
    const Candidate& c = candidates[i];
    if (!look.SegmentInFace(c.face, c.pa, c.pb))
      continue;
    const auto named = [&](int e) {
      std::string n = "edge '" + look.EdgeName(e) + "'";
      if (std::find(edges.begin(), edges.end(), e) == edges.end())
        n += " (tangent to a picked edge, so blended with it)";
      return n;
    };
    const std::string face = look.FaceName(c.face);
    return Bound{c.size,
                 "face '" + face + "' is " + Mm(c.gap) + " mm wide between " +
                     named(c.a) + " and " + named(c.b) + ", so the " +
                     BlendsWord(op) + " meet at " + Mm(c.size) + " mm",
                 face, BoundKind::Meet};
  }
  return std::nullopt;
}

std::vector<Bound> BendBounds(const Body& body, const std::vector<int>& edges,
                              EdgeBlendOp op) {
  std::vector<Bound> out;
  if (body.shape.GetShape().IsNull() || edges.empty())
    return out;
  Look look(body);
  std::vector<int> blended;
  for (const topo::BlendContour& c : ContoursOf(body.shape, edges, op))
    blended.insert(blended.end(), c.edges.begin(), c.edges.end());
  std::sort(blended.begin(), blended.end());
  blended.erase(std::unique(blended.begin(), blended.end()), blended.end());
  for (const int e : blended) {
    if (e < 0 || e + 1 > look.EdgeCount())
      continue;
    const TopoDS_Edge& edge = look.Edge(e);
    if (BRep_Tool::Degenerated(edge))
      continue;
    const GeomAbs_CurveType type = BRepAdaptor_Curve(edge).GetType();
    if (type == GeomAbs_Line || type == GeomAbs_Circle)
      continue;
    const auto faces = look.Faces(e);
    if (!faces)
      continue;
    for (int i = 0; i < 2; i++) {
      const int f = (*faces)[i], g = (*faces)[1 - i];
      const TopoDS_Face& face = look.Face(f);
      const BRepAdaptor_Surface fs(face, false);
      const BRepAdaptor_Surface gs(look.Face(g), false);
      if (fs.GetType() != GeomAbs_Plane ||
          gs.GetType() != GeomAbs_SurfaceOfExtrusion ||
          !gs.Direction().IsParallel(fs.Plane().Axis().Direction(), 1e-9))
        continue;
      // Square to the plane all along: a round's setback on it is its
      // radius (r tan 45 degrees), a chamfer's its distance.
      TopoDS_Edge inFace = edge;
      for (TopExp_Explorer x(face, TopAbs_EDGE); x.More(); x.Next())
        if (x.Current().IsSame(edge))
          inFace = TopoDS::Edge(x.Current());
      double t0 = 0.0, t1 = 0.0;
      const Handle(Geom2d_Curve) pc =
          BRep_Tool::CurveOnSurface(inFace, face, t0, t1);
      if (pc.IsNull() || !(t1 > t0))
        continue;
      // The plane's parameters are lengths, so the p-curve's curvature is
      // the edge's. In its parameters a face lies left of its edges as the
      // face holds them, right of them on a reversed face.
      const double side =
          (inFace.Orientation() == TopAbs_REVERSED ? -1.0 : 1.0) *
          (face.Orientation() == TopAbs_REVERSED ? -1.0 : 1.0);
      // Curvature toward the face, signed: positive where the edge turns
      // toward it.
      const auto toward = [&](double t) {
        gp_Pnt2d p;
        gp_Vec2d d1, d2;
        pc->D2(t, p, d1, d2);
        const double speed = d1.Magnitude();
        if (speed < 1e-12)
          return 0.0;
        return side * d1.Crossed(d2) / (speed * speed * speed);
      };
      // The tightest bend inside the edge, sampled along every span and
      // each local most refined by golden section: a pole thrown aside bends
      // a spline within a fraction of one span. Not at an end: a half
      // ellipse's ends bend tightest, and there the fold runs off the edge
      // into the corner, which the builder rounds past it.
      const Geom2dAdaptor_Curve adaptor(pc, t0, t1);
      const int spans = std::max(1, adaptor.NbIntervals(GeomAbs_C2));
      TColStd_Array1OfReal breaks(1, spans + 1);
      adaptor.Intervals(breaks, GeomAbs_C2);
      constexpr int kPerSpan = 32;
      std::vector<std::pair<double, double>> samples; // (t, curvature)
      for (int span = 1; span <= spans; span++) {
        const double a = breaks(span), b = breaks(span + 1);
        for (int k = span == 1 ? 0 : 1; k <= kPerSpan; k++) {
          const double t = k == kPerSpan ? b : a + (b - a) * k / kPerSpan;
          samples.emplace_back(t, toward(t));
        }
      }
      // The most of the curvature on [lo, hi], by golden section: where,
      // and how much.
      const auto refine = [&](double lo, double hi) {
        const double golden = 0.5 * (std::sqrt(5.0) - 1.0);
        double x1 = hi - golden * (hi - lo), x2 = lo + golden * (hi - lo);
        double k1 = toward(x1), k2 = toward(x2);
        for (int it = 0; it < 60 && hi - lo > 1e-15 * std::max(1.0, t1 - t0);
             it++) {
          if (k1 > k2) {
            hi = x2;
            x2 = x1;
            k2 = k1;
            x1 = hi - golden * (hi - lo);
            k1 = toward(x1);
          } else {
            lo = x1;
            x1 = x2;
            k1 = k2;
            x2 = lo + golden * (hi - lo);
            k2 = toward(x2);
          }
        }
        return k1 > k2 ? std::pair{x1, k1} : std::pair{x2, k2};
      };
      const size_t n = samples.size();
      const double ends = 1e-9 * (t1 - t0);
      double best = 0.0;
      for (size_t i = 1; i + 1 < n; i++) {
        if (!(samples[i].second > 0.0) ||
            samples[i].second < samples[i - 1].second ||
            !(samples[i].second > samples[i + 1].second))
          continue;
        const auto [at, kappa] =
            refine(samples[i - 1].first, samples[i + 1].first);
        if (at - t0 <= ends || t1 - at <= ends)
          continue; // it runs to an end
        best = std::max({best, samples[i].second, kappa});
      }
      // A closed edge has no end: its two ends are one point inside it.
      if (BRep_Tool::IsClosed(edge) && n > 2 && samples[0].second > 0.0 &&
          samples[0].second >= samples[1].second &&
          samples[0].second >= samples[n - 2].second)
        best = std::max({best, samples[0].second,
                         refine(t0, samples[1].first).second,
                         refine(samples[n - 2].first, t1).second});
      if (!(best > 0.0))
        continue;
      const double radius = 1.0 / best;
      if (!std::isfinite(radius) || !(radius > 0.0))
        continue;
      const std::string name = look.EdgeName(e);
      out.push_back(
          Bound{radius,
                "edge '" + name + "' bends toward face '" + look.FaceName(f) +
                    "' with a radius of " + Mm(radius) + " mm, so " +
                    (op == EdgeBlendOp::Chamfer ? "a chamfer's" : "a round's") +
                    " edge on that face folds back on itself past " +
                    Mm(radius) + " mm",
                name, BoundKind::Bend});
    }
  }
  return out;
}

std::vector<Bound> RefusingBounds(const Body& body,
                                  const std::vector<int>& edges, EdgeBlendOp op,
                                  double below) {
  std::vector<Bound> out = RimBounds(body, edges, op);
  if (auto meet = MeetBound(body, edges, op, below))
    out.push_back(std::move(*meet));
  for (Bound& b : BendBounds(body, edges, op))
    out.push_back(std::move(b));
  return out;
}
std::optional<Bound> Least(const std::vector<Bound>& bounds) {
  // Of two equal, the one that holds: a cylinder's end is bounded by its rim
  // and by its side's curvature alike, and only the rim is trusted.
  std::optional<Bound> least;
  for (const Bound& b : bounds)
    if (!least || b.size < least->size * (1.0 - 1e-9) ||
        (b.size <= least->size * (1.0 + 1e-9) && Refuses(b.kind) &&
         !Refuses(least->kind)))
      least = b;
  return least;
}

std::optional<Bound> BlendBound(const Body& body, const std::vector<int>& edges,
                                EdgeBlendOp op) {
  return Least(BlendBounds(body, edges, op));
}

bool Refuses(BoundKind kind) {
  // The sweep (tests/test_blend_fit_sweep.cpp) over the kernel cases' and
  // the action matrix's bodies: past a face's width, the width between two
  // picked edges or a face's curvature OpenCASCADE built hundreds of blends,
  // many at four times the bound (a wedge whose face narrows to a point at
  // one station, a round that runs off its face); past a circle's axis or
  // two blends meeting across a plane, none; a tenth past an edge's bend,
  // none.
  switch (kind) {
  case BoundKind::Rim:
  case BoundKind::Meet:
  case BoundKind::Bend:
    return true;
  case BoundKind::Width:
  case BoundKind::Between:
  case BoundKind::Curvature:
    return false;
  }
  return false;
}

double RefuseMargin(BoundKind kind) {
  switch (kind) {
  case BoundKind::Meet:
    return kMeetRefuseMargin;
  case BoundKind::Bend:
    return kBendRefuseMargin;
  default:
    return kRefuseMargin;
  }
}

std::optional<Bound> RefusingBound(const std::vector<Bound>& bounds,
                                   double asked) {
  std::optional<Bound> least;
  for (const Bound& b : bounds)
    if (Refuses(b.kind) && asked > b.size * (1.0 + RefuseMargin(b.kind)) &&
        (!least || b.size < least->size))
      least = b;
  return least;
}

Answer LargestThatBuilds(double asked, const std::optional<Bound>& bound,
                         const std::function<bool(double)>& builds,
                         const std::function<bool()>& stop,
                         SearchOptions options) {
  Answer a;
  a.asked = asked;
  a.fails = asked;
  double lo = 0.0; // the largest that built; 0 = none yet
  double seedBuilt = 0.0; // the seed's candidate, when it built
  // A candidate whose build was stopped part way (`builds` threw Stopped)
  // is neither: the search ends with the bracket it had.
  const auto tryOne = [&](double c) {
    g_built.fetch_add(1, std::memory_order_relaxed);
    try {
      if (builds(c)) {
        lo = c;
        a.fits = c;
        return true;
      }
    } catch (const Stopped&) {
      a.stopped = true;
      return false;
    }
    a.fails = c;
    return false;
  };
  // Just under the bound: rounded down from a thousandth below it, so two
  // rounds that would meet exactly leave a sliver between them.
  if (bound && bound->size < asked) {
    const double c = RoundDown3(bound->size * (1.0 - 1e-3));
    if (c > 0.0 && c < a.fails) {
      if (stop()) {
        a.stopped = true;
        return a;
      }
      if (tryOne(c)) {
        if (options.seedIsBound) {
          a.bound = bound; // nothing above the bound fits
          return a;
        }
        seedBuilt = c;
      }
      if (a.stopped)
        return a;
    }
  }
  for (int step = 0; step < options.steps; step++) {
    // Down by a factor while nothing has built, so eight steps reach a
    // thousandth of the size; then halving the bracket by ratio, so the
    // answer is as close to the limit in proportion whatever its size.
    const double c =
        RoundDown3(lo > 0.0 ? std::sqrt(lo * a.fails) : 0.4 * a.fails);
    if (c <= lo || c >= a.fails)
      break; // converged at three digits
    if (stop()) {
      a.stopped = true;
      return a;
    }
    tryOne(c);
    if (a.stopped)
      return a;
  }
  // A seed that built and nothing above it did: the bound is why that is
  // the limit, though it was not trusted to be.
  if (seedBuilt > 0.0 && a.fits == seedBuilt)
    a.bound = bound;
  return a;
}

std::uint64_t CandidatesBuilt() { return g_built.load(); }

std::optional<Answer> Memo::For(const CADShape& input,
                                const std::vector<int>& elements, int operation,
                                double asked) const {
  if (op != operation || edges != elements || answer.stopped ||
      body.GetShape().IsNull() || !body.GetShape().IsEqual(input.GetShape()) ||
      asked < answer.fails)
    return std::nullopt;
  Answer a = answer;
  a.asked = asked;
  return a;
}

std::string Sentence(EdgeBlendOp op, const Answer& a, const std::string& places,
                     const std::string& cause) {
  const bool chamfer = op == EdgeBlendOp::Chamfer;
  const std::string what = chamfer ? "chamfer" : "fillet";
  const std::string param = chamfer ? "distance" : "radius";
  std::string s = what + " " + param + " " + Mm(a.asked) + " mm does not fit: ";
  if (a.stopped) {
    if (a.fits)
      s += "a " + param + " of " + Mm(*a.fits) + " mm builds and " +
           Mm(a.fails) + " mm does not (the search was stopped)";
    else
      s += Mm(a.fails) + " mm does not build either (the search was stopped)";
    return s + places;
  }
  if (a.fits) {
    s += "the largest " + param + " that fits is " + Mm(*a.fits) + " mm";
    if (a.bound)
      s += " (" + a.bound->why + ")";
    return s + places;
  }
  s += "no " + param + " builds on these edges, down to " + Mm(a.fails) + " mm";
  if (!cause.empty())
    s += " (" + cause + ")";
  return s + "; fewer edges in one " + what + " usually builds";
}

FitHint HintOf(const Answer& a) {
  FitHint h;
  h.param = "radius";
  h.askedMm = a.asked;
  h.fitsMm = a.fits;
  h.failsMm = a.fails;
  h.basis = "built";
  if (a.bound)
    h.element = a.bound->element;
  h.stopped = a.stopped;
  return h;
}

std::string BoundSentence(EdgeBlendOp op, const Answer& a, const Bound& past,
                          const std::string& places) {
  const bool chamfer = op == EdgeBlendOp::Chamfer;
  const std::string param = chamfer ? "distance" : "radius";
  std::string s = std::string(chamfer ? "chamfer " : "fillet ") + param + " " +
                  Mm(a.asked) + " mm does not fit: " + past.why;
  if (a.stopped)
    return s +
           (a.fits ? "; a " + param + " of " + Mm(*a.fits) + " mm builds and " +
                         Mm(a.fails) + " mm does not (the search was stopped)"
                   : std::string("; the search was stopped")) +
           places;
  if (a.fits)
    return s + "; the largest " + param + " that fits is " + Mm(*a.fits) +
           " mm" + places;
  return s + "; no " + param + " builds on these edges, down to " +
         Mm(a.fails) + " mm; fewer edges in one " +
         (chamfer ? "chamfer" : "fillet") + " usually builds";
}

FitHint BoundHint(const Answer& a) {
  FitHint h = HintOf(a);
  h.basis = "bound";
  return h;
}

namespace {

// The solid's interior angle between the two faces of `edge` at its
// parameter `u`, radians (under pi convex, over it concave), from the faces'
// normals and the direction into the first: Measure::DihedralAngleAt's
// reading, without its stations' classification, for nine stations an edge.
struct EdgeAngle {
  BRepAdaptor_Curve curve;
  struct Side {
    BRepAdaptor_Surface surface;
    Handle(Geom2d_Curve) pcurve;
    bool reversedEdge = false, reversedFace = false;
  };
  std::vector<Side> sides;

  EdgeAngle(const TopoDS_Edge& edge, const TopoDS_Face& f1,
            const TopoDS_Face& f2)
      : curve(edge) {
    for (const TopoDS_Face& f : {f1, f2}) {
      TopoDS_Edge inFace = edge;
      for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next())
        if (ex.Current().IsSame(edge)) {
          inFace = TopoDS::Edge(ex.Current());
          break;
        }
      double a = 0.0, b = 0.0;
      Side s{BRepAdaptor_Surface(f, false),
             BRep_Tool::CurveOnSurface(inFace, f, a, b),
             inFace.Orientation() == TopAbs_REVERSED,
             f.Orientation() == TopAbs_REVERSED};
      sides.push_back(std::move(s));
    }
  }

  std::optional<double> At(double u) const {
    gp_Pnt p;
    gp_Vec t;
    curve.D1(u, p, t);
    if (t.Magnitude() < 1e-12 || sides.size() != 2 ||
        sides[0].pcurve.IsNull() || sides[1].pcurve.IsNull())
      return std::nullopt;
    gp_Vec n[2], into0;
    for (int k = 0; k < 2; k++) {
      const gp_Pnt2d uv = sides[k].pcurve->Value(u);
      gp_Pnt q;
      gp_Vec du, dv;
      sides[k].surface.D1(uv.X(), uv.Y(), q, du, dv);
      n[k] = du.Crossed(dv);
      if (n[k].Magnitude() < 1e-12)
        return std::nullopt;
      n[k].Normalize();
      if (sides[k].reversedFace)
        n[k].Reverse();
    }
    // Into the first face: its normal across the edge as the face runs it
    // (the material on the left, seen from outside).
    // The explorer composes the face's orientation onto its edges', so
    // the outward normal crossed with the edge's direction as the face
    // holds it points into the face either way.
    gp_Vec run = t.Normalized();
    if (sides[0].reversedEdge)
      run.Reverse();
    into0 = n[0].Crossed(run);
    const double between = n[0].Angle(n[1]);
    return into0.Dot(n[1]) < 0.0 ? std::numbers::pi - between
                                 : std::numbers::pi + between;
  }
};

} // namespace

std::optional<std::vector<EdgeReach>>
ReachOf(const Body& body, const std::vector<int>& contourEdges) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(body.shape.GetShape(), TopAbs_EDGE, TopAbs_FACE,
                                edgeFaces);
  // At a cusp (a knife edge, the faces' normals opposite) which side holds
  // the material is a guess: the section counts on both.
  constexpr double kCuspRad = 0.05;
  // Stations to within a fiftieth of each end: the angle can run to nothing
  // at a knife edge's tip, and its section with it to everything.
  constexpr double kStations[] = {0.02, 0.1,  0.25, 0.4, 0.5,
                                  0.6,  0.75, 0.9,  0.98};
  std::vector<EdgeReach> out;
  try {
    for (const int e : contourEdges) {
      if (e < 0 || e + 1 > edges.Extent())
        return std::nullopt;
      const TopoDS_Edge& edge = TopoDS::Edge(edges(e + 1));
      TopoDS_Face f1, f2;
      if (const int at = edgeFaces.FindIndex(edge); at != 0)
        for (const TopoDS_Shape& f : edgeFaces(at)) {
          if (f1.IsNull())
            f1 = TopoDS::Face(f);
          else if (!f.IsSame(f1))
            f2 = TopoDS::Face(f);
        }
      if (f2.IsNull())
        return std::nullopt;
      const EdgeAngle angle(edge, f1, f2);
      const double a = angle.curve.FirstParameter(),
                   b = angle.curve.LastParameter();
      EdgeReach r;
      for (const double t : kStations) {
        const std::optional<double> theta = angle.At(a + (b - a) * t);
        if (!theta || !std::isfinite(*theta))
          return std::nullopt;
        const bool concave = *theta > std::numbers::pi;
        const double phi =
            std::clamp(concave ? 2.0 * std::numbers::pi - *theta : *theta, 1e-6,
                       std::numbers::pi);
        (concave ? r.concave : r.convex).push_back(phi);
        if (phi < kCuspRad)
          (concave ? r.convex : r.concave).push_back(phi);
      }
      r.length = GCPnts_AbscissaPoint::Length(angle.curve);
      if (angle.curve.GetType() == GeomAbs_Circle) {
        r.curvature = 1.0 / angle.curve.Circle().Radius();
      } else if (angle.curve.GetType() != GeomAbs_Line) {
        for (int k = 0; k <= 8; k++) {
          BRepLProp_CLProps props(angle.curve, a + (b - a) * k / 8.0, 2, 1e-9);
          r.curvature = std::max(r.curvature, std::abs(props.Curvature()));
        }
      }
      out.push_back(std::move(r));
    }
  } catch (const Standard_Failure&) {
    return std::nullopt;
  }
  return out;
}

VolumeReach ReachAt(const std::vector<EdgeReach>& reach, EdgeBlendOp op,
                    double size) {
  const bool chamfer = op == EdgeBlendOp::Chamfer;
  // How far from the edge the blend reaches: its setback in the wedge of
  // the faces' tangent planes, or the ball's own size where faces that
  // curve away from it carry its contact further out (two cylinders meeting
  // nearly tangent).
  const auto setback = [&](double phi) {
    return std::max(size, chamfer ? size : size / std::tan(phi / 2.0));
  };
  // The section at an angle -- the wedge's, or the ball's disc where that is
  // more -- swept along a curved edge at its setback.
  const auto moved = [&](double phi, double curvature) {
    const double half = phi / 2.0;
    const double wedge =
        chamfer ? 0.5 * size * size * std::sin(phi)
                : size * size *
                      (1.0 / std::tan(half) - (std::numbers::pi - phi) / 2.0);
    const double section = std::max(wedge, std::numbers::pi * size * size);
    return section * (1.0 + setback(phi) * curvature);
  };
  VolumeReach v;
  double ends = 0.0;
  for (const EdgeReach& r : reach) {
    double most[2] = {0.0, 0.0}, reaches = 0.0;
    for (const double phi : r.convex) {
      most[0] = std::max(most[0], moved(phi, r.curvature));
      reaches = std::max(reaches, setback(phi));
    }
    for (const double phi : r.concave) {
      most[1] = std::max(most[1], moved(phi, r.curvature));
      reaches = std::max(reaches, setback(phi));
    }
    v.remove += r.length * most[0];
    v.add += r.length * most[1];
    // Past each end the blend meets whatever is there -- an end face at a
    // slant, a neighbour it runs out over -- within its setback of the
    // vertex: a ball of that radius, either way.
    ends += 2.0 * (4.0 / 3.0) * std::numbers::pi * reaches * reaches * reaches;
  }
  v.ends = ends;
  return v;
}

constexpr double kBuilderTolerance = 1e-4; // mm

std::string VolumeObjection(EdgeBlendOp op, double size, const CADShape& input,
                            const CADShape& built, const VolumeReach& reach) {
  // First the fixed rule and one estimate of each suspect face, their gap
  // as the uncertainty: a blend's B-spline faces cost the integrator's
  // halvings ten times over, and the margin is a factor, not a digit.
  cad::IntegrateOptions quick;
  quick.area = false;
  quick.maxHalvings = 0;
  const cad::Integral q = cad::Integrate(built.GetShape(), quick);
  std::string why = VolumeObjection(op, size, input.Volume(), q.volume, reach,
                                    4.0 * q.volumeUncertainty);
  if (why.empty())
    return why;
  CADShape::MassProps a, b;
  if (!input.ComputeMassProps(a) || !built.ComputeMassProps(b))
    return why;
  // The integration's own uncertainty, and the builder's: it approximates
  // the faces it rebuilds to 1e-4 mm (ChFi3d_Builder's tolapp3d), over at
  // most the body's area.
  return VolumeObjection(op, size, a.volume, b.volume, reach,
                         4.0 * (a.volumeUncertainty + b.volumeUncertainty) +
                             kBuilderTolerance *
                                 std::max(a.surfaceArea, b.surfaceArea));
}

std::string VolumeObjection(EdgeBlendOp op, double size, double before,
                            double after, const VolumeReach& reach,
                            double slack) {
  if (!std::isfinite(before) || !std::isfinite(after))
    return {};
  const double change = after - before;
  const double floor = kReachFloor * std::abs(before) + slack;
  const double removes = kReachMargin * (reach.remove + reach.ends) + floor;
  const double adds = kReachMargin * (reach.add + reach.ends) + floor;
  if (change >= -removes && change <= adds)
    return {};
  const bool chamfer = op == EdgeBlendOp::Chamfer;
  const std::string what = chamfer ? "chamfer" : "fillet";
  const std::string noun = chamfer ? "a chamfer of " : "a round of ";
  const auto mm3 = [](double v) { return std::format("{:.4g}", v); };
  std::string limit;
  if (change > 0.0)
    limit = "can add at most " + mm3(reach.add + reach.ends) + " mm3" +
            (reach.add > 0.0 ? "" : " (past their ends: they are convex)");
  else
    limit = "can remove at most " + mm3(reach.remove + reach.ends) + " mm3" +
            (reach.remove > 0.0 ? "" : " (past their ends: they are concave)");
  return what + " produced a solid " + mm3(std::abs(change)) + " mm3 " +
         (change > 0.0 ? "larger" : "smaller") + " than the part, where " +
         noun + Mm(size) + " mm on these edges " + limit +
         ": OpenCASCADE's blend builder built a wrong solid";
}

} // namespace fit
