// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cad/MeshRebuild.h"

#include "core/mesh/MeshReaders.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Shell.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Line.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SphericalSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Shell.hxx>
#include <ShapeFix_Wire.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_DataMapOfShapeInteger.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace cad {

namespace {

constexpr double kTwoPi = 6.283185307179586;
// Rounds of demoting what a rebuild faulted before the mesh is kept as
// facets: each is a whole rebuild.
constexpr int kDemoteRounds = 6;

gp_Pnt Pnt(const glm::dvec3& p) { return {p.x, p.y, p.z}; }
gp_Dir Dir(const glm::dvec3& d) { return {d.x, d.y, d.z}; }
glm::dvec3 Vec(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }

std::string Num(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

// A unit vector square to `a`.
glm::dvec3 Square(const glm::dvec3& a) {
  const glm::dvec3 helper = std::abs(a.x) < 0.6   ? glm::dvec3(1, 0, 0)
                            : std::abs(a.y) < 0.6 ? glm::dvec3(0, 1, 0)
                                                  : glm::dvec3(0, 0, 1);
  return glm::normalize(glm::cross(a, helper));
}

struct HalfEdge {
  uint32_t tri = mesh::kNone;
  int edge = 0;
};

struct Chain {
  std::vector<uint32_t> verts; // first to last; a closed chain repeats it
  uint32_t left = mesh::kNone; // the region that traced it forwards
  uint32_t right = mesh::kNone;
  bool closed = false;
  TopoDS_Edge edge;
};

struct LoopUse {
  uint32_t chain;
  bool reversed;
};

class Builder {
public:
  Builder(const mesh::TriMesh& m, const mesh::Segmentation& s)
      : m_mesh(m), m_seg(s) {}

  Rebuilt Run();

private:
  uint32_t RegionOf(uint32_t t) const { return m_seg.regionOf[t]; }
  uint32_t Across(const HalfEdge& h) const {
    return m_mesh.neighbour[h.tri][h.edge];
  }
  bool IsBoundary(const HalfEdge& h) const {
    const uint32_t u = Across(h);
    return u == mesh::kNone || RegionOf(u) != RegionOf(h.tri);
  }
  uint32_t From(const HalfEdge& h) const {
    return m_mesh.triangles[h.tri][h.edge];
  }
  uint32_t To(const HalfEdge& h) const {
    return m_mesh.triangles[h.tri][(h.edge + 1) % 3];
  }
  // The boundary half-edge of the same region that leaves where `h` ends,
  // found by turning about that vertex inside the region.
  HalfEdge Next(const HalfEdge& h) const;
  void Trace();
  glm::dvec3 Snap(uint32_t v) const;
  bool BuildEdges(std::string& why);
  // The face of region `r`; null with `why` when it will not build.
  TopoDS_Face BuildFace(uint32_t r, std::string& why) const;
  // The curved regions to demote for a fault in region `r`'s face: `r`
  // itself when curved, else the curved regions across its edges, whose
  // curves bound it.
  void Blame(uint32_t r, std::vector<uint32_t>& out) const;

  const mesh::TriMesh& m_mesh;
  const mesh::Segmentation& m_seg;
  double m_tol = 0.0;
  std::vector<std::vector<uint32_t>> m_regionsAt; // per vertex, ascending
  std::vector<bool> m_corner;                     // per vertex
  std::vector<uint32_t> m_chainOf;                // per half-edge (t*3+e)
  std::vector<Chain> m_chains;
  std::vector<std::vector<std::vector<LoopUse>>> m_loops; // per region
  std::map<uint32_t, TopoDS_Vertex> m_vertices;
  // A ring's seam: its two rim loops, and the vertex it leaves the first at
  // and reaches the second.
  struct RingSeam {
    size_t loopA = 0, loopB = 0;
    uint32_t vA = 0, vB = 0;
  };
  std::map<uint32_t, RingSeam> m_seams;
  // What fixing a face replaced (an edge split at a seam it added), applied
  // to the faces either side so they still share it.
  Handle(ShapeBuild_ReShape) m_context = new ShapeBuild_ReShape;
};

HalfEdge Builder::Next(const HalfEdge& h) const {
  const uint32_t region = RegionOf(h.tri);
  HalfEdge cur{h.tri, (h.edge + 1) % 3};
  for (size_t guard = 0; guard < m_mesh.triangles.size() + 3; guard++) {
    if (IsBoundary(cur))
      return cur;
    const uint32_t u = Across(cur);
    const int f = m_mesh.neighbourEdge[cur.tri][cur.edge];
    (void)region;
    cur = {u, (f + 1) % 3};
  }
  return {};
}

void Builder::Trace() {
  const size_t nv = m_mesh.positions.size();
  m_regionsAt.assign(nv, {});
  std::vector<uint32_t> outgoing(nv, 0);
  for (uint32_t t = 0; t < m_mesh.triangles.size(); t++)
    for (int c = 0; c < 3; c++) {
      auto& at = m_regionsAt[m_mesh.triangles[t][c]];
      const uint32_t r = RegionOf(t);
      if (std::find(at.begin(), at.end(), r) == at.end())
        at.push_back(r);
      if (IsBoundary({t, c}))
        outgoing[From({t, c})]++;
    }
  m_corner.assign(nv, false);
  for (uint32_t v = 0; v < nv; v++) {
    std::sort(m_regionsAt[v].begin(), m_regionsAt[v].end());
    const size_t n = m_regionsAt[v].size();
    // Three regions meet, or two meet more than once (a pinch).
    m_corner[v] = n >= 3 || (n == 2 && outgoing[v] != 2);
  }

  // Every region's boundary, as loops of half-edges.
  std::vector<std::vector<std::vector<HalfEdge>>> raw(m_seg.regions.size());
  std::vector<bool> traced(m_mesh.triangles.size() * 3, false);
  for (uint32_t r = 0; r < m_seg.regions.size(); r++)
    for (uint32_t t : m_seg.regions[r].triangles)
      for (int e = 0; e < 3; e++) {
        const HalfEdge start{t, e};
        if (traced[t * 3 + e] || !IsBoundary(start))
          continue;
        std::vector<HalfEdge> loop;
        for (HalfEdge h = start;;) {
          traced[h.tri * 3 + h.edge] = true;
          loop.push_back(h);
          h = Next(h);
          if (h.tri == mesh::kNone || loop.size() > m_mesh.triangles.size() * 3)
            break; // not a surface: Run finds the shell open
          if (h.tri == start.tri && h.edge == start.edge)
            break;
        }
        raw[r].push_back(std::move(loop));
      }

  // A cylinder or cone all the way round needs a seam: a straight edge
  // along it from one rim to the other, landing on a vertex of each. The
  // mesh already has one -- a generator, a run of the region's own edges
  // along one line -- and both its ends become corners, so the rims are cut
  // there for the face either side alike.
  const auto wraps = [&](const mesh::FitResult& f,
                         const std::vector<HalfEdge>& loop) {
    double turn = 0.0;
    for (const HalfEdge& h : loop) {
      glm::dvec3 a = m_mesh.positions[From(h)] - f.origin,
                 b = m_mesh.positions[To(h)] - f.origin;
      a -= glm::dot(a, f.axis) * f.axis;
      b -= glm::dot(b, f.axis) * f.axis;
      turn += std::atan2(glm::dot(glm::cross(a, b), f.axis), glm::dot(a, b));
    }
    return std::abs(turn) > kTwoPi / 2;
  };
  std::vector<std::vector<uint32_t>> around(nv);
  for (uint32_t t = 0; t < m_mesh.triangles.size(); t++)
    for (int c = 0; c < 3; c++)
      around[m_mesh.triangles[t][c]].push_back(t);
  // The generator from `v` within region `r` to a vertex in `target`.
  const auto generator = [&](uint32_t r, uint32_t v,
                             const std::vector<bool>& target) -> uint32_t {
    const mesh::FitResult& f = m_seg.regions[r].fit;
    glm::dvec3 dir = f.axis;
    if (f.kind == mesh::Kind::Cone) {
      dir = m_mesh.positions[v] - f.origin;
      if (glm::length(dir) == 0.0)
        return mesh::kNone;
      dir = glm::normalize(dir);
    }
    uint32_t at = v, from = mesh::kNone;
    for (size_t step = 0; step < nv; step++) {
      uint32_t next = mesh::kNone;
      for (uint32_t t : around[at]) {
        if (RegionOf(t) != r)
          continue;
        for (int c = 0; c < 3 && next == mesh::kNone; c++) {
          const uint32_t w = m_mesh.triangles[t][c];
          if (w == at || w == from)
            continue;
          const glm::dvec3 d = m_mesh.positions[w] - m_mesh.positions[at];
          const double len = glm::length(d);
          if (len > 0.0 &&
              glm::length(glm::cross(d / len, dir)) * len <= 2.0 * m_tol &&
              (from == mesh::kNone ||
               glm::dot(d, m_mesh.positions[at] - m_mesh.positions[from]) >
                   0.0))
            next = w;
        }
        if (next != mesh::kNone)
          break;
      }
      if (next == mesh::kNone)
        return mesh::kNone;
      if (target[next])
        return next;
      if (m_regionsAt[next].size() > 1)
        return mesh::kNone; // reached some other boundary
      from = at;
      at = next;
    }
    return mesh::kNone;
  };
  std::vector<bool> onA(nv, false), onB(nv, false);
  for (uint32_t r = 0; r < m_seg.regions.size(); r++) {
    const mesh::FitResult& f = m_seg.regions[r].fit;
    if (f.kind != mesh::Kind::Cylinder && f.kind != mesh::Kind::Cone)
      continue;
    std::vector<size_t> wrapping;
    for (size_t i = 0; i < raw[r].size(); i++)
      if (wraps(f, raw[r][i]))
        wrapping.push_back(i);
    if (wrapping.size() != 2)
      continue;
    std::vector<uint32_t> candidates;
    for (const HalfEdge& h : raw[r][wrapping[0]]) {
      candidates.push_back(From(h));
      onA[From(h)] = true;
    }
    for (const HalfEdge& h : raw[r][wrapping[1]])
      onB[From(h)] = true;
    // A corner first (no rim is cut that need not be), then the lowest.
    std::sort(candidates.begin(), candidates.end(),
              [&](uint32_t a, uint32_t b) {
                return m_corner[a] != m_corner[b] ? m_corner[a] : a < b;
              });
    for (uint32_t v : candidates) {
      const uint32_t w = generator(r, v, onB);
      if (w == mesh::kNone)
        continue;
      m_corner[v] = m_corner[w] = true;
      m_seams[r] = {wrapping[0], wrapping[1], v, w};
      break;
    }
    for (const HalfEdge& h : raw[r][wrapping[0]])
      onA[From(h)] = false;
    for (const HalfEdge& h : raw[r][wrapping[1]])
      onB[From(h)] = false;
  }
  // A loop still without a corner is one closed edge, started at its
  // lowest vertex, which the region across finds as well.
  for (auto& loops : raw)
    for (auto& loop : loops) {
      uint32_t lowest = From(loop.front());
      bool any = false;
      for (const HalfEdge& h : loop) {
        any = any || m_corner[From(h)];
        lowest = std::min(lowest, From(h));
      }
      if (!any)
        m_corner[lowest] = true;
    }

  // Chains: each loop cut at its corners; a chain the region across has
  // already made is used reversed.
  m_chainOf.assign(m_mesh.triangles.size() * 3, mesh::kNone);
  m_loops.assign(m_seg.regions.size(), {});
  for (uint32_t r = 0; r < m_seg.regions.size(); r++)
    for (auto& loop : raw[r]) {
      size_t first = loop.size();
      for (size_t i = 0; i < loop.size() && first == loop.size(); i++)
        if (m_corner[From(loop[i])])
          first = i;
      if (first == loop.size())
        first = 0; // cannot happen: every cycle has a seam vertex
      std::rotate(loop.begin(), loop.begin() + first, loop.end());
      std::vector<LoopUse> uses;
      for (size_t i = 0; i < loop.size();) {
        size_t j = i + 1;
        while (j < loop.size() && !m_corner[From(loop[j])])
          j++;
        const HalfEdge& h0 = loop[i];
        const uint32_t across = Across(h0);
        const uint32_t known =
            across == mesh::kNone
                ? mesh::kNone
                : m_chainOf[across * 3 + m_mesh.neighbourEdge[h0.tri][h0.edge]];
        if (known != mesh::kNone) {
          uses.push_back({known, true});
        } else {
          Chain c;
          c.left = r;
          c.right = across == mesh::kNone ? mesh::kNone : RegionOf(across);
          c.verts.push_back(From(h0));
          for (size_t k = i; k < j; k++)
            c.verts.push_back(To(loop[k]));
          c.closed = c.verts.front() == c.verts.back();
          m_chains.push_back(std::move(c));
          uses.push_back({static_cast<uint32_t>(m_chains.size() - 1), false});
        }
        for (size_t k = i; k < j; k++)
          m_chainOf[loop[k].tri * 3 + loop[k].edge] = uses.back().chain;
        i = j;
      }
      m_loops[r].push_back(std::move(uses));
    }
}

glm::dvec3 Builder::Snap(uint32_t v) const {
  // Alternating projection onto every surface the vertex is on: to their
  // common line (two) or point (three), from a start already within the
  // file's precision of it.
  glm::dvec3 p = m_mesh.positions[v];
  const auto& regions = m_regionsAt[v];
  if (regions.size() < 2)
    return p;
  for (int it = 0; it < 32; it++) {
    const glm::dvec3 was = p;
    for (uint32_t r : regions)
      p = m_seg.regions[r].fit.Project(p);
    if (glm::length(p - was) < 1e-3 * m_tol)
      break;
  }
  // A projection that wandered (tangent surfaces converge slowly) is not
  // better than where the file put it.
  return glm::length(p - m_mesh.positions[v]) <= 4.0 * m_tol
             ? p
             : m_mesh.positions[v];
}

// The circle through `pts` (closed or an arc), within `tol` of every one.
bool CircleThrough(const std::vector<glm::dvec3>& pts, double tol,
                   glm::dvec3& centre, glm::dvec3& normal, double& radius) {
  if (pts.size() < 3)
    return false;
  const mesh::FitResult plane = mesh::FitPlane(pts);
  if (!plane.ok || plane.max > tol)
    return false;
  normal = plane.axis;
  const glm::dvec3 e1 = Square(normal), e2 = glm::cross(normal, e1);
  // x^2 + y^2 + D x + E y + F = 0, least squares (Kasa), about the centroid.
  double a[3][3] = {}, b[3] = {};
  for (const glm::dvec3& p : pts) {
    const glm::dvec3 w = p - plane.origin;
    const double x = glm::dot(w, e1), y = glm::dot(w, e2);
    const double row[3] = {x, y, 1.0}, rhs = -(x * x + y * y);
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++)
        a[i][j] += row[i] * row[j];
      b[i] += row[i] * rhs;
    }
  }
  // Cramer's rule on the 3x3.
  const auto det = [](double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  };
  const double d = det(a);
  if (std::abs(d) < 1e-300)
    return false;
  double sol[3];
  for (int k = 0; k < 3; k++) {
    double m[3][3];
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
        m[i][j] = j == k ? b[i] : a[i][j];
    sol[k] = det(m) / d;
  }
  const double cx = -sol[0] / 2, cy = -sol[1] / 2;
  const double r2 = cx * cx + cy * cy - sol[2];
  if (!(r2 > 0.0))
    return false;
  radius = std::sqrt(r2);
  centre = plane.origin + cx * e1 + cy * e2;
  for (const glm::dvec3& p : pts)
    if (std::abs(glm::length(p - centre) - radius) > tol)
      return false;
  return true;
}

