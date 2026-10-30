// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/Align.h"

#include "core/mesh/Segment.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace mesh {

std::vector<uint32_t> BoundaryLoopAt(const TriMesh& m, uint32_t t) {
  if (t >= m.triangles.size() || t >= m.neighbour.size())
    return {};
  int edge = -1;
  for (int e = 0; e < 3 && edge < 0; e++)
    if (m.neighbour[t][e] == kNone)
      edge = e;
  if (edge < 0)
    return {};
  // Walked as BoundaryLoops walks them: each open edge from its end corner
  // back to its start.
  std::unordered_multimap<uint32_t, uint32_t> next;
  for (uint32_t s = 0; s < m.triangles.size(); s++)
    for (int e = 0; e < 3; e++)
      if (m.neighbour[s][e] == kNone)
        next.emplace(m.triangles[s][(e + 1) % 3], m.triangles[s][e]);
  const uint32_t start = m.triangles[t][(edge + 1) % 3];
  std::vector<uint32_t> loop{start};
  uint32_t at = m.triangles[t][edge];
  const size_t most = next.size();
  while (at != start && loop.size() <= most) {
    loop.push_back(at);
    const auto it = next.find(at);
    if (it == next.end())
      break;
    at = it->second;
  }
  return loop;
}

PlaneRegion GrowPlane(const TriMesh& m, uint32_t seed, double toleranceMm) {
  PlaneRegion r;
  if (seed >= m.triangles.size() || m.neighbour.size() != m.triangles.size())
    return r;
  if (std::vector<uint32_t> loop = BoundaryLoopAt(m, seed); loop.size() >= 3) {
    std::vector<glm::dvec3> pts;
    pts.reserve(loop.size());
    for (uint32_t v : loop)
      pts.push_back(m.positions[v]);
    // Least squares, not robust: a rim is ragged by about an edge length
    // all the way round where the scan stopped, so there is no band of
    // inliers within a tolerance to find -- the mean plane is the floor.
    r.fit = FitPlane(pts);
    if (r.fit.ok) {
      r.loop = std::move(loop);
      return r;
    }
  }
  const glm::dvec3 n = m.AreaNormal(seed);
  if (!(glm::length(n) > 0.0))
    return r;
  // The region fit_primitive grows for a plane: a disk round the seed
  // fitted robustly, grown under it and refitted (GrowFrom).
  const Region grown = GrowFrom(m, seed, Kind::Plane, toleranceMm);
  if (grown.fit.ok) {
    r.fit = grown.fit;
    r.triangles = grown.triangles;
    r.areaMm2 = grown.area;
    return r;
  }
  // Too few triangles round it to fit: the seed's own plane, exactly.
  r.fit.ok = true;
  r.fit.kind = Kind::Plane;
  r.fit.origin =
      (m.Corner(seed, 0) + m.Corner(seed, 1) + m.Corner(seed, 2)) / 3.0;
  r.fit.axis = glm::normalize(n);
  r.fit.inliers = 3;
  r.triangles = {seed};
  for (uint32_t t : r.triangles)
    r.areaMm2 += m.Area(t);
  return r;
}

void SurfaceMoments::Add(const TriMesh& m) {
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    const double a = m.Area(t);
    const glm::dvec3 p0 = m.Corner(t, 0), p1 = m.Corner(t, 1),
                     p2 = m.Corner(t, 2), s = p0 + p1 + p2;
    area += a;
    first += a * s / 3.0;
    // ∫ x xᵀ over a triangle = A/12 (Σ pᵢ pᵢᵀ + s sᵀ).
    second +=
        (a / 12.0) * (glm::outerProduct(p0, p0) + glm::outerProduct(p1, p1) +
                      glm::outerProduct(p2, p2) + glm::outerProduct(s, s));
  }
}

glm::dvec3 SurfaceMoments::Centroid() const {
  return area > 0.0 ? first / area : glm::dvec3(0.0);
}

glm::dmat3 SurfaceMoments::Covariance() const {
  if (!(area > 0.0))
    return glm::dmat3(0.0);
  const glm::dvec3 c = Centroid();
  return second / area - glm::outerProduct(c, c);
}

glm::dmat3 RotationBetween(const glm::dvec3& from, const glm::dvec3& to) {
  const glm::dvec3 f = glm::normalize(from), t = glm::normalize(to);
  const double c = glm::dot(f, t);
  if (c < -1.0 + 1e-12) {
    // Opposite: half a turn about any axis square to them.
    const glm::dvec3 a =
        std::abs(f.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    const glm::dvec3 axis = glm::normalize(glm::cross(f, a));
    return 2.0 * glm::outerProduct(axis, axis) - glm::dmat3(1.0);
  }
  const glm::dvec3 v = glm::cross(f, t);
  const glm::dmat3 k(0.0, v.z, -v.y, -v.z, 0.0, v.x, v.y, -v.x, 0.0);
  return glm::dmat3(1.0) + k + (k * k) * (1.0 / (1.0 + c));
}

} // namespace mesh
