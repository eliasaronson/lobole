// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/PrintCheck.h"

#include "core/Feature.h"
#include "core/Measure.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>

namespace printcheck {

const char* StatusName(Status s) {
  switch (s) {
  case Status::Pass:
    return "pass";
  case Status::Fail:
    return "fail";
  case Status::Skipped:
    break;
  }
  return "skipped";
}

bool Report::Ok() const {
  for (Status s :
       {bed.status, shells.status, overhang.status, bridges.status,
        clearance.status, chords.status, stacking.status, bedFit.status})
    if (s == Status::Fail)
      return false;
  return true;
}

namespace {

constexpr double kPi = 3.14159265358979323846;
double Deg(double rad) { return rad * 180.0 / kPi; }

glm::dvec3 V(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
glm::dvec3 V(const gp_Dir& d) { return {d.X(), d.Y(), d.Z()}; }

// The angle of a surface from vertical, for an OUTWARD normal `n`: 0 a wall,
// 90 a ceiling, negative facing up. asin keeps a 45° flank at 45.
double OverhangDeg(const glm::dvec3& n, const glm::dvec3& up) {
  return Deg(std::asin(std::clamp(-glm::dot(n, up), -1.0, 1.0)));
}

// The lowest point of `shape` along `up`, exact on the B-Rep: rotated so up
// is +Z, then boxed optimally without its triangulation or tolerances.
double LowestAlong(const TopoDS_Shape& shape, const glm::dvec3& up) {
  gp_Trsf t;
  const glm::dvec3 z(0.0, 0.0, 1.0);
  if (glm::length(up - z) > 1e-12) {
    if (glm::length(up + z) < 1e-12)
      t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), kPi);
    else
      t.SetRotation(gp_Quaternion(gp_Vec(up.x, up.y, up.z), gp_Vec(0, 0, 1)));
  }
  Bnd_Box box;
  BRepBndLib::AddOptimal(shape.Moved(TopLoc_Location(t)), box, false, false);
  if (box.IsVoid())
    throw std::runtime_error("a body has no extent");
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  return z0;
}

// A face's surface, evaluated: the outward normal and the point at (u, v).
struct FaceEval {
  TopoDS_Face face;
  BRepAdaptor_Surface surf;
  bool reversed = false;
  BRepTopAdaptor_FClass2d cls;
  double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
  explicit FaceEval(const TopoDS_Face& f)
      : face(f), surf(f), reversed(f.Orientation() == TopAbs_REVERSED),
        cls(f, Precision::PConfusion()) {
    BRepTools::UVBounds(f, u0, u1, v0, v1);
  }
  bool Inside(double u, double v) {
    if (u < u0 || u > u1 || v < v0 || v > v1)
      return false;
    const TopAbs_State s = cls.Perform(gp_Pnt2d(u, v));
    return s == TopAbs_IN || s == TopAbs_ON;
  }
  bool At(double u, double v, glm::dvec3& p, glm::dvec3& n) {
    BRepLProp_SLProps props(surf, u, v, 1, Precision::Confusion());
    if (!props.IsNormalDefined())
      return false;
    p = V(props.Value());
    n = V(props.Normal());
    if (reversed)
      n = -n;
    return true;
  }
};

// Climb from (u, v) on the face's own domain to where `score` is largest,
// halving the step when no neighbour improves. The samples it starts from
// are a tessellation's nodes; this is what takes the answer off them.
template <class Score>
double Climb(FaceEval& fe, double& u, double& v, double du, double dv,
             Score score) {
  double best = score(u, v);
  for (int it = 0; it < 60 && (du > 1e-12 || dv > 1e-12); ++it) {
    bool moved = false;
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy) {
        if (dx == 0 && dy == 0)
          continue;
        const double nu = u + dx * du, nv = v + dy * dv;
        if (!fe.Inside(nu, nv))
          continue;
        const double s = score(nu, nv);
        if (s > best) {
          best = s;
          u = nu;
          v = nv;
          moved = true;
        }
      }
    if (!moved) {
      du *= 0.5;
      dv *= 0.5;
    }
  }
  return best;
}

double TriArea(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c) {
  return 0.5 * glm::length(glm::cross(b - a, c - a));
}

double FaceArea(const TopoDS_Face& f) {
  GProp_GProps g;
  BRepGProp::SurfaceProperties(f, g);
  return std::abs(g.Mass());
}

double SegDist(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b) {
  const glm::dvec3 ab = b - a;
  const double l2 = glm::dot(ab, ab);
  const double t =
      l2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
  return glm::length(p - (a + t * ab));
}

// The widest span of a ceiling: twice the largest distance from a point of
// the face to its boundary, the inscribed circle a bridge crosses between
// edges it is anchored on. A grid over the face's UV, distances in 3D, then
// a climb from its best point; the boundary is its edges at
// `chordMm`, so the span is short by at most twice that.
Bridge CeilingSpan(FaceEval& fe, double chordMm) {
  std::vector<std::pair<glm::dvec3, glm::dvec3>> segs;
  for (TopExp_Explorer e(fe.face, TopAbs_EDGE); e.More(); e.Next()) {
    const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
    if (BRep_Tool::Degenerated(edge))
      continue;
    BRepAdaptor_Curve c(edge);
    GCPnts_QuasiUniformDeflection pts(c, chordMm);
    if (!pts.IsDone() || pts.NbPoints() < 2)
      continue;
    for (int i = 1; i < pts.NbPoints(); ++i)
      segs.emplace_back(V(pts.Value(i)), V(pts.Value(i + 1)));
  }
  Bridge out;
  if (segs.empty())
    return out;
  const auto dist = [&](double u, double v) {
    glm::dvec3 p, n;
    if (!fe.At(u, v, p, n))
      return -1.0;
    double d = std::numeric_limits<double>::infinity();
    for (const auto& [a, b] : segs)
      d = std::min(d, SegDist(p, a, b));
    return d;
  };
  constexpr int kGrid = 48;
  const double du = (fe.u1 - fe.u0) / kGrid, dv = (fe.v1 - fe.v0) / kGrid;
  double best = -1.0, bu = 0.0, bv = 0.0;
  for (int i = 0; i <= kGrid; ++i)
    for (int j = 0; j <= kGrid; ++j) {
      const double u = fe.u0 + i * du, v = fe.v0 + j * dv;
      if (!fe.Inside(u, v))
        continue;
      const double d = dist(u, v);
      if (d > best) {
        best = d;
        bu = u;
        bv = v;
      }
    }
  if (best < 0.0)
    return out;
  best = Climb(fe, bu, bv, du, dv, dist);
  glm::dvec3 p, n;
  fe.At(bu, bv, p, n);
  out.spanMm = 2.0 * best;
  out.where.point = p;
  return out;
}