bool Builder::BuildEdges(std::string& why) {
  BRep_Builder bb;
  const auto vertex = [&](uint32_t v) -> TopoDS_Vertex {
    auto it = m_vertices.find(v);
    if (it != m_vertices.end())
      return it->second;
    TopoDS_Vertex vx;
    bb.MakeVertex(vx, Pnt(Snap(v)), 2.0 * m_tol);
    return m_vertices.emplace(v, vx).first->second;
  };
  for (Chain& c : m_chains) {
    std::vector<glm::dvec3> pts;
    pts.reserve(c.verts.size());
    for (uint32_t v : c.verts)
      pts.push_back(Snap(v));
    const TopoDS_Vertex v0 = vertex(c.verts.front());
    const TopoDS_Vertex v1 = vertex(c.verts.back());
    const glm::dvec3 a = Vec(BRep_Tool::Pnt(v0)), b = Vec(BRep_Tool::Pnt(v1));
    try {
      // A line: the two ends, every point between within tolerance of it.
      if (!c.closed) {
        bool straight = glm::length(b - a) > 0.0;
        const glm::dvec3 dir = straight ? glm::normalize(b - a) : glm::dvec3(0);
        for (size_t i = 1; i + 1 < pts.size() && straight; i++) {
          const glm::dvec3 w = pts[i] - a;
          straight = glm::length(w - glm::dot(w, dir) * dir) <= m_tol;
        }
        if (straight) {
          BRepBuilderAPI_MakeEdge me(v0, v1);
          if (!me.IsDone()) {
            why = "an edge of " + std::to_string(pts.size()) +
                  " points would not build as a line";
            return false;
          }
          c.edge = me.Edge();
          continue;
        }
      }
      glm::dvec3 centre, normal;
      double radius = 0.0;
      if (CircleThrough(pts, m_tol, centre, normal, radius)) {
        // The circle a rim IS: square to the axis of the round surface it
        // bounds, centred on that axis, at that surface's radius. A circle
        // fitted to the points tilts by their noise, and a tilted rim is not
        // an iso-line of the surface it bounds, which an edit that extends
        // the surface from its rim reads as a curved boundary
        // (directedit's neighbours).
        for (const uint32_t side : {c.left, c.right}) {
          if (side == mesh::kNone)
            continue;
          const mesh::FitResult& f = m_seg.regions[side].fit;
          if (f.kind != mesh::Kind::Cylinder && f.kind != mesh::Kind::Cone &&
              f.kind != mesh::Kind::Sphere)
            continue;
          glm::dvec3 axis = f.kind == mesh::Kind::Sphere
                                ? glm::normalize(centre - f.origin)
                                : f.axis;
          if (f.kind == mesh::Kind::Sphere &&
              glm::length(centre - f.origin) < m_tol)
            axis = normal; // a great circle: the fit's own plane
          if (std::abs(glm::dot(axis, normal)) < 1.0 - 1e-6)
            continue; // not square to it: an ellipse's worth of tilt
          const glm::dvec3 onAxis =
              f.origin + axis * glm::dot(centre - f.origin, axis);
          double r = radius;
          if (f.kind == mesh::Kind::Cylinder)
            r = f.radius;
          else if (f.kind == mesh::Kind::Sphere)
            r = std::sqrt(std::max(
                0.0, f.radius * f.radius -
                         glm::dot(onAxis - f.origin, onAxis - f.origin)));
          else // a cone: its radius where the rim's plane cuts it
            r = std::tan(f.halfAngle) *
                std::abs(glm::dot(onAxis - f.origin, axis));
          bool holds = r > 0.0;
          for (size_t i = 0; i < pts.size() && holds; i++) {
            const glm::dvec3 w = pts[i] - onAxis;
            holds = std::abs(glm::dot(w, axis)) <= m_tol &&
                    std::abs(glm::length(w - glm::dot(w, axis) * axis) - r) <=
                        m_tol;
          }
          if (!holds)
            continue;
          centre = onAxis;
          normal = glm::dot(axis, normal) > 0.0 ? axis : -axis;
          radius = r;
          break;
        }
        // Turned so the chain runs the circle's way, from its first point.
        double sweep = 0.0;
        for (size_t i = 0; i + 1 < pts.size(); i++) {
          const glm::dvec3 p = pts[i] - centre, q = pts[i + 1] - centre;
          sweep +=
              std::atan2(glm::dot(glm::cross(p, q), normal), glm::dot(p, q));
        }
        if (sweep < 0.0) {
          normal = -normal;
          sweep = -sweep;
        }
        const glm::dvec3 x = glm::normalize(pts.front() - centre);
        const gp_Circ circ(gp_Ax2(Pnt(centre), Dir(normal), Dir(x)), radius);
        Handle(Geom_Circle) curve = new Geom_Circle(circ);
        const double end = c.closed ? kTwoPi : sweep;
        BRepBuilderAPI_MakeEdge me(curve, v0, v1, 0.0, end);
        if (!me.IsDone()) {
          why = "an arc of radius " + Num(radius) + " mm would not build";
          return false;
        }
        c.edge = me.Edge();
        continue;
      }
      // Anything else: a B-spline within the tolerance of the points --
      // an approximation, not an interpolation, so a boundary that wanders
      // along a tangent junction is not followed wiggle by wiggle (a wire
      // that crossed itself) and the curve stays small for the checks.
      if (pts.size() < 2) {
        why = "an edge has too few points to make a curve";
        return false;
      }
      TColgp_Array1OfPnt arr(1, static_cast<int>(pts.size()));
      for (size_t i = 0; i < pts.size(); i++)
        arr.SetValue(static_cast<int>(i + 1), Pnt(pts[i]));
      GeomAPI_PointsToBSpline approx(arr, 3, 8, GeomAbs_C2, 2.0 * m_tol);
      if (!approx.IsDone()) {
        why = "an edge through " + std::to_string(pts.size()) +
              " points would not approximate";
        return false;
      }
      Handle(Geom_BSplineCurve) curve = approx.Curve();
      BRepBuilderAPI_MakeEdge me(curve, v0, v1, curve->FirstParameter(),
                                 curve->LastParameter());
      if (!me.IsDone()) {
        why = "a curved edge through " + std::to_string(pts.size()) +
              " points would not build";
        return false;
      }
      c.edge = me.Edge();
    } catch (const Standard_Failure& f) {
      why = std::string("an edge would not build: ") + f.GetMessageString();
      return false;
    }
  }
  return true;
}

