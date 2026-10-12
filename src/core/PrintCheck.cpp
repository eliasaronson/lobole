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
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
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
  for (Status s : {bed.status, shells.status, overhang.status, bridges.status,
                   clearance.status, chords.status})
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
    throw std::invalid_argument("a body has no extent");
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
                double bedMm, Report& r) {
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

} // namespace

Report Check(const std::vector<const Body*>& bodies, const Options& o) {
  if (bodies.empty())
    throw std::invalid_argument("no bodies to check");
  if (!(glm::length(o.up) > 0.5) || !std::isfinite(o.up.x + o.up.y + o.up.z))
    throw std::invalid_argument("the print direction is not a unit vector");
  if (!(o.maxOverhangDeg > 0.0 && o.maxOverhangDeg < 90.0))
    throw std::invalid_argument("the overhang limit is not in (0, 90)");
  for (const Body* b : bodies)
    if (b == nullptr || b->shape.GetShape().IsNull())
      throw std::invalid_argument("a body has no shape");
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
  CheckFaces(bodies, opts, bed, r);
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
  for (const auto& g : groups) {
    if (g.empty())
      continue;
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
