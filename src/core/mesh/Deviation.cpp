// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/Deviation.h"

#include "core/Parallel.h"
#include "core/mesh/Facing.h"
#include "core/mesh/MeshBvh.h"
#include "core/mesh/Register.h" // SampleSurface
#include "core/mesh/Segment.h"  // ScanTolerance

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace mesh {

namespace {

// The angle-weighted pseudonormals of a mesh: each triangle's, each
// vertex's (its corners' normals weighted by their angles) and, through the
// adjacency, each edge's (the two triangles' summed).
struct Pseudonormals {
  std::vector<glm::dvec3> face, vertex;
  const TriMesh* mesh = nullptr;

  explicit Pseudonormals(const TriMesh& m) : mesh(&m) {
    face.assign(m.triangles.size(), glm::dvec3(0.0));
    vertex.assign(m.positions.size(), glm::dvec3(0.0));
    for (uint32_t t = 0; t < m.triangles.size(); t++) {
      const glm::dvec3 n = m.AreaNormal(t);
      const double len = glm::length(n);
      if (!(len > 0.0))
        continue;
      face[t] = n / len;
      for (int c = 0; c < 3; c++) {
        const glm::dvec3 u = m.Corner(t, (c + 1) % 3) - m.Corner(t, c),
                         w = m.Corner(t, (c + 2) % 3) - m.Corner(t, c);
        const double lu = glm::length(u), lw = glm::length(w);
        if (!(lu > 0.0 && lw > 0.0))
          continue;
        const double angle =
            std::acos(std::clamp(glm::dot(u, w) / (lu * lw), -1.0, 1.0));
        vertex[m.triangles[t][c]] += angle * face[t];
      }
    }
  }

  // At the point `p` on triangle `t`: the triangle's own inside it, an
  // edge's on an edge, a vertex's at a corner.
  [[nodiscard]] glm::dvec3 At(uint32_t t, const glm::dvec3& p) const {
    const glm::dvec3 a = mesh->Corner(t, 0), b = mesh->Corner(t, 1),
                     c = mesh->Corner(t, 2);
    const glm::dvec3 v0 = b - a, v1 = c - a, v2 = p - a;
    const double d00 = glm::dot(v0, v0), d01 = glm::dot(v0, v1),
                 d11 = glm::dot(v1, v1), d20 = glm::dot(v2, v0),
                 d21 = glm::dot(v2, v1);
    const double den = d00 * d11 - d01 * d01;
    if (!(den > 0.0))
      return face[t];
    const double wb = (d11 * d20 - d01 * d21) / den,
                 wc = (d00 * d21 - d01 * d20) / den, wa = 1.0 - wb - wc;
    constexpr double kOn = 1e-9;
    const double w[3] = {wa, wb, wc};
    int zeros = 0, notZero = -1, zero = -1;
    for (int k = 0; k < 3; k++) {
      if (w[k] <= kOn) {
        zeros++;
        zero = k;
      } else {
        notZero = k;
      }
    }
    if (zeros >= 2)
      return vertex[mesh->triangles[t][notZero]];
    if (zeros == 1) {
      // The edge opposite corner `zero`: from corner zero+1 to zero+2,
      // which is edge index zero+1 (edge e runs from corner e to e+1).
      const int e = (zero + 1) % 3;
      glm::dvec3 n = face[t];
      if (!mesh->neighbour.empty()) {
        const uint32_t across = mesh->neighbour[t][e];
        if (across != kNone)
          n += face[across];
      }
      return n;
    }
    return face[t];
  }
};

std::vector<glm::dvec3> VertexNormals(const TriMesh& m) {
  std::vector<glm::dvec3> n(m.positions.size(), glm::dvec3(0.0));
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    const glm::dvec3 an = m.AreaNormal(t);
    for (int c = 0; c < 3; c++)
      n[m.triangles[t][c]] += an;
  }
  for (glm::dvec3& v : n) {
    const double len = glm::length(v);
    v = len > 0.0 ? v / len : glm::dvec3(0.0);
  }
  return n;
}

// An area-weighted percentile of `values` (each with its weight).
double WeightedPercentile(std::vector<std::pair<double, double>> values,
                          double share) {
  if (values.empty())
    return 0.0;
  std::sort(values.begin(), values.end());
  double total = 0.0;
  for (const auto& v : values)
    total += v.second;
  double run = 0.0;
  for (const auto& v : values) {
    run += v.second;
    if (run >= share * total)
      return v.first;
  }
  return values.back().first;
}

struct CellKey {
  int64_t x, y, z;
  bool operator==(const CellKey&) const = default;
};
struct CellHash {
  size_t operator()(const CellKey& k) const {
    return std::hash<int64_t>()(k.x * 73856093LL ^ k.y * 19349663LL ^
                                k.z * 83492791LL);
  }
};

uint32_t Find(std::vector<uint32_t>& parent, uint32_t x) {
  while (parent[x] != x) {
    parent[x] = parent[parent[x]];
    x = parent[x];
  }
  return x;
}

} // namespace

