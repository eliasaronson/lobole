// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// GrowFrom and ScanTolerance (Segment.h): one region grown from a picked
// triangle under a robust fit, for a scan as much as a CAD export.

#include "core/mesh/Segment.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mesh {

namespace {

std::string Mm(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.3g", v);
  return buf;
}

// What a fit of a region reads: its distinct vertices (on the surface), and
// its triangles' centres with their unit normals (the oriented samples a
// hypothesis is drawn from -- a facet's normal is the surface's, where a
// vertex's at a sharp edge is the average of two surfaces'). At most
// `most` of each, an even stride through the rest.
struct Samples {
  std::vector<glm::dvec3> points, centres, normals;
};

void Gather(const TriMesh& m, const std::vector<uint32_t>& tris,
            std::vector<uint8_t>& stamp, Samples& out, size_t most) {
  std::vector<uint32_t> verts;
  for (uint32_t t : tris)
    for (uint32_t v : m.triangles[t])
      if (!stamp[v]) {
        stamp[v] = 1;
        verts.push_back(v);
      }
  for (uint32_t v : verts)
    stamp[v] = 0;
  std::sort(verts.begin(), verts.end());
  out.points.clear();
  out.centres.clear();
  out.normals.clear();
  const size_t n = verts.size(), take = std::min(n, most);
  for (size_t i = 0; i < take; i++)
    out.points.push_back(m.positions[verts[take == n ? i : i * n / take]]);
  std::vector<uint32_t> sorted = tris;
  std::sort(sorted.begin(), sorted.end());
  const size_t nt = sorted.size(), takeT = std::min(nt, most);
  for (size_t i = 0; i < takeT; i++) {
    const uint32_t t = sorted[takeT == nt ? i : i * nt / takeT];
    const glm::dvec3 a = m.AreaNormal(t);
    const double l = glm::length(a);
    if (!(l > 0.0))
      continue;
    out.centres.push_back((m.Corner(t, 0) + m.Corner(t, 1) + m.Corner(t, 2)) /
                          3.0);
    out.normals.push_back(a / l);
  }
}

FitResult Robust(Kind kind, const Samples& s, double tol,
                 const glm::dvec3* through) {
  return FitRobust(kind, s.points, s.centres, s.normals, tol, through);
}

constexpr size_t kFitPoints = 65536;
const double kCreaseCos = std::cos(2.0 * M_PI / kMinSidesLowest);

} // namespace

double ScanTolerance(const TriMesh& m) {
  const double floor = 4.0 * m.quantum;
  if (m.neighbour.size() != m.triangles.size() || m.triangles.empty())
    return floor;
  // A deterministic stride through the triangles: a median is known well
  // from a few hundred thousand edges.
  const size_t n = m.triangles.size();
  const size_t step = std::max<size_t>(1, n / 200000);
  std::vector<double> d;
  d.reserve(3 * (n / step + 1));
  for (size_t t = 0; t < n; t += step) {
    const glm::dvec3 an = m.AreaNormal(uint32_t(t));
    const double l = glm::length(an);
    if (!(l > 0.0))
      continue;
    const glm::dvec3 unit = an / l, a = m.Corner(uint32_t(t), 0);
    for (int e = 0; e < 3; e++) {
      const uint32_t u = m.neighbour[t][e];
      if (u == kNone || u < t)
        continue;
      // A crease is a feature, not noise: an edge turning more than the
      // fewest-sided polygon recognition admits (kMinSidesLowest) is left
      // out, or a box's would set its tolerance at its own size.
      const glm::dvec3 un = m.AreaNormal(u);
      const double ul = glm::length(un);
      if (!(ul > 0.0) || glm::dot(unit, un / ul) < kCreaseCos)
        continue;
      // Two triangles in one plane to the file's precision say nothing of
      // the noise: a CAD export's flats would put a cylinder's facets'
      // own bend, which a fit from them must allow, below the median.
      const int opposite = (m.neighbourEdge[t][e] + 2) % 3;
      const double off = std::abs(glm::dot(m.Corner(u, opposite) - a, unit));
      if (off > floor)
        d.push_back(off);
    }
  }
  if (d.empty())
    return floor;
  std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
  return std::max(floor, 3.0 * 1.4826 * d[d.size() / 2]);
}

