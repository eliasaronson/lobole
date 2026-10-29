// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/TriMesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace mesh {

glm::dvec3 TriMesh::AreaNormal(uint32_t t) const {
  const glm::dvec3 a = Corner(t, 0), b = Corner(t, 1), c = Corner(t, 2);
  return glm::cross(b - a, c - a);
}

double TriMesh::Area(uint32_t t) const {
  return 0.5 * glm::length(AreaNormal(t));
}

double TriMesh::Volume() const {
  double v = 0.0;
  for (uint32_t t = 0; t < triangles.size(); t++)
    v += glm::dot(Corner(t, 0), glm::cross(Corner(t, 1), Corner(t, 2)));
  return v / 6.0;
}

double TriMesh::SurfaceArea() const {
  double a = 0.0;
  for (uint32_t t = 0; t < triangles.size(); t++)
    a += Area(t);
  return a;
}

void TriMesh::Bounds(glm::dvec3& lo, glm::dvec3& hi) const {
  lo = hi = glm::dvec3(0.0);
  if (positions.empty())
    return;
  lo = hi = positions.front();
  for (const glm::dvec3& p : positions) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
}

uint64_t TriMesh::Hash() const {
  // FNV-1a over the bytes: stable across runs and platforms of one
  // endianness, which is all a cache key needs.
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&](const void* data, size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; i++) {
      h ^= p[i];
      h *= 1099511628211ull;
    }
  };
  mix(positions.data(), positions.size() * sizeof(glm::dvec3));
  mix(triangles.data(), triangles.size() * sizeof(triangles[0]));
  return h;
}