void CheckFaces(const std::vector<const Body*>& bodies, const Options& o,
                double bedMm, Report& r, std::vector<TopoDS_Shape>& meshed) {
  const glm::dvec3 up = o.up;
  const double limit = o.maxOverhangDeg;
  for (size_t bi = 0; bi < bodies.size(); ++bi) {
    const TopoDS_Shape& src = bodies[bi]->shape.GetShape();
    glm::dvec3 lo, hi;
    bodies[bi]->shape.GetBoundingBox(lo, hi);
    const double diag = glm::length(hi - lo);
    // A copy, meshed: BRepMesh writes its triangulation into the shape, and
    // the document's is what the renderer draws. The mesh only places the
    // samples; every normal is the surface's.
    BRepBuilderAPI_Copy copy(src, true, false);
    const TopoDS_Shape& shape = copy.Shape();
    const double lin = std::max(1e-3, 0.002 * diag);
    BRepMesh_IncrementalMesh mesh(shape, lin, false, 0.25, false);
    meshed.push_back(shape); // stacking samples the same tessellation
    int faceIndex = -1;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
      ++faceIndex;
      const TopoDS_Face& face = TopoDS::Face(ex.Current());
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) tri =
          BRep_Tool::Triangulation(face, loc);
      if (tri.IsNull() || !tri->HasUVNodes())
        continue;
      FaceEval fe(face);
      const gp_Trsf tr = loc.Transformation();
      const int nn = tri->NbNodes();
      std::vector<glm::dvec3> nodes(nn);
      std::vector<double> h(nn);
      double hMin = std::numeric_limits<double>::infinity();
      for (int i = 1; i <= nn; ++i) {
        nodes[i - 1] = V(tri->Node(i).Transformed(tr));
        h[i - 1] = glm::dot(up, nodes[i - 1]);
        hMin = std::min(hMin, h[i - 1]);
      }
      const bool plane = fe.surf.GetType() == GeomAbs_Plane;
      if (plane) {
        // One normal: the face is the answer, whole.
        glm::dvec3 p, n;
        const gp_Pnt2d uv = tri->UVNode(1);
        if (!fe.At(uv.X(), uv.Y(), p, n))
          continue;
        const double ang = OverhangDeg(n, up);
        if (ang <= 0.0)
          continue;
        const double hMax = *std::max_element(h.begin(), h.end());
        if (hMax <= bedMm + kBedTolMm) {
          if (ang >= 90.0 - kCeilingDeg)
            r.bed.contactAreaMm2 += FaceArea(face);
          continue; // on the bed
        }
        if (ang >= 90.0 - kCeilingDeg) {
          Bridge b = CeilingSpan(fe, std::max(1e-3, lin / 4.0));
          b.where.body = static_cast<int>(bi);
          b.where.face = faceIndex;
          b.areaMm2 = FaceArea(face);
          b.heightMm = hMin - bedMm;
          r.bridges.areaMm2 += b.areaMm2;
          r.bridges.faces.push_back(b);
          continue;
        }
        // A sloped plane: exact, and whole. Its worst point is its lowest.
        size_t low = 0;
        for (size_t i = 1; i < h.size(); ++i)
          if (h[i] < h[low])
            low = i;
        if (ang > r.overhang.worstDeg) {
          r.overhang.worstDeg = ang;
          r.overhang.worst = Where{static_cast<int>(bi), faceIndex, nodes[low]};
        }
        if (ang > limit + kAngleEpsDeg) {
          // Less what lies on the bed, a triangle at a time.
          double area = FaceArea(face), onBed = 0.0;
          for (int t = 1; t <= tri->NbTriangles(); ++t) {
            int a, b, c;
            tri->Triangle(t).Get(a, b, c);
            if (std::max({h[a - 1], h[b - 1], h[c - 1]}) <= bedMm + kBedTolMm)
              onBed += TriArea(nodes[a - 1], nodes[b - 1], nodes[c - 1]);
          }
          area = std::max(0.0, area - onBed);
          if (area > 0.0) {
            r.overhang.areaMm2 += area;
            r.overhang.faces.push_back(
                {Where{static_cast<int>(bi), faceIndex, nodes[low]}, ang,
                 area});
          }
        }
        continue;
      }
      // A curved face: the exact normal at every node and every triangle's
      // centroid, the area past the limit by triangle, then a climb from the
      // worst sample.
      double faceWorst = -90.0, faceLeast = 90.0, faceArea = 0.0, wu = 0.0,
             wv = 0.0;
      int sampled = 0;
      glm::dvec3 wp(0.0);
      const auto sample = [&](double u, double v, double height) {
        glm::dvec3 p, n;
        if (height <= bedMm + kBedTolMm || !fe.At(u, v, p, n))
          return -90.0;
        const double ang = OverhangDeg(n, up);
        faceLeast = std::min(faceLeast, ang);
        sampled++;
        if (ang > faceWorst) {
          faceWorst = ang;
          wu = u;
          wv = v;
          wp = p;
        }
        return ang;
      };
      for (int i = 1; i <= nn; ++i) {
        const gp_Pnt2d uv = tri->UVNode(i);
        sample(uv.X(), uv.Y(), h[i - 1]);
      }
      double duMax = 0.0, dvMax = 0.0;
      for (int t = 1; t <= tri->NbTriangles(); ++t) {
        int a, b, c;
        tri->Triangle(t).Get(a, b, c);
        const gp_Pnt2d ua = tri->UVNode(a), ub = tri->UVNode(b),
                       uc = tri->UVNode(c);
        duMax =
            std::max({duMax, std::abs(ua.X() - ub.X()),
                      std::abs(ub.X() - uc.X()), std::abs(uc.X() - ua.X())});
        dvMax =
            std::max({dvMax, std::abs(ua.Y() - ub.Y()),
                      std::abs(ub.Y() - uc.Y()), std::abs(uc.Y() - ua.Y())});
        if (std::max({h[a - 1], h[b - 1], h[c - 1]}) <= bedMm + kBedTolMm)
          continue; // on the bed
        const double ang = sample((ua.X() + ub.X() + uc.X()) / 3.0,
                                  (ua.Y() + ub.Y() + uc.Y()) / 3.0,
                                  std::numeric_limits<double>::infinity());
        if (ang > limit + kAngleEpsDeg)
          faceArea += TriArea(nodes[a - 1], nodes[b - 1], nodes[c - 1]);
      }
      if (faceWorst <= 0.0)
        continue; // nothing on it faces down
      if (sampled > 0 && faceLeast >= 90.0 - kCeilingDeg) {
        // Flat and horizontal wherever it is sampled, whatever its surface
        // type (a derived or lofted face is often a B-spline): a ceiling.
        Bridge b = CeilingSpan(fe, std::max(1e-3, lin / 4.0));
        b.where.body = static_cast<int>(bi);
        b.where.face = faceIndex;
        b.areaMm2 = FaceArea(face);
        b.heightMm = hMin - bedMm;
        r.bridges.areaMm2 += b.areaMm2;
        r.bridges.faces.push_back(b);
        continue;
      }
      // Refine only where the answer could cross the limit or be the worst.
      if (faceWorst > limit - 10.0 || faceWorst > r.overhang.worstDeg) {
        double u = wu, v = wv;
        Climb(fe, u, v, std::max(duMax, 1e-9) / 2.0,
              std::max(dvMax, 1e-9) / 2.0, [&](double cu, double cv) {
                glm::dvec3 p, n;
                if (!fe.At(cu, cv, p, n) ||
                    glm::dot(up, p) <= bedMm + kBedTolMm)
                  return -90.0;
                return OverhangDeg(n, up);
              });
        sample(u, v, std::numeric_limits<double>::infinity());
      }
      if (faceWorst > r.overhang.worstDeg) {
        r.overhang.worstDeg = faceWorst;
        r.overhang.worst = Where{static_cast<int>(bi), faceIndex, wp};
      }
      if (faceArea > 0.0) {
        r.overhang.areaMm2 += faceArea;
        r.overhang.faces.push_back(
            {Where{static_cast<int>(bi), faceIndex, wp}, faceWorst, faceArea});
      } else if (faceWorst > limit + kAngleEpsDeg) {
        r.overhang.faces.push_back(
            {Where{static_cast<int>(bi), faceIndex, wp}, faceWorst, 0.0});
      }
    }
  }
}