TopoDS_Face Builder::BuildFace(uint32_t r, std::string& why) const {
  const mesh::Region& region = m_seg.regions[r];
  const mesh::FitResult& f = region.fit;
  // The surface, its seam turned away from the region: the X direction
  // points opposite the region's middle, or, for a full turn, along the
  // ring's seam edge, so u = 0 is the seam as a modeller's is. A ring's
  // middle is the triangulation's noise, and a seam left at some other u
  // differs from the u = 0 a face extended from this surface (Offset Face's
  // strip) is cut at: OCC 7.6 then cannot merge the strip back into it.
  glm::dvec3 middle(0.0);
  for (uint32_t t : region.triangles)
    for (int c = 0; c < 3; c++) {
      const glm::dvec3 w = m_mesh.Corner(t, c) - f.origin;
      middle +=
          f.kind == mesh::Kind::Sphere ? w : w - glm::dot(w, f.axis) * f.axis;
    }
  Handle(Geom_Surface) surface;
  try {
    switch (f.kind) {
    case mesh::Kind::Plane:
      surface = new Geom_Plane(
          gp_Ax3(Pnt(f.origin), Dir(f.axis), Dir(Square(f.axis))));
      break;
    case mesh::Kind::Cylinder:
    case mesh::Kind::Cone: {
      glm::dvec3 x = -middle;
      if (const auto ring = m_seams.find(r); ring != m_seams.end())
        x = m_mesh.positions[ring->second.vA] - f.origin;
      x -= glm::dot(x, f.axis) * f.axis;
      if (glm::length(x) < 1e-6 * glm::length(middle) + 1e-300) {
        const uint32_t v =
            m_chains[m_loops[r].front().front().chain].verts.front();
        x = m_mesh.positions[v] - f.origin;
        x -= glm::dot(x, f.axis) * f.axis;
      }
      if (glm::length(x) == 0.0)
        x = Square(f.axis);
      // A cylinder's frame at the region's lowest point along its axis, so
      // its faces run from v = 0 as a modeller's do (a cone keeps its apex,
      // where its radius is the zero the frame needs).
      glm::dvec3 at = f.origin;
      if (f.kind == mesh::Kind::Cylinder) {
        double low = 0.0;
        bool first = true;
        for (uint32_t t : region.triangles)
          for (int c = 0; c < 3; c++) {
            const double h = glm::dot(m_mesh.Corner(t, c) - f.origin, f.axis);
            low = first ? h : std::min(low, h);
            first = false;
          }
        at = f.origin + f.axis * low;
      }
      const gp_Ax3 ax(Pnt(at), Dir(f.axis), Dir(glm::normalize(x)));
      if (f.kind == mesh::Kind::Cylinder)
        surface = new Geom_CylindricalSurface(ax, f.radius);
      else
        surface = new Geom_ConicalSurface(ax, f.halfAngle, 0.0);
      break;
    }
    case mesh::Kind::Sphere: {
      // A patch (a corner blend) has its poles and seam well away from it:
      // the equator through its middle, the seam behind. A dome reaches
      // round to its rim on every side, so a pole goes in its middle,
      // where the fix closes it with a degenerate edge, rather than on its
      // rim.
      const glm::dvec3 m = glm::length(middle) > 0.0 ? glm::normalize(middle)
                                                     : glm::dvec3(0, 0, 1);
      double reach = 0.0; // the widest angle from the middle to a corner
      for (uint32_t t : region.triangles)
        for (int c = 0; c < 3; c++) {
          const glm::dvec3 w = m_mesh.Corner(t, c) - f.origin;
          const double len = glm::length(w);
          if (len > 0.0)
            reach = std::max(
                reach, std::acos(std::clamp(glm::dot(w / len, m), -1.0, 1.0)));
        }
      const gp_Ax3 frame = reach < kTwoPi / 6 // within 60° of its middle
                               ? gp_Ax3(Pnt(f.origin), Dir(Square(m)), Dir(-m))
                               : gp_Ax3(Pnt(f.origin), Dir(m), Dir(Square(m)));
      surface = new Geom_SphericalSurface(frame, f.radius);
      break;
    }
    }
  } catch (const Standard_Failure& e) {
    why = std::string("its surface would not build: ") + e.GetMessageString();
    return {};
  }
  // Which way the triangles face, against the surface's own normal.
  const uint32_t t0 = region.triangles.front();
  const glm::dvec3 centroid =
      (m_mesh.Corner(t0, 0) + m_mesh.Corner(t0, 1) + m_mesh.Corner(t0, 2)) /
      3.0;
  GeomAPI_ProjectPointOnSurf proj(Pnt(centroid), surface);
  if (!proj.IsDone() || proj.NbPoints() == 0) {
    why = "a triangle does not project onto its surface";
    return {};
  }
  double u = 0.0, v = 0.0;
  proj.LowerDistanceParameters(u, v);
  GeomLProp_SLProps props(surface, u, v, 1, Precision::Confusion());
  if (!props.IsNormalDefined()) {
    why = "its surface has no normal where a triangle is";
    return {};
  }
  const gp_Dir sn = props.Normal();
  const bool agree =
      glm::dot(glm::dvec3(sn.X(), sn.Y(), sn.Z()), m_mesh.AreaNormal(t0)) > 0.0;

  BRep_Builder bb;
  TopoDS_Face face;
  bb.MakeFace(face, surface, m_tol);
  const auto addUses = [&](TopoDS_Wire& wire,
                           const std::vector<LoopUse>& loop) {
    for (const LoopUse& use : loop) {
      const TopoDS_Edge& e = m_chains[use.chain].edge;
      bb.Add(wire, use.reversed ? TopoDS::Edge(e.Reversed()) : e);
    }
  };
  const auto addWire = [&](TopoDS_Wire& wire) {
    wire.Closed(true);
    bb.Add(face, agree ? wire : TopoDS::Wire(wire.Reversed()));
  };
  const auto seam = m_seams.find(r);
  if (seam != m_seams.end()) {
    // One wire round the cut-open ring: the first rim, up the seam, the
    // second rim, back down the seam.
    const RingSeam& rs = seam->second;
    BRepBuilderAPI_MakeEdge me(m_vertices.at(rs.vA), m_vertices.at(rs.vB));
    if (!me.IsDone()) {
      why = "its seam would not build";
      return {};
    }
    const TopoDS_Edge up = me.Edge();
    const auto startingAt = [&](std::vector<LoopUse> loop, uint32_t v) {
      for (size_t i = 0; i < loop.size(); i++) {
        const Chain& c = m_chains[loop[i].chain];
        if ((loop[i].reversed ? c.verts.back() : c.verts.front()) == v) {
          std::rotate(loop.begin(), loop.begin() + i, loop.end());
          break;
        }
      }
      return loop;
    };
    TopoDS_Wire wire;
    bb.MakeWire(wire);
    addUses(wire, startingAt(m_loops[r][rs.loopA], rs.vA));
    bb.Add(wire, up);
    addUses(wire, startingAt(m_loops[r][rs.loopB], rs.vB));
    bb.Add(wire, TopoDS::Edge(up.Reversed()));
    addWire(wire);
  }
  for (size_t i = 0; i < m_loops[r].size(); i++) {
    if (seam != m_seams.end() &&
        (i == seam->second.loopA || i == seam->second.loopB))
      continue;
    TopoDS_Wire wire;
    bb.MakeWire(wire);
    addUses(wire, m_loops[r][i]);
    addWire(wire);
  }
  // A plane needs no p-curves (OpenCASCADE projects onto a plane when it is
  // asked) and has no seam, so only a curved face is fixed: the fix is most
  // of a rebuild's time.
  if (f.kind == mesh::Kind::Plane) {
    if (!agree)
      face.Reverse();
    return face;
  }
  try {
    ShapeFix_Face fix(face);
    fix.SetContext(m_context);
    fix.SetPrecision(m_tol);
    fix.SetMaxTolerance(std::max(1e3 * m_tol, 1e-4));
    fix.FixOrientationMode() = 0; // the wires' sense is the triangles'
    // A wire that crosses itself is not mended here: the check finds it
    // and the region goes back to flats, which is cheaper and says so.
    fix.FixWireTool()->FixSelfIntersectionMode() = 0;
    fix.FixWireTool()->FixIntersectingEdgesMode() = 0;
    fix.FixWireTool()->FixNotchedEdgesMode() = 0;
    fix.Perform();
    face = fix.Face();
  } catch (const Standard_Failure& e) {
    why = std::string("it would not fix: ") + e.GetMessageString();
    return {};
  }
  if (!agree)
    face.Reverse();
  return face;
}

