#include "core/FemSolver.h"

#include <array>
#include <cmath>

#ifdef HAVE_FEM_SOLVER
#include "core/FemSolverInternal.h"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#endif

#ifdef HAVE_SPECTRA
#include <Spectra/MatOp/SparseSymMatProd.h>
#include <Spectra/MatOp/SymShiftInvert.h>
#include <Spectra/SymGEigsShiftSolver.h>
#endif

namespace fem {

#ifdef HAVE_FEM_SOLVER

bool SolverAvailable() { return true; }
#ifdef HAVE_SPECTRA
bool ModalAvailable() { return true; }
#else
bool ModalAvailable() { return false; }
#endif

// The element math (shape functions, Gauss rules, elasticity matrix) and the
// system-building helpers live in FemSolverInternal.h, shared with the
// topology-optimization loop (FemTopo.cpp).
using namespace internal;

std::array<double, 900> Tet10Stiffness(const std::array<glm::dvec3, 10>& nodes,
                                       double youngMPa, double poisson) {
  Vector3d X[10];
  for (int i = 0; i < 10; ++i) X[i] = Vector3d(nodes[i].x, nodes[i].y, nodes[i].z);
  const auto D = Elasticity(youngMPa, poisson);
  Eigen::Matrix<double, 30, 30> Ke = Eigen::Matrix<double, 30, 30>::Zero();
  for (const auto& g : kTetGauss) {
    Eigen::Matrix<double, 6, 30> B;
    const double detJ = StrainDisp(X, g[0], g[1], g[2], B);
    Ke += (kTetW * std::abs(detJ)) * (B.transpose() * D * B);
  }
  std::array<double, 900> out;
  for (int i = 0; i < 30; ++i)
    for (int j = 0; j < 30; ++j) out[i * 30 + j] = Ke(i, j);
  return out;
}

std::array<double, 6> Tet10Stress(const std::array<glm::dvec3, 10>& nodes,
                                  const std::array<glm::dvec3, 10>& disp,
                                  double youngMPa, double poisson, double r,
                                  double s, double t) {
  Vector3d X[10];
  Eigen::Matrix<double, 30, 1> ue;
  for (int i = 0; i < 10; ++i) {
    X[i] = Vector3d(nodes[i].x, nodes[i].y, nodes[i].z);
    ue(3 * i + 0) = disp[i].x;
    ue(3 * i + 1) = disp[i].y;
    ue(3 * i + 2) = disp[i].z;
  }
  Eigen::Matrix<double, 6, 30> B;
  StrainDisp(X, r, s, t, B);
  const Eigen::Matrix<double, 6, 1> sig = Elasticity(youngMPa, poisson) * (B * ue);
  return {sig(0), sig(1), sig(2), sig(3), sig(4), sig(5)};
}

std::optional<FemResults> Solve(const FemMesh& mesh, const Material& material,
                                const std::vector<int>& fixedNodes,
                                const std::vector<FaceLoad>& loads, std::string* error,
                                const glm::dvec3& gravityAccel) {
  auto fail = [&](const std::string& m) -> std::optional<FemResults> {
    if (error) *error = m;
    return std::nullopt;
  };
  const int nNodes = static_cast<int>(mesh.nodes.size());
  if (nNodes == 0 || mesh.tets.empty()) return fail("empty mesh");
  if (material.youngMPa <= 0) return fail("material has no stiffness data");

  // DOF reduction map: -1 for fixed DOFs, else a compacted free-DOF index.
  const DofMap dofs = BuildDofMap(nNodes, fixedNodes);
  const std::vector<int>& dofMap = dofs.map;
  const int nFree = dofs.nFree;
  if (nFree == 0) return fail("model is fully constrained (no free DOF)");

  const auto D = Elasticity(material.youngMPa, material.poisson);

  // Assemble the reduced stiffness matrix from element contributions.
  std::vector<Eigen::Triplet<double>> triplets;
  triplets.reserve(mesh.tets.size() * 900);
  for (const auto& tet : mesh.tets) {
    Vector3d X[10];
    for (int i = 0; i < 10; ++i) {
      const auto& p = mesh.nodes[tet[i]];
      X[i] = Vector3d(p.x, p.y, p.z);
    }
    Eigen::Matrix<double, 30, 30> Ke = Eigen::Matrix<double, 30, 30>::Zero();
    for (const auto& g : kTetGauss) {
      Eigen::Matrix<double, 6, 30> B;
      // B (physical gradients) is orientation-independent; the volume measure
      // is |detJ|. Netgen orders tet corners with negative orientation, so
      // detJ is consistently < 0 — take the magnitude, reject only if ~0.
      const double detJ = StrainDisp(X, g[0], g[1], g[2], B);
      if (std::abs(detJ) < 1e-12) return fail("mesh has a degenerate element");
      Ke += (kTetW * std::abs(detJ)) * (B.transpose() * D * B);
    }
    // Scatter into the reduced system, skipping fixed DOFs.
    for (int a = 0; a < 30; ++a) {
      const int ga = dofMap[3 * tet[a / 3] + (a % 3)];
      if (ga < 0) continue;
      for (int b = 0; b < 30; ++b) {
        const int gb = dofMap[3 * tet[b / 3] + (b % 3)];
        if (gb < 0) continue;
        triplets.emplace_back(ga, gb, Ke(a, b));
      }
    }
  }
  Eigen::SparseMatrix<double> K(nFree, nFree);
  K.setFromTriplets(triplets.begin(), triplets.end());

  // Consistent nodal load vector: face tractions (shared with the topology
  // loop), then the optional gravity body force.
  Eigen::VectorXd F = Eigen::VectorXd::Zero(nFree);
  {
    std::string loadErr;
    if (!AssembleFaceLoads(mesh, loads, dofMap, F, &loadErr)) return fail(loadErr);
  }
  auto addNodal = [&](int node, const Vector3d& fv) {
    for (int k = 0; k < 3; ++k) {
      const int g = dofMap[3 * node + k];
      if (g >= 0) F(g) += fv(k);
    }
  };

  // Body force (gravity / self-weight): b = rho * g per unit volume, consistent
  // nodal loads f_i = integral rho*g*N_i dV. Units: rho in t/mm^3, g in mm/s^2
  // => t*mm/s^2/mm^3 = N/mm^3, integrated over mm^3 => N.
  if ((gravityAccel.x != 0 || gravityAccel.y != 0 || gravityAccel.z != 0) &&
      material.densityTPerMM3 > 0) {
    const Vector3d b = material.densityTPerMM3 *
                       Vector3d(gravityAccel.x, gravityAccel.y, gravityAccel.z);
    for (const auto& tet : mesh.tets) {
      Vector3d X[10];
      for (int i = 0; i < 10; ++i) {
        const auto& p = mesh.nodes[tet[i]];
        X[i] = Vector3d(p.x, p.y, p.z);
      }
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 6, 30> B;  // only need detJ
        const double dV = kTetW * std::abs(StrainDisp(X, g[0], g[1], g[2], B));
        double N[10];
        TetShape(g[0], g[1], g[2], N);
        for (int i = 0; i < 10; ++i) addNodal(tet[i], (N[i] * dV) * b);
      }
    }
  }