// An axis-aligned box in the bodies' frame.
struct Box3 {
  glm::dvec3 lo{std::numeric_limits<double>::infinity()};
  glm::dvec3 hi{-std::numeric_limits<double>::infinity()};
  void Add(const glm::dvec3& p) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  void Add(const Box3& b) {
    lo = glm::min(lo, b.lo);
    hi = glm::max(hi, b.hi);
  }
  [[nodiscard]] bool Overlaps(const Box3& b, double pad) const {
    for (int k = 0; k < 3; ++k)
      if (lo[k] > b.hi[k] + pad || b.lo[k] > hi[k] + pad)
        return false;
    return true;
  }
  // Does the segment a -> b pass within `pad` of the box? Slabs.
  [[nodiscard]] bool Crosses(const glm::dvec3& a, const glm::dvec3& b,
                             double pad) const {
    double t0 = 0.0, t1 = 1.0;
    const glm::dvec3 d = b - a;
    for (int k = 0; k < 3; ++k) {
      const double l = lo[k] - pad, h = hi[k] + pad;
      if (std::abs(d[k]) < 1e-15) {
        if (a[k] < l || a[k] > h)
          return false;
        continue;
      }
      double ta = (l - a[k]) / d[k], tb = (h - a[k]) / d[k];
      if (ta > tb)
        std::swap(ta, tb);
      t0 = std::max(t0, ta);
      t1 = std::min(t1, tb);
      if (t0 > t1)
        return false;
    }
    return true;
  }
};

// One solid of the print, as the stacking item sees it: its meshed copy, the
// piece it belongs to and its box.
struct StackUnit {
  int body = 0, piece = 0;
  TopoDS_Shape shape;
  Box3 box;
  std::vector<int> around; // units within the limit of its box, itself too
  bool otherNear = false;  // one of them is another piece
  double lowestH = std::numeric_limits<double>::infinity();
  Where lowest; // the lowest tessellation node, and its face
};

// What the air under one point of a downward face is: the piece below
// within the limit (-1 for none: nothing, or the point's own piece first),
// and how far.
struct AirSample {
  int piece = -1;
  double airMm = 0.0;
  glm::dvec3 p{0.0};
};

