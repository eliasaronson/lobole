#include "core/fem/FemSolver.h"

#include <array>
#include <cmath>

#ifdef HAVE_FEM_SOLVER
#include "core/fem/FemSolverInternal.h"
#include "core/fem/FemTopo.h" // TopoLinearSolver, the external solver's seam

#include <Eigen/Dense>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#endif

#ifdef HAVE_SPECTRA
#include <Spectra/MatOp/SparseSymMatProd.h>
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
  for (int i = 0; i < 10; ++i)
    X[i] = Vector3d(nodes[i].x, nodes[i].y, nodes[i].z);
  const auto D = Elasticity(youngMPa, poisson);
  Eigen::Matrix<double, 30, 30> Ke = Eigen::Matrix<double, 30, 30>::Zero();
  for (const auto& g : kTetGauss) {
    Eigen::Matrix<double, 6, 30> B;
    const double detJ = StrainDisp(X, g[0], g[1], g[2], B);
    Ke += (kTetW * std::abs(detJ)) * (B.transpose() * D * B);
  }
  std::array<double, 900> out;
  for (int i = 0; i < 30; ++i)
    for (int j = 0; j < 30; ++j)
      out[i * 30 + j] = Ke(i, j);
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
  const Eigen::Matrix<double, 6, 1> sig =
      Elasticity(youngMPa, poisson) * (B * ue);
  return {sig(0), sig(1), sig(2), sig(3), sig(4), sig(5)};
}

std::optional<FemResults> Solve(const FemMesh& mesh, const Material& material,
                                const std::vector<int>& fixedNodes,
                                const std::vector<FaceLoad>& loads,
                                std::string* error,
                                const glm::dvec3& gravityAccel) {
  return Solve(mesh, std::vector<Material>{material}, fixedNodes, loads, error,
               gravityAccel);
}

std::optional<FemResults>
Solve(const FemMesh& mesh, const std::vector<Material>& byDomain,
      const std::vector<int>& fixedNodes, const std::vector<FaceLoad>& loads,
      std::string* error, const glm::dvec3& gravityAccel) {
  auto all =
      SolveCases(mesh, byDomain, fixedNodes, {loads}, error, gravityAccel);
  if (!all)
    return std::nullopt;
  return std::move(all->front());
}

namespace {

// Preconditioned conjugate gradients on the assembled K, written out rather
// than Eigen's ConjugateGradient so it can be cancelled mid-solve (Eigen's
// would have to be restarted in chunks, losing its search directions each
// time) and so a stall is seen as one.
//
// Converged at |K u - f| <= tol |f|. Fails "cancelled" when asked, and
// "stalled" when the best residual has not improved by a tenth in
// kStallIterations: the signature of a singular K (an under-constrained
// model has a rigid-body mode CG cannot resolve), which would otherwise
// iterate to the cap.
enum class PcgOutcome { Converged, Cancelled, Stalled, Capped };

template <class Precond>
PcgOutcome Pcg(const Eigen::SparseMatrix<double>& K, const Precond& M,
               const Eigen::VectorXd& f, Eigen::VectorXd& u, double tol,
               int maxIterations, const std::function<bool()>& cancelled,
               int* iterations, double* relResidual) {
  constexpr int kCancelEvery = 32;
  constexpr int kStallIterations = 2000;
  const double fNorm = f.norm();
  u = Eigen::VectorXd::Zero(f.size());
  *iterations = 0;
  *relResidual = 0.0;
  if (fNorm == 0.0)
    return PcgOutcome::Converged;
  Eigen::VectorXd r = f; // u = 0
  Eigen::VectorXd z = M.solve(r);
  Eigen::VectorXd p = z;
  Eigen::VectorXd Kp(f.size());
  double rz = r.dot(z);
  double best = 1.0;
  int bestAt = 0;
  for (int it = 1; it <= maxIterations; ++it) {
    Kp.noalias() = K * p;
    const double alpha = rz / p.dot(Kp);
    u.noalias() += alpha * p;
    r.noalias() -= alpha * Kp;
    const double rel = r.norm() / fNorm;
    *iterations = it;
    *relResidual = rel;
    if (!std::isfinite(rel))
      return PcgOutcome::Stalled;
    if (rel <= tol)
      return PcgOutcome::Converged;
    if (rel < 0.9 * best) {
      best = rel;
      bestAt = it;
    } else if (it - bestAt > kStallIterations) {
      return PcgOutcome::Stalled;
    }
    if (cancelled && it % kCancelEvery == 0 && cancelled())
      return PcgOutcome::Cancelled;
    z = M.solve(r);
    const double rzNext = r.dot(z);
    p = z + (rzNext / rz) * p;
    rz = rzNext;
  }
  return PcgOutcome::Capped;
}

} // namespace

