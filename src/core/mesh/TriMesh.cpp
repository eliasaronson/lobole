// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/TriMesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

} // namespace mesh
