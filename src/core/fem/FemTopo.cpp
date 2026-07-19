#include "core/fem/FemTopo.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

#ifdef HAVE_FEM_SOLVER
#include "core/fem/FemSolverInternal.h"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCore>
#endif

namespace fem {

// ---- Iso-surface extraction (always compiled; pure geometry) --------------
namespace {

// Signed-magnitude tet volume from the 4 corner nodes (no Eigen).
double CornerTetVolume(const FemMesh& m, const std::array<int, 10>& t) {
  const glm::dvec3& p0 = m.nodes[t[0]];
  const glm::dvec3 a = m.nodes[t[1]] - p0, b = m.nodes[t[2]] - p0,
                   c = m.nodes[t[3]] - p0;
  return std::abs(glm::dot(a, glm::cross(b, c))) / 6.0;
}

// Per-tet density averaged onto the corner nodes it touches.
std::vector<double> CornerNodeDensity(const FemMesh& m,
                                      const std::vector<double>& density) {
  std::vector<double> sum(m.nodes.size(), 0.0);
  std::vector<int> cnt(m.nodes.size(), 0);
  for (size_t e = 0; e < m.tets.size(); ++e)
    for (int i = 0; i < 4; ++i) {
      const int n = m.tets[e][i];
      sum[n] += density[e];
      ++cnt[n];
    }
  for (size_t n = 0; n < sum.size(); ++n)
    if (cnt[n]) sum[n] /= cnt[n];
  return sum;
}

}  // namespace

void ExtractIsoSurface(const FemMesh& mesh, const std::vector<double>& density,
                       double threshold, std::vector<glm::vec3>& positions,
                       std::vector<glm::vec3>& normals,
                       std::vector<uint32_t>& indices) {
  if (density.size() != mesh.tets.size()) return;
  const std::vector<double> nd = CornerNodeDensity(mesh, density);
  const double t = threshold;

  // Position on edge (a,b) where the node-averaged density crosses t.
  auto interp = [&](int a, int b) {
    const glm::dvec3& pa = mesh.nodes[a];
    const glm::dvec3& pb = mesh.nodes[b];
    const double va = nd[a], vb = nd[b];
    double s = std::abs(vb - va) < 1e-12 ? 0.5 : (t - va) / (vb - va);
    s = std::clamp(s, 0.0, 1.0);
    return glm::vec3(pa + s * (pb - pa));
  };

  for (const std::array<int, 10>& tet : mesh.tets) {
    glm::dvec3 meanIn(0.0);
    int nin = 0;
    std::array<int, 4> ins{}, outs{};
    int ni = 0, no = 0;
    for (int i = 0; i < 4; ++i) {
      const int n = tet[i];
      if (nd[n] >= t) { ins[ni++] = n; meanIn += mesh.nodes[n]; ++nin; }
      else { outs[no++] = n; }
    }
    if (nin == 0 || nin == 4) continue;  // no crossing
    meanIn /= nin;

    // Emit a triangle with an outward normal (pointing away from the solid
    // interior) and matching winding, so a backface-culling renderer shows it.
    auto emit = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
      glm::vec3 n = glm::cross(b - a, c - a);
      const float len = glm::length(n);
      if (len < 1e-12f) return;  // degenerate
      n /= len;
      const glm::vec3 ctr = (a + b + c) / 3.0f;
      if (glm::dot(n, glm::vec3(meanIn) - ctr) > 0.0f) {  // points into solid
        std::swap(b, c);
        n = -n;
      }
      const uint32_t base = static_cast<uint32_t>(positions.size());
      positions.push_back(a);
      positions.push_back(b);
      positions.push_back(c);
      normals.push_back(n);
      normals.push_back(n);
      normals.push_back(n);
      indices.push_back(base);
      indices.push_back(base + 1);
      indices.push_back(base + 2);
    };

    if (ni == 1) {  // one solid corner -> a triangle cutting its 3 edges
      emit(interp(ins[0], outs[0]), interp(ins[0], outs[1]),
           interp(ins[0], outs[2]));
    } else if (ni == 3) {  // one void corner -> the complementary triangle
      emit(interp(outs[0], ins[0]), interp(outs[0], ins[1]),
           interp(outs[0], ins[2]));
    } else {  // 2 solid / 2 void -> a quad split into two triangles
      const glm::vec3 ac = interp(ins[0], outs[0]);
      const glm::vec3 ad = interp(ins[0], outs[1]);
      const glm::vec3 bc = interp(ins[1], outs[0]);
      const glm::vec3 bd = interp(ins[1], outs[1]);
      emit(ac, ad, bd);
      emit(ac, bd, bc);
    }
  }