std::optional<std::vector<FemResults>>
SolveCases(const FemMesh& mesh, const std::vector<Material>& byDomain,
           const std::vector<int>& fixedNodes,
           const std::vector<std::vector<FaceLoad>>& cases, std::string* error,
           const glm::dvec3& gravityAccel, const SolveOptions& options,
           SolveReport* report) {
  auto fail =
      [&](const std::string& m) -> std::optional<std::vector<FemResults>> {
    if (error)
      *error = m;
    return std::nullopt;
  };
  const int nNodes = static_cast<int>(mesh.nodes.size());
  if (nNodes == 0 || mesh.tets.empty())
    return fail("empty mesh");
  if (byDomain.empty())
    return fail("no material");
  for (const Material& m : byDomain)
    if (m.youngMPa <= 0)
      return fail("material has no stiffness data");
  // The material of tet `t`: its domain's, or the only one.
  const auto matOf = [&](size_t t) -> const Material& {
    const size_t d = mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[t];
    return byDomain[std::min(d, byDomain.size() - 1)];
  };
  std::vector<Eigen::Matrix<double, 6, 6>> Ds;
  for (const Material& m : byDomain)
    Ds.push_back(Elasticity(m.youngMPa, m.poisson));
  const auto DOf = [&](size_t t) -> const Eigen::Matrix<double, 6, 6>& {
    const size_t d = mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[t];
    return Ds[std::min(d, Ds.size() - 1)];
  };

  // DOF reduction map: -1 for fixed DOFs, else a compacted free-DOF index.
  const DofMap dofs = BuildDofMap(nNodes, fixedNodes);
  const std::vector<int>& dofMap = dofs.map;
  const int nFree = dofs.nFree;
  if (nFree == 0)
    return fail("model is fully constrained (no free DOF)");

  // Assemble the reduced stiffness matrix from element contributions, each
  // with its own domain's elasticity (S2: a bonded group of materials).
  // The triplet scratch is SCOPED: 900 of them per element is 1.5 GB on a
  // 100k-element bonded group, and holding it through the factorization is
  // what puts an assembly study out of memory.
  Eigen::SparseMatrix<double> K(nFree, nFree);
  {
    std::vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(mesh.tets.size() * 900);
    for (size_t ti = 0; ti < mesh.tets.size(); ++ti) {
      const auto& tet = mesh.tets[ti];
      const Eigen::Matrix<double, 6, 6>& D = DOf(ti);
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
        if (std::abs(detJ) < 1e-12)
          return fail("mesh has a degenerate element");
        Ke += (kTetW * std::abs(detJ)) * (B.transpose() * D * B);
      }
      // Scatter into the reduced system, skipping fixed DOFs.
      for (int a = 0; a < 30; ++a) {
        const int ga = dofMap[3 * tet[a / 3] + (a % 3)];
        if (ga < 0)
          continue;
        for (int b = 0; b < 30; ++b) {
          const int gb = dofMap[3 * tet[b / 3] + (b % 3)];
          if (gb < 0)
            continue;
          triplets.emplace_back(ga, gb, Ke(a, b));
        }
      }
    }
    K.setFromTriplets(triplets.begin(), triplets.end());
  }

  if (cases.empty())
    return fail("no load case to solve");
  // Consistent nodal load vector: the gravity body force every case shares,
  // then each case's face tractions (shared with the topology loop).
  Eigen::VectorXd F = Eigen::VectorXd::Zero(nFree);
  auto addNodal = [&](int node, const Vector3d& fv) {
    for (int k = 0; k < 3; ++k) {
      const int g = dofMap[3 * node + k];
      if (g >= 0)
        F(g) += fv(k);
    }
  };

  // Body force (gravity / self-weight): b = rho * g per unit volume, consistent
  // nodal loads f_i = integral rho*g*N_i dV. Units: rho in t/mm^3, g in mm/s^2
  // => t*mm/s^2/mm^3 = N/mm^3, integrated over mm^3 => N.
  if (gravityAccel.x != 0 || gravityAccel.y != 0 || gravityAccel.z != 0) {
    const Vector3d gvec(gravityAccel.x, gravityAccel.y, gravityAccel.z);
    for (size_t ti = 0; ti < mesh.tets.size(); ++ti) {
      const auto& tet = mesh.tets[ti];
      if (matOf(ti).densityTPerMM3 <= 0)
        continue;
      const Vector3d b = matOf(ti).densityTPerMM3 * gvec;
      Vector3d X[10];
      for (int i = 0; i < 10; ++i) {
        const auto& p = mesh.nodes[tet[i]];
        X[i] = Vector3d(p.x, p.y, p.z);
      }
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 6, 30> B; // only need detJ
        const double dV = kTetW * std::abs(StrainDisp(X, g[0], g[1], g[2], B));
        double N[10];
        TetShape(g[0], g[1], g[2], N);
        for (int i = 0; i < 10; ++i)
          addNodal(tet[i], (N[i] * dV) * b);
      }
    }
  }

  // Direct below the threshold: factorise once, every case one right-hand
  // side. Above it, PCG per case on K with one incomplete-Cholesky
  // preconditioner. Both are held to the same residual below.
  const bool iterative = nFree > options.iterativeAboveDofs;
  // An iterative solve goes to the external (GPU) solver when one is
  // offered and takes this system; the CPU's PCG is its fallback.
  std::unique_ptr<TopoLinearSolver> external;
  std::string declined;
  if (iterative && options.makeExternal) {
    external = options.makeExternal(&declined);
    if (external &&
        !external->SetupDomains(mesh, byDomain, dofMap, nFree, &declined))
      external.reset();
  }
  const char* solverName = !iterative ? kDirectSolverName
                           : external ? kGpuSolverName
                                      : kIterativeSolverName;
  if (options.solving)
    options.solving(solverName);
  if (report) {
    *report = SolveReport{};
    report->solver = solverName;
    if (iterative && !external && options.makeExternal)
      report->externalDeclined = declined;
  }
  if (options.cancelled && options.cancelled())
    return fail("cancelled");
  constexpr const char* kSingular =
      "model is insufficiently constrained (singular stiffness matrix)";
  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> direct;
  Eigen::IncompleteCholesky<double> ic;
  bool icReady = false;
  // The CPU preconditioner, built when the CPU first needs it: a GPU solve
  // that succeeds never pays for it.
  const auto prepareIc = [&]() {
    if (!icReady) {
      ic.compute(K);
      icReady = ic.info() == Eigen::Success;
    }
    return icReady;
  };
  if (iterative) {
    if (!external && !prepareIc())
      return fail(kSingular);
  } else {
    direct.compute(K);
    if (direct.info() != Eigen::Success)
      return fail(kSingular);
  }
  // A cap no converging solve comes near (a well-posed tet10 model takes a
  // few hundred to a few thousand iterations); the stall test ends a
  // singular one long before it.
  const int maxIterations = std::max(10000, nFree / 4);
  std::vector<FemResults> all;
  all.reserve(cases.size());
  const Eigen::VectorXd Fgravity = F;
  for (const std::vector<FaceLoad>& loads : cases) {
    Eigen::VectorXd F = Fgravity;
    {
      std::string loadErr;
      if (!AssembleFaceLoads(mesh, loads, dofMap, F, &loadErr))
        return fail(loadErr);
    }
    Eigen::VectorXd u;
    if (external) {
      std::vector<double> fv(F.data(), F.data() + F.size()), uv;
      LinearSolveControl control;
      // A decade inside the acceptance below.
      control.tolerance = 1e-7;
      control.cancelled = options.cancelled;
      std::string why;
      const bool ok = external->SolveStrict(fv, uv, control, &why);
      if (report)
        report->iterations += control.iterations;
      if (options.cancelled && options.cancelled())
        return fail("cancelled");
      if (ok && static_cast<int>(uv.size()) == nFree) {
        u = Eigen::Map<const Eigen::VectorXd>(uv.data(), nFree);
      } else {
        // Not an answer: finish this case and the rest on the CPU, and say
        // why the result names the CPU.
        external.reset();
        if (options.solving)
          options.solving(kIterativeSolverName);
        if (report) {
          report->solver = kIterativeSolverName;
          report->externalDeclined = why;
        }
        if (!prepareIc())
          return fail(kSingular);
      }
    }
    if (u.size() == 0 && iterative) {
      int its = 0;
      double rel = 0.0;
      // Two decades under the acceptance below, so the check never decides
      // a converged solve.
      const PcgOutcome o =
          Pcg(K, ic, F, u, 1e-8, maxIterations, options.cancelled, &its, &rel);
      if (report)
        report->iterations += its;
      if (o == PcgOutcome::Cancelled)
        return fail("cancelled");
      if (o == PcgOutcome::Stalled)
        return fail(std::string(kSingular) +
                    ": the iterative solve stalled at a relative residual of " +
                    std::to_string(rel) + " after " + std::to_string(its) +
                    " iterations");
      if (o == PcgOutcome::Capped)
        return fail("the iterative solve did not converge in " +
                    std::to_string(its) + " iterations (relative residual " +
                    std::to_string(rel) + ")");
    } else if (!iterative) {
      u = direct.solve(F);
      if (direct.info() != Eigen::Success)
        return fail("linear solve failed");
    }
    // SimplicialLDLT can factor a singular K (a rigid-body mode the fixed faces
    // didn't kill) with a near-zero pivot and still report Success, then return
    // a garbage solution. The robust signal is the residual: a genuine solution
    // satisfies K u = F, an under-constrained one does not. The iterative
    // answer is held to the same test, recomputed rather than trusted from
    // its recurrence.
    const double residual = F.norm() > 0 ? (K * u - F).norm() / F.norm() : 0.0;
    if (!u.allFinite() || residual > 1e-6)
      return fail(kSingular);
    if (report)
      report->worstResidual = std::max(report->worstResidual, residual);

    // Expand to full displacement (fixed DOFs are zero).
    FemResults out;
    out.displacement.resize(nNodes);
    for (int n = 0; n < nNodes; ++n) {
      Vector3d d(0, 0, 0);
      for (int k = 0; k < 3; ++k) {
        const int g = dofMap[3 * n + k];
        if (g >= 0)
          d(k) = u(g);
      }
      out.displacement[n] = glm::dvec3(d(0), d(1), d(2));
    }

    // Nodal stress recovery: evaluate element stress at each of its 10 nodes,
    // accumulate to global nodes weighted by element volume, then average.
    std::vector<Eigen::Matrix<double, 6, 1>> stressAccum(
        nNodes, Eigen::Matrix<double, 6, 1>::Zero());
    std::vector<double> weight(nNodes, 0.0);
    for (size_t ti = 0; ti < mesh.tets.size(); ++ti) {
      const auto& tet = mesh.tets[ti];
      const Eigen::Matrix<double, 6, 6>& D = DOf(ti);
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
    all.push_back(std::move(out));
  }
  return all;
}