// Where a ray down first meets material. Only a surface that faces UP can be
// met first from outside, so only those triangles are kept: binned on a grid
// in the plane across the print direction, a ray is a lookup and a few 2D
// point-in-triangle tests. The triangle says which face is met and where on
// it; the air is then taken off the face's own surface, closed-form on a
// plane and by Newton from the triangle's UV on anything else, so only the
// face's boundary is the tessellation's.
class RayDown {
public:
  RayDown(const glm::dvec3& up, double cell) : up_(up), cell_(cell) {
    e1_ = glm::normalize(std::abs(up.x) < 0.9
                             ? glm::cross(up, glm::dvec3(1, 0, 0))
                             : glm::cross(up, glm::dvec3(0, 1, 0)));
    e2_ = glm::cross(up, e1_);
  }
  // A face's upward triangles, of unit `unit`.
  void AddFace(int unit, const TopoDS_Face& face,
               const Handle(Poly_Triangulation) & tri, const gp_Trsf& tr) {
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    const int f = static_cast<int>(faces_.size());
    faces_.push_back({face, nullptr, false});
    const int base = static_cast<int>(pos_.size());
    for (int i = 1; i <= tri->NbNodes(); ++i) {
      const glm::dvec3 p = V(tri->Node(i).Transformed(tr));
      pos_.push_back(p);
      const gp_Pnt2d uv = tri->HasUVNodes() ? tri->UVNode(i) : gp_Pnt2d();
      uv_.push_back({uv.X(), uv.Y()});
    }
    for (int t = 1; t <= tri->NbTriangles(); ++t) {
      int a, b, c;
      tri->Triangle(t).Get(a, b, c);
      if (reversed)
        std::swap(b, c);
      const glm::dvec3 &pa = pos_[base + a - 1], &pb = pos_[base + b - 1],
                       &pc = pos_[base + c - 1];
      if (glm::dot(glm::cross(pb - pa, pc - pa), up_) <= 0.0)
        continue; // faces down or sideways: never met first from outside
      const int id = static_cast<int>(tris_.size());
      tris_.push_back({{base + a - 1, base + b - 1, base + c - 1}, f, unit});
      double x0 = std::numeric_limits<double>::infinity(), y0 = x0;
      double x1 = -x0, y1 = -x0;
      for (const glm::dvec3* q : {&pa, &pb, &pc}) {
        x0 = std::min(x0, glm::dot(*q, e1_));
        x1 = std::max(x1, glm::dot(*q, e1_));
        y0 = std::min(y0, glm::dot(*q, e2_));
        y1 = std::max(y1, glm::dot(*q, e2_));
      }
      for (long long i = Cell(x0); i <= Cell(x1); ++i)
        for (long long j = Cell(y0); j <= Cell(y1); ++j)
          bins_.emplace_back(Key(i, j), id);
    }
  }
  void Build() { std::sort(bins_.begin(), bins_.end()); }

  // The nearest material straight down from `p` with tMin < t < tMax: its
  // unit and the distance, or unit -1.
  std::pair<int, double> Cast(const glm::dvec3& p, double tMin, double tMax) {
    const double x = glm::dot(p, e1_), y = glm::dot(p, e2_),
                 h = glm::dot(p, up_);
    const long long k = Key(Cell(x), Cell(y));
    auto it = std::lower_bound(bins_.begin(), bins_.end(),
                               std::pair<long long, int>(k, -1));
    int bestTri = -1;
    double best = tMax;
    glm::dvec3 bestBary(0.0);
    for (; it != bins_.end() && it->first == k; ++it) {
      const Tri& t = tris_[it->second];
      const glm::dvec3 &a = pos_[t.n[0]], &b = pos_[t.n[1]], &c = pos_[t.n[2]];
      const glm::dvec2 a2(glm::dot(a, e1_), glm::dot(a, e2_)),
          b2(glm::dot(b, e1_), glm::dot(b, e2_)),
          c2(glm::dot(c, e1_), glm::dot(c, e2_));
      const double den =
          (b2.y - c2.y) * (a2.x - c2.x) + (c2.x - b2.x) * (a2.y - c2.y);
      if (std::abs(den) < 1e-18)
        continue;
      const double l0 =
          ((b2.y - c2.y) * (x - c2.x) + (c2.x - b2.x) * (y - c2.y)) / den;
      const double l1 =
          ((c2.y - a2.y) * (x - c2.x) + (a2.x - c2.x) * (y - c2.y)) / den;
      const double l2 = 1.0 - l0 - l1;
      constexpr double eps = -1e-9;
      if (l0 < eps || l1 < eps || l2 < eps)
        continue;
      const double hit =
          l0 * glm::dot(a, up_) + l1 * glm::dot(b, up_) + l2 * glm::dot(c, up_);
      const double tt = h - hit;
      if (tt > tMin && tt < best) {
        best = tt;
        bestTri = it->second;
        bestBary = {l0, l1, l2};
      }
    }
    if (bestTri < 0)
      return {-1, tMax};
    const Tri& t = tris_[bestTri];
    // Off the face's own surface.
    Face& f = faces_[t.face];
    if (!f.surf) {
      f.surf = std::make_unique<BRepAdaptor_Surface>(f.face);
      f.plane = f.surf->GetType() == GeomAbs_Plane;
    }
    double exact = best;
    if (f.plane) {
      const gp_Pln pl = f.surf->Plane();
      const gp_Dir& n = pl.Axis().Direction();
      const double dn = -(n.X() * up_.x + n.Y() * up_.y + n.Z() * up_.z);
      if (std::abs(dn) > 1e-12)
        exact =
            gp_Vec(n).Dot(gp_Vec(gp_Pnt(p.x, p.y, p.z), pl.Location())) / dn;
    } else {
      glm::dvec2 uv = bestBary.x * uv_[t.n[0]] + bestBary.y * uv_[t.n[1]] +
                      bestBary.z * uv_[t.n[2]];
      double tt = best;
      bool ok = false;
      for (int it2 = 0; it2 < 12; ++it2) {
        gp_Pnt s;
        gp_Vec su, sv;
        f.surf->D1(uv.x, uv.y, s, su, sv);
        // S(u, v) - (p - t up) = 0, for u, v and t.
        const glm::dvec3 r = V(s) - (p - tt * up_);
        const glm::dvec3 cu(su.X(), su.Y(), su.Z()), cv(sv.X(), sv.Y(), sv.Z());
        const glm::dmat3 J(cu, cv, up_);
        if (std::abs(glm::determinant(J)) < 1e-14)
          break;
        const glm::dvec3 step = glm::inverse(J) * r;
        uv -= glm::dvec2(step.x, step.y);
        tt -= step.z;
        if (glm::length(r) < 1e-9) {
          ok = true;
          break;
        }
      }
      if (ok && std::abs(tt - best) < 4.0 * cell_)
        exact = tt;
    }
    if (exact > tMin && exact < tMax)
      best = exact;
    else if (!(best > tMin && best < tMax))
      return {-1, tMax};
    return {t.unit, best};
  }

private:
  struct Tri {
    int n[3];
    int face, unit;
  };
  struct Face {
    TopoDS_Face face;
    std::unique_ptr<BRepAdaptor_Surface> surf;
    bool plane = false;
  };
  long long Cell(double v) const {
    return static_cast<long long>(std::floor(v / cell_));
  }
  static long long Key(long long i, long long j) {
    return (i + (1LL << 30)) * (1LL << 31) + (j + (1LL << 30));
  }
  glm::dvec3 up_, e1_, e2_;
  double cell_;
  std::vector<glm::dvec3> pos_;
  std::vector<glm::dvec2> uv_;
  std::vector<Tri> tris_;
  std::vector<Face> faces_;
  std::vector<std::pair<long long, int>> bins_;
};