  // ---- Boundary caps ------------------------------------------------------
  // The marching-tet interface above exists only where the density crosses
  // the threshold INSIDE the volume. Solid regions touching the domain
  // boundary have no crossing there, so without caps they render (and STL-
  // export) as OPEN shells — a wall-hugging plate becomes a one-sided
  // membrane. March the boundary triangles too, emitting the solid
  // sub-region of each facet. Cap rim edges use the same nodal interpolation
  // as the interior surface, so the two meet watertight. Winding follows the
  // facet's stored orientation (outward for both mesh sources — the same
  // convention the pressure loads rely on).
  auto emitCap = [&](const glm::vec3& a, const glm::vec3& b,
                     const glm::vec3& c) {
    glm::vec3 n = glm::cross(b - a, c - a);
    const float len = glm::length(n);
    if (len < 1e-12f) return;
    n /= len;
    const uint32_t base = static_cast<uint32_t>(positions.size());
    positions.push_back(a);
    positions.push_back(b);
    positions.push_back(c);
    normals.push_back(n);
    normals.push_back(n);
    normals.push_back(n);
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
  };
  for (const FemMesh::BoundaryTri& bt : mesh.boundary) {
    const int c0 = bt.nodes[0], c1 = bt.nodes[1], c2 = bt.nodes[2];
    const int nin = (nd[c0] >= t) + (nd[c1] >= t) + (nd[c2] >= t);
    if (nin == 0) continue;
    auto P = [&](int n) { return glm::vec3(mesh.nodes[n]); };
    if (nin == 3) {
      emitCap(P(c0), P(c1), P(c2));
      continue;
    }
    // Rotate to a canonical corner so the sub-polygon keeps the facet's
    // cyclic order (and so its outward orientation).
    const int cyc[3] = {c0, c1, c2};
    for (int k = 0; k < 3; ++k) {
      const int i0 = cyc[k], i1 = cyc[(k + 1) % 3], i2 = cyc[(k + 2) % 3];
      const bool b0 = nd[i0] >= t, b1 = nd[i1] >= t, b2 = nd[i2] >= t;
      if (nin == 1 && b0 && !b1 && !b2) {  // one solid corner
        emitCap(P(i0), interp(i0, i1), interp(i0, i2));
        break;
      }
      if (nin == 2 && b0 && b1 && !b2) {  // one void corner -> quad
        emitCap(P(i0), P(i1), interp(i1, i2));
        emitCap(P(i0), interp(i1, i2), interp(i0, i2));
        break;
      }
    }
  }
}

double AutoIsoThreshold(const FemMesh& mesh, const std::vector<double>& density,
                        double targetFraction) {
  if (density.size() != mesh.tets.size() || mesh.tets.empty()) return 0.5;
  std::vector<double> vol(mesh.tets.size());
  double vtot = 0.0;
  for (size_t e = 0; e < mesh.tets.size(); ++e) {
    vol[e] = CornerTetVolume(mesh, mesh.tets[e]);
    vtot += vol[e];
  }
  if (vtot <= 0.0) return 0.5;
  const double target = std::clamp(targetFraction, 0.0, 1.0) * vtot;
  // solidVol(t) = sum vol[e] where density[e] >= t is monotone decreasing in t.
  double lo = 0.0, hi = 1.0;
  for (int it = 0; it < 40; ++it) {
    const double mid = 0.5 * (lo + hi);
    double sv = 0.0;
    for (size_t e = 0; e < vol.size(); ++e)
      if (density[e] >= mid) sv += vol[e];
    if (sv > target) lo = mid; else hi = mid;  // too much solid -> raise t
  }
  return 0.5 * (lo + hi);
}