#ifdef HAVE_SPECTRA
namespace {
// (K - sigma*M)^-1 by CHOLESKY, which is what the shift-invert eigensolve
// needs here: a modal study refuses without a support, so K is symmetric
// positive definite. Spectra's stock SymShiftInvert takes the sparse/sparse
// path instead — it EXPANDS both operands to full symmetric matrices and
// factorizes with SparseLU, storing an L and a U for a matrix that needs one
// triangle. On the ~500k-DOF bonded group of an assembly study that is
// gigabytes, and it does not fail politely: the process is killed and the
// study simply never answers.
class SpdShiftInvert {
public:
  using Scalar = double;
  // Both matrices hold their LOWER triangle only, which is all
  // SimplicialLDLT<Lower> and SparseSymMatProd<Lower> ever read.
  SpdShiftInvert(const Eigen::SparseMatrix<double>& k,
                 const Eigen::SparseMatrix<double>& m)
      : k(k), m(m) {}
  Eigen::Index rows() const { return k.rows(); }
  Eigen::Index cols() const { return k.rows(); }
  void set_shift(const Scalar& sigma) {
    if (sigma == 0.0) {
      solver.compute(k);
    } else {
      const Eigen::SparseMatrix<double> a = k - sigma * m;
      solver.compute(a);
    }
    factored = solver.info() == Eigen::Success;
  }
  bool ok() const { return factored; }
  void perform_op(const Scalar* xIn, Scalar* yOut) const {
    Eigen::Map<const Eigen::VectorXd> x(xIn, k.rows());
    Eigen::Map<Eigen::VectorXd> y(yOut, k.rows());
    y = solver.solve(x);
  }

private:
  const Eigen::SparseMatrix<double>& k;
  const Eigen::SparseMatrix<double>& m;
  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>, Eigen::Lower> solver;
  bool factored = false;
};
} // namespace

