// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemMeshReport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

namespace fem {

namespace {

glm::dvec3 Centroid(const FemMesh& mesh, std::size_t t) {
  const auto& e = mesh.tets[t];
  return 0.25 * (mesh.nodes[e[0]] + mesh.nodes[e[1]] + mesh.nodes[e[2]] +
                 mesh.nodes[e[3]]);
}

uint64_t PairKey(int a, int b) {
  if (a > b)
    std::swap(a, b);
  return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) |
         static_cast<uint32_t>(b);
}

} // namespace

MeshBodyReport ReportMesh(const FemMesh& mesh, const MeshSettings& asked,
                          double autoSize, std::size_t worstLimit) {
  MeshBodyReport r;
  r.nodes = mesh.nodes.size();
  r.tets = mesh.tets.size();
  r.boundaryTris = mesh.boundary.size();
  r.straightenedNodes = mesh.stats.straightenedNodes;
  r.volumeMm3 = mesh.stats.volumeMm3;
  r.shapeVolumeMm3 = mesh.stats.shapeVolumeMm3;
  if (r.shapeVolumeMm3 > 0.0)
    r.volumeSlackMm3 =
        MeshVolumeSlack(mesh, r.shapeVolumeMm3, mesh.stats.shapeVolumeErrorMm3);
  r.worstQuality = 1.0;

  // --- Every element's measures, and the worst of them.
  const int sign = MeshOrientation(mesh);
  r.quality.resize(mesh.tets.size());
  r.size.resize(mesh.tets.size());
  std::vector<TetMeasure> measures(mesh.tets.size());
  double sizeSum = 0.0;
  for (std::size_t t = 0; t < mesh.tets.size(); ++t) {
    const TetMeasure m = MeasureTet(mesh, t, sign == 0 ? 1 : sign);
    measures[t] = m;
    r.quality[t] = static_cast<float>(m.Quality());
    r.size[t] = static_cast<float>(m.size);
    r.sizeMin = t == 0 ? m.size : std::min(r.sizeMin, m.size);
    r.sizeMax = std::max(r.sizeMax, m.size);
    sizeSum += m.size;
  }
  if (!mesh.tets.empty())
    r.sizeMean = sizeSum / static_cast<double>(mesh.tets.size());
  {
    std::vector<int> order(mesh.tets.size());
    std::iota(order.begin(), order.end(), 0);
    const std::size_t n = std::min(worstLimit, order.size());
    std::partial_sort(
        order.begin(), order.begin() + n, order.end(), [&](int a, int b) {
          return r.quality[a] != r.quality[b] ? r.quality[a] < r.quality[b]
                                              : a < b;
        });
    for (std::size_t k = 0; k < n; ++k) {
      const int t = order[k];
      r.worst.push_back({t, Centroid(mesh, t), measures[t].Quality(),
                         measures[t].shape, measures[t].jacobian,
                         measures[t].size});
    }
    if (!r.worst.empty())
      r.worstQuality = r.worst.front().quality;
  }

  // --- Which faces each boundary node lies on, and which faces touch.
  std::vector<std::vector<int>> facesOf(mesh.nodes.size());
  std::set<uint64_t> touching;
  for (const FemMesh::BoundaryTri& tri : mesh.boundary)
    for (int k = 0; k < 3; ++k) {
      std::vector<int>& f = facesOf[tri.nodes[k]];
      if (std::find(f.begin(), f.end(), tri.faceIndex) == f.end())
        f.push_back(tri.faceIndex);
    }
  for (const std::vector<int>& f : facesOf)
    for (std::size_t i = 0; i < f.size(); ++i)
      for (std::size_t j = i + 1; j < f.size(); ++j)
        touching.insert(PairKey(f[i], f[j]));

  // --- Walls one element through.
  {
    std::map<uint64_t, MeshBodyReport::ThinWall> walls;
    for (std::size_t t = 0; t < mesh.tets.size(); ++t) {
      const auto& e = mesh.tets[t];
      bool allOnBoundary = true;
      for (int k = 0; k < 4 && allOnBoundary; ++k)
        allOnBoundary = !facesOf[e[k]].empty();
      if (!allOnBoundary)
        continue;
      // The closest pair of corners on two faces that do not touch.
      double best = -1.0;
      int bestA = -1, bestB = -1;
      for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
          for (int fa : facesOf[e[i]])
            for (int fb : facesOf[e[j]]) {
              if (fa == fb || touching.count(PairKey(fa, fb)))
                continue;
              const double d = glm::length(mesh.nodes[e[i]] - mesh.nodes[e[j]]);
              if (best < 0.0 || d < best) {
                best = d;
                bestA = std::min(fa, fb);
                bestB = std::max(fa, fb);
              }
            }
      if (bestA < 0)
        continue;
      MeshBodyReport::ThinWall& w = walls[PairKey(bestA, bestB)];
      if (w.tets == 0 || best < w.thicknessMm) {
        w.thicknessMm = best;
        w.at = Centroid(mesh, t);
      }
      w.faceA = bestA;
      w.faceB = bestB;
      ++w.tets;
    }
    for (auto& [key, w] : walls)
      r.thinWalls.push_back(w);
    std::sort(r.thinWalls.begin(), r.thinWalls.end(),
              [](const auto& a, const auto& b) { return a.tets > b.tets; });
  }

  // --- Refinements that did not take.
  const double global = asked.maxSize > 0.0 ? asked.maxSize : autoSize;
  for (const auto& [face, size] : asked.faceSizes) {
    double len = 0.0;
    std::size_t edges = 0;
    glm::dvec3 at(0.0);
    for (const FemMesh::BoundaryTri& tri : mesh.boundary) {
      if (tri.faceIndex != face)
        continue;
      for (int k = 0; k < 3; ++k) {
        len += glm::length(mesh.nodes[tri.nodes[k]] -
                           mesh.nodes[tri.nodes[(k + 1) % 3]]);
        at += mesh.nodes[tri.nodes[k]];
        ++edges;
      }
    }
    MeshBodyReport::RefinementMiss miss;
    miss.faceIndex = face;
    miss.askedMm = size;
    if (edges == 0) {
      miss.why = "no element lies on the face";
      r.refinementMisses.push_back(miss);
      continue;
    }
    miss.gotMm = len / static_cast<double>(edges);
    miss.at = at / static_cast<double>(edges);
    // Netgen holds an edge near the size it is given, so a face whose
    // edges average half again as much was not sized by it.
    if (global > 0.0 && size >= global) {
      miss.why = "it asks for no less than the study's mesh size";
      r.refinementMisses.push_back(miss);
    } else if (miss.gotMm > 1.5 * size) {
      miss.why = "the face's elements are larger than it asked for";
      r.refinementMisses.push_back(miss);
    }
  }

  // --- Glued solids: what each pair of domains shares.
  if (!mesh.tetDomain.empty()) {
    std::set<int> doms(mesh.tetDomain.begin(), mesh.tetDomain.end());
    r.domains = static_cast<int>(doms.size());
    if (r.domains > 1) {
      // Each element face by its sorted corners; one met from two domains
      // is a shared interface face.
      struct Seen {
        int domain;
        int tet;
      };
      std::map<std::array<int, 3>, Seen> open;
      std::map<uint64_t, MeshBodyReport::Interface> pairs;
      static const int kFace[4][3] = {
          {0, 1, 2}, {0, 1, 3}, {1, 2, 3}, {0, 2, 3}};
      for (std::size_t t = 0; t < mesh.tets.size(); ++t) {
        const auto& e = mesh.tets[t];
        const int dom = mesh.tetDomain[t];
        for (const auto& f : kFace) {
          std::array<int, 3> key = {e[f[0]], e[f[1]], e[f[2]]};
          std::sort(key.begin(), key.end());
          auto it = open.find(key);
          if (it == open.end()) {
            open[key] = {dom, static_cast<int>(t)};
            continue;
          }
          if (it->second.domain != dom) {
            MeshBodyReport::Interface& p =
                pairs[PairKey(it->second.domain, dom)];
            p.domainA = std::min(it->second.domain, dom);
            p.domainB = std::max(it->second.domain, dom);
            if (p.sharedFaces == 0)
              p.at = (mesh.nodes[key[0]] + mesh.nodes[key[1]] +
                      mesh.nodes[key[2]]) /
                     3.0;
            ++p.sharedFaces;
          }
          open.erase(it);
        }
      }
      std::set<int> joined;
      for (auto& [key, p] : pairs) {
        r.interfaces.push_back(p);
        joined.insert(p.domainA);
        joined.insert(p.domainB);
      }
      for (int d : doms)
        if (!joined.count(d))
          r.looseDomains.push_back(d);
      // Corner nodes of two domains at one place that are two nodes: the
      // solids touch there and the mesh does not join them.
      glm::dvec3 lo = mesh.nodes.empty() ? glm::dvec3(0.0) : mesh.nodes[0],
                 hi = lo;
      for (const glm::dvec3& p : mesh.nodes) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
      }
      const double cell = std::max(glm::length(hi - lo) * 1e-7, 1e-9);
      std::unordered_map<uint64_t, int> at;
      const auto cellOf = [&](const glm::dvec3& p) {
        const auto q = [&](double v, double o) {
          return static_cast<uint64_t>(std::llround((v - o) / cell));
        };
        return q(p.x, lo.x) * 73856093ull ^ q(p.y, lo.y) * 19349663ull ^
               q(p.z, lo.z) * 83492791ull;
      };
      std::vector<char> onBoundary(mesh.nodes.size(), 0);
      for (const FemMesh::BoundaryTri& tri : mesh.boundary)
        for (int n : tri.nodes)
          onBoundary[n] = 1;
      for (std::size_t n = 0; n < mesh.nodes.size(); ++n) {
        if (!onBoundary[n])
          continue;
        const auto [it, fresh] =
            at.emplace(cellOf(mesh.nodes[n]), static_cast<int>(n));
        if (!fresh &&
            glm::length(mesh.nodes[it->second] - mesh.nodes[n]) <= cell) {
          if (r.unsharedNodes == 0)
            r.unsharedAt = mesh.nodes[n];
          ++r.unsharedNodes;
        }
      }
    }
  }
  return r;
}

} // namespace fem
