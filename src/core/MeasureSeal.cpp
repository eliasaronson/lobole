// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MeasureSeal.h"

#include "cad/NonDestructiveBoolean.h"
#include "core/Feature.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GProp_GProps.hxx>
#include <GProp_PrincipalProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Measure {

namespace {

// A rigid placement as a gp_Trsf, from a normalized quaternion (as
// Measure.cpp's ToTrsf: OCC infers a scale from a float 3x3 otherwise).
gp_Trsf Rigid(const glm::dmat4& m) {
  const glm::dquat q = glm::normalize(glm::quat_cast(glm::dmat3(m)));
  gp_Trsf t;
  t.SetRotation(gp_Quaternion(q.x, q.y, q.z, q.w));
  t.SetTranslationPart(gp_Vec(m[3][0], m[3][1], m[3][2]));
  return t;
}

glm::dvec3 V(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
glm::dvec3 V(const gp_Dir& d) { return {d.X(), d.Y(), d.Z()}; }

std::vector<TopoDS_Shape> Placed(const std::vector<Body>& bodies,
                                 const glm::dmat4& xf) {
  const TopLoc_Location loc(Rigid(xf));
  std::vector<TopoDS_Shape> out;
  for (const Body& b : bodies)
    if (!b.reference && !b.shape.GetShape().IsNull())
      out.push_back(b.shape.GetShape().Moved(loc));
  return out;
}

Bnd_Box BoxOf(const TopoDS_Shape& s, double gap) {
  Bnd_Box b;
  BRepBndLib::Add(s, b, false);
  b.SetGap(gap);
  return b;
}

// A surface two faces can lie on each other over: the analytic ones. A
// thread's helicoid is not a sealing face, and pairing every one of them
// would cost the run.
bool Analytic(const TopoDS_Face& f) {
  switch (BRepAdaptor_Surface(f, false).GetType()) {
  case GeomAbs_Plane:
  case GeomAbs_Cylinder:
  case GeomAbs_Cone:
  case GeomAbs_Sphere:
  case GeomAbs_Torus:
    return true;
  default:
    return false;
  }
}

// One trace of the contact on one half-plane: how wide it is along the
// contact, how deep the overlap is (0 where faces only touch), and its box
// in the half-plane's (radius, along) coordinates.
struct Trace {
  double widthMm = 0.0, depthMm = 0.0;
  double s0 = 0.0, s1 = 0.0, t0 = 0.0, t1 = 0.0;
};

struct Frame {
  glm::dvec3 p, d, x, y; // the axis, and two directions across it
  glm::dvec3 Dir(double th) const {
    return std::cos(th) * x + std::sin(th) * y;
  }
};

void AddPoint(Trace& tr, bool& first, const Frame& fr, const glm::dvec3& q,
              double th) {
  const glm::dvec3 r = q - fr.p;
  const double s = glm::dot(r, fr.Dir(th)), t = glm::dot(r, fr.d);
  if (first) {
    tr.s0 = tr.s1 = s;
    tr.t0 = tr.t1 = t;
    first = false;
  } else {
    tr.s0 = std::min(tr.s0, s);
    tr.s1 = std::max(tr.s1, s);
    tr.t0 = std::min(tr.t0, t);
    tr.t1 = std::max(tr.t1, t);
  }
}

void AddEdge(Trace& tr, bool& first, const Frame& fr, const TopoDS_Edge& e,
             double th) {
  BRepAdaptor_Curve c(e);
  const double a = c.FirstParameter(), b = c.LastParameter();
  for (int i = 0; i <= 8; i++)
    AddPoint(tr, first, fr, V(c.Value(a + (b - a) * i / 8.0)), th);
}

double EdgeLength(const TopoDS_Edge& e) {
  try {
    BRepAdaptor_Curve c(e);
    return GCPnts_AbscissaPoint::Length(c);
  } catch (const Standard_Failure&) {
    return 0.0;
  }
}

// The half-plane at `th`: from the axis outwards to `rMax`, `t0` to `t1`
// along it.
TopoDS_Face HalfPlane(const Frame& fr, double th, double rMax, double t0,
                      double t1) {
  const glm::dvec3 u = fr.Dir(th);
  const glm::dvec3 n = glm::cross(u, fr.d); // so the plane's Y is the axis
  const gp_Ax3 ax(gp_Pnt(fr.p.x, fr.p.y, fr.p.z), gp_Dir(n.x, n.y, n.z),
                  gp_Dir(u.x, u.y, u.z));
  return BRepBuilderAPI_MakeFace(gp_Pln(ax), 0.0, rMax, t0, t1).Face();
}

// The traces on one half-plane: chains of section edges through the faces
// that lie on each other, and the regions the overlap cuts.
std::vector<Trace> TracesAt(const Frame& fr, double th, double rMax, double t0,
                            double t1, const TopoDS_Shape& touch,
                            const TopoDS_Shape& overlap) {
  std::vector<Trace> out;
  const TopoDS_Face hp = HalfPlane(fr, th, rMax, t0, t1);
  if (!touch.IsNull()) {
    BRepAlgoAPI_Section sec;
    if (cad::BuildNonDestructive(sec, touch, hp)) {
      const TopoDS_Shape& r = sec.Shape();
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(r, TopAbs_EDGE, edges);
      TopTools_IndexedDataMapOfShapeListOfShape v2e;
      TopExp::MapShapesAndAncestors(r, TopAbs_VERTEX, TopAbs_EDGE, v2e);
      // Chains: edges joined by a vertex.
      std::vector<int> parent(edges.Extent());
      std::iota(parent.begin(), parent.end(), 0);
      const auto find = [&](int i) {
        while (parent[i] != i)
          i = parent[i] = parent[parent[i]];
        return i;
      };
      for (int v = 1; v <= v2e.Extent(); v++) {
        int first = -1;
        for (const TopoDS_Shape& e : v2e(v)) {
          const int k = edges.FindIndex(e) - 1;
          if (k < 0)
            continue;
          if (first < 0)
            first = k;
          else
            parent[find(k)] = find(first);
        }
      }
      // out is empty here: the touching traces come first.
      std::vector<int> slot(edges.Extent(), -1);
      std::vector<bool> firsts;
      for (int k = 0; k < edges.Extent(); k++) {
        const int root = find(k);
        if (slot[root] < 0) {
          slot[root] = static_cast<int>(out.size());
          out.emplace_back();
          firsts.push_back(true);
        }
        const size_t at = slot[root];
        const TopoDS_Edge& e = TopoDS::Edge(edges(k + 1));
        out[at].widthMm += EdgeLength(e);
        bool f = firsts[at];
        AddEdge(out[at], f, fr, e, th);
        firsts[at] = f;
      }
    }
  }
  if (!overlap.IsNull()) {
    BRepAlgoAPI_Common com;
    if (cad::BuildNonDestructive(com, overlap, hp)) {
      for (TopExp_Explorer ex(com.Shape(), TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& f = TopoDS::Face(ex.Current());
        GProp_GProps area;
        BRepGProp::SurfaceProperties(f, area);
        if (area.Mass() <= 1e-12)
          continue;
        Trace tr;
        // The region's width is its longest extent, across the contact (a
        // pressed ring's lens is as wide as its chord), and its depth the
        // area over that.
        bool first = true;
        std::vector<glm::dvec3> rimPts;
        for (TopExp_Explorer ee(f, TopAbs_EDGE); ee.More(); ee.Next()) {
          const TopoDS_Edge& e = TopoDS::Edge(ee.Current());
          AddEdge(tr, first, fr, e, th);
          BRepAdaptor_Curve c(e);
          const double a = c.FirstParameter(), b = c.LastParameter();
          for (int i = 0; i <= 32; i++)
            rimPts.push_back(V(c.Value(a + (b - a) * i / 32.0)));
        }
        for (size_t i = 0; i < rimPts.size(); i++)
          for (size_t j = i + 1; j < rimPts.size(); j++)
            tr.widthMm =
                std::max(tr.widthMm, glm::length(rimPts[i] - rimPts[j]));
        if (tr.widthMm <= 0.0)
          continue;
        tr.depthMm = area.Mass() / tr.widthMm;
        out.push_back(tr);
      }
    }
  }
  return out;
}

bool Touch(const Trace& a, const Trace& b, double pad) {
  return a.s0 <= b.s1 + pad && b.s0 <= a.s1 + pad && a.t0 <= b.t1 + pad &&
         b.t0 <= a.t1 + pad;
}

// The axis the contact goes round: the one most of its circular edges
// share, else the principal axis of its own that stands apart from the
// other two (a ring's: the two across it are equal).
bool AxisOf(const TopoDS_Shape& contact, double tol, Frame& fr,
            std::string& from) {
  struct Ax {
    glm::dvec3 p, d;
    int n;
  };
  std::vector<Ax> axes;
  for (TopExp_Explorer ex(contact, TopAbs_EDGE); ex.More(); ex.Next()) {
    BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
    if (c.GetType() != GeomAbs_Circle)
      continue;
    const gp_Circ ci = c.Circle();
    const glm::dvec3 p = V(ci.Location()), d = V(ci.Axis().Direction());
    bool found = false;
    for (Ax& a : axes) {
      const glm::dvec3 off = p - a.p;
      if (std::abs(std::abs(glm::dot(a.d, d)) - 1.0) < 1e-6 &&
          glm::length(off - glm::dot(off, a.d) * a.d) < tol) {
        a.n++;
        found = true;
        break;
      }
    }
    if (!found)
      axes.push_back({p, d, 1});
  }
  if (!axes.empty()) {
    const Ax& best =
        *std::max_element(axes.begin(), axes.end(),
                          [](const Ax& a, const Ax& b) { return a.n < b.n; });
    fr.p = best.p;
    fr.d = best.d;
    from = "the circular edges of the contact (" + std::to_string(best.n) +
           " of them)";
  } else {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(contact, g);
    if (g.Mass() <= 1e-12)
      BRepGProp::VolumeProperties(contact, g);
    if (g.Mass() <= 1e-12)
      return false;
    const GProp_PrincipalProps pp = g.PrincipalProperties();
    double i1, i2, i3;
    pp.Moments(i1, i2, i3);
    const double spread[3] = {std::min(std::abs(i1 - i2), std::abs(i1 - i3)),
                              std::min(std::abs(i2 - i1), std::abs(i2 - i3)),
                              std::min(std::abs(i3 - i1), std::abs(i3 - i2))};
    const int k =
        static_cast<int>(std::max_element(spread, spread + 3) - spread);
    const gp_Vec ax = k == 0   ? pp.FirstAxisOfInertia()
                      : k == 1 ? pp.SecondAxisOfInertia()
                               : pp.ThirdAxisOfInertia();
    fr.p = V(g.CentreOfMass());
    fr.d = glm::normalize(glm::dvec3(ax.X(), ax.Y(), ax.Z()));
    from = "the contact's own principal axis";
  }
  fr.d = glm::normalize(fr.d);
  // Angles from world X seen along the axis (world Y for an axis near X).
  const glm::dvec3 ref =
      std::abs(fr.d.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
  fr.x = glm::normalize(ref - glm::dot(ref, fr.d) * fr.d);
  fr.y = glm::cross(fr.d, fr.x);
  return true;
}

} // namespace

std::expected<SealReport, std::string>
Seal(const std::vector<Body>& bodiesA, const glm::dmat4& xfA,
     const std::vector<Body>& bodiesB, const glm::dmat4& xfB, int samples,
     double touchTolMm, const std::atomic<bool>* cancel) {
  if (samples < 8)
    return std::unexpected("a seal is sampled at 8 half-planes or more");
  const std::vector<TopoDS_Shape> A = Placed(bodiesA, xfA),
                                  B = Placed(bodiesB, xfB);
  if (A.empty() || B.empty())
    return std::unexpected("one side has no bodies, so there is no contact");
  SealReport rep;
  rep.samples = samples;
  rep.touchTolMm = touchTolMm;
  BRep_Builder bb;
  TopoDS_Compound touch, overlap, all;
  bb.MakeCompound(touch);
  bb.MakeCompound(overlap);
  bb.MakeCompound(all);
  bool anyTouch = false, anyOverlap = false;
  try {
    for (const TopoDS_Shape& a : A)
      for (const TopoDS_Shape& b : B) {
        if (BoxOf(a, touchTolMm).IsOut(BoxOf(b, touchTolMm)))
          continue;
        // Where they overlap: a gasket pressed, a lid pushed on.
        BRepAlgoAPI_Common com;
        if (cad::BuildNonDestructive(com, a, b, touchTolMm))
          for (TopExp_Explorer ex(com.Shape(), TopAbs_SOLID); ex.More();
               ex.Next()) {
            GProp_GProps g;
            BRepGProp::VolumeProperties(ex.Current(), g);
            if (g.Mass() <= 1e-9)
              continue;
            rep.overlapVolumeMm3 += g.Mass();
            bb.Add(overlap, ex.Current());
            bb.Add(all, ex.Current());
            anyOverlap = true;
          }
        // Where a face lies on a face.
        for (TopExp_Explorer fa(a, TopAbs_FACE); fa.More(); fa.Next()) {
          const TopoDS_Face& f1 = TopoDS::Face(fa.Current());
          if (!Analytic(f1))
            continue;
          const Bnd_Box b1 = BoxOf(f1, touchTolMm);
          for (TopExp_Explorer fb(b, TopAbs_FACE); fb.More(); fb.Next()) {
            const TopoDS_Face& f2 = TopoDS::Face(fb.Current());
            if (!Analytic(f2) || b1.IsOut(BoxOf(f2, touchTolMm)))
              continue;
            BRepAlgoAPI_Common on;
            if (!cad::BuildNonDestructive(on, f1, f2, touchTolMm))
              continue;
            for (TopExp_Explorer ex(on.Shape(), TopAbs_FACE); ex.More();
                 ex.Next()) {
              GProp_GProps g;
              BRepGProp::SurfaceProperties(ex.Current(), g);
              if (g.Mass() <= 1e-9)
                continue;
              rep.contactAreaMm2 += g.Mass();
              bb.Add(touch, ex.Current());
              bb.Add(all, ex.Current());
              anyTouch = true;
            }
          }
        }
      }
  } catch (const Standard_Failure& e) {
    return std::unexpected(std::string("the kernel failed finding the "
                                       "contact: ") +
                           e.GetMessageString());
  }
  if (!anyTouch && !anyOverlap)
    return std::unexpected("the two sides do not touch: no face lies on "
                           "another within " +
                           std::to_string(touchTolMm) +
                           " mm and nothing overlaps");
  Frame fr;
  if (!AxisOf(all, 10.0 * touchTolMm, fr, rep.axisFrom))
    return std::unexpected("no axis: the contact has no circular edge and "
                           "no extent to take one from");
  rep.axisPoint = fr.p;
  rep.axisDir = fr.d;

  // The half-planes' extent: the contact's box, seen from the axis.
  Bnd_Box box;
  BRepBndLib::Add(all, box, false);
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  double rMax = 0.0, t0 = std::numeric_limits<double>::infinity(), t1 = -t0;
  for (int i = 0; i < 8; i++) {
    const glm::dvec3 c((i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? z1 : z0);
    const glm::dvec3 r = c - fr.p;
    const double t = glm::dot(r, fr.d);
    t0 = std::min(t0, t);
    t1 = std::max(t1, t);
    rMax = std::max(rMax, glm::length(r - t * fr.d));
  }
  const double margin = 1.0;
  rMax += margin;
  t0 -= margin;
  t1 += margin;

  std::vector<std::vector<Trace>> at(samples);
  try {
    for (int k = 0; k < samples; k++) {
      if (cancel && cancel->load())
        return std::unexpected("cancelled");
      at[k] = TracesAt(fr, 2.0 * M_PI * k / samples, rMax, t0, t1,
                       anyTouch ? TopoDS_Shape(touch) : TopoDS_Shape(),
                       anyOverlap ? TopoDS_Shape(overlap) : TopoDS_Shape());
    }
  } catch (const Standard_Failure& e) {
    return std::unexpected(std::string("the kernel failed cutting the "
                                       "contact: ") +
                           e.GetMessageString());
  }

  // Where there is no trace at all.
  for (int k = 0; k < samples; k++) {
    if (!at[k].empty())
      continue;
    const double deg = 360.0 * k / samples, stepDeg = 360.0 / samples;
    if (!rep.open.empty() &&
        std::abs(rep.open.back().toDeg - deg) < 1e-9 + stepDeg)
      rep.open.back().toDeg = deg;
    else
      rep.open.push_back({deg, deg});
  }
  // An arc that runs through 0 is one arc.
  if (rep.open.size() > 1 && rep.open.front().fromDeg == 0.0 &&
      std::abs(rep.open.back().toDeg - (360.0 - 360.0 / samples)) < 1e-9) {
    rep.open.front().fromDeg = rep.open.back().fromDeg - 360.0;
    rep.open.pop_back();
  }

  // The rings: from each trace on the first half-plane, the path round
  // whose narrowest trace is widest (each trace touching the next), and
  // whether it comes back to the trace it left.
  const double pad = std::max(10.0 * touchTolMm, 0.02);
  struct Best {
    double width = -1.0, depth = 0.0;
    int atK = 0;
    double sSum = 0.0, tSum = 0.0;
  };
  for (size_t start = 0; start < at[0].size(); start++) {
    std::vector<Best> cur(at[0].size());
    const Trace& s = at[0][start];
    cur[start] = {s.widthMm, s.depthMm, 0, 0.5 * (s.s0 + s.s1),
                  0.5 * (s.t0 + s.t1)};
    for (int k = 1; k <= samples; k++) {
      const std::vector<Trace>& prev = at[k - 1];
      const std::vector<Trace>& now = at[k % samples];
      std::vector<Best> next(now.size());
      for (size_t j = 0; j < now.size(); j++)
        for (size_t i = 0; i < prev.size(); i++) {
          if (cur[i].width < 0.0 || !Touch(prev[i], now[j], pad))
            continue;
          const double w = k == samples
                               ? cur[i].width
                               : std::min(cur[i].width, now[j].widthMm);
          if (w <= next[j].width)
            continue;
          next[j] = cur[i];
          next[j].width = w;
          if (k < samples) {
            if (now[j].widthMm < cur[i].width)
              next[j].atK = k;
            next[j].depth = std::min(cur[i].depth, now[j].depthMm);
            next[j].sSum += 0.5 * (now[j].s0 + now[j].s1);
            next[j].tSum += 0.5 * (now[j].t0 + now[j].t1);
          }
        }
      cur = std::move(next);
    }
    if (start < cur.size() && cur[start].width > 0.0) {
      SealRing r;
      r.leastWidthMm = cur[start].width;
      r.atDeg = 360.0 * cur[start].atK / samples;
      r.leastInterferenceMm = cur[start].depth;
      r.radiusMm = cur[start].sSum / samples;
      r.centre = fr.p + (cur[start].tSum / samples) * fr.d;
      rep.rings.push_back(r);
    }
  }
  std::sort(rep.rings.begin(), rep.rings.end(),
            [](const SealRing& a, const SealRing& b) {
              return a.leastWidthMm > b.leastWidthMm;
            });
  rep.roundButOpen = rep.rings.empty() && rep.open.empty();
  return rep;
}

} // namespace Measure
