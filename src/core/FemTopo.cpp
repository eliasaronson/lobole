#include "core/FemTopo.h"

#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

#ifdef HAVE_FEM_SOLVER
#include "core/FemSolverInternal.h"

#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#endif

namespace fem {

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
    const std::function<bool(const TopoProgress&)>& progress) {
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

  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt;
  std::vector<Eigen::Triplet<double>> trip;
  trip.reserve(static_cast<size_t>(nTets) * 900);

  TopoResult result;
  result.density.assign(nTets, 0.0);
  std::vector<double> dcRaw(nTets, 0.0), dcFilt(nTets, 0.0);
  constexpr double kGamma = 1e-3;  // sensitivity-filter divide-by-zero guard

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

    // Assemble the penalized stiffness K(x) (physical density = design var).
    trip.clear();
    for (int e = 0; e < nTets; ++e) {
      const auto Ke0 = ElementK0(mesh, mesh.tets[e], D0);
      const double scale = (Emin + std::pow(x[e], p) * (E0 - Emin)) / E0;
      const auto& tet = mesh.tets[e];
      for (int a = 0; a < 30; ++a) {
        const int ga = dofMap[3 * tet[a / 3] + (a % 3)];
        if (ga < 0) continue;
        for (int b = 0; b < 30; ++b) {
          const int gb = dofMap[3 * tet[b / 3] + (b % 3)];
          if (gb < 0) continue;
          trip.emplace_back(ga, gb, scale * Ke0(a, b));
        }
      }
    }
    Eigen::SparseMatrix<double> K(nFree, nFree);
    K.setFromTriplets(trip.begin(), trip.end());

    if (iter == 0) ldlt.analyzePattern(K);
    ldlt.factorize(K);
    if (ldlt.info() != Eigen::Success)
      return fail("stiffness matrix factorization failed (insufficiently constrained)");
    const Eigen::VectorXd u = ldlt.solve(F);
    if (ldlt.info() != Eigen::Success || !u.allFinite())
      return fail("linear solve failed during optimization");

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
    const double coef = p * (E0 - Emin) / E0;
    for (int e = 0; e < nTets; ++e) {
      const auto Ke0 = ElementK0(mesh, mesh.tets[e], D0);
      const auto& tet = mesh.tets[e];
      Eigen::Matrix<double, 30, 1> ue;
      for (int i = 0; i < 10; ++i) {
        const auto& d = disp[tet[i]];
        ue(3 * i + 0) = d.x;
        ue(3 * i + 1) = d.y;
        ue(3 * i + 2) = d.z;
      }
      const double se = ue.dot(Ke0 * ue);  // >= 0
      dcRaw[e] = -coef * std::pow(x[e], p - 1.0) * se;  // <= 0
    }
    // Sensitivity filter (Sigmund): a length-scale regularizer that keeps the
    // density crisply 0/1. dcFilt_e = (sum_j H_ej x_j dcRaw_j)/(max(g,x_e) sum_j H_ej).
    for (int e = 0; e < nTets; ++e) {
      double num = 0.0;
      for (const auto& [j, w] : nbr[e]) num += w * x[j] * dcRaw[j];
      dcFilt[e] = num / (std::max(kGamma, x[e]) * Hs[e]);
    }

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

    if (atFullPenalty && maxChange < params.changeTol) {
      result.converged = true;
      break;
    }
  }

  return result;
}

#else  // !HAVE_FEM_SOLVER

bool TopoAvailable() { return false; }

std::optional<TopoResult> TopologyOptimize(
    const FemMesh&, const Material&, const std::vector<int>&,
    const std::vector<FaceLoad>&, const TopoParams&, std::string* error,
    const std::function<bool(const TopoProgress&)>&) {
  if (error) *error = "built without FEM solver support (Eigen not found)";
  return std::nullopt;
}

#endif  // HAVE_FEM_SOLVER

}  // namespace fem
