// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Shared test helper: a deterministic, Netgen-free structured tet10 box mesh,
// used by the topology-optimization gate (test_topopt.cpp) and the GPU solver
// gate (test_femgpu.cpp) so their metrics can't drift with mesher versions.
// nx*ny*nz hexes over [0,Lx]x[0,Ly]x[0,Lz], each hex split into 6 tets sharing
// the main diagonal (Kuhn decomposition). Midside nodes are shared across tets
// and hexes via an edge map, giving a conforming tet10 mesh; boundary tri6
// facets on the six box planes are tagged faceIndex 0..5.

#include "core/fem/FemMesher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

inline fem::FemMesh GenerateBoxMesh(int nx, int ny, int nz, double Lx,
                                    double Ly, double Lz) {
  fem::FemMesh m;
  auto cidx = [&](int i, int j, int k) {
    return (i * (ny + 1) + j) * (nz + 1) + k;
  };
  for (int i = 0; i <= nx; ++i)
    for (int j = 0; j <= ny; ++j)
      for (int k = 0; k <= nz; ++k)
        m.nodes.push_back(glm::dvec3(i * Lx / nx, j * Ly / ny, k * Lz / nz));

  std::map<std::pair<int, int>, int> midOf;
  auto mid = [&](int a, int b) {
    auto k = std::minmax(a, b);
    auto it = midOf.find({k.first, k.second});
    if (it != midOf.end())
      return it->second;
    const int id = static_cast<int>(m.nodes.size());
    m.nodes.push_back(0.5 * (m.nodes[a] + m.nodes[b]));
    midOf[{k.first, k.second}] = id;
    return id;
  };

  // Hex corner offsets (v0..v7) and the 6-tet decomposition (all share v0-v6).
  static const int off[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
  static const int tets6[6][4] = {{0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6},
                                  {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}};
  // Canonical tet10 edge order for slots 4..9.
  static const int te[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}};
  // The tet's 4 triangular faces (each omitting one corner).
  static const int tf[4][3] = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}};

  auto faceIndexOf = [&](int a, int b, int c) -> int {
    const glm::dvec3 &pa = m.nodes[a], &pb = m.nodes[b], &pc = m.nodes[c];
    const double tol = 1e-9;
    auto all = [&](int axis, double v) {
      return std::abs(pa[axis] - v) < tol && std::abs(pb[axis] - v) < tol &&
             std::abs(pc[axis] - v) < tol;
    };
    if (all(0, 0))
      return 0;
    if (all(0, Lx))
      return 1;
    if (all(1, 0))
      return 2;
    if (all(1, Ly))
      return 3;
    if (all(2, 0))
      return 4;
    if (all(2, Lz))
      return 5;
    return -1;
  };

  for (int ci = 0; ci < nx; ++ci)
    for (int cj = 0; cj < ny; ++cj)
      for (int ck = 0; ck < nz; ++ck) {
        int gc[8];
        for (int v = 0; v < 8; ++v)
          gc[v] = cidx(ci + off[v][0], cj + off[v][1], ck + off[v][2]);
        for (const auto& t : tets6) {
          const int cc[4] = {gc[t[0]], gc[t[1]], gc[t[2]], gc[t[3]]};
          std::array<int, 10> tn;
          for (int i = 0; i < 4; ++i)
            tn[i] = cc[i];
          for (int s = 0; s < 6; ++s)
            tn[4 + s] = mid(cc[te[s][0]], cc[te[s][1]]);
          m.tets.push_back(tn);
          for (const auto& f : tf) {
            const int a = cc[f[0]], b = cc[f[1]], c = cc[f[2]];
            const int fi = faceIndexOf(a, b, c);
            if (fi < 0)
              continue;
            fem::FemMesh::BoundaryTri bt;
            bt.faceIndex = fi;
            bt.nodes = {a, b, c, mid(a, b), mid(b, c), mid(c, a)};
            m.boundary.push_back(bt);
          }
        }
      }
  m.stats.nodeCount = m.nodes.size();
  m.stats.tetCount = m.tets.size();
  m.stats.worstQuality = 0.3; // decorative; the solver ignores it
  return m;
}

inline std::vector<int> NodesAtPlane(const fem::FemMesh& m, int axis,
                                     double value, double tol) {
  std::vector<int> out;
  for (int i = 0; i < (int)m.nodes.size(); ++i)
    if (std::abs(m.nodes[i][axis] - value) < tol)
      out.push_back(i);
  return out;
}
inline std::vector<int> TrisAtPlane(const fem::FemMesh& m, int axis,
                                    double value, double tol) {
  std::vector<int> out;
  for (int t = 0; t < (int)m.boundary.size(); ++t) {
    bool on = true;
    for (int k = 0; k < 6; ++k)
      if (std::abs(m.nodes[m.boundary[t].nodes[k]][axis] - value) >= tol)
        on = false;
    if (on)
      out.push_back(t);
  }
  return out;
}
