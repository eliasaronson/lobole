// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/Align.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>

namespace mesh {

namespace {

// The plane through the corners of `region`, its normal turned to the side
// the triangles face.
FitResult FitRegion(const TriMesh& m, const std::vector<uint32_t>& region) {
  std::vector<uint8_t> seen(m.positions.size(), 0);
  std::vector<glm::dvec3> pts;
  glm::dvec3 facing(0.0);
  for (uint32_t t : region) {
    facing += m.AreaNormal(t);
    for (uint32_t v : m.triangles[t])
      if (!seen[v]) {
        seen[v] = 1;
        pts.push_back(m.positions[v]);
      }
  }
  FitResult f = FitPlane(pts);
  if (f.ok && glm::dot(f.axis, facing) < 0.0)
    f.axis = -f.axis;
  return f;
}

// The triangles reached from `seed` across edges, each facing the plane's
// side with every corner within `tol` of it; with `refit`, the plane is
// fitted again each time the region doubles.
std::vector<uint32_t> Grow(const TriMesh& m, uint32_t seed, FitResult plane,
                           double tol, bool refit) {
  std::vector<uint8_t> in(m.triangles.size(), 0);
  std::vector<uint32_t> region{seed};
  std::deque<uint32_t> queue{seed};
  in[seed] = 1;
  size_t nextFit = 8;
  const auto fits = [&](uint32_t t) {
    if (glm::dot(m.AreaNormal(t), plane.axis) <= 0.0)
      return false;
    for (uint32_t v : m.triangles[t])
      if (std::abs(glm::dot(m.positions[v] - plane.origin, plane.axis)) > tol)
        return false;
    return true;
  };
  while (!queue.empty()) {
    const uint32_t t = queue.front();
    queue.pop_front();
    for (int e = 0; e < 3; e++) {
      const uint32_t u = m.neighbour[t][e];
      if (u == kNone || in[u] || !fits(u))
        continue;
      in[u] = 1;
      region.push_back(u);
      queue.push_back(u);
      if (refit && region.size() >= nextFit) {
        const FitResult f = FitRegion(m, region);
        if (f.ok)
          plane = f;
        nextFit *= 2;
      }
    }
  }
  std::sort(region.begin(), region.end());
  return region;
}

} // namespace

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
    r.fit = FitPlane(pts);
    if (r.fit.ok) {
      r.loop = std::move(loop);
      return r;
    }
  }
  const glm::dvec3 n = m.AreaNormal(seed);
  if (!(glm::length(n) > 0.0))
    return r;
  FitResult start;
  start.ok = true;
  start.kind = Kind::Plane;
  start.origin =
      (m.Corner(seed, 0) + m.Corner(seed, 1) + m.Corner(seed, 2)) / 3.0;
  start.axis = glm::normalize(n);
  const std::vector<uint32_t> first =
      Grow(m, seed, start, toleranceMm, /*refit=*/true);
  FitResult settled = first.size() >= 2 ? FitRegion(m, first) : start;
  if (!settled.ok)
    settled = start;
  r.triangles = Grow(m, seed, settled, toleranceMm, /*refit=*/false);
  r.fit = FitRegion(m, r.triangles);
  if (!r.fit.ok) { // one triangle: its own plane, exactly
    r.fit = start;
    r.fit.rms = r.fit.max = 0.0;
  }
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