  // Solve the reduced system.
  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
  solver.compute(K);
  if (solver.info() != Eigen::Success)
    return fail("model is insufficiently constrained (singular stiffness matrix)");
  const Eigen::VectorXd u = solver.solve(F);
  if (solver.info() != Eigen::Success) return fail("linear solve failed");
  // SimplicialLDLT can factor a singular K (a rigid-body mode the fixed faces
  // didn't kill) with a near-zero pivot and still report Success, then return a
  // garbage solution. The robust signal is the residual: a genuine solution
  // satisfies K u = F, an under-constrained one does not.
  if (!u.allFinite() ||
      (F.norm() > 0 && (K * u - F).norm() > 1e-6 * F.norm()))
    return fail("model is insufficiently constrained (singular stiffness matrix)");

  // Expand to full displacement (fixed DOFs are zero).
  FemResults out;
  out.displacement.resize(nNodes);
  for (int n = 0; n < nNodes; ++n) {
    Vector3d d(0, 0, 0);
    for (int k = 0; k < 3; ++k) {
      const int g = dofMap[3 * n + k];
      if (g >= 0) d(k) = u(g);
    }
    out.displacement[n] = glm::dvec3(d(0), d(1), d(2));
  }

  // Nodal stress recovery: evaluate element stress at each of its 10 nodes,
  // accumulate to global nodes weighted by element volume, then average.
  std::vector<Eigen::Matrix<double, 6, 1>> stressAccum(
      nNodes, Eigen::Matrix<double, 6, 1>::Zero());
  std::vector<double> weight(nNodes, 0.0);
  for (const auto& tet : mesh.tets) {
    Vector3d X[10];
    Eigen::Matrix<double, 30, 1> ue;
    for (int i = 0; i < 10; ++i) {
      const auto& p = mesh.nodes[tet[i]];
      X[i] = Vector3d(p.x, p.y, p.z);
      const auto& d = out.displacement[tet[i]];
      ue(3 * i + 0) = d.x;
      ue(3 * i + 1) = d.y;
      ue(3 * i + 2) = d.z;
    }
    // Element volume (from the centroid Gauss sum) as the averaging weight.
    double vol = 0.0;
    for (const auto& g : kTetGauss) {
      Eigen::Matrix<double, 6, 30> B;
      vol += kTetW * std::abs(StrainDisp(X, g[0], g[1], g[2], B));
    }
    for (int i = 0; i < 10; ++i) {
      Eigen::Matrix<double, 6, 30> B;
      StrainDisp(X, kNodeRST[i][0], kNodeRST[i][1], kNodeRST[i][2], B);
      const Eigen::Matrix<double, 6, 1> s = D * (B * ue);
      stressAccum[tet[i]] += vol * s;
      weight[tet[i]] += vol;
    }
  }