void Builder::Blame(uint32_t r, std::vector<uint32_t>& out) const {
  const auto add = [&](uint32_t region) {
    if (region != mesh::kNone &&
        m_seg.regions[region].fit.kind != mesh::Kind::Plane &&
        std::find(out.begin(), out.end(), region) == out.end())
      out.push_back(region);
  };
  if (m_seg.regions[r].fit.kind != mesh::Kind::Plane) {
    add(r);
    return;
  }
  for (const auto& loop : m_loops[r])
    for (const LoopUse& use : loop) {
      const Chain& c = m_chains[use.chain];
      add(c.left == r ? c.right : c.left);
    }
}

Rebuilt Builder::Run() {
  Rebuilt out;
  if (!m_mesh.Closed()) {
    out.failed = "the mesh is not a closed solid (" +
                 std::to_string(m_mesh.boundaryEdges) +
                 " edges on one triangle, " +
                 std::to_string(m_mesh.nonManifoldEdges) +
                 " on three or more, " + std::to_string(m_mesh.flippedEdges) +
                 " between triangles facing opposite ways)";
    return out;
  }
  m_tol = std::max(m_seg.tolerance, Precision::Confusion());
  Trace();
  for (uint32_t r = 0; r < m_loops.size(); r++)
    if (m_loops[r].empty()) {
      out.failed = "a region has no boundary to bound a face with";
      return out;
    }
  if (!BuildEdges(out.failed))
    return out;
  std::vector<uint32_t> component;
  const size_t components = mesh::Components(m_mesh, component);
  BRep_Builder bb;
  std::vector<TopoDS_Shell> shells(components);
  for (TopoDS_Shell& s : shells)
    bb.MakeShell(s);
  std::vector<TopoDS_Face> faceOf(m_seg.regions.size());
  std::string firstWhy;
  for (uint32_t r = 0; r < m_seg.regions.size(); r++) {
    std::string why;
    faceOf[r] = BuildFace(r, why);
    if (faceOf[r].IsNull()) {
      if (firstWhy.empty())
        firstWhy = "face " + std::to_string(r + 1) + " (a " +
                   mesh::KindName(m_seg.regions[r].fit.kind) +
                   ") would not build: " + why;
      Blame(r, out.badRegions);
      continue;
    }
    bb.Add(shells[component[m_seg.regions[r].firstTriangle]], faceOf[r]);
  }
  // A face BRepCheck faults on its own (a wire that crosses itself where a
  // boundary wandered along a tangent junction) is found before the shells
  // are made, so a round that will be retried costs only its faces.
  size_t faulted = 0;
  if (firstWhy.empty()) {
    TopoDS_Compound all;
    bb.MakeCompound(all);
    for (const TopoDS_Face& f : faceOf) {
      try {
        BRepLib::SameParameter(f, m_tol, Standard_True);
      } catch (const Standard_Failure&) {
      }
      bb.Add(all, f);
    }
    // One analysis of them all, on OpenCASCADE's own threads; each face's
    // verdict read from it.
    BRepCheck_Analyzer check(all, Standard_True, Standard_True);
    for (uint32_t r = 0; r < faceOf.size(); r++)
      if (!check.IsValid(faceOf[r])) {
        faulted++;
        Blame(r, out.badRegions);
      }
  }
  if (firstWhy.empty() && faulted > 0)
    firstWhy = "the rebuilt solid is not valid (BRepCheck: " +
               std::to_string(faulted) + " faces)";
  if (!firstWhy.empty()) {
    out.failed = firstWhy;
    return out;
  }
  // Which region each face now in the shells is (the fix may have replaced
  // one), for the checks to name what they find.
  // By its surface: a face the context rebuilt keeps its surface.
  std::map<const Geom_Surface*, uint32_t> regionOfSurface;
  for (uint32_t r = 0; r < faceOf.size(); r++)
    regionOfSurface[BRep_Tool::Surface(faceOf[r]).get()] = r;
  const auto regionOf = [&](const TopoDS_Shape& face) -> uint32_t {
    const auto it =
        regionOfSurface.find(BRep_Tool::Surface(TopoDS::Face(face)).get());
    return it == regionOfSurface.end() ? mesh::kNone : it->second;
  };

  TopoDS_Compound compound;
  bb.MakeCompound(compound);
  TopoDS_Shape result;
  size_t open = 0;
  for (TopoDS_Shell& shell : shells) {
    shell = TopoDS::Shell(m_context->Apply(shell));
    shell.Closed(true);
    BRepCheck_Shell check(shell);
    if (check.Closed() != BRepCheck_NoError) {
      // Every face with an edge only it has: where the shell is open.
      TopTools_IndexedDataMapOfShapeListOfShape faces;
      TopExp::MapShapesAndUniqueAncestors(shell, TopAbs_EDGE, TopAbs_FACE,
                                          faces);
      for (int i = 1; i <= faces.Extent(); i++) {
        if (faces(i).Extent() != 1)
          continue;
        const TopoDS_Face& f = TopoDS::Face(faces(i).First());
        if (BRep_Tool::IsClosed(TopoDS::Edge(faces.FindKey(i)), f))
          continue; // a seam
        open++;
        if (const uint32_t r = regionOf(f); r != mesh::kNone)
          Blame(r, out.badRegions);
      }
      continue;
    }
    TopoDS_Solid solid;
    bb.MakeSolid(solid);
    bb.Add(solid, shell);
    BRepLib::OrientClosedSolid(solid);
    if (components == 1)
      result = solid;
    else
      bb.Add(compound, solid);
  }
  if (open > 0) {
    out.failed = "the rebuilt shell is not closed (" + std::to_string(open) +
                 " edges on one face)";
    return out;
  }
  if (components != 1)
    result = compound;
  try {
    BRepLib::SameParameter(result, m_tol, Standard_True);
  } catch (const Standard_Failure&) {
  }
  BRepCheck_Analyzer analyzer(result, Standard_True, Standard_True);
  if (!analyzer.IsValid()) {
    // Every face it faults, each blamed.
    size_t bad = 0;
    std::string first;
    for (TopExp_Explorer ex(result, TopAbs_FACE); ex.More(); ex.Next()) {
      if (BRepCheck_Analyzer(ex.Current()).IsValid())
        continue;
      bad++;
      const uint32_t r = regionOf(ex.Current());
      if (r == mesh::kNone)
        continue;
      if (first.empty())
        first = "face " + std::to_string(r + 1) + ", a " +
                mesh::KindName(m_seg.regions[r].fit.kind);
      Blame(r, out.badRegions);
    }
    out.failed =
        "the rebuilt solid is not valid (BRepCheck: " + std::to_string(bad) +
        " faces" + (first.empty() ? std::string() : ", first " + first) + ")";
    return out;
  }
  // The volume the facets enclose against the solid's: no further apart
  // than the facets stood off the surfaces fitted to them.
  GProp_GProps props;
  BRepGProp::VolumeProperties(result, props);
  const double rebuilt = props.Mass();
  const double meshed = m_mesh.Volume();
  double allowed = m_mesh.SurfaceArea() * m_tol;
  for (const mesh::Region& r : m_seg.regions)
    allowed += r.area * r.sagitta;
  if (!(std::abs(rebuilt - meshed) <= allowed)) {
    out.failed = "the rebuilt volume, " + Num(rebuilt) +
                 " mm³, is not within " + Num(allowed) + " mm³ of the mesh's " +
                 Num(meshed) + " mm³";
    return out;
  }
  out.shape = CADShape(result);
  return out;
}

} // namespace