std::optional<std::vector<FemResults::ModeShape>>
SolveModal(const FemMesh& mesh, const Material& material,
           const std::vector<int>& fixedNodes, int numModes,
           std::string* error) {
  return SolveModal(mesh, std::vector<Material>{material}, fixedNodes, numModes,
                    error);
}

std::optional<std::vector<FemResults::ModeShape>>
SolveModal(const FemMesh& mesh, const std::vector<Material>& byDomain,
           const std::vector<int>& fixedNodes, int numModes,
           std::string* error) {
  auto fail = [&](const std::string& m)
      -> std::optional<std::vector<FemResults::ModeShape>> {
    if (error)
      *error = m;
    return std::nullopt;
  };
  const int nNodes = static_cast<int>(mesh.nodes.size());
  if (nNodes == 0 || mesh.tets.empty())
    return fail("empty mesh");
  if (byDomain.empty())
    return fail("no material");
  for (const Material& m : byDomain) {
    if (m.youngMPa <= 0)
      return fail("material has no stiffness data");
    if (m.densityTPerMM3 <= 0)
      return fail("material has no density (needed for modal mass)");
  }
  const auto matOf = [&](size_t t) -> const Material& {
    const size_t d = mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[t];
    return byDomain[std::min(d, byDomain.size() - 1)];
  };
  std::vector<Eigen::Matrix<double, 6, 6>> Ds;
  for (const Material& m : byDomain)
    Ds.push_back(Elasticity(m.youngMPa, m.poisson));
  if (numModes < 1)
    numModes = 1;

  // Reduced-DOF map; v1 requires a support so K is positive-definite (sigma=0
  // shift-invert). Free-free (unconstrained) modes are deferred.
  std::vector<char> fixed(3 * nNodes, 0);
  int nFixed = 0;
  for (int n : fixedNodes)
    if (n >= 0 && n < nNodes)
      for (int k = 0; k < 3; ++k)
        if (!fixed[3 * n + k]) {
          fixed[3 * n + k] = 1;
          ++nFixed;
        }
  if (nFixed == 0)
    return fail("modal analysis needs at least one fixed support");
  std::vector<int> dofMap(3 * nNodes, -1);
  int nFree = 0;
  for (int d = 0; d < 3 * nNodes; ++d)
    if (!fixed[d])
      dofMap[d] = nFree++;
  if (nFree < numModes + 1)
    return fail("mesh too small for the requested modes");

  // K and M are assembled in two SCOPED passes, each holding only the
  // triplets of the matrix it builds, and only the LOWER triangle of it.
  // Both matter at assembly-study size: the full 30x30 element block is 900
  // triplets, 1.5 GB of scratch on a 100k-element bonded group, and Spectra
  // reads both operands through selfadjointView<Lower> — so the upper half we
  // used to assemble was built, stored and then ignored. A study that died
  // without a word (the kernel kills the process; there is no bad_alloc to
  // report) now has room to finish.
  Eigen::SparseMatrix<double> K(nFree, nFree), M(nFree, nFree);
  {
    std::vector<Eigen::Triplet<double>> kt;
    kt.reserve(mesh.tets.size() * 465); // 30*31/2, the lower block
    for (size_t ti = 0; ti < mesh.tets.size(); ++ti) {
      const auto& tet = mesh.tets[ti];
      const size_t dom =
          mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[ti];
      const Eigen::Matrix<double, 6, 6>& D = Ds[std::min(dom, Ds.size() - 1)];
      Vector3d X[10];
      for (int i = 0; i < 10; ++i) {
        const auto& p = mesh.nodes[tet[i]];
        X[i] = Vector3d(p.x, p.y, p.z);
      }
      Eigen::Matrix<double, 30, 30> Ke = Eigen::Matrix<double, 30, 30>::Zero();
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 6, 30> B;
        const double detJ = StrainDisp(X, g[0], g[1], g[2], B);
        if (std::abs(detJ) < 1e-12)
          return fail("mesh has a degenerate element");
        Ke += (kTetW * std::abs(detJ)) * (B.transpose() * D * B);
      }
      for (int a = 0; a < 30; ++a) {
        const int ga = dofMap[3 * tet[a / 3] + (a % 3)];
        if (ga < 0)
          continue;
        for (int b = 0; b < 30; ++b) {
          const int gb = dofMap[3 * tet[b / 3] + (b % 3)];
          if (gb < 0 || gb > ga)
            continue;
          kt.emplace_back(ga, gb, Ke(a, b));
        }
      }
    }
    K.setFromTriplets(kt.begin(), kt.end());
  }
  K.makeCompressed();
  {
    std::vector<Eigen::Triplet<double>> mt;
    mt.reserve(mesh.tets.size() * 165); // 10*11/2 pairs, three directions
    for (size_t ti = 0; ti < mesh.tets.size(); ++ti) {
      const auto& tet = mesh.tets[ti];
      const double rho = matOf(ti).densityTPerMM3;
      Vector3d X[10];
      for (int i = 0; i < 10; ++i) {
        const auto& p = mesh.nodes[tet[i]];
        X[i] = Vector3d(p.x, p.y, p.z);
      }
      Eigen::Matrix<double, 10, 10> Me = Eigen::Matrix<double, 10, 10>::Zero();
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 6, 30> B; // only detJ is wanted here
        const double dV = kTetW * std::abs(StrainDisp(X, g[0], g[1], g[2], B));
        double N[10];
        TetShape(g[0], g[1], g[2], N);
        for (int i = 0; i < 10; ++i)
          for (int j = 0; j < 10; ++j)
            Me(i, j) += rho * dV * N[i] * N[j];
      }
      // Consistent mass couples same-direction DOFs (block-diagonal in x/y/z).
      for (int i = 0; i < 10; ++i)
        for (int j = 0; j < 10; ++j)
          for (int d = 0; d < 3; ++d) {
            const int ga = dofMap[3 * tet[i] + d];
            const int gb = dofMap[3 * tet[j] + d];
            if (ga >= 0 && gb >= 0 && ga >= gb)
              mt.emplace_back(ga, gb, Me(i, j));
          }
    }
    M.setFromTriplets(mt.begin(), mt.end());
  }
  M.makeCompressed();

  // Generalized eigenproblem K phi = lambda M phi, lowest eigenvalues via
  // shift-invert at sigma=0 (K is positive-definite here).
  using OpType = SpdShiftInvert;
  using BOpType = Spectra::SparseSymMatProd<double>;
  OpType op(K, M);
  BOpType Bop(M);
  const int nev = std::min(numModes, nFree - 1);
  const int ncv = std::min(std::max(2 * nev + 1, 20), nFree);
  Spectra::SymGEigsShiftSolver<OpType, BOpType, Spectra::GEigsMode::ShiftInvert>
      geigs(op, Bop, nev, ncv, 0.0);
  // The constructor sets the shift, which is where the factorization happens.
  if (!op.ok())
    return fail(
        "model is insufficiently constrained (singular stiffness matrix)");
  try {
    geigs.init();
    geigs.compute(Spectra::SortRule::LargestMagn, 1000, 1e-10,
                  Spectra::SortRule::SmallestAlge);
  } catch (const std::exception& e) {
    return fail(std::string("modal eigensolver failed: ") + e.what());
  }
  if (geigs.info() != Spectra::CompInfo::Successful)
    return fail("modal eigensolver did not converge");
  const Eigen::VectorXd evals = geigs.eigenvalues(); // omega^2, ascending
  const Eigen::MatrixXd evecs = geigs.eigenvectors();

  std::vector<FemResults::ModeShape> modes;
  for (int m = 0; m < evals.size(); ++m) {
    const double lambda = std::max(evals(m), 0.0); // clamp tiny negatives
    constexpr double kTwoPi = 6.283185307179586;
    FemResults::ModeShape mode;
    mode.frequencyHz = std::sqrt(lambda) / kTwoPi;
    mode.displacement.assign(nNodes, glm::dvec3(0, 0, 0));
    double maxMag = 0.0;
    for (int n = 0; n < nNodes; ++n) {
      glm::dvec3 u(0, 0, 0);
      for (int k = 0; k < 3; ++k) {
        const int g = dofMap[3 * n + k];
        if (g >= 0)
          u[k] = evecs(g, m);
      }
      mode.displacement[n] = u;
      maxMag = std::max(maxMag, glm::length(u));
    }
    if (maxMag > 0) // normalize the shape to max |u| = 1 for display
      for (auto& u : mode.displacement)
        u /= maxMag;
    modes.push_back(std::move(mode));
  }
  return modes;
}
#else  // !HAVE_SPECTRA
// Both overloads, and the single-material one delegates exactly as the real
// implementation does: a stub that covers only one of them links on a build
// with Spectra and fails on the build that needs the stub — which is the
// only build nobody compiles by accident (Ubuntu ships no Spectra, so the
// stock-container job is what catches it).
std::optional<std::vector<FemResults::ModeShape>>
SolveModal(const FemMesh&, const std::vector<Material>&,
           const std::vector<int>&, int, std::string* error) {
  if (error)
    *error = "built without modal support (Spectra not found)";
  return std::nullopt;
}
std::optional<std::vector<FemResults::ModeShape>>
SolveModal(const FemMesh& mesh, const Material& material,
           const std::vector<int>& fixedNodes, int numModes,
           std::string* error) {
  return SolveModal(mesh, std::vector<Material>{material}, fixedNodes, numModes,
                    error);
}
#endif // HAVE_SPECTRA