namespace {

// Welds a triangle soup (positions in triangle order, 3 per tri) into unique
// vertices + triangle index triples. Iso-surface vertices coincident across
// tets are bit-identical, so a fine quantization welds them exactly.
void WeldSurface(const std::vector<glm::vec3>& pos,
                 std::vector<glm::vec3>& outVerts,
                 std::vector<std::array<uint32_t, 3>>& outTris) {
  glm::vec3 mn(1e30f), mx(-1e30f);
  for (const glm::vec3& p : pos) { mn = glm::min(mn, p); mx = glm::max(mx, p); }
  const float diag = glm::length(mx - mn);
  const float eps = std::max(diag * 1e-6f, 1e-9f);
  auto quant = [&](const glm::vec3& p) {
    return std::array<int64_t, 3>{static_cast<int64_t>(std::llround(p.x / eps)),
                                  static_cast<int64_t>(std::llround(p.y / eps)),
                                  static_cast<int64_t>(std::llround(p.z / eps))};
  };
  std::map<std::array<int64_t, 3>, uint32_t> lookup;
  std::vector<uint32_t> remap(pos.size());
  for (size_t i = 0; i < pos.size(); ++i) {
    const auto key = quant(pos[i]);
    auto it = lookup.find(key);
    if (it == lookup.end()) {
      const uint32_t id = static_cast<uint32_t>(outVerts.size());
      lookup.emplace(key, id);
      outVerts.push_back(pos[i]);
      remap[i] = id;
    } else {
      remap[i] = it->second;
    }
  }
  for (size_t t = 0; t + 3 <= pos.size(); t += 3)
    outTris.push_back({remap[t], remap[t + 1], remap[t + 2]});
}

// Taubin lambda/mu smoothing: a Laplacian pass with factor `lambda` (>0) then
// one with `mu` (<0, |mu|>lambda), which cancels the shrinkage a plain
// Laplacian smooth would cause. Vertices on an open (boundary) edge are pinned
// so the surface stays on the design-domain faces it was cut against.
void TaubinSmooth(std::vector<glm::vec3>& verts,
                  const std::vector<std::array<uint32_t, 3>>& tris, int iters) {
  if (iters <= 0 || verts.empty()) return;
  const size_t n = verts.size();
  std::vector<std::vector<uint32_t>> nbr(n);
  std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
  auto addEdge = [&](uint32_t a, uint32_t b) {
    nbr[a].push_back(b);
    nbr[b].push_back(a);
    edgeCount[{std::min(a, b), std::max(a, b)}]++;
  };
  for (const auto& t : tris) {
    addEdge(t[0], t[1]);
    addEdge(t[1], t[2]);
    addEdge(t[2], t[0]);
  }
  std::vector<char> pinned(n, 0);
  for (const auto& [e, c] : edgeCount)
    if (c == 1) { pinned[e.first] = 1; pinned[e.second] = 1; }  // open edge
  // Dedup neighbor lists.
  for (auto& list : nbr) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  }

  const float lambda = 0.5f, mu = -0.53f;
  std::vector<glm::vec3> next(n);
  auto pass = [&](float f) {
    for (size_t i = 0; i < n; ++i) {
      if (pinned[i] || nbr[i].empty()) { next[i] = verts[i]; continue; }
      glm::vec3 c(0.0f);
      for (uint32_t j : nbr[i]) c += verts[j];
      c /= static_cast<float>(nbr[i].size());
      next[i] = verts[i] + f * (c - verts[i]);
    }
    verts.swap(next);
  };
  for (int it = 0; it < iters; ++it) { pass(lambda); pass(mu); }
}

}  // namespace