// The stacking item (docs/tolerances/README.md#checking-a-print): every
// piece starts on the bed, and no downward face lies within the minimum air
// over another piece. The tessellation CheckFaces made places the samples:
// each is a point of a downward face's own surface, at a UV a triangle
// interpolates, where the exact normal faces down; the air under it is a
// ray down to the nearest material (RayDown). A triangle is split while it
// is wider than four limits, or while its samples disagree and it is wider
// than a sixteenth of one; the area under the limit is the leaves whose
// centroid is.
void CheckStacking(const std::vector<const Body*>& bodies,
                   const std::vector<TopoDS_Shape>& meshed,
                   const std::vector<std::pair<int, int>>& unitBodySolid,
                   const std::vector<int>& unitPiece, int pieces,
                   const Options& o, double bedMm, Report& r) {
  Stacking& st = r.stacking;
  const glm::dvec3 up = o.up;
  const double limit = *o.minAirMm;
  st.limitMm = limit;
  st.toleranceMm = kGapToleranceShare * limit;
  const double failBelow = limit - st.toleranceMm;
  const int nu = static_cast<int>(unitBodySolid.size());
  const int nb = static_cast<int>(bodies.size());

  // The units over the meshed copies, in the order Check made them.
  std::vector<StackUnit> units(nu);
  std::vector<std::vector<int>> unitOf(nb); // body -> its units by solid
  for (int i = 0; i < nu; ++i) {
    units[i].body = unitBodySolid[i].first;
    units[i].piece = unitPiece[i];
    unitOf[units[i].body].push_back(i);
  }
  // Each body's solids, and which of them each face is on.
  std::vector<std::vector<TopTools_IndexedMapOfShape>> faceMaps(nb);
  for (int b = 0; b < nb; ++b) {
    std::vector<TopoDS_Shape> solids;
    for (TopExp_Explorer ex(meshed[b], TopAbs_SOLID); ex.More(); ex.Next())
      solids.push_back(ex.Current());
    if (solids.empty())
      solids.push_back(meshed[b]);
    for (size_t k = 0; k < solids.size() && k < unitOf[b].size(); ++k) {
      StackUnit& u = units[unitOf[b][k]];
      u.shape = solids[k];
      Bnd_Box bb;
      BRepBndLib::Add(u.shape, bb, false);
      if (!bb.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        u.box.Add({x0, y0, z0});
        u.box.Add({x1, y1, z1});
      }
      faceMaps[b].emplace_back();
      TopExp::MapShapes(u.shape, TopAbs_FACE, faceMaps[b].back());
    }
  }
  for (int i = 0; i < nu; ++i)
    for (int j = 0; j < nu; ++j)
      if (i == j || units[i].box.Overlaps(units[j].box, limit)) {
        units[i].around.push_back(j);
        if (units[j].piece != units[i].piece)
          units[i].otherNear = true;
      }

  // The pieces, with their bodies and lowest points.
  st.pieces.resize(pieces);
  std::vector<double> pieceLow(pieces, std::numeric_limits<double>::infinity());
  for (int i = 0; i < nu; ++i) {
    Piece& pc = st.pieces[units[i].piece];
    if (std::find(pc.bodies.begin(), pc.bodies.end(), units[i].body) ==
        pc.bodies.end())
      pc.bodies.push_back(units[i].body);
    pieceLow[units[i].piece] =
        std::min(pieceLow[units[i].piece],
                 units[i].shape.IsNull() ? r.bodies[units[i].body].lowestMm
                                         : LowestAlong(units[i].shape, up));
  }
  for (Piece& pc : st.pieces)
    std::sort(pc.bodies.begin(), pc.bodies.end());

  // Every face once: its unit, its lowest node, its upward triangles for
  // the rays, and whether it is worth sampling.
  struct ToSample {
    int body, faceIndex, unit;
    TopoDS_Face face;
  };
  std::vector<ToSample> todo;
  RayDown rays(up, std::max(limit / 2.0, 1e-3));
  for (int b = 0; b < nb; ++b) {
    int faceIndex = -1;
    for (TopExp_Explorer ex(meshed[b], TopAbs_FACE); ex.More(); ex.Next()) {
      ++faceIndex;
      const TopoDS_Face& face = TopoDS::Face(ex.Current());
      int ui = -1;
      for (size_t k = 0; k < faceMaps[b].size(); ++k)
        if (faceMaps[b][k].Contains(face)) {
          ui = unitOf[b][k];
          break;
        }
      if (ui < 0)
        continue;
      StackUnit& u = units[ui];
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) tri =
          BRep_Tool::Triangulation(face, loc);
      if (tri.IsNull() || !tri->HasUVNodes())
        continue;
      const gp_Trsf tr = loc.Transformation();
      Box3 fbox;
      for (int i = 1; i <= tri->NbNodes(); ++i) {
        const glm::dvec3 p = V(tri->Node(i).Transformed(tr));
        fbox.Add(p);
        if (glm::dot(up, p) < u.lowestH) {
          u.lowestH = glm::dot(up, p);
          u.lowest = Where{b, faceIndex, p};
        }
      }
      rays.AddFace(ui, face, tri, tr);
      if (!u.otherNear)
        continue;
      // Only a face with another piece's box in the column under it.
      Box3 column = fbox;
      column.Add(Box3{fbox.lo - limit * up, fbox.hi - limit * up});
      for (int v : u.around)
        if (units[v].piece != u.piece && column.Overlaps(units[v].box, 1e-6)) {
          todo.push_back({b, faceIndex, ui, face});
          break;
        }
    }
  }
  rays.Build();

  const double res = limit / 16.0, spacing = 4.0 * limit;
  const double tMin = 1e-4; // past the point's own surface
  std::map<std::pair<int, int>, Stacked> pairs;
  for (const ToSample& ts : todo) {
    const StackUnit& u = units[ts.unit];
    const int b = ts.body, faceIndex = ts.faceIndex;
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri =
        BRep_Tool::Triangulation(ts.face, loc);
    const gp_Trsf tr = loc.Transformation();
    const int nn = tri->NbNodes();
    std::vector<glm::dvec3> nodes(nn);
    std::vector<double> h(nn);
    for (int i = 1; i <= nn; ++i) {
      nodes[i - 1] = V(tri->Node(i).Transformed(tr));
      h[i - 1] = glm::dot(up, nodes[i - 1]);
    }
    FaceEval fe(ts.face);
    const auto air = [&](const glm::dvec2& uv) {
      AirSample s;
      glm::dvec3 n;
      if (!fe.At(uv.x, uv.y, s.p, n) || OverhangDeg(n, up) <= 1e-6 ||
          glm::dot(up, s.p) <= bedMm + kBedTolMm)
        return s;
      st.samples++;
      const auto [hit, t] = rays.Cast(s.p, tMin, limit);
      if (hit >= 0 && units[hit].piece != u.piece) {
        s.piece = units[hit].piece;
        s.airMm = t;
        Stacked& pr = pairs[{u.piece, s.piece}];
        if (pr.above < 0 || t < pr.minAirMm) {
          pr.above = u.piece;
          pr.below = s.piece;
          pr.minAirMm = t;
          pr.where = Where{b, faceIndex, s.p};
        }
      }
      return s;
    };
    // Under the limit: the piece below; anything else, -1.
    const auto key = [&](const AirSample& s) {
      return s.piece >= 0 && s.airMm < failBelow ? s.piece : -1;
    };
    struct Corner {
      glm::dvec3 p;  // on the triangle, for its size and area
      glm::dvec2 uv; // on the face
      AirSample s;
    };
    const auto mid = [&](const Corner& a, const Corner& c) {
      Corner m{0.5 * (a.p + c.p), 0.5 * (a.uv + c.uv), {}};
      m.s = air(m.uv);
      return m;
    };
    std::function<void(const Corner&, const Corner&, const Corner&, int)>
        visit = [&](const Corner& a, const Corner& b2, const Corner& c,
                    int depth) {
          const double edge =
              std::max({glm::length(b2.p - a.p), glm::length(c.p - b2.p),
                        glm::length(a.p - c.p)});
          const AirSample cs = air((a.uv + b2.uv + c.uv) / 3.0);
          const int k = key(cs);
          const bool uniform = key(a.s) == k && key(b2.s) == k && key(c.s) == k;
          if (depth < 14 && (edge > spacing || (!uniform && edge > res))) {
            const Corner ab = mid(a, b2), bc = mid(b2, c), ca = mid(c, a);
            visit(a, ab, ca, depth + 1);
            visit(ab, b2, bc, depth + 1);
            visit(ca, bc, c, depth + 1);
            visit(ab, bc, ca, depth + 1);
            return;
          }
          if (k >= 0)
            pairs[{u.piece, k}].areaMm2 += TriArea(a.p, b2.p, c.p);
        };
    // Each node once: neighbouring triangles share it.
    std::vector<std::optional<Corner>> at(nn);
    const auto node = [&](int i) -> const Corner& {
      if (!at[i - 1]) {
        const gp_Pnt2d uv = tri->UVNode(i);
        Corner c{nodes[i - 1], {uv.X(), uv.Y()}, {}};
        c.s = air(c.uv);
        at[i - 1] = c;
      }
      return *at[i - 1];
    };
    for (int t = 1; t <= tri->NbTriangles(); ++t) {
      int ia, ib, ic;
      tri->Triangle(t).Get(ia, ib, ic);
      if (std::max({h[ia - 1], h[ib - 1], h[ic - 1]}) <= bedMm + kBedTolMm)
        continue; // on the bed
      // A triangle with no other piece's box in the column under it is
      // clear without a sample.
      Box3 tcol;
      for (int i : {ia, ib, ic}) {
        tcol.Add(nodes[i - 1]);
        tcol.Add(nodes[i - 1] - limit * up);
      }
      bool below = false;
      for (int v : u.around)
        if (units[v].piece != u.piece && tcol.Overlaps(units[v].box, 1e-6))
          below = true;
      if (!below)
        continue;
      visit(node(ia), node(ib), node(ic), 0);
    }
  }

  // Pieces that start in the air.
  for (int pc = 0; pc < pieces; ++pc) {
    st.pieces[pc].lowestMm = pieceLow[pc] - bedMm;
    if (st.pieces[pc].lowestMm <= kBedTolMm)
      continue;
    Lifted l;
    l.piece = pc;
    l.heightMm = st.pieces[pc].lowestMm;
    double best = std::numeric_limits<double>::infinity();
    for (const StackUnit& u : units)
      if (u.piece == pc && u.lowest.body >= 0 && u.lowestH < best) {
        best = u.lowestH;
        l.where = u.lowest;
      }
    st.lifted.push_back(l);
  }
  std::sort(
      st.lifted.begin(), st.lifted.end(),
      [](const Lifted& a, const Lifted& b) { return a.heightMm > b.heightMm; });
  bool close = false;
  for (auto& [k, pr] : pairs) {
    close = close || pr.minAirMm < failBelow;
    st.pairs.push_back(pr);
  }
  std::sort(st.pairs.begin(), st.pairs.end(),
            [](const Stacked& a, const Stacked& b) {
              return a.minAirMm < b.minAirMm;
            });
  st.status = st.lifted.empty() && !close ? Status::Pass : Status::Fail;
  st.note =
      "every piece's lowest point from the B-Rep; the air under each "
      "downward face by rays down from points of the face's own surface "
      "that a tessellation places (split to a sixteenth of the limit where "
      "the answer changes): the face a ray meets is the tessellation's, the "
      "air is taken off that face's exact surface; the area is the leaves "
      "whose centre is under the limit";
}

} // namespace

