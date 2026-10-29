// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MeshMeasure.h"

#include "core/mesh/MeshBvh.h"
#include "core/mesh/Slice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace meshmeasure {

namespace {

// The bodies a target covers, placed.
std::vector<Body> Covered(const Measure::Target& t) {
  std::vector<Body> out;
  if (t.bodies == nullptr)
    return out;
  for (const Body& b : *t.bodies) {
    if (b.reference)
      continue;
    if (t.ref.featureId != 0 && b.createdBy != t.ref.featureId)
      continue;
    if (!t.Whole() && !b.elementMap.Resolve(t.ref.name))
      continue;
    out.push_back(t.xf == glm::dmat4(1.0) ? b : PlacedBody(b, t.xf));
  }
  return out;
}

std::shared_ptr<const mesh::Bvh> TreeOf(const Body& b) {
  return mesh::Bvh::For(b.mesh);
}

} // namespace

bool IsMeshElement(const std::vector<Body>& bodies, const ElementRef& ref) {
  if (ref.name.empty())
    return false;
  for (const Body& b : bodies)
    if (b.IsMesh() && (ref.featureId == 0 || b.createdBy == ref.featureId))
      if (const std::string* n = b.elementMap.NameOf(0); n && *n == ref.name)
        return true;
  return false;
}

bool HasMesh(const Measure::Target& t) {
  if (t.bodies == nullptr)
    return false;
  for (const Body& b : *t.bodies) {
    if (!b.IsMesh() || b.reference)
      continue;
    if (t.ref.featureId != 0 && b.createdBy != t.ref.featureId)
      continue;
    if (t.Whole() || b.elementMap.Resolve(t.ref.name))
      return true;
  }
  return false;
}

Stats Describe(const mesh::TriMesh& m) {
  Stats s;
  s.triangles = m.triangles.size();
  s.vertices = m.positions.size();
  std::vector<uint32_t> componentOf;
  s.components = mesh::Components(m, componentOf);
  s.closed = m.Closed();
  s.loops = mesh::BoundaryLoops(m);
  const mesh::MassProperties mp = mesh::ComputeMassProperties(m);
  s.area = mp.area;
  if (s.closed)
    s.volume = mp.volume;
  m.Bounds(s.lo, s.hi);
  s.medianEdge = mesh::MedianEdgeLength(m);
  return s;
}

std::string OpenWhy(const Stats& s) {
  char buf[256];
  if (!s.loops.empty())
    std::snprintf(buf, sizeof buf,
                  "the mesh is open (%zu boundary loop%s, the longest "
                  "%.1f mm), so it encloses no volume: close %s in MeshLab, "
                  "Blender or the scanning app and re-import it",
                  s.loops.size(), s.loops.size() == 1 ? "" : "s",
                  s.loops.front(), s.loops.size() == 1 ? "it" : "them");
  else
    std::snprintf(buf, sizeof buf,
                  "the mesh is not a closed solid (edges shared by three or "
                  "more triangles, or triangles facing opposite ways), so "
                  "it encloses no volume: repair it in MeshLab or Blender "
                  "and re-import it");
  return buf;
}

std::optional<Snap> SnapPoint(const std::vector<Body>& bodies,
                              uint64_t featureId, const glm::dvec3& p,
                              std::string& why) {
  std::optional<Snap> best;
  for (const Body& b : bodies) {
    if (b.reference || (featureId != 0 && b.createdBy != featureId))
      continue;
    std::optional<glm::dvec3> q;
    if (b.IsMesh()) {
      const auto c = TreeOf(b)->ClosestPoint(p);
      if (c.found)
        q = c.point;
    } else if (b.shape.IsValid()) {
      const std::vector<Body> one{b};
      q = Measure::NearestPointOn(Measure::Target{&one, glm::dmat4(1.0), {}}, p,
                                  Measure::DistanceKind::Minimum);
    }
    if (q && (!best || glm::distance(*q, p) < best->moved))
      best = Snap{*q, glm::distance(*q, p)};
  }
  if (!best)
    why = featureId == 0 ? "there is no body to put the point on"
                         : "feature " + std::to_string(featureId) +
                               " has no body to put the point on";
  return best;
}

std::optional<glm::dvec3> NearestPoint(const Measure::Target& t,
                                       const glm::dvec3& p) {
  std::optional<glm::dvec3> best;
  double bestD = std::numeric_limits<double>::max();
  bool brep = false;
  for (const Body& b : Covered(t)) {
    if (!b.IsMesh()) {
      brep = true;
      continue;
    }
    const auto c = TreeOf(b)->ClosestPoint(p);
    if (c.found && c.distance < bestD) {
      bestD = c.distance;
      best = c.point;
    }
  }
  if (brep) {
    const auto q =
        Measure::NearestPointOn(t, p, Measure::DistanceKind::Minimum);
    if (q && glm::distance(*q, p) < bestD)
      best = q;
  }
  return best;
}