bool ExportTopoStl(const FemMesh& mesh, const std::vector<double>& density,
                   double threshold, int smoothIters, const std::string& path,
                   std::string* error) {
  std::vector<glm::vec3> pos, nrm;
  std::vector<uint32_t> idx;
  ExtractIsoSurface(mesh, density, threshold, pos, nrm, idx);
  if (pos.empty()) {
    if (error) *error = "the iso-surface is empty at this threshold";
    return false;
  }

  std::vector<glm::vec3> verts;
  std::vector<std::array<uint32_t, 3>> tris;
  WeldSurface(pos, verts, tris);
  TaubinSmooth(verts, tris, smoothIters);

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (error) *error = "cannot open '" + path + "' for writing";
    return false;
  }
  const char header[80] = {0};
  out.write(header, 80);
  const uint32_t nTri = static_cast<uint32_t>(tris.size());
  out.write(reinterpret_cast<const char*>(&nTri), 4);
  auto writeVec = [&](const glm::vec3& v) {
    const float f[3] = {v.x, v.y, v.z};
    out.write(reinterpret_cast<const char*>(f), 12);
  };
  for (const auto& t : tris) {
    const glm::vec3& a = verts[t[0]];
    const glm::vec3& b = verts[t[1]];
    const glm::vec3& c = verts[t[2]];
    glm::vec3 nn = glm::cross(b - a, c - a);
    const float len = glm::length(nn);
    nn = len > 1e-20f ? nn / len : glm::vec3(0.0f);
    writeVec(nn);
    writeVec(a);
    writeVec(b);
    writeVec(c);
    const uint16_t attr = 0;
    out.write(reinterpret_cast<const char*>(&attr), 2);
  }
  if (!out.good()) {
    if (error) *error = "failed writing '" + path + "'";
    return false;
  }
  return true;
}

#ifdef HAVE_FEM_SOLVER

bool TopoAvailable() { return true; }

namespace {

using namespace internal;
using Eigen::Vector3d;

// Element stiffness of one tet10 at full modulus (the D0 matrix passed in),
// as a 30x30 matrix. Recomputed each time it is needed (assembly + sensitivity)
// rather than cached — at ~900 doubles/element the cache would be hundreds of
// MB on a real mesh, and this is negligible next to the sparse factorization.
Eigen::Matrix<double, 30, 30> ElementK0(const FemMesh& mesh,
                                        const std::array<int, 10>& tet,
                                        const Eigen::Matrix<double, 6, 6>& D0) {
  Vector3d X[10];
  for (int i = 0; i < 10; ++i) {
    const auto& p = mesh.nodes[tet[i]];
    X[i] = Vector3d(p.x, p.y, p.z);
  }
  Eigen::Matrix<double, 30, 30> Ke = Eigen::Matrix<double, 30, 30>::Zero();
  for (const auto& g : kTetGauss) {
    Eigen::Matrix<double, 6, 30> B;
    const double detJ = StrainDisp(X, g[0], g[1], g[2], B);
    Ke += (kTetW * std::abs(detJ)) * (B.transpose() * D0 * B);
  }
  return Ke;
}

// Element volume (Gauss sum of |detJ|).
double ElementVolume(const FemMesh& mesh, const std::array<int, 10>& tet) {
  Vector3d X[10];
  for (int i = 0; i < 10; ++i) {
    const auto& p = mesh.nodes[tet[i]];
    X[i] = Vector3d(p.x, p.y, p.z);
  }
  double vol = 0.0;
  for (const auto& g : kTetGauss) {
    Eigen::Matrix<double, 6, 30> B;
    vol += kTetW * std::abs(StrainDisp(X, g[0], g[1], g[2], B));
  }
  return vol;
}

}  // namespace