double Float32Quantum(const std::vector<glm::dvec3>& positions) {
  double largest = 0.0;
  for (const glm::dvec3& p : positions)
    largest = std::max({largest, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
  if (largest == 0.0)
    return 0.0;
  int exponent = 0;
  std::frexp(largest, &exponent); // largest = m * 2^exponent, m in [0.5, 1)
  return std::ldexp(1.0, exponent - 24);
}

namespace {

struct BitsKey {
  uint64_t x, y, z;
  bool operator==(const BitsKey& o) const {
    return x == o.x && y == o.y && z == o.z;
  }
};
struct BitsHash {
  size_t operator()(const BitsKey& k) const {
    uint64_t h = k.x * 0x9E3779B97F4A7C15ull;
    h ^= k.y + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= k.z + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return static_cast<size_t>(h);
  }
};

BitsKey Bits(const glm::dvec3& p) {
  BitsKey k{};
  // +0.0 and -0.0 are one point.
  const double x = p.x == 0.0 ? 0.0 : p.x, y = p.y == 0.0 ? 0.0 : p.y,
               z = p.z == 0.0 ? 0.0 : p.z;
  std::memcpy(&k.x, &x, 8);
  std::memcpy(&k.y, &y, 8);
  std::memcpy(&k.z, &z, 8);
  return k;
}

struct CellKey {
  int64_t x, y, z;
  bool operator==(const CellKey& o) const {
    return x == o.x && y == o.y && z == o.z;
  }
};
struct CellHash {
  size_t operator()(const CellKey& k) const {
    return static_cast<size_t>(k.x * 73856093ll ^ k.y * 19349663ll ^
                               k.z * 83492791ll);
  }
};

} // namespace

TriMesh FromSoup(const std::vector<glm::dvec3>& soup, double quantum) {
  TriMesh m;
  m.quantum = quantum;
  // Exact first.
  std::unordered_map<BitsKey, uint32_t, BitsHash> exact;
  exact.reserve(soup.size() / 2 + 1);
  std::vector<uint32_t> index(soup.size());
  for (size_t i = 0; i < soup.size(); i++) {
    const auto [it, added] =
        exact.emplace(Bits(soup[i]), static_cast<uint32_t>(m.positions.size()));
    if (added)
      m.positions.push_back(soup[i]);
    index[i] = it->second;
  }
  // Then within a quantum: a grid of quantum-sized cells, each vertex looked
  // up in its own and the 26 neighbouring cells. Points closer than that are
  // the same point written twice with different rounding.
  if (quantum > 0.0) {
    std::unordered_map<CellKey, std::vector<uint32_t>, CellHash> grid;
    grid.reserve(m.positions.size());
    std::vector<uint32_t> remap(m.positions.size());
    std::vector<glm::dvec3> kept;
    kept.reserve(m.positions.size());
    const auto cell = [&](const glm::dvec3& p) {
      return CellKey{static_cast<int64_t>(std::floor(p.x / quantum)),
                     static_cast<int64_t>(std::floor(p.y / quantum)),
                     static_cast<int64_t>(std::floor(p.z / quantum))};
    };
    for (uint32_t v = 0; v < m.positions.size(); v++) {
      const glm::dvec3& p = m.positions[v];
      const CellKey c = cell(p);
      uint32_t found = kNone;
      for (int dx = -1; dx <= 1 && found == kNone; dx++)
        for (int dy = -1; dy <= 1 && found == kNone; dy++)
          for (int dz = -1; dz <= 1 && found == kNone; dz++) {
            const auto it = grid.find({c.x + dx, c.y + dy, c.z + dz});
            if (it == grid.end())
              continue;
            for (uint32_t k : it->second)
              if (glm::length(kept[k] - p) <= quantum) {
                found = k;
                break;
              }
          }
      if (found == kNone) {
        found = static_cast<uint32_t>(kept.size());
        kept.push_back(p);
        grid[c].push_back(found);
      }
      remap[v] = found;
    }
    if (kept.size() != m.positions.size()) {
      m.positions = std::move(kept);
      for (uint32_t& i : index)
        i = remap[i];
    }
  }
  m.triangles.reserve(soup.size() / 3);
  for (size_t i = 0; i + 2 < soup.size(); i += 3) {
    const uint32_t a = index[i], b = index[i + 1], c = index[i + 2];
    if (a == b || b == c || a == c)
      continue;
    m.triangles.push_back({a, b, c});
  }
  BuildAdjacency(m);
  return m;
}

void BuildAdjacency(TriMesh& m) {
  const size_t n = m.triangles.size();
  m.neighbour.assign(n, {kNone, kNone, kNone});
  m.neighbourEdge.assign(n, {0, 0, 0});
  m.boundaryEdges = m.nonManifoldEdges = m.flippedEdges = 0;
  // Every half-edge keyed by its undirected vertex pair, sorted, so the
  // pairs come out together without a hash map.
  struct Half {
    uint64_t key;
    uint32_t tri;
    uint8_t edge;
    bool forward; // a < b along the triangle's own direction
  };
  std::vector<Half> halves;
  halves.reserve(n * 3);
  for (uint32_t t = 0; t < n; t++)
    for (uint8_t e = 0; e < 3; e++) {
      const uint32_t a = m.triangles[t][e], b = m.triangles[t][(e + 1) % 3];
      const uint32_t lo = std::min(a, b), hi = std::max(a, b);
      halves.push_back({(uint64_t(lo) << 32) | hi, t, e, a < b});
    }
  std::sort(halves.begin(), halves.end(), [](const Half& x, const Half& y) {
    return x.key != y.key ? x.key < y.key
                          : (x.tri != y.tri ? x.tri < y.tri : x.edge < y.edge);
  });
  for (size_t i = 0; i < halves.size();) {
    size_t j = i + 1;
    while (j < halves.size() && halves[j].key == halves[i].key)
      j++;
    const size_t count = j - i;
    if (count == 1) {
      m.boundaryEdges++;
    } else if (count == 2) {
      const Half& x = halves[i];
      const Half& y = halves[i + 1];
      if (x.forward == y.forward)
        m.flippedEdges++;
      m.neighbour[x.tri][x.edge] = y.tri;
      m.neighbourEdge[x.tri][x.edge] = y.edge;
      m.neighbour[y.tri][y.edge] = x.tri;
      m.neighbourEdge[y.tri][y.edge] = x.edge;
    } else {
      m.nonManifoldEdges++;
    }
    i = j;
  }
}

size_t Components(const TriMesh& m, std::vector<uint32_t>& componentOf) {
  componentOf.assign(m.triangles.size(), kNone);
  size_t count = 0;
  std::vector<uint32_t> stack;
  for (uint32_t seed = 0; seed < m.triangles.size(); seed++) {
    if (componentOf[seed] != kNone)
      continue;
    const auto id = static_cast<uint32_t>(count++);
    componentOf[seed] = id;
    stack.push_back(seed);
    while (!stack.empty()) {
      const uint32_t t = stack.back();
      stack.pop_back();
      for (int e = 0; e < 3; e++) {
        const uint32_t u = m.neighbour[t][e];
        if (u != kNone && componentOf[u] == kNone) {
          componentOf[u] = id;
          stack.push_back(u);
        }
      }
    }
  }
  return count;
}

std::vector<double> BoundaryLoops(const TriMesh& m) {
  std::unordered_multimap<uint32_t, uint32_t> next; // from -> to
  for (uint32_t t = 0; t < m.triangles.size() && t < m.neighbour.size(); t++)
    for (int e = 0; e < 3; e++)
      if (m.neighbour[t][e] == kNone)
        next.emplace(m.triangles[t][(e + 1) % 3], m.triangles[t][e]);
  std::vector<double> loops;
  while (!next.empty()) {
    auto it = next.begin();
    const uint32_t start = it->first;
    uint32_t at = start;
    double length = 0.0;
    do {
      const uint32_t to = it->second;
      length += glm::length(m.positions[to] - m.positions[at]);
      next.erase(it);
      at = to;
      it = next.find(at);
    } while (at != start && it != next.end());
    loops.push_back(length);
  }
  std::sort(loops.begin(), loops.end(), std::greater<>());
  return loops;
}

MassProperties ComputeMassProperties(const TriMesh& m) {
  // Tonon (2004) / Eberly: per tetrahedron (origin, a, b, c), accumulated as
  // the integrals of 1, x, y, z, x², y², z², xy, yz, zx.
  MassProperties r;
  double intg[10] = {0};
  const auto sub = [](double w0, double w1, double w2, double& f1, double& f2,
                      double& f3, double& g0, double& g1, double& g2) {
    const double t0 = w0 + w1;
    f1 = t0 + w2;
    const double t1 = w0 * w0, t2 = t1 + w1 * t0;
    f2 = t2 + w2 * f1;
    f3 = w0 * t1 + w1 * t2 + w2 * f2;
    g0 = f2 + w0 * (f1 + w0);
    g1 = f2 + w1 * (f1 + w1);
    g2 = f2 + w2 * (f1 + w2);
  };
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    const glm::dvec3 p0 = m.Corner(t, 0), p1 = m.Corner(t, 1),
                     p2 = m.Corner(t, 2);
    r.area += m.Area(t);
    const glm::dvec3 d = glm::cross(p1 - p0, p2 - p0);
    double f1x, f2x, f3x, g0x, g1x, g2x, f1y, f2y, f3y, g0y, g1y, g2y, f1z, f2z,
        f3z, g0z, g1z, g2z;
    sub(p0.x, p1.x, p2.x, f1x, f2x, f3x, g0x, g1x, g2x);
    sub(p0.y, p1.y, p2.y, f1y, f2y, f3y, g0y, g1y, g2y);
    sub(p0.z, p1.z, p2.z, f1z, f2z, f3z, g0z, g1z, g2z);
    intg[0] += d.x * f1x;
    intg[1] += d.x * f2x;
    intg[2] += d.y * f2y;
    intg[3] += d.z * f2z;
    intg[4] += d.x * f3x;
    intg[5] += d.y * f3y;
    intg[6] += d.z * f3z;
    intg[7] += d.x * (p0.y * g0x + p1.y * g1x + p2.y * g2x);
    intg[8] += d.y * (p0.z * g0y + p1.z * g1y + p2.z * g2y);
    intg[9] += d.z * (p0.x * g0z + p1.x * g1z + p2.x * g2z);
  }
  const double mult[10] = {1.0 / 6,  1.0 / 24, 1.0 / 24,  1.0 / 24,  1.0 / 60,
                           1.0 / 60, 1.0 / 60, 1.0 / 120, 1.0 / 120, 1.0 / 120};
  for (int i = 0; i < 10; i++)
    intg[i] *= mult[i];
  r.volume = intg[0];
  if (r.volume == 0.0)
    return r;
  const glm::dvec3 c(intg[1] / r.volume, intg[2] / r.volume,
                     intg[3] / r.volume);
  r.centroid = c;
  const double V = r.volume;
  const double xx = intg[5] + intg[6] - V * (c.y * c.y + c.z * c.z);
  const double yy = intg[4] + intg[6] - V * (c.z * c.z + c.x * c.x);
  const double zz = intg[4] + intg[5] - V * (c.x * c.x + c.y * c.y);
  const double xy = -(intg[7] - V * c.x * c.y);
  const double yz = -(intg[8] - V * c.y * c.z);
  const double zx = -(intg[9] - V * c.z * c.x);
  r.inertia = glm::dmat3(xx, xy, zx, xy, yy, yz, zx, yz, zz);
  return r;
}

TriMesh Transformed(const TriMesh& m, const glm::dmat4& xf) {
  TriMesh out;
  out.quantum = m.quantum;
  out.positions.reserve(m.positions.size());
  for (const glm::dvec3& p : m.positions)
    out.positions.push_back(glm::dvec3(xf * glm::dvec4(p, 1.0)));
  out.triangles = m.triangles;
  if (glm::determinant(glm::dmat3(xf)) < 0.0)
    for (auto& t : out.triangles)
      std::swap(t[1], t[2]);
  // A scale scales the precision the file was written at too.
  out.quantum *= std::cbrt(std::abs(glm::determinant(glm::dmat3(xf))));
  BuildAdjacency(out);
  return out;
}

double MedianEdgeLength(const TriMesh& m) {
  std::vector<double> lengths;
  lengths.reserve(m.triangles.size() * 3);
  for (uint32_t t = 0; t < m.triangles.size(); t++)
    for (int e = 0; e < 3; e++)
      lengths.push_back(glm::length(m.Corner(t, (e + 1) % 3) - m.Corner(t, e)));
  if (lengths.empty())
    return 0.0;
  auto mid = lengths.begin() + lengths.size() / 2;
  std::nth_element(lengths.begin(), mid, lengths.end());
  return *mid;
}

} // namespace mesh
