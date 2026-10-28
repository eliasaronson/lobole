// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemSolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

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
  Supports supports;
  supports.fixedNodes = fixedNodes;
  return SolveCases(mesh, byDomain, supports, cases, error, gravityAccel,
                    options, report);
}

std::optional<std::vector<FemResults>>
SolveCases(const FemMesh& mesh, const std::vector<Material>& byDomain,
           const Supports& supports,
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

  // Nodes held in their own frames: their DOF are local, R's columns. A
  // node also fixed is fixed (all three held, its frame immaterial).
  std::vector<char> isFixed(nNodes, 0);
  for (int n : supports.fixedNodes)
    if (n >= 0 && n < nNodes)
      isFixed[n] = 1;
  std::vector<int> frameOf(nNodes, -1);
  std::vector<Eigen::Matrix3d> frames;
  std::vector<char> heldDof(3 * nNodes, 0);
  for (int n = 0; n < nNodes; ++n)
    if (isFixed[n])
      heldDof[3 * n] = heldDof[3 * n + 1] = heldDof[3 * n + 2] = 1;
  for (const HeldNode& h : supports.held) {
    if (h.node < 0 || h.node >= nNodes || isFixed[h.node])
      continue;
    if (frameOf[h.node] < 0) {
      Eigen::Matrix3d R;
      for (int k = 0; k < 3; ++k)
        R.col(k) = Vector3d(h.frame[k].x, h.frame[k].y, h.frame[k].z);
      frameOf[h.node] = static_cast<int>(frames.size());
      frames.push_back(R);
    }
    for (int k = 0; k < 3; ++k)
      if (h.held[k])
        heldDof[3 * h.node + k] = 1;
  }
  const bool anyFrame = !frames.empty();
  // Ties: each one's largest free entry is the MASTER, eliminated as
  // u_m = sum_s f_s u_s over the others (f_s = -c_s / c_m). A tie left with
  // one free entry holds it; with none, nothing.
  struct TieElim {
    int master = -1;                            // full DOF index
    std::vector<std::pair<int, double>> slaves; // full DOF, factor
  };
  std::vector<TieElim> elims;
  std::vector<int> tieOfDof(3 * nNodes, -1);
  for (const Tie& tie : supports.ties) {
    std::vector<std::pair<int, double>> live;
    for (const Tie::Entry& e : tie.entries) {
      if (e.node < 0 || e.node >= nNodes || e.k < 0 || e.k > 2 ||
          frameOf[e.node] < 0 || e.coef == 0.0)
        continue;
      const int d = 3 * e.node + e.k;
      if (!heldDof[d] && tieOfDof[d] < 0)
        live.push_back({d, e.coef});
    }
    if (live.empty())
      continue;
    if (live.size() == 1) {
      heldDof[live[0].first] = 1;
      continue;
    }
    size_t m = 0;
    for (size_t i = 1; i < live.size(); ++i)
      if (std::abs(live[i].second) > std::abs(live[m].second))
        m = i;
    TieElim te;
    te.master = live[m].first;
    for (size_t i = 0; i < live.size(); ++i)
      if (i != m)
        te.slaves.push_back({live[i].first, -live[i].second / live[m].second});
    tieOfDof[te.master] = static_cast<int>(elims.size());
    heldDof[te.master] = 1;
    elims.push_back(std::move(te));
  }
  // DOF reduction map: -1 for held DOFs, else a compacted free-DOF index.
  DofMap dofs;
  dofs.map.assign(3 * nNodes, -1);
  for (int d = 0; d < 3 * nNodes; ++d)
    if (!heldDof[d])
      dofs.map[d] = dofs.nFree++;
  const std::vector<int>& dofMap = dofs.map;
  const int nFree = dofs.nFree;
  if (nFree == 0)
    return fail("model is fully constrained (no free DOF)");
  // A full DOF as free ones: itself, a tie's master as its slaves, a held
  // one as nothing.
  const auto expand = [&](int d, std::vector<std::pair<int, double>>& out) {
    out.clear();
    if (dofMap[d] >= 0)
      out.push_back({dofMap[d], 1.0});
    else if (tieOfDof[d] >= 0)
      for (const auto& [sd, f] : elims[tieOfDof[d]].slaves)
        out.push_back({dofMap[sd], f});
  };
  // A node's local vector from its x, y, z one, and back.
  const auto toLocal = [&](int n, const Vector3d& v) -> Vector3d {
    return frameOf[n] < 0 ? v : Vector3d(frames[frameOf[n]].transpose() * v);
  };
  const auto toGlobal = [&](int n, const Vector3d& v) -> Vector3d {
    return frameOf[n] < 0 ? v : Vector3d(frames[frameOf[n]] * v);
  };

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
      // A node held in its own frame takes its rows and columns there.
      if (anyFrame) {
        bool rotated = false;
        Eigen::Matrix<double, 30, 30> T =
            Eigen::Matrix<double, 30, 30>::Identity();
        for (int i = 0; i < 10; ++i)
          if (frameOf[tet[i]] >= 0) {
            T.block<3, 3>(3 * i, 3 * i) = frames[frameOf[tet[i]]];
            rotated = true;
          }
        if (rotated)
          Ke = T.transpose() * Ke * T;
      }
      // Scatter into the reduced system, skipping held DOFs; a tie's
      // master spreads over its slaves.
      bool tied = false;
      if (!elims.empty())
        for (int a = 0; a < 30 && !tied; ++a)
          tied = tieOfDof[3 * tet[a / 3] + (a % 3)] >= 0;
      if (tied) {
        std::array<std::vector<std::pair<int, double>>, 30> ex;
        for (int a = 0; a < 30; ++a)
          expand(3 * tet[a / 3] + (a % 3), ex[a]);
        for (int a = 0; a < 30; ++a)
          for (const auto& [ga, fa] : ex[a])
            for (int b = 0; b < 30; ++b)
              for (const auto& [gb, fb] : ex[b])
                triplets.emplace_back(ga, gb, fa * fb * Ke(a, b));
        continue;
      }
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
  // Loads are assembled over every node in x, y, z (the reactions need the
  // whole of them), then each node's taken into its frame and reduced.
  std::vector<int> allDofs(3 * nNodes);
  for (int d = 0; d < 3 * nNodes; ++d)
    allDofs[d] = d;
  Eigen::VectorXd Fall = Eigen::VectorXd::Zero(3 * nNodes);
  auto addNodal = [&](int node, const Vector3d& fv) {
    Fall.segment<3>(3 * node) += fv;
  };
  const auto reduce = [&](const Eigen::VectorXd& full) {
    Eigen::VectorXd r = Eigen::VectorXd::Zero(nFree);
    for (int n = 0; n < nNodes; ++n) {
      const Vector3d v = toLocal(n, full.segment<3>(3 * n));
      for (int k = 0; k < 3; ++k)
        if (dofMap[3 * n + k] >= 0)
          r(dofMap[3 * n + k]) += v(k);
        else if (tieOfDof[3 * n + k] >= 0)
          for (const auto& [sd, f] : elims[tieOfDof[3 * n + k]].slaves)
            r(dofMap[sd]) += f * v(k);
    }
    return r;
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
  if (iterative && options.makeExternal && anyFrame) {
    // The GPU's SpMV applies each element matrix relative to its first
    // node's translation, which holds only when every node is in x, y, z.
    declined = "a pin support holds nodes in rotated frames, which the GPU "
               "solve does not take";
  } else if (iterative && options.makeExternal) {
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
  const Eigen::VectorXd Fgravity = Fall;
  for (const std::vector<FaceLoad>& loads : cases) {
    Eigen::VectorXd Fcase = Fgravity;
    {
      std::string loadErr;
      if (!AssembleFaceLoads(mesh, loads, allDofs, Fcase, &loadErr))
        return fail(loadErr);
    }
    const Eigen::VectorXd F = reduce(Fcase);
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
      // A device that went away is not one that cannot do the problem: the
      // solve fails, naming it, rather than starting over on the CPU.
      if (!ok && external->DeviceLost())
        return fail("the GPU was lost during the solve (" + why +
                    "); run it again");
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

    // Expand to full displacement (held DOFs are zero), back in x, y, z.
    FemResults out;
    out.displacement.resize(nNodes);
    for (int n = 0; n < nNodes; ++n) {
      Vector3d d(0, 0, 0);
      for (int k = 0; k < 3; ++k) {
        const int g = dofMap[3 * n + k];
        if (g >= 0)
          d(k) = u(g);
        else if (tieOfDof[3 * n + k] >= 0)
          for (const auto& [sd, f] : elims[tieOfDof[3 * n + k]].slaves)
            d(k) += f * u(dofMap[sd]);
      }
      d = toGlobal(n, d);
      out.displacement[n] = glm::dvec3(d(0), d(1), d(2));
    }
    // The internal force K u at every node, less the applied load: what the
    // supports carry (zero, to the residual, where nothing holds).
    Eigen::VectorXd fint;
    if (supports.reactions)
      fint = Eigen::VectorXd::Zero(3 * nNodes);

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
      if (supports.reactions) {
        Eigen::Matrix<double, 30, 1> fe = Eigen::Matrix<double, 30, 1>::Zero();
        for (const auto& g : kTetGauss) {
          Eigen::Matrix<double, 6, 30> B;
          const double detJ = StrainDisp(X, g[0], g[1], g[2], B);
          fe += (kTetW * std::abs(detJ)) * (B.transpose() * (D * (B * ue)));
        }
        for (int i = 0; i < 10; ++i)
          fint.segment<3>(3 * tet[i]) += fe.segment<3>(3 * i);
      }
    }
    if (supports.reactions) {
      out.nodeReaction.resize(nNodes);
      for (int n = 0; n < nNodes; ++n) {
        const Vector3d r = fint.segment<3>(3 * n) - Fcase.segment<3>(3 * n);
        out.nodeReaction[n] = glm::dvec3(r(0), r(1), r(2));
      }
    }

    out.vonMises.resize(nNodes, 0.0);
    out.stress.assign(nNodes, Stress{});
    for (int n = 0; n < nNodes; ++n) {
      if (weight[n] > 0) {
        const Eigen::Matrix<double, 6, 1> s = stressAccum[n] / weight[n];
        out.vonMises[n] = VonMises(s);
        for (int k = 0; k < 6; ++k)
          out.stress[n][k] = s(k);
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

std::string FreeRigidMotion(const FemMesh& mesh, const Supports& supports,
                            glm::dvec3* axisDir, glm::dvec3* axisPoint,
                            bool* turns) {
  const int nNodes = static_cast<int>(mesh.nodes.size());
  if (nNodes == 0)
    return "move freely (the mesh is empty)";
  Vector3d lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300),
      c = Vector3d::Zero();
  for (const glm::dvec3& p : mesh.nodes) {
    const Vector3d v(p.x, p.y, p.z);
    lo = lo.cwiseMin(v);
    hi = hi.cwiseMax(v);
    c += v;
  }
  c /= nNodes;
  const double L = std::max((hi - lo).norm(), 1e-9);
  // Each held direction d at x is the row [d, (x - c)/L x d]: its product
  // with a rigid motion (t, w) is d . (t + w x (x - c)/L), the motion's
  // displacement along d at x.
  Eigen::Matrix<double, 6, 6> G = Eigen::Matrix<double, 6, 6>::Zero();
  std::vector<char> fixed(nNodes, 0);
  const auto add = [&](int n, const Vector3d& d) {
    const glm::dvec3& p = mesh.nodes[n];
    Eigen::Matrix<double, 6, 1> row;
    row.head<3>() = d;
    row.tail<3>() = ((Vector3d(p.x, p.y, p.z) - c) / L).cross(d);
    G += row * row.transpose();
  };
  for (int n : supports.fixedNodes)
    if (n >= 0 && n < nNodes && !fixed[n]) {
      fixed[n] = 1;
      for (int k = 0; k < 3; ++k)
        add(n, Vector3d::Unit(k));
    }
  std::vector<const HeldNode*> heldOf(nNodes, nullptr);
  for (const HeldNode& h : supports.held)
    if (h.node >= 0 && h.node < nNodes && !fixed[h.node]) {
      heldOf[h.node] = &h;
      for (int k = 0; k < 3; ++k)
        if (h.held[k])
          add(h.node, Vector3d(h.frame[k].x, h.frame[k].y, h.frame[k].z));
    }
  for (const Tie& tie : supports.ties) {
    Eigen::Matrix<double, 6, 1> row = Eigen::Matrix<double, 6, 1>::Zero();
    for (const Tie::Entry& e : tie.entries) {
      if (e.node < 0 || e.node >= nNodes || !heldOf[e.node] || e.k < 0 ||
          e.k > 2)
        continue;
      const glm::dvec3& f = heldOf[e.node]->frame[e.k];
      const Vector3d d(f.x, f.y, f.z);
      const glm::dvec3& p = mesh.nodes[e.node];
      row.head<3>() += e.coef * d;
      row.tail<3>() += e.coef * ((Vector3d(p.x, p.y, p.z) - c) / L).cross(d);
    }
    G += row * row.transpose();
  }
  Eigen::SelfAdjointEigenSolver<Eigen::Matrix<double, 6, 6>> eig(G);
  const auto& ev = eig.eigenvalues(); // ascending
  if (ev(5) > 0 && ev(0) > 1e-10 * ev(5))
    return {};
  const Eigen::Matrix<double, 6, 1> v = eig.eigenvectors().col(0);
  const Vector3d t = v.head<3>();
  const Vector3d w = v.tail<3>() / L; // the rotation rate, per mm
  char buf[200];
  if (v.tail<3>().norm() < 1e-6 * v.norm()) {
    const Vector3d d = t.normalized();
    std::snprintf(buf, sizeof(buf), "slide along (%.3g, %.3g, %.3g)", d.x(),
                  d.y(), d.z());
    if (axisDir)
      *axisDir = glm::dvec3(d.x(), d.y(), d.z());
    if (turns)
      *turns = false;
    return buf;
  }
  // The point the motion turns about: where u is along w.
  Vector3d d = w.normalized();
  const Vector3d p = c + w.cross(t) / w.squaredNorm();
  // Signed so the largest component is positive, as a face's axis is.
  int big = 0;
  for (int k = 1; k < 3; ++k)
    if (std::abs(d(k)) > std::abs(d(big)))
      big = k;
  if (d(big) < 0)
    d = -d;
  std::snprintf(buf, sizeof(buf),
                "turn about an axis along (%.3g, %.3g, %.3g) through "
                "(%.4g, %.4g, %.4g) mm",
                d.x(), d.y(), d.z(), p.x(), p.y(), p.z());
  if (axisDir)
    *axisDir = glm::dvec3(d.x(), d.y(), d.z());
  if (axisPoint)
    *axisPoint = glm::dvec3(p.x(), p.y(), p.z());
  if (turns)
    *turns = true;
  return buf;
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

std::string FreeRigidMotion(const FemMesh&, const Supports&, glm::dvec3*,
                            glm::dvec3*, bool*) {
  return {};
}

std::optional<std::vector<FemResults>>
SolveCases(const FemMesh&, const std::vector<Material>&, const Supports&,
           const std::vector<std::vector<FaceLoad>>&, std::string* error,
           const glm::dvec3&, const SolveOptions&, SolveReport*) {
  if (error)
    *error = "built without the FEM solver (Eigen)";
  return std::nullopt;
}

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

double MaxPrincipal(const Stress& s) {
  // The largest root of the characteristic cubic of a symmetric 3x3, in
  // closed form (Smith 1961): no iteration, and no Eigen, so a build
  // without the solver still reads a saved result.
  const double xx = s[0], yy = s[1], zz = s[2], xy = s[3], yz = s[4], zx = s[5];
  const double off = xy * xy + yz * yz + zx * zx;
  const double q = (xx + yy + zz) / 3.0;
  const double a = xx - q, b = yy - q, c = zz - q;
  const double p2 = a * a + b * b + c * c + 2.0 * off;
  if (p2 <= 1e-30 * (q * q + 1e-300))
    return q; // hydrostatic: every direction is principal
  const double p = std::sqrt(p2 / 6.0);
  // det((sigma - q I) / p) / 2, clamped against rounding.
  const double det =
      a * (b * c - yz * yz) - xy * (xy * c - yz * zx) + zx * (xy * yz - b * zx);
  const double r = std::clamp(det / (2.0 * p * p * p), -1.0, 1.0);
  return q + 2.0 * p * std::cos(std::acos(r) / 3.0);
}

double NormalStress(const Stress& s, const glm::dvec3& n) {
  const double len2 = glm::dot(n, n);
  if (len2 <= 0.0)
    return 0.0;
  const double x = n.x, y = n.y, z = n.z;
  return (s[0] * x * x + s[1] * y * y + s[2] * z * z +
          2.0 * (s[3] * x * y + s[4] * y * z + s[5] * z * x)) /
         len2;
}

std::vector<double> MaxPrincipalField(const FemResults& r) {
  std::vector<double> out;
  out.reserve(r.stress.size());
  for (const Stress& s : r.stress)
    out.push_back(MaxPrincipal(s));
  return out;
}

std::vector<double> AcrossLayersField(const FemResults& r,
                                      const glm::dvec3& n) {
  std::vector<double> out;
  out.reserve(r.stress.size());
  for (const Stress& s : r.stress)
    out.push_back(NormalStress(s, n));
  return out;
}

FieldPeak PeakOf(const FemMesh& mesh, const std::vector<double>& field) {
  FieldPeak out;
  if (field.empty() || field.size() != mesh.nodes.size())
    return out;
  for (size_t i = 0; i < field.size(); ++i)
    if (out.node < 0 || field[i] > out.value) {
      out.value = field[i];
      out.node = static_cast<int>(i);
      out.at = mesh.nodes[i];
    }
  return out;
}

std::optional<FaceStress> FaceStressOf(const FemMesh& mesh,
                                       const std::vector<double>& vonMises,
                                       int faceIndex) {
  if (vonMises.size() != mesh.nodes.size())
    return std::nullopt;
  FaceStress out;
  double weighted = 0.0;
  std::vector<char> seen(mesh.nodes.size(), 0);
  bool any = false;
  for (const FemMesh::BoundaryTri& t : mesh.boundary) {
    if (t.faceIndex != faceIndex)
      continue;
    any = true;
    const glm::dvec3& a = mesh.nodes[t.nodes[0]];
    const double triArea =
        0.5 * glm::length(glm::cross(mesh.nodes[t.nodes[1]] - a,
                                     mesh.nodes[t.nodes[2]] - a));
    // The integral of the quadratic (tri6) interpolant over a flat
    // triangle: the corner shape functions integrate to zero and each
    // mid-side one to a third of the area.
    weighted +=
        triArea *
        (vonMises[t.nodes[3]] + vonMises[t.nodes[4]] + vonMises[t.nodes[5]]) /
        3.0;
    out.area += triArea;
    for (int n : t.nodes) {
      if (!seen[n]) {
        seen[n] = 1;
        ++out.nodes;
      }
      if (out.nodes == 1 || vonMises[n] > out.maxMPa) { // first, or larger
        out.maxMPa = vonMises[n];
        out.maxAt = mesh.nodes[n];
      }
    }
  }
  if (!any || out.area <= 0.0)
    return std::nullopt;
  out.meanMPa = weighted / out.area;
  return out;
}

FemResults::SupportEdgePeak
FindSupportEdgePeak(const FemMesh& mesh, const FemResults& results,
                    const std::vector<SupportFacets>& supports) {
  FemResults::SupportEdgePeak out;
  const size_t nNodes = mesh.nodes.size();
  if (results.vonMises.size() != nNodes || supports.empty())
    return out;
  // Which support's edge a node is near (0 = none); a support's edge is its
  // facets' nodes that a boundary facet it does not hold also has.
  std::vector<int> nearOf(nNodes, -1);
  std::vector<char> edge(nNodes, 0);
  std::vector<char> mark(nNodes, 0);
  for (size_t si = 0; si < supports.size(); ++si) {
    std::vector<char> on(mesh.boundary.size(), 0);
    for (int t : supports[si].tris)
      if (t >= 0 && static_cast<size_t>(t) < on.size())
        on[t] = 1;
    std::fill(mark.begin(), mark.end(), 0);
    for (size_t t = 0; t < mesh.boundary.size(); ++t)
      if (on[t])
        for (int v : mesh.boundary[t].nodes)
          mark[v] = 1;
    std::fill(edge.begin(), edge.end(), 0);
    bool any = false;
    for (size_t t = 0; t < mesh.boundary.size(); ++t)
      if (!on[t])
        for (int v : mesh.boundary[t].nodes)
          if (mark[v]) {
            edge[v] = 1;
            any = true;
          }
    if (!any)
      continue;
    // Within one element: every node of a tet that has an edge node.
    for (const auto& tet : mesh.tets) {
      bool touches = false;
      for (int v : tet)
        touches = touches || edge[v];
      if (touches)
        for (int v : tet)
          if (nearOf[v] < 0)
            nearOf[v] = static_cast<int>(si);
    }
  }
  const int peak = results.maxVmNode;
  if (peak < 0 || static_cast<size_t>(peak) >= nNodes || nearOf[peak] < 0)
    return out;
  out.flagged = true;
  out.support = supports[nearOf[peak]].id;
  out.type = supports[nearOf[peak]].type;
  for (size_t n = 0; n < nNodes; ++n)
    if (nearOf[n] < 0 && results.vonMises[n] > out.awayMPa) {
      out.awayMPa = results.vonMises[n];
      out.awayAt = mesh.nodes[n];
    }
  return out;
}

} // namespace fem