std::optional<TopoResult> TopologyOptimize(
    const FemMesh& mesh, const Material& material,
    const std::vector<int>& fixedNodes, const std::vector<FaceLoad>& loads,
    const TopoParams& params, std::string* error,
    const std::function<bool(const TopoProgress&)>& progress,
    const std::vector<int>& keepNodes, TopoLinearSolver* solver) {
  auto fail = [&](const std::string& m) -> std::optional<TopoResult> {
    if (error) *error = m;
    return std::nullopt;
  };
  const int nNodes = static_cast<int>(mesh.nodes.size());
  const int nTets = static_cast<int>(mesh.tets.size());
  if (nNodes == 0 || nTets == 0) return fail("empty mesh");
  if (material.youngMPa <= 0) return fail("material has no stiffness data");
  if (params.volumeFraction <= 0.0 || params.volumeFraction >= 1.0)
    return fail("volume fraction must be in (0, 1)");
  if (fixedNodes.empty()) return fail("topology optimization needs a fixed support");
  if (loads.empty()) return fail("topology optimization needs at least one load");

  const double E0 = material.youngMPa;
  const double Emin = 1e-6 * E0;  // ersatz void stiffness (modified SIMP)
  const auto D0 = Elasticity(E0, material.poisson);

  // ---- Per-element geometry (centroid + volume), computed once ------------
  std::vector<Vector3d> centroid(nTets);
  std::vector<double> volE(nTets);
  double Vtot = 0.0, meanSize = 0.0;
  for (int e = 0; e < nTets; ++e) {
    const auto& tet = mesh.tets[e];
    Vector3d c(0, 0, 0);
    for (int i = 0; i < 4; ++i) {  // centroid from the 4 corner nodes
      const auto& pp = mesh.nodes[tet[i]];
      c += Vector3d(pp.x, pp.y, pp.z);
    }
    centroid[e] = c / 4.0;
    volE[e] = ElementVolume(mesh, tet);
    Vtot += volE[e];
    meanSize += std::cbrt(6.0 * volE[e]);  // ~ hex edge for a 6-tet subdivision
  }
  if (Vtot <= 0) return fail("mesh has zero volume");
  meanSize /= nTets;

  const double R = params.filterRadius > 0 ? params.filterRadius : 2.5 * meanSize;
  if (R <= 0) return fail("filter radius must be positive");

  // ---- Frozen set: tets touching a fixed node or a loaded boundary tri ----
  std::vector<char> fixedNode(nNodes, 0);
  for (int n : fixedNodes)
    if (n >= 0 && n < nNodes) fixedNode[n] = 1;
  std::vector<char> loadedNode(nNodes, 0);
  for (const FaceLoad& load : loads)
    for (int ti : load.tris)
      if (ti >= 0 && ti < static_cast<int>(mesh.boundary.size()))
        for (int n : mesh.boundary[ti].nodes) loadedNode[n] = 1;
  for (int n : keepNodes)  // user keep-faces freeze solid too
    if (n >= 0 && n < nNodes) loadedNode[n] = 1;
  std::vector<char> frozen(nTets, 0);
  for (int e = 0; e < nTets; ++e)
    for (int i = 0; i < 10; ++i) {
      const int n = mesh.tets[e][i];
      if (fixedNode[n] || loadedNode[n]) { frozen[e] = 1; break; }
    }

  // ---- Density filter: uniform grid over centroids, cell size R -----------
  // Weight w_ej = max(0, R - |c_e - c_j|); two centroids within R lie at most
  // one cell apart, so a 3x3x3 cell scan finds every neighbor.
  Vector3d gmin(centroid[0]);
  for (const auto& c : centroid) gmin = gmin.cwiseMin(c);
  auto cellOf = [&](const Vector3d& c) {
    return std::array<int, 3>{static_cast<int>(std::floor((c.x() - gmin.x()) / R)),
                              static_cast<int>(std::floor((c.y() - gmin.y()) / R)),
                              static_cast<int>(std::floor((c.z() - gmin.z()) / R))};
  };
  auto key = [](int x, int y, int z) {
    return (static_cast<int64_t>(x) * 2654435761LL ^
            static_cast<int64_t>(y) * 40503LL) *
               2654435761LL ^
           static_cast<int64_t>(z);
  };
  std::unordered_map<int64_t, std::vector<int>> grid;
  grid.reserve(nTets * 2);
  for (int e = 0; e < nTets; ++e) {
    const auto c = cellOf(centroid[e]);
    grid[key(c[0], c[1], c[2])].push_back(e);
  }

  std::vector<std::vector<std::pair<int, double>>> nbr(nTets);
  std::vector<double> Hs(nTets, 0.0);
  for (int e = 0; e < nTets; ++e) {
    const auto c = cellOf(centroid[e]);
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) {
          auto it = grid.find(key(c[0] + dx, c[1] + dy, c[2] + dz));
          if (it == grid.end()) continue;
          for (int j : it->second) {
            const double d = (centroid[e] - centroid[j]).norm();
            if (d < R) {
              const double w = R - d;
              nbr[e].emplace_back(j, w);
              Hs[e] += w;
            }
          }
        }
  }

  // Sensitivity filtering (below) leaves the physical density equal to the raw
  // design variable — nothing blurs the density field, so it goes crisply
  // black/white and the volume stays linear in x: dV/dx_e = v_e, and the frozen
  // (support/load) skin is exactly 1. The filter regularizes the *sensitivities*
  // instead, imposing a minimum length scale without the gray transition band a
  // density filter would leave. (Density filter + Heaviside projection is the
  // production-grade alternative — crisper min-member-size control — noted for
  // the TO1 build.)
  const double Vtarget = params.volumeFraction * Vtot;
  double Vfrozen = 0.0, sumVfree = 0.0;
  for (int e = 0; e < nTets; ++e) (frozen[e] ? Vfrozen : sumVfree) += volE[e];
  if (Vfrozen > Vtarget)
    return fail("volume fraction target is below the frozen support/load volume");
  if (sumVfree <= 0) return fail("no free design region (every element is frozen)");

  // ---- DOF map + load vector (shared with the static solver) --------------
  const DofMap dofs = BuildDofMap(nNodes, fixedNodes);
  const std::vector<int>& dofMap = dofs.map;
  const int nFree = dofs.nFree;
  if (nFree == 0) return fail("model is fully constrained (no free DOF)");
  Eigen::VectorXd F = Eigen::VectorXd::Zero(nFree);
  {
    std::string loadErr;
    if (!AssembleFaceLoads(mesh, loads, dofMap, F, &loadErr)) return fail(loadErr);
  }
  if (F.norm() <= 0) return fail("loads integrate to zero force");

  // ---- Design variables: uniform feasible start --------------------------
  // The design variable IS the physical density (sensitivity filtering); frozen
  // support/load elements stay pinned at 1.
  std::vector<double> x(nTets, 1.0);
  const double xStart =
      std::min(1.0, std::max(0.0, (Vtarget - Vfrozen) / sumVfree));
  for (int e = 0; e < nTets; ++e)
    if (!frozen[e]) x[e] = xStart;

  // Iterative solver: Conjugate Gradient (K is symmetric positive-definite),
  // diagonal-preconditioned, warm-started from the previous iteration's
  // displacement. K changes only slightly between SIMP steps, so the warm start
  // makes later solves converge in a handful of iterations — far cheaper than
  // re-factorizing a direct solver every iteration (the resolution wall). The
  // one-shot static solver (fem::Solve) keeps its exact direct factorization.
  Eigen::ConjugateGradient<Eigen::SparseMatrix<double>,
                           Eigen::Lower | Eigen::Upper,
                           Eigen::IncompleteCholesky<double>>
      cg;
  cg.setTolerance(1e-6);
  Eigen::VectorXd uPrev = Eigen::VectorXd::Zero(nFree);
  std::vector<Eigen::Triplet<double>> trip;
  if (!solver) trip.reserve(static_cast<size_t>(nTets) * 900);

  // External solver (the GPU seam): set up once, then hand it the per-element
  // scales each iteration instead of assembling K here.
  std::vector<double> scaleE(nTets, 1.0);
  std::vector<double> uExt, Fext;
  if (solver) {
    std::string serr;
    if (!solver->Setup(mesh, material, dofMap, nFree, &serr))
      return fail("linear solver setup failed: " + serr);
    Fext.assign(F.data(), F.data() + nFree);
    uExt.assign(nFree, 0.0);  // carries the warm start between iterations
  }

  TopoResult result;
  result.density.assign(nTets, 0.0);
  std::vector<double> dcRaw(nTets, 0.0), dcFilt(nTets, 0.0);
  constexpr double kGamma = 1e-3;  // sensitivity-filter divide-by-zero guard

  // TOPO_PROFILE=1: per-phase wall-time accumulators, one stderr line at run
  // end — the honest attribution of where a SIMP iteration goes (the GPU4
  // bench proved gut-feel attribution wrong).
  const bool profile = std::getenv("TOPO_PROFILE") != nullptr;
  double tSolve = 0, tSens = 0, tFilter = 0, tOc = 0;
  using pclk = std::chrono::steady_clock;
  auto lap = [&](pclk::time_point& t0) {
    const auto now = pclk::now();
    const double s = std::chrono::duration<double>(now - t0).count();
    t0 = now;
    return s;
  };

  for (int iter = 0; iter < params.maxIterations; ++iter) {
    // Penalty continuation (optional): p ramps 1 -> params.penalty over the
    // first penaltyRampIters iterations, then holds. Disabled (0) by default —
    // sensitivity filtering already yields a discrete design at constant p.
    const double p =
        params.penaltyRampIters > 0
            ? 1.0 + (params.penalty - 1.0) *
                        std::min(1.0, iter / params.penaltyRampIters)
            : params.penalty;
    const bool atFullPenalty = p >= params.penalty - 1e-9;

    // Penalized per-element stiffness scale (physical density = design var).
    for (int e = 0; e < nTets; ++e)
      scaleE[e] = (Emin + std::pow(x[e], p) * (E0 - Emin)) / E0;

    pclk::time_point tp = pclk::now();
    Eigen::VectorXd u(nFree);
    if (solver) {
      // External (GPU) solver: matrix-free, warm-started via uExt.
      std::string serr;
      if (!solver->Solve(scaleE, Fext, uExt, &serr))
        return fail("linear solve failed: " + serr);
      for (int i = 0; i < nFree; ++i) u(i) = uExt[i];
      if (!u.allFinite()) return fail("linear solve failed during optimization");
    } else {
      // Built-in path: assemble K(x) and run the Eigen CG-IC.
      trip.clear();
      for (int e = 0; e < nTets; ++e) {
        const auto Ke0 = ElementK0(mesh, mesh.tets[e], D0);
        const auto& tet = mesh.tets[e];
        for (int a = 0; a < 30; ++a) {
          const int ga = dofMap[3 * tet[a / 3] + (a % 3)];
          if (ga < 0) continue;
          for (int b = 0; b < 30; ++b) {
            const int gb = dofMap[3 * tet[b / 3] + (b % 3)];
            if (gb < 0) continue;
            trip.emplace_back(ga, gb, scaleE[e] * Ke0(a, b));
          }
        }
      }
      Eigen::SparseMatrix<double> K(nFree, nFree);
      K.setFromTriplets(trip.begin(), trip.end());

      cg.compute(K);
      if (cg.info() != Eigen::Success)
        return fail(
            "stiffness matrix is not solvable (insufficiently constrained)");
      u = cg.solveWithGuess(F, uPrev);
      if (!u.allFinite())
        return fail("linear solve failed during optimization");
      uPrev = u;  // warm-start the next iteration
    }

    tSolve += lap(tp);
    const double compliance = F.dot(u);

    // Expand the reduced displacement to a full per-node vector for the
    // element strain-energy (sensitivity) evaluation.
    std::vector<glm::dvec3> disp(nNodes, glm::dvec3(0.0));
    for (int n = 0; n < nNodes; ++n)
      for (int k = 0; k < 3; ++k) {
        const int g = dofMap[3 * n + k];
        if (g >= 0) disp[n][k] = u(g);
      }

    // Raw per-element compliance sensitivity dc/dx_e = -p x_e^(p-1)(1-Emin/E0) se
    // with se = u_e^T Ke0 u_e (the element strain energy at full modulus).
    // An injected solver may compute all the energies at once (GPU4 — the
    // element matrices are resident on the device); on decline, the CPU
    // loop below recomputes Ke0 per element, which stays the oracle.
    const double coef = p * (E0 - Emin) / E0;
    std::vector<double> seExt;
    const bool haveSe =
        solver && solver->ElementEnergies(uExt, seExt, nullptr) &&
        static_cast<int>(seExt.size()) == nTets;
    for (int e = 0; e < nTets; ++e) {
      double se;
      if (haveSe) {
        se = seExt[e];
      } else {
        const auto Ke0 = ElementK0(mesh, mesh.tets[e], D0);
        const auto& tet = mesh.tets[e];
        Eigen::Matrix<double, 30, 1> ue;
        for (int i = 0; i < 10; ++i) {
          const auto& d = disp[tet[i]];
          ue(3 * i + 0) = d.x;
          ue(3 * i + 1) = d.y;
          ue(3 * i + 2) = d.z;
        }
        se = ue.dot(Ke0 * ue);  // >= 0
      }
      dcRaw[e] = -coef * std::pow(x[e], p - 1.0) * se;  // <= 0
    }
    tSens += lap(tp);
    // Sensitivity filter (Sigmund): a length-scale regularizer that keeps the
    // density crisply 0/1. dcFilt_e = (sum_j H_ej x_j dcRaw_j)/(max(g,x_e) sum_j H_ej).
    for (int e = 0; e < nTets; ++e) {
      double num = 0.0;
      for (const auto& [j, w] : nbr[e]) num += w * x[j] * dcRaw[j];
      dcFilt[e] = num / (std::max(kGamma, x[e]) * Hs[e]);
    }
    tFilter += lap(tp);

    // ---- Optimality-criteria update: bisect the volume multiplier --------
    // Volume is linear in x, so the constraint check is O(n) per bisection step.
    std::vector<double> xnew(x);
    const double move = params.moveLimit;
    double l1 = 0.0, l2 = 1e12;
    while ((l2 - l1) > 1e-9 * (1.0 + l2)) {
      const double lmid = 0.5 * (l1 + l2);
      double Vp = Vfrozen;
      for (int e = 0; e < nTets; ++e) {
        if (frozen[e]) continue;
        const double be = -dcFilt[e] / (lmid * volE[e]);  // >= 0
        double xn = x[e] * std::pow(std::max(be, 0.0), params.damping);
        xn = std::min(x[e] + move, std::max(x[e] - move, xn));
        xn = std::min(1.0, std::max(0.0, xn));
        xnew[e] = xn;
        Vp += volE[e] * xn;
      }
      if (Vp > Vtarget) l1 = lmid; else l2 = lmid;
    }

    double maxChange = 0.0;
    for (int e = 0; e < nTets; ++e)
      if (!frozen[e]) maxChange = std::max(maxChange, std::abs(xnew[e] - x[e]));
    x.swap(xnew);

    double Vphys = 0.0;
    for (int e = 0; e < nTets; ++e) Vphys += volE[e] * x[e];

    result.complianceHistory.push_back(compliance);
    result.iterations = iter + 1;
    result.density = x;
    result.volumeFractionAchieved = Vphys / Vtot;

    if (progress) {
      TopoProgress prog;
      prog.iteration = iter + 1;
      prog.compliance = compliance;
      prog.maxChange = maxChange;
      prog.volumeFraction = Vphys / Vtot;
      if (!progress(prog)) {  // caller cancelled: keep best-so-far
        result.cancelled = true;
        return result;
      }
    }

    tOc += lap(tp);
    if (atFullPenalty && maxChange < params.changeTol) {
      result.converged = true;
      break;
    }
  }

  if (profile)
    std::fprintf(stderr,
                 "TOPO_PROFILE: %d iters — solve %.2fs, sensitivity %.2fs, "
                 "filter %.2fs, oc+rest %.2fs\n",
                 result.iterations, tSolve, tSens, tFilter, tOc);
  return result;
}

#else  // !HAVE_FEM_SOLVER

bool TopoAvailable() { return false; }

std::optional<TopoResult> TopologyOptimize(
    const FemMesh&, const Material&, const std::vector<int>&,
    const std::vector<FaceLoad>&, const TopoParams&, std::string* error,
    const std::function<bool(const TopoProgress&)>&, const std::vector<int>&,
    TopoLinearSolver*) {
  if (error) *error = "built without FEM solver support (Eigen not found)";
  return std::nullopt;
}

#endif  // HAVE_FEM_SOLVER

}  // namespace fem