Deviation CompareToBody(std::shared_ptr<const TriMesh> scanPtr,
                        std::shared_ptr<const TriMesh> bodyPtr,
                        const std::vector<uint32_t>& bodyFaces,
                        const DeviationOptions& o) {
  Deviation out;
  const TriMesh& scan = *scanPtr;
  const TriMesh& body = *bodyPtr;
  const size_t nv = scan.positions.size();
  out.distanceMm.assign(nv, std::numeric_limits<float>::quiet_NaN());
  out.faceOf.assign(nv, kNone);
  if (scan.triangles.empty() || body.triangles.empty())
    return out;

  // The tolerance: asked, or the scan's own noise; never under the chord.
  out.noiseMm = ScanTolerance(scan);
  out.chordMm = o.chordMm;
  out.toleranceMm = o.toleranceMm > 0.0 ? o.toleranceMm : out.noiseMm;
  out.toleranceFrom = o.toleranceMm > 0.0 ? "asked" : "scan_noise";
  if (o.chordMm > out.toleranceMm) {
    out.toleranceMm = o.chordMm;
    out.toleranceFrom = "chord";
  }
  const double tol = out.toleranceMm;
  glm::dvec3 blo, bhi;
  body.Bounds(blo, bhi);
  out.reachMm = std::max(20.0 * tol, glm::length(bhi - blo) / 10.0);
  out.spacingMm = MedianEdgeLength(scan);

  const auto cancelled = [&]() {
    return o.cancel != nullptr && o.cancel->load(std::memory_order_relaxed);
  };
  const auto report = [&](double f) {
    if (o.progress)
      o.progress(f);
  };

  // Scan -> body: every vertex, in blocks.
  const std::vector<glm::dvec3> scanNormal = VertexNormals(scan);
  std::vector<double> vertexArea(nv, 0.0);
  for (uint32_t t = 0; t < scan.triangles.size(); t++) {
    const double a = scan.Area(t) / 3.0;
    for (int c = 0; c < 3; c++)
      vertexArea[scan.triangles[t][c]] += a;
  }
  const FacingSurface bodySurface(Bvh::For(bodyPtr));
  const Pseudonormals pn(body);
  constexpr size_t kBlock = 2048;
  const size_t blocks = (nv + kBlock - 1) / kBlock;
  // The other direction is a sample of the body: its share of the work.
  const size_t samples = std::clamp<size_t>(nv, size_t{4000}, size_t{100000});
  const double forward = double(nv) / double(nv + samples);
  std::vector<uint8_t> opposedAt(nv, 0);
  std::atomic<size_t> doneBlocks{0};
  std::atomic<bool> stop{false};
  parallel::For(blocks, [&](size_t b) {
    if (stop.load(std::memory_order_relaxed))
      return;
    if (cancelled()) {
      stop.store(true);
      return;
    }
    const size_t end = std::min(nv, (b + 1) * kBlock);
    for (size_t i = b * kBlock; i < end; i++) {
      if (!(vertexArea[i] > 0.0))
        continue; // an unused vertex
      const glm::dvec3& v = scan.positions[i];
      Bvh::Closest c = o.minFacingCos > -1.0
                           ? bodySurface.Nearest(v, scanNormal[i],
                                                 o.minFacingCos, out.reachMm)
                           : bodySurface.Tree().ClosestPoint(v, out.reachMm);
      if (!c.found)
        c = bodySurface.Tree().ClosestPoint(v, out.reachMm);
      if (!c.found)
        continue;
      // Measured to a surface that faces against the scan there: none
      // faced its way within reach, or (every surface admitted) the body
      // is turned inside out.
      opposedAt[i] =
          glm::dot(bodySurface.Normal(c.triangle), scanNormal[i]) < 0.0 ? 1 : 0;
      const glm::dvec3 d = v - c.point;
      const double sign =
          glm::dot(d, pn.At(c.triangle, c.point)) < 0.0 ? -1.0 : 1.0;
      out.distanceMm[i] = static_cast<float>(sign * c.distance);
      out.faceOf[i] =
          c.triangle < bodyFaces.size() ? bodyFaces[c.triangle] : kNone;
    }
    const size_t k = doneBlocks.fetch_add(1) + 1;
    if (o.progress && (k % 16 == 0 || k == blocks))
      report(forward * double(k) / double(blocks));
  });
  if (stop.load() || cancelled()) {
    out.cancelled = true;
    return out;
  }

  // The statistics, over the area that has a body within reach.
  double area = 0.0, matched = 0.0, inside = 0.0, sum = 0.0, sumSq = 0.0,
         opposed = 0.0;
  out.minMm = std::numeric_limits<double>::max();
  out.maxMm = -out.minMm;
  std::vector<std::pair<double, double>> magnitudes;
  magnitudes.reserve(nv);
  for (size_t i = 0; i < nv; i++) {
    area += vertexArea[i];
    const double d = out.distanceMm[i];
    if (!(vertexArea[i] > 0.0) || std::isnan(d))
      continue;
    const double w = vertexArea[i];
    matched += w;
    if (opposedAt[i] != 0)
      opposed += w;
    if (std::abs(d) <= tol)
      inside += w;
    sum += w * d;
    sumSq += w * d * d;
    out.minMm = std::min(out.minMm, d);
    out.maxMm = std::max(out.maxMm, d);
    magnitudes.emplace_back(std::abs(d), w);
  }
  if (matched > 0.0) {
    out.within = inside / matched;
    out.meanMm = sum / matched;
    out.rmsMm = std::sqrt(sumSq / matched);
    out.p95Mm = WeightedPercentile(std::move(magnitudes), 0.95);
  } else {
    out.minMm = out.maxMm = 0.0;
  }
  out.unmatched = area > 0.0 ? 1.0 - matched / area : 0.0;
  out.opposed = area > 0.0 ? opposed / area : 0.0;

  // Body -> scan: the body's surface, sampled by area, each point measured
  // to the scan's surface facing its way and lying under it.
  {
    const std::vector<SurfaceSample> s =
        SampleSurface(body, samples, 0xdee1a7ull);
    out.bodySamples = s.size();
    const FacingSurface scanSurface(Bvh::For(scanPtr));
    const double under = std::max(5.0 * tol, 2.0 * out.spacingMm);
    const size_t sblocks = (s.size() + kBlock - 1) / kBlock;
    std::vector<size_t> missing(sblocks, 0);
    std::atomic<size_t> doneS{0};
    parallel::For(sblocks, [&](size_t b) {
      if (stop.load(std::memory_order_relaxed))
        return;
      if (cancelled()) {
        stop.store(true);
        return;
      }
      const size_t end = std::min(s.size(), (b + 1) * kBlock);
      for (size_t i = b * kBlock; i < end; i++)
        if (!scanSurface.Nearest(s[i].point, s[i].normal, o.minFacingCos, under)
                 .found)
          missing[b]++;
      const size_t k = doneS.fetch_add(1) + 1;
      if (o.progress && (k % 16 == 0 || k == sblocks))
        report(forward + (1.0 - forward) * double(k) / double(sblocks));
    });
    if (stop.load() || cancelled()) {
      out.cancelled = true;
      return out;
    }
    const size_t miss =
        std::accumulate(missing.begin(), missing.end(), size_t{0});
    out.noScan = s.empty() ? 0.0 : double(miss) / double(s.size());
  }

  // The hotspots: the vertices past the tolerance, joined to every other of
  // the same sign within a few point spacings -- by distance, not by shared
  // triangles, so a patch the noise breaks up is still one.
  const double link = std::max(3.0 * out.spacingMm, 1e-9);
  std::vector<uint32_t> past;
  for (uint32_t i = 0; i < nv; i++) {
    const double d = out.distanceMm[i];
    if (!std::isnan(d) && std::abs(d) > tol && vertexArea[i] > 0.0)
      past.push_back(i);
  }
  std::vector<uint32_t> parent(past.size());
  std::iota(parent.begin(), parent.end(), 0u);
  {
    std::unordered_map<CellKey, std::vector<uint32_t>, CellHash> grid;
    grid.reserve(past.size());
    const auto cell = [&](const glm::dvec3& p) {
      return CellKey{static_cast<int64_t>(std::floor(p.x / link)),
                     static_cast<int64_t>(std::floor(p.y / link)),
                     static_cast<int64_t>(std::floor(p.z / link))};
    };
    for (uint32_t k = 0; k < past.size(); k++)
      grid[cell(scan.positions[past[k]])].push_back(k);
    for (uint32_t k = 0; k < past.size(); k++) {
      const glm::dvec3& p = scan.positions[past[k]];
      const bool positive = out.distanceMm[past[k]] > 0.0f;
      const CellKey c = cell(p);
      for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
          for (int dz = -1; dz <= 1; dz++) {
            const auto it = grid.find({c.x + dx, c.y + dy, c.z + dz});
            if (it == grid.end())
              continue;
            for (uint32_t j : it->second) {
              if (j <= k || (out.distanceMm[past[j]] > 0.0f) != positive ||
                  glm::length(scan.positions[past[j]] - p) > link)
                continue;
              const uint32_t a = Find(parent, k), bb = Find(parent, j);
              if (a != bb)
                parent[std::max(a, bb)] = std::min(a, bb);
            }
          }
    }
  }
  std::unordered_map<uint32_t, size_t> spotOf;
  for (uint32_t k = 0; k < past.size(); k++) {
    const uint32_t root = Find(parent, k);
    const uint32_t v = past[k];
    const glm::dvec3& p = scan.positions[v];
    const double d = out.distanceMm[v];
    auto [it, added] = spotOf.emplace(root, out.hotspots.size());
    if (added) {
      DeviationHotspot h;
      h.lo = h.hi = p;
      out.hotspots.push_back(h);
    }
    DeviationHotspot& h = out.hotspots[it->second];
    h.vertices++;
    h.areaMm2 += vertexArea[v];
    h.lo = glm::min(h.lo, p);
    h.hi = glm::max(h.hi, p);
    if (std::abs(d) > std::abs(h.depthMm)) {
      h.depthMm = d;
      h.atMm = p;
      h.face = out.faceOf[v];
    }
  }
  for (DeviationHotspot& h : out.hotspots)
    h.feature =
        h.areaMm2 >= kHotspotFeatureAreaMm2 && std::abs(h.depthMm) >= 2.0 * tol;
  std::stable_sort(out.hotspots.begin(), out.hotspots.end(),
                   [](const DeviationHotspot& a, const DeviationHotspot& b) {
                     return std::abs(a.depthMm) > std::abs(b.depthMm);
                   });
  report(1.0);
  return out;
}

} // namespace mesh