#else // !HAVE_FEM_SOLVER

bool SolverAvailable() { return false; }
bool ModalAvailable() { return false; }

std::optional<FemResults> Solve(const FemMesh&, const std::vector<Material>&,
                                const std::vector<int>&,
                                const std::vector<FaceLoad>&,
                                std::string* error, const glm::dvec3&) {
  if (error)
    *error = "built without FEM solver support (Eigen not found)";
  return std::nullopt;
}

std::optional<FemResults> Solve(const FemMesh& mesh, const Material& material,
                                const std::vector<int>& fixedNodes,
                                const std::vector<FaceLoad>& loads,
                                std::string* error,
                                const glm::dvec3& gravityAccel) {
  return Solve(mesh, std::vector<Material>{material}, fixedNodes, loads, error,
               gravityAccel);
}

std::optional<std::vector<FemResults>>
SolveCases(const FemMesh&, const std::vector<Material>&,
           const std::vector<int>&, const std::vector<std::vector<FaceLoad>>&,
           std::string* error, const glm::dvec3&, const SolveOptions&,
           SolveReport*) {
  if (error)
    *error = "built without FEM solver support (Eigen not found)";
  return std::nullopt;
}

std::optional<std::vector<FemResults::ModeShape>>
SolveModal(const FemMesh&, const std::vector<Material>&,
           const std::vector<int>&, int, std::string* error) {
  if (error)
    *error = "built without FEM solver support (Eigen not found)";
  return std::nullopt;
}