Report Check(const std::vector<const Body*>& bodies, const Options& o) {
  if (bodies.empty())
    throw std::runtime_error("no bodies to check");
  if (!(glm::length(o.up) > 0.5) || !std::isfinite(o.up.x + o.up.y + o.up.z))
    throw std::runtime_error("the print direction is not a unit vector");
  if (!(o.maxOverhangDeg > 0.0 && o.maxOverhangDeg < 90.0))
    throw std::runtime_error("the overhang limit is not in (0, 90)");
  if (o.minAirMm && !(std::isfinite(*o.minAirMm) && *o.minAirMm > 0.0))
    throw std::runtime_error("the minimum air is not more than 0");
  for (const Body* b : bodies)
    if (b == nullptr || b->shape.GetShape().IsNull())
      throw std::runtime_error("a body has no shape");
  const glm::dvec3 up = glm::normalize(o.up);
  Options opts = o;
  opts.up = up;

  Report r;
  const int n = static_cast<int>(bodies.size());
  r.bodies.resize(n);

  // Bed.
  double bed = std::numeric_limits<double>::infinity();
  for (int i = 0; i < n; ++i) {
    const TopoDS_Shape& s = bodies[i]->shape.GetShape();
    BodyInfo& bi = r.bodies[i];
    for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next())
      bi.solids++;
    const Measure::ShellReport sr =
        Measure::AnalyzeWatertight(bodies[i]->shape);
    bi.watertight = sr.watertight;
    bi.freeEdges = sr.freeEdges;
    bi.lowestMm = LowestAlong(s, up);
    bed = std::min(bed, bi.lowestMm);
  }
  r.bed.lowestMm = bed;
  for (int i = 0; i < n; ++i)
    if (r.bodies[i].lowestMm <= bed + kBedTolMm)
      r.bed.onBed.push_back(i);
  r.bed.status = std::abs(bed) <= kBedTolMm ? Status::Pass : Status::Fail;

  // Overhangs and bridges, measured from the part's own lowest point: a part
  // off the bed is the bed item's to fail, not every face's.
  r.overhang.limitDeg = o.maxOverhangDeg;
  r.overhang.allowanceMm2 = o.overhangAllowanceMm2;
  r.bridges.limitMm = o.maxBridgeMm;
  std::vector<TopoDS_Shape> meshed;
  CheckFaces(bodies, opts, bed, r, meshed);
  std::sort(r.overhang.faces.begin(), r.overhang.faces.end(),
            [](const FaceOver& a, const FaceOver& b) {
              return a.worstDeg > b.worstDeg;
            });
  r.overhang.status =
      r.overhang.areaMm2 > o.overhangAllowanceMm2 ? Status::Fail : Status::Pass;
  r.overhang.note =
      "angles are the B-Rep's surface normals; the area past the limit is "
      "the triangles of a tessellation at 0.2% of each body's size, each "
      "classified by the exact normal at its centroid";
  if (r.overhang.areaMm2 <= 0.0 &&
      r.overhang.worstDeg > o.maxOverhangDeg + kAngleEpsDeg)
    r.overhang.note += "; the worst point is past the limit on a sliver "
                       "smaller than one triangle";
  std::sort(
      r.bridges.faces.begin(), r.bridges.faces.end(),
      [](const Bridge& a, const Bridge& b) { return a.spanMm > b.spanMm; });
  if (!r.bridges.faces.empty())
    r.bridges.worstSpanMm = r.bridges.faces.front().spanMm;
  r.bridges.status =
      r.bridges.worstSpanMm > o.maxBridgeMm ? Status::Fail : Status::Pass;
  r.bridges.note =
      "a ceiling is a downward face within 1° of horizontal "
      "everywhere, off the bed; its span is its inscribed diameter, as if "
      "anchored along its whole edge";

  // Clearance: every pair of solids, closest first -- a body may be several
  // (an extrude of loose discs is one body). The pieces follow from the
  // same distances.
  const double required = o.gapSideMm.value_or(0.0);
  // A pair whose oriented boxes are four gaps apart is clear without
  // measuring; unprinted, only touching matters.
  const double prune = required > 0.0 ? 4.0 * required : 2.0 * kTouchMm;
  struct Unit {
    int body = 0, solid = 0;
    std::vector<Body> one; // what Measure's set queries take
  };
  std::vector<Unit> units;
  for (int i = 0; i < n; ++i) {
    const TopoDS_Shape& s = bodies[i]->shape.GetShape();
    int k = 0;
    for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next(), ++k) {
      Unit u{i, k, {}};
      Body b;
      b.name = bodies[i]->name;
      b.createdBy = bodies[i]->createdBy;
      b.shape = CADShape(ex.Current());
      u.one.push_back(std::move(b));
      units.push_back(std::move(u));
    }
    if (k == 0) // no solid: the shape is the one piece it has
      units.push_back(Unit{i, 0, {*bodies[i]}});
    if (k > 1)
      r.shells.split.push_back(i);
  }
  const int nu = static_cast<int>(units.size());
  const glm::dmat4 I(1.0);
  std::vector<std::pair<int, int>> touching;
  for (int i = 0; i < nu; ++i)
    for (int j = i + 1; j < nu; ++j) {
      if (Measure::SeparatedByOBB(units[i].one, I, units[j].one, I, prune)) {
        r.clearance.pruned++;
        continue;
      }
      Pair pr;
      pr.a = units[i].body;
      pr.b = units[j].body;
      pr.solidA = units[i].solid;
      pr.solidB = units[j].solid;
      if (const auto c =
              Measure::ClearanceBetween(units[i].one, I, units[j].one, I)) {
        pr.distanceMm = c->distance;
      } else {
        const auto full =
            Measure::ContactBetween(units[i].one, I, units[j].one, I, true);
        if (!full)
          throw std::runtime_error("the distance between two bodies could "
                                   "not be measured");
        pr.overlap = full->overlaps;
        pr.distanceMm = full->overlaps ? 0.0 : full->distance;
      }
      if (pr.distanceMm <= kTouchMm)
        touching.emplace_back(i, j);
      r.clearance.pairs.push_back(pr);
    }
  std::sort(
      r.clearance.pairs.begin(), r.clearance.pairs.end(),
      [](const Pair& a, const Pair& b) { return a.distanceMm < b.distanceMm; });
  if (!r.clearance.pairs.empty())
    r.clearance.minMm = r.clearance.pairs.front().distanceMm;

  // Pieces: solids within kTouchMm print as one.
  std::vector<int> parent(nu);
  std::iota(parent.begin(), parent.end(), 0);
  const auto find = [&](int x) {
    while (parent[x] != x)
      x = parent[x] = parent[parent[x]];
    return x;
  };
  for (const auto& [a, b] : touching)
    parent[find(a)] = find(b);
  std::vector<std::vector<int>> groups(nu);
  for (int i = 0; i < nu; ++i)
    groups[find(i)].push_back(i);
  int pieces = 0;
  std::vector<int> unitPiece(nu, -1);
  for (const auto& g : groups) {
    if (g.empty())
      continue;
    for (int u : g)
      unitPiece[u] = pieces;
    pieces++;
    if (g.size() > 1) {
      std::vector<int> joined;
      for (int u : g)
        if (std::find(joined.begin(), joined.end(), units[u].body) ==
            joined.end())
          joined.push_back(units[u].body);
      r.shells.joined.push_back(joined);
    }
  }
  r.shells.watertight = true;
  for (int i = 0; i < n; ++i)
    r.shells.watertight = r.shells.watertight && r.bodies[i].watertight;
  r.shells.pieces = pieces;
  r.shells.expected = nu;
  r.shells.status =
      pieces == nu && r.shells.watertight ? Status::Pass : Status::Fail;

  // Stacking: the pieces again, against the minimum air.
  if (o.minAirMm) {
    std::vector<std::pair<int, int>> unitBodySolid;
    for (const Unit& u : units)
      unitBodySolid.emplace_back(u.body, u.solid);
    CheckStacking(bodies, meshed, unitBodySolid, unitPiece, pieces, opts, bed,
                  r);
  } else {
    r.stacking.note = "no minimum air to judge a part printed over another "
                      "by (set_print_process, or state min_air_mm)";
  }

  // Bed fit: the plate's box in the printer's frame -- x on the bed (the
  // world's x, or y when the print direction is x), up the print direction
  // -- against the build volume.
  if (o.bedMm) {
    const glm::dvec3 world_x(1.0, 0.0, 0.0), world_y(0.0, 1.0, 0.0);
    glm::dvec3 bx = world_x - glm::dot(world_x, up) * up;
    if (glm::length(bx) < 1e-6)
      bx = world_y - glm::dot(world_y, up) * up;
    bx = glm::normalize(bx);
    const glm::dvec3 by = glm::cross(up, bx);
    gp_Trsf t;
    t.SetValues(bx.x, bx.y, bx.z, 0.0, by.x, by.y, by.z, 0.0, up.x, up.y, up.z,
                0.0);
    Bnd_Box box;
    for (const Body* b : bodies)
      BRepBndLib::AddOptimal(b->shape.GetShape().Moved(TopLoc_Location(t)), box,
                             false, false);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    BedFit& f = r.bedFit;
    f.bedMm = *o.bedMm;
    f.extentMm = {x1 - x0, y1 - y0, z1 - z0};
    bool fits = true;
    for (int k = 0; k < 3; k++) {
      f.shortfallMm[k] = std::max(0.0, f.extentMm[k] - f.bedMm[k]);
      if (f.extentMm[k] > f.bedMm[k] + kBedTolMm)
        fits = false;
    }
    f.status = fits ? Status::Pass : Status::Fail;
    f.note = "the plate's box, x along the bed's x and z up the print "
             "direction, against the build volume: a slicer places the "
             "plate, so it fits when its box does";
  } else {
    r.bedFit.note = o.bedNote.empty()
                        ? std::string("no build volume to fit the plate in")
                        : o.bedNote;
  }

  if (!o.gapSideMm) {
    r.clearance.note = "the part is not printed: no gap to hold the bodies "
                       "apart (set_print_process, or state min_gap_mm)";
    r.chords.note = r.clearance.note;
  } else {
    r.clearance.requiredMm = required;
    r.clearance.toleranceMm = kGapToleranceShare * required;
    const bool clear = !r.clearance.minMm ||
                       *r.clearance.minMm >= required - r.clearance.toleranceMm;
    r.clearance.status = clear ? Status::Pass : Status::Fail;
    r.clearance.note =
        n < 2 ? "one body: nothing to keep apart"
              : "every pair by BRepExtrema; pairs whose oriented boxes are "
                "further apart than the gap are clear without it";
    r.chords.limitMm = kChordShare * required;
    if (o.stlDeflectionMm) {
      r.chords.deflectionMm = *o.stlDeflectionMm;
      r.chords.status = *o.stlDeflectionMm <= r.chords.limitMm + 1e-12
                            ? Status::Pass
                            : Status::Fail;
      r.chords.note = "an STL's chords sit up to their deviation inside a "
                      "round surface, taking it off the gap";
    } else {
      r.chords.note = "no chord deviation to judge";
    }
  }
  return r;
}

} // namespace printcheck
