// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/Slice.h"

#include "core/mesh/MeshBoolean.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace mesh {

namespace {

uint64_t EdgeKey(uint32_t a, uint32_t b) {
  if (a > b)
    std::swap(a, b);
  return (uint64_t(a) << 32) | b;
}

struct Segment {
  uint64_t from = 0, to = 0; // edge keys
  glm::dvec3 a{0.0}, b{0.0};
};

} // namespace

void PlaneAxes(const glm::dvec3& normal, glm::dvec3& u, glm::dvec3& v) {
  const glm::dvec3 n = glm::normalize(normal);
  const glm::dvec3 seed =
      std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
  u = glm::normalize(seed - n * glm::dot(seed, n));
  v = glm::cross(n, u);
}

SliceResult Slice(const TriMesh& m, const glm::dvec3& origin,
                  const glm::dvec3& normal) {
  SliceResult result;
  if (m.triangles.empty() || glm::length(normal) == 0.0)
    return result;
  const glm::dvec3 n = glm::normalize(normal);
  std::vector<double> d(m.positions.size());
  for (size_t i = 0; i < m.positions.size(); i++)
    d[i] = glm::dot(m.positions[i] - origin, n);
  const auto above = [&](uint32_t v) { return d[v] >= 0.0; };
  const auto cross = [&](uint32_t a, uint32_t b) {
    // Computed from the lower index so both triangles get the same bits.
    if (a > b)
      std::swap(a, b);
    const double t = d[a] / (d[a] - d[b]);
    return m.positions[a] + (m.positions[b] - m.positions[a]) * t;
  };

  std::vector<Segment> segs;
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    const auto& tri = m.triangles[t];
    const bool s0 = above(tri[0]), s1 = above(tri[1]), s2 = above(tri[2]);
    if (s0 == s1 && s1 == s2)
      continue;
    // The odd corner out and the two edges from it.
    int k = s0 == s1 ? 2 : (s0 == s2 ? 1 : 0);
    const uint32_t o = tri[k], p = tri[(k + 1) % 3], q = tri[(k + 2) % 3];
    Segment s;
    s.from = EdgeKey(o, p);
    s.to = EdgeKey(o, q);
    s.a = cross(o, p);
    s.b = cross(o, q);
    // Material on the left looking down the normal, decided by the
    // triangle's winding, not by the segment's geometry: a corner on the
    // plane makes a segment of length zero, whose direction is no answer.
    // With (o, p, q) in winding order, o above runs from edge op to oq.
    if (!above(o)) {
      std::swap(s.from, s.to);
      std::swap(s.a, s.b);
    }
    segs.push_back(s);
  }

  // Chain: each segment's end is the next one's start, on the same edge.
  std::unordered_map<uint64_t, uint32_t> startAt;
  std::unordered_map<uint64_t, uint32_t> endAt;
  startAt.reserve(segs.size());
  endAt.reserve(segs.size());
  for (uint32_t i = 0; i < segs.size(); i++) {
    startAt.emplace(segs[i].from, i);
    endAt.emplace(segs[i].to, i);
  }
  std::vector<bool> used(segs.size(), false);
  const auto chainFrom = [&](uint32_t first) {
    SlicePolyline line;
    uint32_t cur = first;
    line.points.push_back(segs[cur].a);
    while (true) {
      used[cur] = true;
      auto it = startAt.find(segs[cur].to);
      if (it == startAt.end() || it->second == first || used[it->second]) {
        line.closed = it != startAt.end() && it->second == first;
        if (!line.closed)
          line.points.push_back(segs[cur].b);
        break;
      }
      cur = it->second;
      line.points.push_back(segs[cur].a);
    }
    return line;
  };
  // Open chains first, each from a start no segment ends on.
  for (uint32_t i = 0; i < segs.size(); i++)
    if (!used[i] && !endAt.count(segs[i].from))
      result.polylines.push_back(chainFrom(i));
  for (uint32_t i = 0; i < segs.size(); i++)
    if (!used[i])
      result.polylines.push_back(chainFrom(i));

  glm::dvec3 u, v;
  PlaneAxes(n, u, v);
  for (SlicePolyline& line : result.polylines) {
    const size_t k = line.points.size();
    for (size_t i = 0; i + 1 < k || (line.closed && i < k); i++) {
      const glm::dvec3& a = line.points[i];
      const glm::dvec3& b = line.points[(i + 1) % k];
      line.length += glm::length(b - a);
      if (line.closed) {
        const glm::dvec2 pa(glm::dot(a - origin, u), glm::dot(a - origin, v));
        const glm::dvec2 pb(glm::dot(b - origin, u), glm::dot(b - origin, v));
        line.signedArea += 0.5 * (pa.x * pb.y - pb.x * pa.y);
      }
    }
    if (line.closed)
      result.area += line.signedArea;
  }
  return result;
}

SliceCap Cap(const SliceResult& slice, const glm::dvec3& origin,
             const glm::dvec3& normal) {
  SliceCap cap;
  if (!slice.Closed())
    return cap;
  glm::dvec3 u, v;
  PlaneAxes(normal, u, v);
  std::vector<std::vector<glm::dvec2>> loops;
  for (const SlicePolyline& line : slice.polylines) {
    if (line.points.size() < 3)
      continue;
    std::vector<glm::dvec2> loop;
    loop.reserve(line.points.size());
    for (const glm::dvec3& p : line.points) {
      loop.emplace_back(glm::dot(p - origin, u), glm::dot(p - origin, v));
      cap.positions.push_back(p);
    }
    loops.push_back(std::move(loop));
  }
  if (loops.empty() || !TriangulatePolygons(loops, cap.triangles))
    return {};
  return cap;
}

namespace {

double SegDist(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b) {
  const glm::dvec3 ab = b - a;
  const double l2 = glm::dot(ab, ab);
  const double t =
      l2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
  return glm::length(p - (a + ab * t));
}

void Dp(const std::vector<glm::dvec3>& pts, size_t lo, size_t hi, double tol,
        std::vector<bool>& keep) {
  if (hi <= lo + 1)
    return;
  double worst = -1.0;
  size_t at = lo;
  for (size_t i = lo + 1; i < hi; i++) {
    const double e = SegDist(pts[i], pts[lo], pts[hi]);
    if (e > worst) {
      worst = e;
      at = i;
    }
  }
  if (worst > tol) {
    keep[at] = true;
    Dp(pts, lo, at, tol, keep);
    Dp(pts, at, hi, tol, keep);
  }
}

} // namespace

std::vector<glm::dvec3> Simplify(const std::vector<glm::dvec3>& points,
                                 bool closed, double tolerance) {
  if (points.size() < 3)
    return points;
  std::vector<glm::dvec3> pts = points;
  size_t split = 0;
  if (closed) {
    // Split a loop at its first point and the point farthest from it.
    double farthest = -1.0;
    for (size_t i = 1; i < pts.size(); i++)
      if (double l = glm::length(pts[i] - pts[0]); l > farthest) {
        farthest = l;
        split = i;
      }
    pts.push_back(pts[0]);
  }
  std::vector<bool> keep(pts.size(), false);
  keep.front() = keep.back() = true;
  if (closed) {
    keep[split] = true;
    Dp(pts, 0, split, tolerance, keep);
    Dp(pts, split, pts.size() - 1, tolerance, keep);
  } else {
    Dp(pts, 0, pts.size() - 1, tolerance, keep);
  }
  std::vector<glm::dvec3> out;
  for (size_t i = 0; i < pts.size(); i++)
    if (keep[i] && !(closed && i + 1 == pts.size()))
      out.push_back(pts[i]);
  return out;
}

} // namespace mesh