std::optional<std::vector<FemResults::ModeShape>>
SolveModal(const FemMesh& mesh, const Material& material,
           const std::vector<int>& fixedNodes, int numModes,
           std::string* error) {
  return SolveModal(mesh, std::vector<Material>{material}, fixedNodes, numModes,
                    error);
}

#endif // HAVE_FEM_SOLVER

std::optional<FaceMotion>
FitFaceMotion(const FemMesh& mesh, const std::vector<glm::dvec3>& displacement,
              int faceIndex) {
  if (displacement.size() != mesh.nodes.size())
    return std::nullopt;
  // Each node stands for a sixth of every triangle it is a node of, so a
  // finely meshed corner of the face does not outvote the rest of it.
  std::vector<double> weight(mesh.nodes.size(), 0.0);
  double area = 0.0;
  for (const FemMesh::BoundaryTri& t : mesh.boundary) {
    if (t.faceIndex != faceIndex)
      continue;
    const glm::dvec3& a = mesh.nodes[t.nodes[0]];
    const double triArea =
        0.5 * glm::length(glm::cross(mesh.nodes[t.nodes[1]] - a,
                                     mesh.nodes[t.nodes[2]] - a));
    area += triArea;
    for (int n : t.nodes)
      weight[n] += triArea / 6.0;
  }
  if (area <= 0.0)
    return std::nullopt;

  FaceMotion out;
  out.area = area;
  double total = 0.0;
  for (std::size_t i = 0; i < weight.size(); ++i) {
    if (weight[i] <= 0.0)
      continue;
    ++out.nodes;
    total += weight[i];
    out.centroid += weight[i] * mesh.nodes[i];
    out.translation += weight[i] * displacement[i];
  }
  out.centroid /= total;
  out.translation /= total;

  // Least squares on u - t = theta x r over the nodes: with the centroid as
  // the origin the translation decouples, and theta solves the face's own
  // "inertia" system  sum w (|r|^2 I - r r^T) theta = sum w r x (u - t).
  glm::dmat3 a(0.0);
  glm::dvec3 b(0.0);
  for (std::size_t i = 0; i < weight.size(); ++i) {
    if (weight[i] <= 0.0)
      continue;
    const glm::dvec3 r = mesh.nodes[i] - out.centroid;
    a += weight[i] *
         (glm::dot(r, r) * glm::dmat3(1.0) - glm::outerProduct(r, r));
    b += weight[i] * glm::cross(r, displacement[i] - out.translation);
  }
  // Singular for nodes on one line (a rotation about it moves nothing).
  const double trace = a[0][0] + a[1][1] + a[2][2];
  if (trace > 0.0 &&
      std::abs(glm::determinant(a)) > 1e-12 * trace * trace * trace) {
    out.rotation = glm::inverse(a) * b;
    out.rotationDetermined = true;
  }
  double sq = 0.0;
  for (std::size_t i = 0; i < weight.size(); ++i) {
    if (weight[i] <= 0.0)
      continue;
    const glm::dvec3 e = displacement[i] - out.At(mesh.nodes[i]);
    sq += weight[i] * glm::dot(e, e);
  }
  out.rmsResidual = std::sqrt(sq / total);
  return out;
}

} // namespace fem
