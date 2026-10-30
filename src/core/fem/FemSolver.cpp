// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemSolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>

#ifdef HAVE_FEM_SOLVER
#include "core/fem/FemSolverInternal.h"
#include "core/fem/FemTopo.h" // TopoLinearSolver, the external solver's seam

#include <Eigen/Dense>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#endif

#ifdef HAVE_SPECTRA
#include <Spectra/MatOp/SparseCholesky.h>
#include <Spectra/MatOp/SparseSymMatProd.h>
#include <Spectra/SymGEigsShiftSolver.h>
#include <Spectra/SymGEigsSolver.h>
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

// What holds a mesh, as the reduced system SolveCases and SolveBuckling both
// solve in: each held node's DOF in its own frame (R's columns; a node also
// fixed is fixed, all three held, its frame immaterial), a tie's master
// eliminated in terms of its slaves, and the free DOF compacted.
struct TieElim {
  int master = -1;                            // full DOF index
  std::vector<std::pair<int, double>> slaves; // full DOF, factor
};
struct Reduction {
  int nNodes = 0;
  std::vector<int> frameOf;
  std::vector<Eigen::Matrix3d> frames;
  std::vector<TieElim> elims;
  std::vector<int> tieOfDof;
  DofMap dofs;
  bool AnyFrame() const { return !frames.empty(); }
  // A full DOF as free ones: itself, a tie's master as its slaves, a held
  // one as nothing.
  void Expand(int d, std::vector<std::pair<int, double>>& out) const {
    out.clear();
    if (dofs.map[d] >= 0)
      out.push_back({dofs.map[d], 1.0});
    else if (tieOfDof[d] >= 0)
      for (const auto& [sd, f] : elims[tieOfDof[d]].slaves)
        out.push_back({dofs.map[sd], f});
  }
  // A node's local vector from its x, y, z one, and back.
  Vector3d ToLocal(int n, const Vector3d& v) const {
    return frameOf[n] < 0 ? v : Vector3d(frames[frameOf[n]].transpose() * v);
  }
  Vector3d ToGlobal(int n, const Vector3d& v) const {
    return frameOf[n] < 0 ? v : Vector3d(frames[frameOf[n]] * v);
  }
  // An element matrix in x, y, z scattered into the reduced system: a node
  // held in its own frame takes its rows and columns there, held DOF are
  // skipped, and a tie's master spreads over its slaves. `lowerOnly` keeps
  // the entries on and below the diagonal.
  void Scatter(const std::array<int, 10>& tet, Eigen::Matrix<double, 30, 30> Ke,
               std::vector<Eigen::Triplet<double>>& out, bool lowerOnly) const {
    if (AnyFrame()) {
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
    bool tied = false;
    if (!elims.empty())
      for (int a = 0; a < 30 && !tied; ++a)
        tied = tieOfDof[3 * tet[a / 3] + (a % 3)] >= 0;
    if (tied) {
      std::array<std::vector<std::pair<int, double>>, 30> ex;
      for (int a = 0; a < 30; ++a)
        Expand(3 * tet[a / 3] + (a % 3), ex[a]);
      for (int a = 0; a < 30; ++a)
        for (const auto& [ga, fa] : ex[a])
          for (int b = 0; b < 30; ++b)
            for (const auto& [gb, fb] : ex[b])
              if (!lowerOnly || gb <= ga)
                out.emplace_back(ga, gb, fa * fb * Ke(a, b));
      return;
    }
    for (int a = 0; a < 30; ++a) {
      const int ga = dofs.map[3 * tet[a / 3] + (a % 3)];
      if (ga < 0)
        continue;
      for (int b = 0; b < 30; ++b) {
        const int gb = dofs.map[3 * tet[b / 3] + (b % 3)];
        if (gb < 0 || (lowerOnly && gb > ga))
          continue;
        out.emplace_back(ga, gb, Ke(a, b));
      }
    }
  }
  // A vector over every node in x, y, z, reduced.
  Eigen::VectorXd Reduce(const Eigen::VectorXd& full) const {
    Eigen::VectorXd r = Eigen::VectorXd::Zero(dofs.nFree);
    for (int n = 0; n < nNodes; ++n) {
      const Vector3d v = ToLocal(n, full.segment<3>(3 * n));
      for (int k = 0; k < 3; ++k)
        if (dofs.map[3 * n + k] >= 0)
          r(dofs.map[3 * n + k]) += v(k);
        else if (tieOfDof[3 * n + k] >= 0)
          for (const auto& [sd, f] : elims[tieOfDof[3 * n + k]].slaves)
            r(dofs.map[sd]) += f * v(k);
    }
    return r;
  }
  // Node `n`'s displacement in x, y, z from a reduced one (a held DOF is
  // zero).
  glm::dvec3 NodeDisplacement(const Eigen::VectorXd& u, int n) const {
    Vector3d d(0, 0, 0);
    for (int k = 0; k < 3; ++k) {
      const int g = dofs.map[3 * n + k];
      if (g >= 0)
        d(k) = u(g);
      else if (tieOfDof[3 * n + k] >= 0)
        for (const auto& [sd, f] : elims[tieOfDof[3 * n + k]].slaves)
          d(k) += f * u(dofs.map[sd]);
    }
    d = ToGlobal(n, d);
    return glm::dvec3(d(0), d(1), d(2));
  }
};

Reduction MakeReduction(const FemMesh& mesh, const Supports& supports) {
  Reduction red;
  const int nNodes = static_cast<int>(mesh.nodes.size());
  red.nNodes = nNodes;
  std::vector<char> isFixed(nNodes, 0);
  for (int n : supports.fixedNodes)
    if (n >= 0 && n < nNodes)
      isFixed[n] = 1;
  red.frameOf.assign(nNodes, -1);
  std::vector<char> heldDof(3 * nNodes, 0);
  for (int n = 0; n < nNodes; ++n)
    if (isFixed[n])
      heldDof[3 * n] = heldDof[3 * n + 1] = heldDof[3 * n + 2] = 1;
  for (const HeldNode& h : supports.held) {
    if (h.node < 0 || h.node >= nNodes || isFixed[h.node])
      continue;
    if (red.frameOf[h.node] < 0) {
      Eigen::Matrix3d R;
      for (int k = 0; k < 3; ++k)
        R.col(k) = Vector3d(h.frame[k].x, h.frame[k].y, h.frame[k].z);
      red.frameOf[h.node] = static_cast<int>(red.frames.size());
      red.frames.push_back(R);
    }
    for (int k = 0; k < 3; ++k)
      if (h.held[k])
        heldDof[3 * h.node + k] = 1;
  }
  // Ties: each one's largest free entry is the MASTER, eliminated as
  // u_m = sum_s f_s u_s over the others (f_s = -c_s / c_m). A tie left with
  // one free entry holds it; with none, nothing.
  red.tieOfDof.assign(3 * nNodes, -1);
  for (const Tie& tie : supports.ties) {
    std::vector<std::pair<int, double>> live;
    for (const Tie::Entry& e : tie.entries) {
      if (e.node < 0 || e.node >= nNodes || e.k < 0 || e.k > 2 ||
          red.frameOf[e.node] < 0 || e.coef == 0.0)
        continue;
      const int d = 3 * e.node + e.k;
      if (!heldDof[d] && red.tieOfDof[d] < 0)
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
    red.tieOfDof[te.master] = static_cast<int>(red.elims.size());
    heldDof[te.master] = 1;
    red.elims.push_back(std::move(te));
  }
  // DOF reduction map: -1 for held DOFs, else a compacted free-DOF index.
  red.dofs.map.assign(3 * nNodes, -1);
  for (int d = 0; d < 3 * nNodes; ++d)
    if (!heldDof[d])
      red.dofs.map[d] = red.dofs.nFree++;
  return red;
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

  const Reduction red = MakeReduction(mesh, supports);
  const std::vector<int>& dofMap = red.dofs.map;
  const int nFree = red.dofs.nFree;
  const bool anyFrame = red.AnyFrame();
  if (nFree == 0)
    return fail("model is fully constrained (no free DOF)");
  const auto toLocal = [&](int n, const Vector3d& v) {
    return red.ToLocal(n, v);
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
      red.Scatter(tet, Ke, triplets, false);
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
    return red.Reduce(full);
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

  // A thermal load (docs/simulation/README.md
  // #the-thermal-load-on-a-static-study): the strain alpha (T - T0) on the
  // volumetric components, T interpolated at each Gauss point, its load
  // integral B^T D eps_th shared by every case as gravity is.
  const NodalTemperature* thermal = options.thermal.get();
  if (thermal && thermal->temperature.size() != static_cast<size_t>(nNodes))
    return fail("the temperature field is not one temperature per node");
  const auto thermalStrain = [&](size_t ti, const double N[10]) {
    Eigen::Matrix<double, 6, 1> e = Eigen::Matrix<double, 6, 1>::Zero();
    double T = 0.0;
    for (int i = 0; i < 10; ++i)
      T += N[i] * thermal->temperature[mesh.tets[ti][i]];
    const double v = matOf(ti).expansionPerK * (T - thermal->stressFreeC);
    e(0) = e(1) = e(2) = v;
    return e;
  };
  if (thermal) {
    for (size_t ti = 0; ti < mesh.tets.size(); ++ti) {
      if (matOf(ti).expansionPerK == 0.0)
        continue;
      const auto& tet = mesh.tets[ti];
      Vector3d X[10];
      for (int i = 0; i < 10; ++i) {
        const auto& p = mesh.nodes[tet[i]];
        X[i] = Vector3d(p.x, p.y, p.z);
      }
      Eigen::Matrix<double, 30, 1> fe = Eigen::Matrix<double, 30, 1>::Zero();
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 6, 30> B;
        const double dV = kTetW * std::abs(StrainDisp(X, g[0], g[1], g[2], B));
        double N[10];
        TetShape(g[0], g[1], g[2], N);
        fe += dV * (B.transpose() * (DOf(ti) * thermalStrain(ti, N)));
      }
      for (int i = 0; i < 10; ++i)
        addNodal(tet[i], fe.segment<3>(3 * i));
    }
  }

  // Direct below the threshold: factorise once, every case one right-hand
  // side. Above it, PCG per case on K with one incomplete-Cholesky
  // preconditioner. Both are held to the same residual below.
  // An answer solved elsewhere (CalculiX) is checked, not solved for.
  const bool given = options.given != nullptr;
  const bool iterative = !given && nFree > options.iterativeAboveDofs;
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
  if (options.solving && !given)
    options.solving(solverName);
  if (report) {
    *report = SolveReport{};
    report->solver = given ? options.givenName : std::string(solverName);
    if (iterative && !external && options.makeExternal)
      report->externalDeclined = declined;
  }
  if (options.cancelled && options.cancelled())
    return fail("cancelled");
  constexpr const char* kSingular =
      "model is insufficiently constrained (singular stiffness matrix)";
  // A rigid motion no support resists makes K singular, and is read from
  // the supports, exactly, before any solver is asked: a failed iterative
  // solve says nothing about why it failed, and a slow one is not a
  // singular one (docs/simulation/README.md#the-solver). An answer solved
  // elsewhere is checked against K whatever holds it (a contact's springs).
  if (!given) {
    const std::string free = FreeRigidMotion(mesh, supports);
    if (!free.empty())
      return fail(std::string(kSingular) + ": its supports leave it free to " +
                  free);
  }
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
      return fail("the CPU's preconditioner (an incomplete Cholesky factor "
                  "of K) could not be built");
  } else if (!given) {
    direct.compute(K);
    if (direct.info() != Eigen::Success)
      return fail(kSingular);
  }
  // A cap no converging solve comes near (a well-posed tet10 model takes a
  // few hundred to a few thousand iterations; a thin plate at 900k DOF
  // several thousand).
  const int maxIterations = std::max(10000, nFree / 4);
  // Why the GPU gave a case up, carried into the CPU's error.
  std::string gpuWhy;
  int iterationsSoFar = 0; // every case's, for options.iterated
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
    bool gpuSolved = false;
    const size_t caseIndex = all.size();
    if (given) {
      // The answer in this system's DOF: each node's displacement in its
      // frame, its free directions kept. A direction this study holds that
      // the answer moved is not this study's answer; one the printed
      // precision moved is.
      const std::string& who = options.givenName;
      if (caseIndex >= options.given->size() ||
          (*options.given)[caseIndex].size() != static_cast<size_t>(nNodes))
        return fail("the answer from " + who +
                    " is not one displacement per node");
      const std::vector<glm::dvec3>& gu = (*options.given)[caseIndex];
      u = Eigen::VectorXd::Zero(nFree);
      double scale = 0.0, heldMove = 0.0;
      int heldNode = -1;
      for (int n = 0; n < nNodes; ++n) {
        const Vector3d d(gu[n].x, gu[n].y, gu[n].z);
        scale = std::max(scale, d.norm());
        const Vector3d l = toLocal(n, d);
        for (int k = 0; k < 3; ++k) {
          const int dof = 3 * n + k;
          if (dofMap[dof] >= 0)
            u(dofMap[dof]) = l(k);
          else if (red.tieOfDof[dof] < 0 && std::abs(l(k)) > heldMove) {
            heldMove = std::abs(l(k));
            heldNode = n;
          }
        }
      }
      if (!u.allFinite())
        return fail("the answer from " + who + " is not finite");
      if (heldMove > (1e-6 + 4.0 * options.givenPrecision) * scale)
        return fail("the answer from " + who + " moves node " +
                    std::to_string(heldNode + 1) + " by " +
                    std::to_string(heldMove) +
                    " mm along a direction this study holds");
      // Held to the in-house rule against our own K. Printed to a few
      // digits, an exact answer leaves a residual of its own, at most
      // |K| |u| times the precision: the floor the rule stands on.
      const double fn = F.norm();
      const double residual = fn > 0 ? (K * u - F).norm() / fn : 0.0;
      const double floor = fn > 0 ? (K.cwiseAbs() * u.cwiseAbs()).norm() *
                                        options.givenPrecision / fn
                                  : 0.0;
      if (!(residual <= 1e-6 + floor))
        return fail("the answer from " + who +
                    " does not solve this study's own stiffness: |K u - f| / "
                    "|f| = " +
                    std::to_string(residual) + ", above 1e-6 plus the " +
                    std::to_string(floor) +
                    " its printed precision accounts for, so the deck and "
                    "the study it was written from differ");
      // That floor is wide: rounding is high-frequency noise, which K
      // amplifies. The energy balance is not: under rounding delta,
      // u . (K u - f) moves by delta . f + delta . K delta -- first order in
      // the precision, and first order in any difference between the deck
      // and the study -- plus the second-order term, which is what a soft
      // part shows: a 0.5 mm bent sheet stores little energy for how stiff
      // its elements are, and the exact answer rounded to the 7 digits ccx
      // prints read 8e-6 against a first-order floor of 2e-6
      // (docs/simulation/README.md#reading-the-answer-back). The first
      // term is bounded, |delta . f| <= p |u| . |f|; the second is a sum
      // over K's entries of independent roundings, whose cross terms
      // average out: its size is the diagonal's, p^2 sum K_ii u_i^2. (Every
      // entry's |K_ij| |u_i| |u_j| bounds it too, but forty neighbours a
      // row over, which let a deck 0.1 % stiffer than the study through.)
      const double work = u.dot(F);
      const double energy =
          work != 0.0 ? std::abs(u.dot(K * u - F)) / std::abs(work) : 0.0;
      const double p = options.givenPrecision;
      const double energyFloor =
          work != 0.0 ? 4.0 *
                            (p * u.cwiseAbs().dot(F.cwiseAbs()) +
                             p * p * u.cwiseAbs2().dot(K.diagonal())) /
                            std::abs(work)
                      : 0.0;
      if (!(energy <= 1e-6 + energyFloor))
        return fail("the answer from " + who +
                    " does not balance this study's own stiffness: |u . (K "
                    "u - f)| / |u . f| = " +
                    std::to_string(energy) + ", above 1e-6 plus the " +
                    std::to_string(energyFloor) +
                    " its printed precision accounts for, so the deck and "
                    "the study it was written from differ");
      if (report) {
        report->worstResidual = std::max(report->worstResidual, residual);
        report->worstResidualFloor =
            std::max(report->worstResidualFloor, floor);
        report->worstEnergyResidual =
            std::max(report->worstEnergyResidual, energy);
      }
    } else if (external) {
      std::vector<double> fv(F.data(), F.data() + F.size()), uv;
      LinearSolveControl control;
      // A decade inside the acceptance below.
      control.tolerance = 1e-7;
      control.cancelled = options.cancelled;
      if (options.iterated)
        control.iterated = [&](int n) {
          options.iterated(iterationsSoFar + n);
        };
      std::string why;
      const bool ok = external->SolveStrict(fv, uv, control, &why);
      iterationsSoFar += control.iterations;
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
        gpuSolved = true;
      } else {
        // Not an answer: finish this case and the rest on the CPU, from
        // where the GPU got to (Pcg drops a start worse than zero), and say
        // why the result names the CPU -- and the error, if the CPU fails
        // too.
        external.reset();
        if (options.solving)
          options.solving(kIterativeSolverName);
        if (report) {
          report->solver = kIterativeSolverName;
          report->externalDeclined = why;
        }
        gpuWhy = "; the GPU solve before it: " + why;
        if (static_cast<int>(uv.size()) == nFree)
          u = Eigen::Map<const Eigen::VectorXd>(uv.data(), nFree);
        if (!prepareIc())
          return fail("the CPU's preconditioner (an incomplete Cholesky "
                      "factor of K) could not be built" +
                      gpuWhy);
      }
    }
    if (iterative && !given && !gpuSolved) {
      int its = 0;
      double rel = 0.0;
      // Two decades under the acceptance below, so the check never decides
      // a converged solve.
      std::function<void(int)> iterated;
      if (options.iterated)
        iterated = [&](int n) { options.iterated(iterationsSoFar + n); };
      const PcgOutcome o = Pcg(K, ic, F, u, 1e-8, maxIterations,
                               options.cancelled, &its, &rel, iterated);
      iterationsSoFar += its;
      if (report)
        report->iterations += its;
      if (o == PcgOutcome::Cancelled)
        return fail("cancelled");
      if (o != PcgOutcome::Converged)
        return fail(PcgFailure(o, its, rel) + gpuWhy);
    } else if (!iterative && !given) {
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
    if (!given) {
      const double residual =
          F.norm() > 0 ? (K * u - F).norm() / F.norm() : 0.0;
      // The supports were checked above: a direct factor that misses is a
      // singular K they do not explain (a piece of the mesh nothing holds);
      // an iterative answer is held to its own tolerance before this.
      if (!u.allFinite() || residual > 1e-6)
        return fail(iterative ? "the iterative solve's answer does not "
                                "satisfy K u = f: relative residual " +
                                    std::to_string(residual)
                              : std::string(kSingular));
      if (report)
        report->worstResidual = std::max(report->worstResidual, residual);
    }

    // Expand to full displacement (held DOFs are zero), back in x, y, z.
    FemResults out;
    out.displacement.resize(nNodes);
    for (int n = 0; n < nNodes; ++n)
      out.displacement[n] = red.NodeDisplacement(u, n);
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
        Eigen::Matrix<double, 6, 1> strain = B * ue;
        if (thermal) {
          // At its own node the field is that node's temperature.
          double Ni[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
          Ni[i] = 1.0;
          strain -= thermalStrain(ti, Ni);
        }
        const Eigen::Matrix<double, 6, 1> s = D * strain;
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

namespace {

// LargeDeformationFields, or with `large` false the small-deformation
// stress of the same displacement (SmallDeformationFields).
FemResults DeformationFields(const FemMesh& mesh,
                             const std::vector<Material>& byDomain,
                             const std::vector<glm::dvec3>& displacement,
                             bool large, const NodalTemperature* thermal) {
  FemResults out;
  const size_t nNodes = mesh.nodes.size();
  out.displacement = displacement;
  out.displacement.resize(nNodes, glm::dvec3(0.0));
  std::vector<Eigen::Matrix3d> accum(nNodes, Eigen::Matrix3d::Zero());
  std::vector<double> weight(nNodes, 0.0);
  for (size_t ti = 0; ti < mesh.tets.size() && !byDomain.empty(); ++ti) {
    const size_t d = mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[ti];
    const Material& m = byDomain[std::min(d, byDomain.size() - 1)];
    const double lam =
        m.youngMPa * m.poisson / ((1 + m.poisson) * (1 - 2 * m.poisson));
    const double mu = m.youngMPa / (2 * (1 + m.poisson));
    const auto& tet = mesh.tets[ti];
    Vector3d X[10], U[10];
    for (int i = 0; i < 10; ++i) {
      const glm::dvec3& p = mesh.nodes[tet[i]];
      const glm::dvec3& u = out.displacement[tet[i]];
      X[i] = Vector3d(p.x, p.y, p.z);
      U[i] = Vector3d(u.x, u.y, u.z);
    }
    double vol = 0.0;
    for (const auto& g : kTetGauss) {
      Eigen::Matrix<double, 6, 30> B;
      vol += kTetW * std::abs(StrainDisp(X, g[0], g[1], g[2], B));
    }
    for (int k = 0; k < 10; ++k) {
      double dN[10][3];
      TetShapeGrad(kNodeRST[k][0], kNodeRST[k][1], kNodeRST[k][2], dN);
      Eigen::Matrix3d J = Eigen::Matrix3d::Zero();
      for (int i = 0; i < 10; ++i)
        J += X[i] * Vector3d(dN[i][0], dN[i][1], dN[i][2]).transpose();
      const Eigen::Matrix3d invJt = J.inverse().transpose();
      // F = I + du/dX, with dN/dX = J^-T dN/d(rst).
      Eigen::Matrix3d F = Eigen::Matrix3d::Identity();
      for (int i = 0; i < 10; ++i)
        F +=
            U[i] * (invJt * Vector3d(dN[i][0], dN[i][1], dN[i][2])).transpose();
      // The thermal strain at this node, alpha (T - T0) I.
      Eigen::Matrix3d eth = Eigen::Matrix3d::Zero();
      if (thermal && thermal->temperature.size() == nNodes)
        eth = m.expansionPerK *
              (thermal->temperature[tet[k]] - thermal->stressFreeC) *
              Eigen::Matrix3d::Identity();
      if (!large) {
        // The linear strain and Hooke's law, as SolveCases recovers them.
        const Eigen::Matrix3d G = F - Eigen::Matrix3d::Identity();
        const Eigen::Matrix3d eps = 0.5 * (G + G.transpose()) - eth;
        accum[tet[k]] +=
            vol *
            (lam * eps.trace() * Eigen::Matrix3d::Identity() + 2.0 * mu * eps);
        weight[tet[k]] += vol;
        continue;
      }
      const Eigen::Matrix3d E =
          0.5 * (F.transpose() * F - Eigen::Matrix3d::Identity()) - eth;
      const Eigen::Matrix3d S =
          lam * E.trace() * Eigen::Matrix3d::Identity() + 2.0 * mu * E;
      const double detF = F.determinant();
      if (!(std::abs(detF) > 0.0))
        continue;
      accum[tet[k]] += vol * (F * S * F.transpose()) / detF;
      weight[tet[k]] += vol;
    }
  }
  out.vonMises.assign(nNodes, 0.0);
  out.stress.assign(nNodes, Stress{});
  for (size_t n = 0; n < nNodes; ++n) {
    if (weight[n] > 0) {
      const Eigen::Matrix3d c = accum[n] / weight[n];
      Eigen::Matrix<double, 6, 1> v;
      v << c(0, 0), c(1, 1), c(2, 2), c(0, 1), c(1, 2), c(2, 0);
      out.vonMises[n] = VonMises(v);
      for (int k = 0; k < 6; ++k)
        out.stress[n][k] = v(k);
    }
    if (out.vonMises[n] > out.maxVonMises) {
      out.maxVonMises = out.vonMises[n];
      out.maxVmNode = static_cast<int>(n);
      out.maxVmAt = mesh.nodes[n];
    }
    const double dmag = glm::length(out.displacement[n]);
    if (dmag > out.maxDisplacement) {
      out.maxDisplacement = dmag;
      out.maxDispNode = static_cast<int>(n);
      out.maxDispAt = mesh.nodes[n];
    }
  }
  return out;
}

} // namespace

FemResults LargeDeformationFields(const FemMesh& mesh,
                                  const std::vector<Material>& byDomain,
                                  const std::vector<glm::dvec3>& displacement,
                                  const NodalTemperature* thermal) {
  return DeformationFields(mesh, byDomain, displacement, true, thermal);
}

FemResults
IntegrationPointFields(const FemMesh& mesh,
                       const std::vector<glm::dvec3>& displacement,
                       const std::vector<std::array<Stress, 4>>& ipStress,
                       const std::vector<std::array<double, 4>>& ipScalar) {
  FemResults out;
  const size_t nNodes = mesh.nodes.size();
  out.displacement = displacement;
  out.displacement.resize(nNodes, glm::dvec3(0.0));
  const bool scalar = !ipScalar.empty();
  std::vector<Eigen::Matrix<double, 7, 1>> accum(
      nNodes, Eigen::Matrix<double, 7, 1>::Zero());
  std::vector<double> weight(nNodes, 0.0);
  // The point nearest corner j has barycentric kGA there and kGB at the
  // others: a linear field c has the value kGB sum(c) + (kGA - kGB) c_j.
  constexpr int kMid[6][2] = {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {1, 3}, {2, 3}};
  for (size_t ti = 0; ti < mesh.tets.size() && ti < ipStress.size(); ++ti) {
    const auto& tet = mesh.tets[ti];
    Vector3d X[10];
    for (int i = 0; i < 10; ++i) {
      const glm::dvec3& p = mesh.nodes[tet[i]];
      X[i] = Vector3d(p.x, p.y, p.z);
    }
    double vol = 0.0;
    for (const auto& g : kTetGauss) {
      Eigen::Matrix<double, 6, 30> B;
      vol += kTetW * std::abs(StrainDisp(X, g[0], g[1], g[2], B));
    }
    Eigen::Matrix<double, 7, 1> ip[4],
        sum = Eigen::Matrix<double, 7, 1>::Zero();
    for (int k = 0; k < 4; ++k) {
      for (int c = 0; c < 6; ++c)
        ip[k](c) = ipStress[ti][k][c];
      ip[k](6) = scalar && ti < ipScalar.size() ? ipScalar[ti][k] : 0.0;
      sum += ip[k];
    }
    Eigen::Matrix<double, 7, 1> at[10];
    for (int j = 0; j < 4; ++j)
      at[j] = (ip[j] - kGB * sum) / (kGA - kGB);
    for (int m = 0; m < 6; ++m)
      at[4 + m] = 0.5 * (at[kMid[m][0]] + at[kMid[m][1]]);
    for (int i = 0; i < 10; ++i) {
      accum[tet[i]] += vol * at[i];
      weight[tet[i]] += vol;
    }
  }
  out.vonMises.assign(nNodes, 0.0);
  out.stress.assign(nNodes, Stress{});
  if (scalar)
    out.plasticStrain.assign(nNodes, 0.0);
  for (size_t n = 0; n < nNodes; ++n) {
    if (weight[n] > 0) {
      const Eigen::Matrix<double, 7, 1> v = accum[n] / weight[n];
      const Eigen::Matrix<double, 6, 1> sv = v.head<6>();
      out.vonMises[n] = VonMises(sv);
      for (int k = 0; k < 6; ++k)
        out.stress[n][k] = v(k);
      if (scalar)
        out.plasticStrain[n] = std::max(0.0, v(6));
    }
    if (out.vonMises[n] > out.maxVonMises) {
      out.maxVonMises = out.vonMises[n];
      out.maxVmNode = static_cast<int>(n);
      out.maxVmAt = mesh.nodes[n];
    }
    if (scalar && out.plasticStrain[n] > out.maxPlasticStrain) {
      out.maxPlasticStrain = out.plasticStrain[n];
      out.maxPeNode = static_cast<int>(n);
      out.maxPeAt = mesh.nodes[n];
    }
    const double dmag = glm::length(out.displacement[n]);
    if (dmag > out.maxDisplacement) {
      out.maxDisplacement = dmag;
      out.maxDispNode = static_cast<int>(n);
      out.maxDispAt = mesh.nodes[n];
    }
  }
  return out;
}

FemResults SmallDeformationFields(const FemMesh& mesh,
                                  const std::vector<Material>& byDomain,
                                  const std::vector<glm::dvec3>& displacement,
                                  const NodalTemperature* thermal) {
  return DeformationFields(mesh, byDomain, displacement, false, thermal);
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

std::optional<std::vector<FemResults::ModeShape>>
SolveBuckling(const FemMesh& mesh, const std::vector<Material>& byDomain,
              const Supports& supports,
              const std::vector<glm::dvec3>& baseDisplacement, int numFactors,
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
  for (const Material& m : byDomain)
    if (m.youngMPa <= 0)
      return fail("material has no stiffness data");
  if (baseDisplacement.size() != mesh.nodes.size())
    return fail("the base state is not one displacement per node");
  std::vector<Eigen::Matrix<double, 6, 6>> Ds;
  for (const Material& m : byDomain)
    Ds.push_back(Elasticity(m.youngMPa, m.poisson));
  numFactors = std::max(numFactors, 1);

  const Reduction red = MakeReduction(mesh, supports);
  const int nFree = red.dofs.nFree;
  if (nFree == 0)
    return fail("model is fully constrained (no free DOF)");
  if (nFree < numFactors + 2)
    return fail("mesh too small for the requested buckling factors");

  // K and the geometric stiffness K_G, LOWER triangles only (all the
  // Cholesky factor and SparseSymMatProd<Lower> read), each in a scoped
  // pass. K_G at each of the four Gauss points: the base state's stress
  // sigma = D B u_e, and for every pair of nodes grad N_a . sigma . grad N_b
  // dV on the identity block -- the rule CalculiX's C3D10 uses.
  Eigen::SparseMatrix<double> K(nFree, nFree), A(nFree, nFree);
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<Eigen::Triplet<double>> trip;
    trip.reserve(mesh.tets.size() * 465);
    for (size_t ti = 0; ti < mesh.tets.size(); ++ti) {
      const auto& tet = mesh.tets[ti];
      const size_t dom =
          mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[ti];
      const Eigen::Matrix<double, 6, 6>& D = Ds[std::min(dom, Ds.size() - 1)];
      Vector3d X[10];
      Eigen::Matrix<double, 30, 1> ue;
      for (int i = 0; i < 10; ++i) {
        const auto& p = mesh.nodes[tet[i]];
        X[i] = Vector3d(p.x, p.y, p.z);
        const auto& d = baseDisplacement[tet[i]];
        ue(3 * i + 0) = d.x;
        ue(3 * i + 1) = d.y;
        ue(3 * i + 2) = d.z;
      }
      Eigen::Matrix<double, 30, 30> Ke = Eigen::Matrix<double, 30, 30>::Zero();
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 6, 30> B;
        const double detJ = StrainDisp(X, g[0], g[1], g[2], B);
        if (std::abs(detJ) < 1e-12)
          return fail("mesh has a degenerate element");
        const double w = kTetW * std::abs(detJ);
        if (pass == 0) {
          Ke += w * (B.transpose() * D * B);
          continue;
        }
        const Eigen::Matrix<double, 6, 1> s = D * (B * ue);
        Eigen::Matrix3d sig;
        sig << s(0), s(3), s(5), s(3), s(1), s(4), s(5), s(4), s(2);
        // The physical gradients, read back off B's diagonal entries.
        Eigen::Matrix<double, 3, 10> G;
        for (int i = 0; i < 10; ++i)
          G.col(i) = Vector3d(B(0, 3 * i), B(1, 3 * i + 1), B(2, 3 * i + 2));
        const Eigen::Matrix<double, 10, 10> S = G.transpose() * sig * G;
        // -K_G: the matrix whose largest eigenvalues are 1 / the smallest
        // positive factors.
        for (int a = 0; a < 10; ++a)
          for (int b = 0; b < 10; ++b)
            for (int k = 0; k < 3; ++k)
              Ke(3 * a + k, 3 * b + k) -= w * S(a, b);
      }
      red.Scatter(tet, Ke, trip, true);
    }
    (pass == 0 ? K : A).setFromTriplets(trip.begin(), trip.end());
  }
  K.makeCompressed();
  A.makeCompressed();

  // -K_G phi = mu K phi: K is the B matrix, positive definite once held,
  // and -K_G may be indefinite, which the Cholesky mode takes as it is.
  // lambda = 1 / mu; the largest |mu| are the factors nearest zero, of
  // either sign.
  using OpType = Spectra::SparseSymMatProd<double>;
  using BOpType = Spectra::SparseCholesky<double>;
  BOpType Bop(K);
  if (Bop.info() != Spectra::CompInfo::Successful)
    return fail(
        "model is insufficiently constrained (singular stiffness matrix)");
  OpType op(A);
  const auto solve = [&](Spectra::SortRule rule, int nev, Eigen::VectorXd& mu,
                         Eigen::MatrixXd& phi, std::string* why) {
    nev = std::min(nev, nFree - 2);
    const int ncv = std::min(std::max(2 * nev + 1, 20), nFree);
    Spectra::SymGEigsSolver<OpType, BOpType, Spectra::GEigsMode::Cholesky>
        geigs(op, Bop, nev, ncv);
    try {
      geigs.init();
      geigs.compute(rule, 1000, 1e-10, rule);
    } catch (const std::exception& e) {
      *why = std::string("the buckling eigensolve failed: ") + e.what();
      return false;
    }
    if (geigs.info() != Spectra::CompInfo::Successful) {
      *why = "the buckling eigensolve did not converge";
      return false;
    }
    mu = geigs.eigenvalues();
    phi = geigs.eigenvectors();
    return true;
  };
  Eigen::VectorXd mu;
  Eigen::MatrixXd phi;
  std::string why;
  if (!solve(Spectra::SortRule::LargestMagn, numFactors, mu, phi, &why))
    return fail(why);
  // Rounding leaves a mu of a few parts in 1e10 of the largest where there
  // is none (pure tension): below this it is no factor at all.
  double scale = 0.0;
  for (int m = 0; m < mu.size(); ++m)
    scale = std::max(scale, std::abs(mu(m)));
  if (!(scale > 0.0))
    return std::vector<FemResults::ModeShape>{}; // no stress, nothing buckles
  const double tiny = 1e-8 * scale;
  bool positive = false;
  for (int m = 0; m < mu.size(); ++m)
    positive = positive || mu(m) > tiny;
  // Every factor nearest zero is negative (the load mostly pulls): the
  // smallest positive one, if the load has one, is looked for on its own.
  std::vector<std::pair<double, Eigen::VectorXd>> found;
  for (int m = 0; m < mu.size(); ++m)
    if (std::abs(mu(m)) > tiny)
      found.push_back({mu(m), phi.col(m)});
  if (!positive) {
    Eigen::VectorXd mu2;
    Eigen::MatrixXd phi2;
    if (solve(Spectra::SortRule::LargestAlge, 1, mu2, phi2, &why) &&
        mu2.size() > 0 && mu2(0) > tiny)
      found.push_back({mu2(0), phi2.col(0)});
  }
  // Ascending by |factor|, i.e. descending by |mu|.
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
    return std::abs(a.first) > std::abs(b.first);
  });
  std::vector<FemResults::ModeShape> modes;
  for (const auto& [m, v] : found) {
    FemResults::ModeShape mode;
    mode.factor = 1.0 / m;
    mode.displacement.resize(nNodes);
    double maxMag = 0.0;
    for (int n = 0; n < nNodes; ++n) {
      mode.displacement[n] = red.NodeDisplacement(v, n);
      maxMag = std::max(maxMag, glm::length(mode.displacement[n]));
    }
    if (maxMag > 0)
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
std::optional<std::vector<FemResults::ModeShape>>
SolveBuckling(const FemMesh&, const std::vector<Material>&, const Supports&,
              const std::vector<glm::dvec3>&, int, std::string* error) {
  if (error)
    *error = "built without buckling support (Spectra not found)";
  return std::nullopt;
}
#endif // HAVE_SPECTRA

#else // !HAVE_FEM_SOLVER

bool SolverAvailable() { return false; }
bool ModalAvailable() { return false; }

FemResults SmallDeformationFields(const FemMesh&, const std::vector<Material>&,
                                  const std::vector<glm::dvec3>& displacement,
                                  const NodalTemperature*) {
  FemResults out;
  out.displacement = displacement;
  return out;
}
FemResults IntegrationPointFields(const FemMesh&,
                                  const std::vector<glm::dvec3>& displacement,
                                  const std::vector<std::array<Stress, 4>>&,
                                  const std::vector<std::array<double, 4>>&) {
  FemResults out;
  out.displacement = displacement;
  return out;
}
FemResults LargeDeformationFields(const FemMesh&, const std::vector<Material>&,
                                  const std::vector<glm::dvec3>& displacement,
                                  const NodalTemperature*) {
  FemResults out;
  out.displacement = displacement;
  return out;
}

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

std::optional<std::vector<FemResults::ModeShape>>
SolveBuckling(const FemMesh&, const std::vector<Material>&, const Supports&,
              const std::vector<glm::dvec3>&, int, std::string* error) {
  if (error)
    *error = "built without buckling support (Eigen not found)";
  return std::nullopt;
}

#endif // HAVE_FEM_SOLVER

std::optional<FaceMotion>
FitFaceMotion(const FemMesh& mesh, const std::vector<glm::dvec3>& displacement,
              int faceIndex) {
  return FitFaceMotion(mesh, displacement, std::vector<int>{faceIndex});
}

std::optional<FaceMotion>
FitFaceMotion(const FemMesh& mesh, const std::vector<glm::dvec3>& displacement,
              const std::vector<int>& faceIndices) {
  if (displacement.size() != mesh.nodes.size())
    return std::nullopt;
  const auto on = [&](int f) {
    return std::find(faceIndices.begin(), faceIndices.end(), f) !=
           faceIndices.end();
  };
  // Each node stands for a sixth of every triangle it is a node of, so a
  // finely meshed corner of the face does not outvote the rest of it.
  std::vector<double> weight(mesh.nodes.size(), 0.0);
  double area = 0.0;
  for (const FemMesh::BoundaryTri& t : mesh.boundary) {
    if (!on(t.faceIndex))
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
  return FaceStressOf(mesh, vonMises, std::vector<int>{faceIndex});
}

std::optional<FaceStress> FaceStressOf(const FemMesh& mesh,
                                       const std::vector<double>& vonMises,
                                       const std::vector<int>& faceIndices) {
  if (vonMises.size() != mesh.nodes.size())
    return std::nullopt;
  const auto on = [&](int f) {
    return std::find(faceIndices.begin(), faceIndices.end(), f) !=
           faceIndices.end();
  };
  FaceStress out;
  double weighted = 0.0;
  std::vector<char> seen(mesh.nodes.size(), 0);
  bool any = false;
  for (const FemMesh::BoundaryTri& t : mesh.boundary) {
    if (!on(t.faceIndex))
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

namespace {

// The eigenvectors of a symmetric 3x3 matrix (columns of `v`), by Jacobi
// rotations: enough for a facet cloud's principal axes.
void SymmetricEigen3(double a[3][3], double v[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      v[i][j] = i == j ? 1.0 : 0.0;
  for (int sweep = 0; sweep < 50; ++sweep) {
    const double off =
        std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
    if (off < 1e-30)
      return;
    for (int p = 0; p < 2; ++p)
      for (int q = p + 1; q < 3; ++q) {
        if (std::abs(a[p][q]) < 1e-300)
          continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0) /
                         (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        for (int k = 0; k < 3; ++k) {
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < 3; ++k) {
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (int k = 0; k < 3; ++k) {
          const double vkp = v[k][p], vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
  }
}

// A support's rim as segments: each tri6 edge it shares with a boundary
// facet it does not hold, in two halves through the edge's midside node.
std::vector<std::pair<glm::dvec3, glm::dvec3>>
SupportRim(const FemMesh& mesh, const std::vector<int>& tris) {
  std::vector<char> on(mesh.boundary.size(), 0);
  for (int t : tris)
    if (t >= 0 && static_cast<size_t>(t) < on.size())
      on[t] = 1;
  // A facet edge by its corner pair: whether a held and a free facet use it.
  std::map<std::pair<int, int>, std::pair<int, int>> uses; // -> held, mid
  std::map<std::pair<int, int>, bool> free;
  static const int kEdge[3][3] = {{0, 1, 3}, {1, 2, 4}, {2, 0, 5}};
  for (size_t t = 0; t < mesh.boundary.size(); ++t) {
    const auto& n = mesh.boundary[t].nodes;
    for (const auto& e : kEdge) {
      const std::pair<int, int> key{std::min(n[e[0]], n[e[1]]),
                                    std::max(n[e[0]], n[e[1]])};
      if (on[t]) {
        auto& u = uses[key];
        u.first += 1;
        u.second = n[e[2]];
      } else {
        free[key] = true;
      }
    }
  }
  std::vector<std::pair<glm::dvec3, glm::dvec3>> out;
  for (const auto& [key, u] : uses)
    if (free.count(key)) {
      const glm::dvec3 mid = mesh.nodes[u.second];
      out.emplace_back(mesh.nodes[key.first], mid);
      out.emplace_back(mid, mesh.nodes[key.second]);
    }
  return out;
}

double SegmentDistance(const glm::dvec3& p, const glm::dvec3& a,
                       const glm::dvec3& b) {
  const glm::dvec3 ab = b - a;
  const double len2 = glm::dot(ab, ab);
  const double t =
      len2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
  return glm::length(p - (a + t * ab));
}

} // namespace

double SupportAwayMm(const FemMesh& mesh, const std::vector<int>& tris,
                     LoadType type, const glm::dvec3& axisOrigin,
                     const glm::dvec3& axisDirection) {
  std::vector<glm::dvec3> pts;
  for (int t : tris)
    if (t >= 0 && static_cast<size_t>(t) < mesh.boundary.size())
      for (int k = 0; k < 3; ++k)
        pts.push_back(mesh.nodes[mesh.boundary[t].nodes[k]]);
  if (pts.empty())
    return 0.0;
  if (type == LoadType::Pin) {
    // A quarter of the bore's radius: the facets' mean distance from the
    // axis.
    const double len = glm::length(axisDirection);
    if (len <= 0.0)
      return 0.0;
    const glm::dvec3 d = axisDirection / len;
    double r = 0.0;
    for (const glm::dvec3& p : pts) {
      const glm::dvec3 v = p - axisOrigin;
      r += glm::length(v - glm::dot(v, d) * d);
    }
    return 0.25 * r / static_cast<double>(pts.size());
  }
  // A fixed face: a quarter of its facets' extent along their principal
  // axes, the largest of the three. A clamp's singular zone is the size of
  // the section it holds, not of its thinnest side, and the bending stress
  // a clamped member carries changes along it over that length too.
  glm::dvec3 c(0.0);
  for (const glm::dvec3& p : pts)
    c += p;
  c /= static_cast<double>(pts.size());
  double cov[3][3] = {};
  for (const glm::dvec3& p : pts) {
    const glm::dvec3 q = p - c;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        cov[i][j] += q[i] * q[j];
  }
  double axes[3][3];
  SymmetricEigen3(cov, axes);
  double largest = 0.0;
  for (int k = 0; k < 3; ++k) {
    const glm::dvec3 a(axes[0][k], axes[1][k], axes[2][k]);
    double lo = 1e300, hi = -1e300;
    for (const glm::dvec3& p : pts) {
      const double s = glm::dot(p, a);
      lo = std::min(lo, s);
      hi = std::max(hi, s);
    }
    largest = std::max(largest, hi - lo);
  }
  return 0.25 * largest;
}

std::vector<int> SupportEdgeZones(const FemMesh& mesh,
                                  const std::vector<SupportFacets>& supports) {
  const size_t nNodes = mesh.nodes.size();
  std::vector<int> zone(nNodes, -1);
  if (supports.empty())
    return {};
  bool anyRim = false;
  for (size_t si = 0; si < supports.size(); ++si) {
    const auto rim = SupportRim(mesh, supports[si].tris);
    if (rim.empty())
      continue;
    anyRim = true;
    // At least one element: every node of a tet that has a rim node.
    std::vector<char> edge(nNodes, 0);
    {
      std::vector<char> on(mesh.boundary.size(), 0);
      for (int t : supports[si].tris)
        if (t >= 0 && static_cast<size_t>(t) < on.size())
          on[t] = 1;
      std::vector<char> held(nNodes, 0);
      for (size_t t = 0; t < mesh.boundary.size(); ++t)
        if (on[t])
          for (int v : mesh.boundary[t].nodes)
            held[v] = 1;
      for (size_t t = 0; t < mesh.boundary.size(); ++t)
        if (!on[t])
          for (int v : mesh.boundary[t].nodes)
            if (held[v])
              edge[v] = 1;
    }
    for (const auto& tet : mesh.tets) {
      bool touches = false;
      for (int v : tet)
        touches = touches || edge[v];
      if (touches)
        for (int v : tet)
          if (zone[v] < 0)
            zone[v] = static_cast<int>(si);
    }
    // Within the support's reach of its rim, in millimetres: a grid of
    // cells that size holds the rim's segments, so a node looks in its own
    // cell and the 26 round it.
    const double reach = supports[si].awayMm;
    if (reach <= 0.0)
      continue;
    const double cell = reach;
    const auto key = [&](const glm::dvec3& p) {
      return std::array<long long, 3>{
          static_cast<long long>(std::floor(p.x / cell)),
          static_cast<long long>(std::floor(p.y / cell)),
          static_cast<long long>(std::floor(p.z / cell))};
    };
    std::map<std::array<long long, 3>, std::vector<int>> grid;
    for (size_t s = 0; s < rim.size(); ++s) {
      const auto a = key(glm::min(rim[s].first, rim[s].second));
      const auto b = key(glm::max(rim[s].first, rim[s].second));
      for (long long x = a[0]; x <= b[0]; ++x)
        for (long long y = a[1]; y <= b[1]; ++y)
          for (long long z = a[2]; z <= b[2]; ++z)
            grid[{x, y, z}].push_back(static_cast<int>(s));
    }
    for (size_t n = 0; n < nNodes; ++n) {
      if (zone[n] >= 0)
        continue;
      const auto k = key(mesh.nodes[n]);
      bool inReach = false;
      for (long long dx = -1; dx <= 1 && !inReach; ++dx)
        for (long long dy = -1; dy <= 1 && !inReach; ++dy)
          for (long long dz = -1; dz <= 1 && !inReach; ++dz) {
            const auto it = grid.find({k[0] + dx, k[1] + dy, k[2] + dz});
            if (it == grid.end())
              continue;
            for (int s : it->second)
              if (SegmentDistance(mesh.nodes[n], rim[s].first, rim[s].second) <=
                  reach) {
                inReach = true;
                break;
              }
          }
      if (inReach)
        zone[n] = static_cast<int>(si);
    }
  }
  if (!anyRim)
    return {};
  return zone;
}

FemResults::SupportEdgePeak
PeakAwayFromEdges(const FemMesh& mesh, const std::vector<double>& field,
                  int peakNode, const std::vector<int>& zones,
                  const std::vector<FemResults::SupportReaction>& supports) {
  FemResults::SupportEdgePeak out;
  const size_t n = mesh.nodes.size();
  if (zones.size() != n || field.size() != n || peakNode < 0 ||
      static_cast<size_t>(peakNode) >= n || zones[peakNode] < 0 ||
      static_cast<size_t>(zones[peakNode]) >= supports.size())
    return out;
  out.flagged = true;
  out.support = supports[zones[peakNode]].support;
  out.type = supports[zones[peakNode]].type;
  for (size_t i = 0; i < n; ++i)
    if (zones[i] < 0 && field[i] > out.awayMPa) {
      out.awayMPa = field[i];
      out.awayAt = mesh.nodes[i];
    }
  return out;
}

FemResults::SupportEdgePeak
FindSupportEdgePeak(const FemMesh& mesh, const FemResults& results,
                    const std::vector<SupportFacets>& supports) {
  std::vector<FemResults::SupportReaction> ids;
  for (const SupportFacets& sf : supports) {
    FemResults::SupportReaction r;
    r.support = sf.id;
    r.type = sf.type;
    ids.push_back(r);
  }
  return PeakAwayFromEdges(mesh, results.vonMises, results.maxVmNode,
                           SupportEdgeZones(mesh, supports), ids);
}

// Peak resident memory of a buckling run (mesh, base state, K, K_G, the
// Cholesky factor and the Lanczos basis), measured over the process's start
// (getrusage ru_maxrss) on the 10 x 10 x 200 steel column meshed by our
// pipeline at 5 to 1.35 mm, three factors: 5.7k DOF 45 MB, 16k 109 MB, 31k
// 186 MB, 66k 423 MB, 143k 1.17 GB, 205k 2.01 GB
// (test_fem_buckling.cpp's hidden "[buckling-calibrate]" case prints them).
// The factor fills in faster than the DOF count grows -- DOF^1.45 through the
// largest runs -- plus what is held before the matrix, and a tenth more,
// bounds every measured run from above (by 1.1x at 16k-205k, 1.35x at 5.7k).
uint64_t BucklingMemoryEstimate(size_t freeDofs) {
  constexpr double kBaseBytes = 60e6;
  constexpr double kCoefficient = 39.0; // bytes per DOF^1.45
  constexpr double kExponent = 1.45;
  constexpr double kMargin = 1.1;
  return static_cast<uint64_t>(
      kMargin *
      (kBaseBytes +
       kCoefficient * std::pow(static_cast<double>(freeDofs), kExponent)));
}

} // namespace fem