  out.vonMises.resize(nNodes, 0.0);
  for (int n = 0; n < nNodes; ++n) {
    if (weight[n] > 0) {
      const Eigen::Matrix<double, 6, 1> s = stressAccum[n] / weight[n];
      out.vonMises[n] = VonMises(s);
    }
    if (out.vonMises[n] > out.maxVonMises) {
      out.maxVonMises = out.vonMises[n];
      out.maxVmNode = n;
      out.maxVmAt = mesh.nodes[n];
    }
    const double dmag = glm::length(out.displacement[n]);
    if (dmag > out.maxDisplacement) {
      out.maxDisplacement = dmag;
      out.maxDispNode = n;
      out.maxDispAt = mesh.nodes[n];
    }
  }
  return out;
}

#ifdef HAVE_SPECTRA
std::optional<std::vector<FemResults::ModeShape>> SolveModal(
    const FemMesh& mesh, const Material& material,
    const std::vector<int>& fixedNodes, int numModes, std::string* error) {
  auto fail = [&](const std::string& m)
      -> std::optional<std::vector<FemResults::ModeShape>> {
    if (error) *error = m;
    return std::nullopt;
  };
  const int nNodes = static_cast<int>(mesh.nodes.size());
  if (nNodes == 0 || mesh.tets.empty()) return fail("empty mesh");
  if (material.youngMPa <= 0) return fail("material has no stiffness data");
  if (material.densityTPerMM3 <= 0)
    return fail("material has no density (needed for modal mass)");
  if (numModes < 1) numModes = 1;

  // Reduced-DOF map; v1 requires a support so K is positive-definite (sigma=0
  // shift-invert). Free-free (unconstrained) modes are deferred.
  std::vector<char> fixed(3 * nNodes, 0);
  int nFixed = 0;
  for (int n : fixedNodes)
    if (n >= 0 && n < nNodes)
      for (int k = 0; k < 3; ++k)
        if (!fixed[3 * n + k]) { fixed[3 * n + k] = 1; ++nFixed; }
  if (nFixed == 0)
    return fail("modal analysis needs at least one fixed support");
  std::vector<int> dofMap(3 * nNodes, -1);
  int nFree = 0;
  for (int d = 0; d < 3 * nNodes; ++d)
    if (!fixed[d]) dofMap[d] = nFree++;
  if (nFree < numModes + 1) return fail("mesh too small for the requested modes");

  const auto D = Elasticity(material.youngMPa, material.poisson);
  std::vector<Eigen::Triplet<double>> kt, mt;
  kt.reserve(mesh.tets.size() * 900);
  mt.reserve(mesh.tets.size() * 300);
  for (const auto& tet : mesh.tets) {
    Vector3d X[10];
    for (int i = 0; i < 10; ++i) {
      const auto& p = mesh.nodes[tet[i]];
      X[i] = Vector3d(p.x, p.y, p.z);
    }
    Eigen::Matrix<double, 30, 30> Ke = Eigen::Matrix<double, 30, 30>::Zero();
    Eigen::Matrix<double, 10, 10> Me = Eigen::Matrix<double, 10, 10>::Zero();
    for (const auto& g : kTetGauss) {
      Eigen::Matrix<double, 6, 30> B;
      const double detJ = StrainDisp(X, g[0], g[1], g[2], B);
      if (std::abs(detJ) < 1e-12) return fail("mesh has a degenerate element");
      const double dV = kTetW * std::abs(detJ);
      Ke += dV * (B.transpose() * D * B);
      double N[10];
      TetShape(g[0], g[1], g[2], N);
      for (int i = 0; i < 10; ++i)
        for (int j = 0; j < 10; ++j)
          Me(i, j) += material.densityTPerMM3 * dV * N[i] * N[j];
    }
    for (int a = 0; a < 30; ++a) {
      const int ga = dofMap[3 * tet[a / 3] + (a % 3)];
      if (ga < 0) continue;
      for (int b = 0; b < 30; ++b) {
        const int gb = dofMap[3 * tet[b / 3] + (b % 3)];
        if (gb < 0) continue;
        kt.emplace_back(ga, gb, Ke(a, b));
      }
    }
    // Consistent mass couples same-direction DOFs (block-diagonal in x/y/z).
    for (int i = 0; i < 10; ++i)
      for (int j = 0; j < 10; ++j)
        for (int d = 0; d < 3; ++d) {
          const int ga = dofMap[3 * tet[i] + d];
          const int gb = dofMap[3 * tet[j] + d];
          if (ga >= 0 && gb >= 0) mt.emplace_back(ga, gb, Me(i, j));
        }
  }
  Eigen::SparseMatrix<double> K(nFree, nFree), M(nFree, nFree);
  K.setFromTriplets(kt.begin(), kt.end());
  M.setFromTriplets(mt.begin(), mt.end());
  K.makeCompressed();
  M.makeCompressed();

  // Generalized eigenproblem K phi = lambda M phi, lowest eigenvalues via
  // shift-invert at sigma=0 (K is positive-definite here).
  using OpType = Spectra::SymShiftInvert<double, Eigen::Sparse, Eigen::Sparse>;
  using BOpType = Spectra::SparseSymMatProd<double>;
  OpType op(K, M);
  BOpType Bop(M);
  const int nev = std::min(numModes, nFree - 1);
  const int ncv = std::min(std::max(2 * nev + 1, 20), nFree);
  Spectra::SymGEigsShiftSolver<OpType, BOpType, Spectra::GEigsMode::ShiftInvert>
      geigs(op, Bop, nev, ncv, 0.0);
  try {
    geigs.init();
    geigs.compute(Spectra::SortRule::LargestMagn, 1000, 1e-10,
                  Spectra::SortRule::SmallestAlge);
  } catch (const std::exception& e) {
    return fail(std::string("modal eigensolver failed: ") + e.what());
  }
  if (geigs.info() != Spectra::CompInfo::Successful)
    return fail("modal eigensolver did not converge");
  const Eigen::VectorXd evals = geigs.eigenvalues();   // omega^2, ascending
  const Eigen::MatrixXd evecs = geigs.eigenvectors();

  std::vector<FemResults::ModeShape> modes;
  for (int m = 0; m < evals.size(); ++m) {
    const double lambda = std::max(evals(m), 0.0);  // clamp tiny negatives
    constexpr double kTwoPi = 6.283185307179586;
    FemResults::ModeShape mode;
    mode.frequencyHz = std::sqrt(lambda) / kTwoPi;
    mode.displacement.assign(nNodes, glm::dvec3(0, 0, 0));
    double maxMag = 0.0;
    for (int n = 0; n < nNodes; ++n) {
      glm::dvec3 u(0, 0, 0);
      for (int k = 0; k < 3; ++k) {
        const int g = dofMap[3 * n + k];
        if (g >= 0) u[k] = evecs(g, m);
      }
      mode.displacement[n] = u;
      maxMag = std::max(maxMag, glm::length(u));
    }
    if (maxMag > 0)  // normalize the shape to max |u| = 1 for display
      for (auto& u : mode.displacement) u /= maxMag;
    modes.push_back(std::move(mode));
  }
  return modes;
}
#else   // !HAVE_SPECTRA
std::optional<std::vector<FemResults::ModeShape>> SolveModal(
    const FemMesh&, const Material&, const std::vector<int>&, int, std::string* error) {
  if (error) *error = "built without modal support (Spectra not found)";
  return std::nullopt;
}
#endif  // HAVE_SPECTRA

#else  // !HAVE_FEM_SOLVER

bool SolverAvailable() { return false; }
bool ModalAvailable() { return false; }

std::optional<FemResults> Solve(const FemMesh&, const Material&, const std::vector<int>&,
                                const std::vector<FaceLoad>&, std::string* error,
                                const glm::dvec3&) {
  if (error) *error = "built without FEM solver support (Eigen not found)";
  return std::nullopt;
}

std::optional<std::vector<FemResults::ModeShape>> SolveModal(
    const FemMesh&, const Material&, const std::vector<int>&, int, std::string* error) {
  if (error) *error = "built without FEM solver support (Eigen not found)";
  return std::nullopt;
}

#endif  // HAVE_FEM_SOLVER

}  // namespace fem