Rebuilt RebuildFromRegions(const mesh::TriMesh& mesh,
                           const mesh::Segmentation& segmentation) {
  Builder b(mesh, segmentation);
  try {
    return b.Run();
  } catch (const Standard_Failure& f) {
    Rebuilt out;
    out.failed =
        std::string("OpenCASCADE failed building it: ") + f.GetMessageString();
    return out;
  }
}

bool RebuildRecognized(const mesh::TriMesh& m, mesh::Segmentation& seg,
                       int minSides, CADShape& shape, mesh::Recognition& report,
                       const std::function<bool()>& cancelled) {
  // A face that will not build, or that a check faults, puts its region
  // (or, for a flat, the curved ones round it) back as flats, all at once,
  // and the rebuild is tried again: a few rounds, each a whole rebuild.
  size_t demoted = 0;
  Rebuilt built;
  for (int round = 0;; round++) {
    built = RebuildFromRegions(m, seg);
    if (built.failed.empty() || built.badRegions.empty() ||
        round == kDemoteRounds || (cancelled && cancelled()))
      break;
    demoted += built.badRegions.size();
    mesh::Demote(seg, m, built.badRegions);
  }
  report = mesh::Summarize(m, seg, minSides);
  report.demoted = demoted;
  if (built.failed.empty()) {
    shape = built.shape;
    return true;
  }
  report.fellBack = built.failed;
  return false;
}