Cut Section(const std::vector<Body>& bodies, const glm::dvec3& origin,
            const glm::dvec3& normal, bool withLoops) {
  Cut cut;
  const glm::dvec3 n = glm::normalize(normal);
  glm::dvec3 u, v;
  mesh::PlaneAxes(n, u, v);
  const auto to2 = [&](const glm::dvec3& p) {
    return glm::dvec2(glm::dot(p - origin, u), glm::dot(p - origin, v));
  };
  for (const Body& b : bodies) {
    if (!b.IsMesh() || b.reference)
      continue;
    const mesh::SliceResult s = mesh::Slice(*b.mesh, origin, n);
    if (s.polylines.empty())
      continue;
    if (!s.Closed()) {
      cut.openBodies.push_back(b.name);
      continue;
    }
    cut.area += s.area;
    // Outer loops and holes, each hole in the smallest outer loop holding
    // its first point.
    struct Loop2 {
      const mesh::SlicePolyline* line;
      std::vector<glm::dvec2> pts;
    };
    std::vector<Loop2> outers, holes;
    for (const mesh::SlicePolyline& l : s.polylines) {
      Loop2 lp{&l, {}};
      for (const glm::dvec3& p : l.points)
        lp.pts.push_back(to2(p));
      (l.signedArea >= 0.0 ? outers : holes).push_back(std::move(lp));
    }
    const auto inside = [](const std::vector<glm::dvec2>& poly,
                           const glm::dvec2& q) {
      bool in = false;
      for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y > q.y) != (poly[j].y > q.y) &&
            q.x < (poly[j].x - poly[i].x) * (q.y - poly[i].y) /
                          (poly[j].y - poly[i].y) +
                      poly[i].x)
          in = !in;
      return in;
    };
    std::vector<std::vector<const Loop2*>> holesOf(outers.size());
    for (const Loop2& h : holes) {
      int owner = -1;
      for (size_t o = 0; o < outers.size(); o++)
        if (!h.pts.empty() && inside(outers[o].pts, h.pts.front()) &&
            (owner < 0 ||
             outers[o].line->signedArea < outers[owner].line->signedArea))
          owner = int(o);
      if (owner >= 0)
        holesOf[owner].push_back(&h);
    }
    // Area and centroid of a loop by the shoelace sums.
    const auto moments = [&](const Loop2& l, double& a, glm::dvec3& c) {
      double area = 0.0;
      glm::dvec2 m(0.0);
      for (size_t i = 0; i < l.pts.size(); i++) {
        const glm::dvec2& p = l.pts[i];
        const glm::dvec2& q = l.pts[(i + 1) % l.pts.size()];
        const double cr = p.x * q.y - q.x * p.y;
        area += 0.5 * cr;
        m += (p + q) * cr / 6.0;
      }
      a += area;
      c += origin + u * m.x + v * m.y; // area-weighted, divided below
    };
    for (size_t o = 0; o < outers.size(); o++) {
      Measure::SectionRegion r;
      glm::dvec3 weighted(0.0);
      double area = 0.0;
      const auto add = [&](const Loop2& l) {
        double a = 0.0;
        glm::dvec3 c(0.0);
        moments(l, a, c);
        // `c` holds origin + the first moment: take the moment back out.
        weighted += (c - origin);
        area += a;
      };
      add(outers[o]);
      for (const Loop2* h : holesOf[o])
        add(*h);
      r.area = area;
      r.centroid = area != 0.0 ? origin + weighted / area : origin;
      if (withLoops) {
        const auto loopOf = [&](const Loop2& l, bool outer) {
          Measure::SectionLoop sl;
          sl.outer = outer;
          const auto& pts = l.line->points;
          for (size_t i = 0; i < pts.size(); i++) {
            const glm::dvec3& a = pts[i];
            const glm::dvec3& c = pts[(i + 1) % pts.size()];
            if (a == c)
              continue;
            sl.segments.push_back({"line", a, c, glm::dvec3(0.0), 0.0});
          }
          return sl;
        };
        r.loops.push_back(loopOf(outers[o], true));
        for (const Loop2* h : holesOf[o])
          r.loops.push_back(loopOf(*h, false));
      }
      cut.regions.push_back(std::move(r));
    }
  }
  std::sort(cut.regions.begin(), cut.regions.end(),
            [](const auto& a, const auto& b) { return a.area > b.area; });
  return cut;
}

} // namespace meshmeasure