Region GrowFrom(const TriMesh& m, uint32_t seed, Kind kind, double tol,
                std::string* why) {
  Region r;
  r.fit.kind = kind;
  const auto fail = [&](std::string msg) {
    if (why != nullptr)
      *why = std::move(msg);
    Region failed;
    failed.fit.kind = kind;
    return failed;
  };
  if (seed >= m.triangles.size() || m.neighbour.size() != m.triangles.size())
    return fail("the point is on no triangle of the mesh");
  if (!(tol > 0.0) || !std::isfinite(tol))
    return fail("the tolerance is not a number of mm greater than 0");
  if (!(m.Area(seed) > 0.0))
    return fail("the triangle at the point has no area");
  std::vector<uint8_t> stamp(m.positions.size(), 0);
  std::vector<uint8_t> in(m.triangles.size(), 0);
  Samples smp;

  // 1. The seed disk.
  std::vector<uint32_t> disk{seed}, frontier{seed};
  in[seed] = 1;
  size_t lastFit = 0;
  for (int ring = 0; !frontier.empty(); ring++) {
    std::vector<uint32_t> next;
    for (uint32_t t : frontier)
      for (int e = 0; e < 3; e++) {
        const uint32_t u = m.neighbour[t][e];
        if (u != kNone && !in[u]) {
          in[u] = 1;
          next.push_back(u);
        }
      }
    disk.insert(disk.end(), next.begin(), next.end());
    frontier = std::move(next);
    if (ring < 1 || disk.size() < 12 || disk.size() < lastFit * 3 / 2)
      continue;
    lastFit = disk.size();
    if (kind == Kind::Plane)
      break;
    Gather(m, disk, stamp, smp, kFitPoints);
    const FitResult flat = FitPlane(smp.points);
    if (flat.ok && flat.max > 3.0 * tol)
      break;
  }
  for (uint32_t t : disk)
    in[t] = 0;
  Gather(m, disk, stamp, smp, kFitPoints);
  // The surface the seed is on: through its centre.
  const glm::dvec3 seedCentre =
      (m.Corner(seed, 0) + m.Corner(seed, 1) + m.Corner(seed, 2)) / 3.0;
  FitResult fit = Robust(kind, smp, tol, &seedCentre);
  if (!fit.ok)
    return fail(std::string("no ") + KindName(kind) +
                " fits the surface round the point to " + Mm(tol) + " mm");

  // 3. Growth under the fit; 4. once more against the final one.
  const double side =
      glm::dot(m.AreaNormal(seed), fit.Normal(seedCentre)) < 0.0 ? -1.0 : 1.0;
  const auto takes = [&](const FitResult& f, uint32_t t) {
    for (uint32_t v : m.triangles[t])
      if (!(std::abs(f.Distance(m.positions[v])) <= tol))
        return false;
    const glm::dvec3 c =
        (m.Corner(t, 0) + m.Corner(t, 1) + m.Corner(t, 2)) / 3.0;
    return side * glm::dot(m.AreaNormal(t), f.Normal(c)) > 0.0;
  };
  const auto grow = [&](FitResult& f, bool refit) {
    std::vector<uint32_t> region{seed};
    in[seed] = 1;
    size_t nextFit = std::max<size_t>(2 * disk.size(), 16);
    for (size_t i = 0; i < region.size(); i++) {
      const uint32_t t = region[i];
      for (int e = 0; e < 3; e++) {
        const uint32_t u = m.neighbour[t][e];
        if (u == kNone || in[u] || !takes(f, u))
          continue;
        in[u] = 1;
        region.push_back(u);
      }
      if (refit && region.size() >= nextFit) {
        nextFit *= 2;
        Gather(m, region, stamp, smp, kFitPoints);
        const FitResult g = Robust(kind, smp, tol, &seedCentre);
        if (g.ok)
          f = g;
      }
    }
    for (uint32_t t : region)
      in[t] = 0;
    return region;
  };
  (void)grow(fit, /*refit=*/true);
  std::vector<uint32_t> region = grow(fit, /*refit=*/false);
  Gather(m, region, stamp, smp, kFitPoints);
  if (const FitResult g = Robust(kind, smp, tol, &seedCentre); g.ok)
    fit = g;
  // rms and max over every vertex of the region.
  Gather(m, region, stamp, smp, m.positions.size());
  const std::vector<glm::dvec3>& pts = smp.points;
  double sum = 0.0, worst = 0.0;
  for (const glm::dvec3& p : pts) {
    const double d = std::abs(fit.Distance(p));
    sum += d * d;
    worst = std::max(worst, d);
  }
  fit.rms = pts.empty() ? 0.0 : std::sqrt(sum / double(pts.size()));
  fit.max = worst;
  fit.inliers = pts.size();
  if (kind != Kind::Plane) {
    const FitResult flat = FitPlane(pts);
    if (!flat.ok || flat.max <= tol)
      return fail(std::string("the surface at the point is flat to ") +
                  Mm(tol) + " mm over the " + std::to_string(region.size()) +
                  (region.size() == 1 ? " triangle" : " triangles") +
                  " round it, so no " + KindName(kind) +
                  " is defined by it: fit a plane, or, if it is curved and "
                  "only rougher than that, a larger tolerance_mm");
  } else if (side < 0.0) {
    fit.axis = -fit.axis; // the way the triangles face
  }
  std::sort(region.begin(), region.end());
  r.fit = fit;
  r.firstTriangle = region.front();
  for (uint32_t t : region)
    r.area += m.Area(t);
  r.triangles = std::move(region);
  return r;
}

} // namespace mesh