bool LoadStlRecognized(const std::string& path, const RecognizeOptions& options,
                       CADShape& shape, mesh::Recognition& report,
                       std::string& error, CADKernel::ImportProgress* progress,
                       bool* facetsRefused) {
  using Phase = CADKernel::ImportProgress::Phase;
  const auto cancelled = [&] {
    return progress != nullptr && progress->Cancelled();
  };
  if (progress != nullptr)
    progress->OnPhase(Phase::ReadingFile, -1.0);
  mesh::TriMesh m;
  std::string unread;
  if (!mesh::ReadStl(path, m, unread, cancelled)) {
    if (cancelled()) {
      error = "cancelled";
      return false;
    }
    // What this reader will not take, OpenCASCADE's may (or says why not,
    // as it always has): read as facets, and say so.
    report = {};
    report.minSides = options.minSides;
    report.fellBack = unread;
    if (!options.facetsAllowed) {
      if (facetsRefused != nullptr)
        *facetsRefused = true;
      error = "recognition could not read it (" + unread + ")";
      return false;
    }
    if (!CADKernel::Instance().LoadSTL(path, shape, progress)) {
      error =
          cancelled() ? "cancelled" : "failed to read '" + path + "' as STL";
      return false;
    }
    return true;
  }
  if (progress != nullptr)
    progress->OnPhase(Phase::Building, 0.0);
  mesh::SegmentOptions so;
  so.minSides = options.minSides;
  so.cancelled = cancelled;
  mesh::Segmentation seg = mesh::SegmentAll(m, so);
  if (seg.cancelled || cancelled()) {
    error = "cancelled";
    return false;
  }
  if (progress != nullptr)
    progress->OnPhase(Phase::Building, 0.5);
  if (RebuildRecognized(m, seg, options.minSides, shape, report, cancelled))
    return true;
  // Sewn as facets, as before, with the reason.
  if (!options.facetsAllowed) {
    if (facetsRefused != nullptr)
      *facetsRefused = true;
    error = "recognition kept it as facets (" + report.fellBack + ")";
    return false;
  }
  if (!CADKernel::Instance().LoadSTL(path, shape, progress)) {
    error = cancelled() ? "cancelled" : "failed to read '" + path + "' as STL";
    return false;
  }
  return true;
}

} // namespace cad
