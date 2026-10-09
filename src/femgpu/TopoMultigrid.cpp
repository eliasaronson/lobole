// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "femgpu/TopoMultigrid.h"

#include "femgpu/TopoCgSolver.h" // MgGpuData

#include <Eigen/SparseCholesky>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace gpu {

namespace {

// The element matrix the GPU SpMV actually applies (fem_spmv.comp): the fp32
// Ke taken relative to the element's first node, S^T Ke' S with Ke' = Ke
// without node 0's rows and columns and S = I - (node 0's translation). In
// exact arithmetic it IS Ke; from fp32 entries it keeps the translation null
// space exact, which the plain rounded Ke does not — and the coarse space is
// made of exactly those rigid motions.
Eigen::Matrix<double, 30, 30> RelativeKe(const float* ke) {
  Eigen::Matrix<double, 30, 30> K = Eigen::Matrix<double, 30, 30>::Zero();
  for (int a = 3; a < 30; ++a)
    for (int b = 3; b < 30; ++b)
      K(a, b) = static_cast<double>(ke[a * 30 + b]);
  Eigen::Matrix<double, 30, 30> S = Eigen::Matrix<double, 30, 30>::Identity();
  for (int a = 0; a < 30; ++a)
    S(a, a % 3) -= 1.0;
  return S.transpose() * K * S;
}

// Assemble the fp64 sparse reduced stiffness from the packed fp32 element
// data (the same numbers the GPU consumes — oracle discipline from GPU0/1).
Eigen::SparseMatrix<double> AssembleA(const SpmvSystem& sys,
                                      const std::vector<float>& scales) {
  std::vector<Eigen::Triplet<double>> trip;
  trip.reserve(static_cast<size_t>(sys.nElems) * 400);
  for (int e = 0; e < sys.nElems; ++e) {
    const int32_t* dof = &sys.dof[static_cast<size_t>(e) * 30];
    const Eigen::Matrix<double, 30, 30> Ke =
        RelativeKe(&sys.ke[static_cast<size_t>(e) * 900]);
    const double s = scales[e];
    for (int a = 0; a < 30; ++a) {
      if (dof[a] < 0)
        continue;
      for (int b = 0; b < 30; ++b)
        if (dof[b] >= 0)
          trip.emplace_back(dof[a], dof[b], s * Ke(a, b));
    }
  }
  Eigen::SparseMatrix<double> A(sys.nFree, sys.nFree);
  A.setFromTriplets(trip.begin(), trip.end());
  return A;
}

// lambda_max(D^-1 A) by power iteration (fp64) — aims the Chebyshev smoother.
double PowerLambdaMax(const Eigen::SparseMatrix<double>& A,
                      const Eigen::VectorXd& diag) {
  // A pseudo-random start, as on the GPU (TopoCgSolver::EstimateLambdaMax):
  // ones is nearly orthogonal to the top eigenvector.
  Eigen::VectorXd v(A.rows());
  uint32_t state = 0x2545f491u;
  for (Eigen::Index i = 0; i < v.size(); ++i) {
    state = state * 1664525u + 1013904223u;
    v[i] = static_cast<double>(state >> 8) / double(1u << 23) - 1.0;
  }
  double lambda = 1.0;
  for (int it = 0; it < 20; ++it) {
    Eigen::VectorXd w = diag.cwiseInverse().cwiseProduct(A * v);
    const double vw = v.dot(w), vv = v.squaredNorm();
    if (vv <= 0.0)
      break;
    lambda = vw / vv;
    const double n = w.norm();
    if (n <= 0.0)
      break;
    v = w / n;
  }
  return 1.1 * lambda; // Chebyshev must bound the spectrum from above
}

// Chebyshev correction z += q(D^-1 A)(r - A z) over [lmax/ratio, lmax]
// (Saad Alg. 12.1) — the smoother. Polynomial in D^-1 A composed with D^-1,
// self-adjoint, so the equal pre/post V-cycle stays a valid symmetric CG
// preconditioner.
void ChebSmooth(const Eigen::SparseMatrix<double>& A,
                const Eigen::VectorXd& invDiag, double lmax, int degree,
                const Eigen::VectorXd& r, Eigen::VectorXd& z) {
  constexpr double kSmoothRatio = 15.0; // smooth the top of the spectrum
  const double lmin = lmax / kSmoothRatio;
  const double theta = 0.5 * (lmax + lmin), delta = 0.5 * (lmax - lmin);
  const double sigma1 = theta / delta;

  Eigen::VectorXd rc = invDiag.cwiseProduct(r - A * z); // b - A' z in D^-1 A
  Eigen::VectorXd d = rc / theta;
  z += d;
  double rhoPrev = 1.0 / sigma1;
  for (int k = 1; k < degree; ++k) {
    const double rho = 1.0 / (2.0 * sigma1 - rhoPrev);
    rc -= invDiag.cwiseProduct(A * d);
    d = (rho * rhoPrev) * d + (2.0 * rho / delta) * rc;
    z += d;
    rhoPrev = rho;
  }
}

} // namespace

bool BuildMgSetup(const std::vector<double>& nodes, const SpmvSystem& sys,
                  const std::vector<int>& dofMap, MgSetup& out,
                  std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  const int nNodes = static_cast<int>(nodes.size() / 3);
  if (nNodes <= 0 || sys.nElems <= 0)
    return fail("empty multigrid input");
  if (static_cast<int>(dofMap.size()) != 3 * nNodes)
    return fail("dofMap size mismatch");

  // ---- Node adjacency (clique per tet, corner+midside) --------------------
  // Derive each element's 10 nodes back from its DOF slots: slot 3i is node
  // i's x-DOF; fixed slots (-1) don't identify the node, so recover node ids
  // from the dof map by inverting where possible and fall back to scanning
  // the element's free slots. Simpler and fully robust: the caller packed
  // dof[30] from tet nodes in order, so reconstruct via dofMap inversion.
  std::vector<int> nodeOfFreeDof(sys.nFree, -1);
  for (int n = 0; n < nNodes; ++n)
    for (int k = 0; k < 3; ++k)
      if (dofMap[3 * n + k] >= 0)
        nodeOfFreeDof[dofMap[3 * n + k]] = n;

  std::vector<std::vector<int>> elemNodes(sys.nElems);
  for (int e = 0; e < sys.nElems; ++e) {
    std::vector<int>& en = elemNodes[e];
    for (int slot = 0; slot < 30; ++slot) {
      const int32_t g = sys.dof[static_cast<size_t>(e) * 30 + slot];
      if (g >= 0) {
        const int n = nodeOfFreeDof[g];
        if (en.empty() || en.back() != n)
          en.push_back(n);
      }
    }
    std::sort(en.begin(), en.end());
    en.erase(std::unique(en.begin(), en.end()), en.end());
  }
  std::vector<std::vector<int>> adj(nNodes);
  for (const auto& en : elemNodes)
    for (size_t a = 0; a < en.size(); ++a)
      for (size_t b = a + 1; b < en.size(); ++b) {
        adj[en[a]].push_back(en[b]);
        adj[en[b]].push_back(en[a]);
      }
  for (auto& v : adj) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  }

  // ---- Two-phase aggregation (standard smoothed-aggregation shape) --------
  // Phase 1: a seed only where the whole clique neighborhood is unclaimed —
  // an independent set of full-size aggregates. Phase 2: every leftover node
  // joins the aggregate most represented among its neighbors (no fragments).
  // Greedy single-pass seeding fragments badly (later seeds find their
  // neighborhoods claimed and become 1-2 node aggregates), which left the
  // coarse space nearly half the fine one.
  out.aggOfNode.assign(nNodes, -1);
  int nAgg = 0;
  for (int seed = 0; seed < nNodes; ++seed) {
    if (out.aggOfNode[seed] >= 0)
      continue;
    bool clean = true;
    for (int nb : adj[seed])
      if (out.aggOfNode[nb] >= 0) {
        clean = false;
        break;
      }
    if (!clean)
      continue;
    const int a = nAgg++;
    out.aggOfNode[seed] = a;
    for (int nb : adj[seed])
      out.aggOfNode[nb] = a;
  }
  for (int n = 0; n < nNodes; ++n) {
    if (out.aggOfNode[n] >= 0)
      continue;
    // Most-connected neighboring aggregate.
    int best = -1, bestCount = 0;
    for (int nb : adj[n]) {
      const int a = out.aggOfNode[nb];
      if (a < 0)
        continue;
      int count = 0;
      for (int nb2 : adj[n])
        if (out.aggOfNode[nb2] == a)
          ++count;
      if (count > bestCount) {
        bestCount = count;
        best = a;
      }
    }
    if (best >= 0)
      out.aggOfNode[n] = best;
    else
      out.aggOfNode[n] = nAgg++; // truly isolated
  }
  out.nAgg = nAgg;
  out.nCoarse = 6 * nAgg;
  out.nFree = sys.nFree;

  // ---- Rigid-body prolongation over the reduced system --------------------
  std::vector<Eigen::Vector3d> centroid(nAgg, Eigen::Vector3d::Zero());
  std::vector<int> aggCount(nAgg, 0);
  for (int n = 0; n < nNodes; ++n) {
    const int a = out.aggOfNode[n];
    centroid[a] +=
        Eigen::Vector3d(nodes[3 * n], nodes[3 * n + 1], nodes[3 * n + 2]);
    aggCount[a]++;
  }
  for (int a = 0; a < nAgg; ++a)
    if (aggCount[a] > 0)
      centroid[a] /= aggCount[a];
  out.aggCentroid.resize(3 * nAgg);
  for (int a = 0; a < nAgg; ++a) {
    out.aggCentroid[3 * a + 0] = centroid[a].x();
    out.aggCentroid[3 * a + 1] = centroid[a].y();
    out.aggCentroid[3 * a + 2] = centroid[a].z();
  }

  std::vector<Eigen::Triplet<double>> pt;
  pt.reserve(static_cast<size_t>(sys.nFree) * 4);
  for (int n = 0; n < nNodes; ++n) {
    const int a = out.aggOfNode[n];
    const Eigen::Vector3d d =
        Eigen::Vector3d(nodes[3 * n], nodes[3 * n + 1], nodes[3 * n + 2]) -
        centroid[a];
    const int gx = dofMap[3 * n + 0], gy = dofMap[3 * n + 1],
              gz = dofMap[3 * n + 2];
    const int c = 6 * a;
    // u = t + omega x d
    if (gx >= 0) {
      pt.emplace_back(gx, c + 0, 1.0);
      pt.emplace_back(gx, c + 4, d.z());
      pt.emplace_back(gx, c + 5, -d.y());
    }
    if (gy >= 0) {
      pt.emplace_back(gy, c + 1, 1.0);
      pt.emplace_back(gy, c + 3, -d.z());
      pt.emplace_back(gy, c + 5, d.x());
    }
    if (gz >= 0) {
      pt.emplace_back(gz, c + 2, 1.0);
      pt.emplace_back(gz, c + 3, d.y());
      pt.emplace_back(gz, c + 4, -d.x());
    }
  }
  out.P.resize(sys.nFree, out.nCoarse);
  out.P.setFromTriplets(pt.begin(), pt.end());

  // ---- Per-element Galerkin blocks P^T Ke0 P ------------------------------
  // An element touches only the aggregates of its own nodes, so each block
  // is a small dense matrix scattered into (aggregate DOF) x (aggregate DOF)
  // coordinates. Stored flat; per-SIMP-iteration reassembly is then a single
  // weighted pass (no Eigen products).
  out.blockRow.clear();
  out.blockCol.clear();
  out.blockVal.clear();
  out.elemBlockOffset.assign(sys.nElems + 1, 0);
  for (int e = 0; e < sys.nElems; ++e) {
    const int32_t* dof = &sys.dof[static_cast<size_t>(e) * 30];
    // Local coarse DOF set: aggregates of this element's nodes.
    std::vector<int> coarse; // global coarse dof ids
    coarse.reserve(24);
    for (int n : elemNodes[e])
      for (int k = 0; k < 6; ++k) {
        const int cd = 6 * out.aggOfNode[n] + k;
        if (std::find(coarse.begin(), coarse.end(), cd) == coarse.end())
          coarse.push_back(cd);
      }
    std::sort(coarse.begin(), coarse.end());
    const int nc = static_cast<int>(coarse.size());

    // Local P: rows = the element's 30 slots, cols = local coarse dofs.
    Eigen::MatrixXd Pl = Eigen::MatrixXd::Zero(30, nc);
    for (int slot = 0; slot < 30; ++slot) {
      const int32_t g = dof[slot];
      if (g < 0)
        continue;
      // Row g of P has <=6 entries, all within this element's coarse set.
      for (int c = 0; c < nc; ++c)
        Pl(slot, c) = out.P.coeff(g, coarse[c]);
    }
    Eigen::MatrixXd Ke = RelativeKe(&sys.ke[static_cast<size_t>(e) * 900]);
    // Zero the fixed slots (they are absent from the reduced system).
    for (int slot = 0; slot < 30; ++slot)
      if (dof[slot] < 0) {
        Ke.row(slot).setZero();
        Ke.col(slot).setZero();
      }
    const Eigen::MatrixXd B = Pl.transpose() * Ke * Pl; // nc x nc
    for (int r = 0; r < nc; ++r)
      for (int c = 0; c < nc; ++c)
        if (B(r, c) != 0.0) {
          out.blockRow.push_back(coarse[r]);
          out.blockCol.push_back(coarse[c]);
          out.blockVal.push_back(B(r, c));
        }
    out.elemBlockOffset[e + 1] = static_cast<int>(out.blockRow.size());
  }
  return true;
}

bool MgCoarseSolver::UpdateScales(const std::vector<float>& scales,
                                  std::string* error) {
  std::vector<Eigen::Triplet<double>> ct;
  ct.reserve(m_setup.blockVal.size());
  const int nElems = static_cast<int>(m_setup.elemBlockOffset.size()) - 1;
  if (static_cast<int>(scales.size()) != nElems) {
    if (error)
      *error = "coarse update: scale count mismatch";
    return false;
  }
  for (int e = 0; e < nElems; ++e) {
    const double s = scales[e];
    for (int i = m_setup.elemBlockOffset[e]; i < m_setup.elemBlockOffset[e + 1];
         ++i)
      ct.emplace_back(m_setup.blockRow[i], m_setup.blockCol[i],
                      s * m_setup.blockVal[i]);
  }
  Eigen::SparseMatrix<double> Ac(m_setup.nCoarse, m_setup.nCoarse);
  Ac.setFromTriplets(ct.begin(), ct.end());
  double tr = 0.0;
  for (int i = 0; i < m_setup.nCoarse; ++i)
    tr += Ac.coeff(i, i);
  const double eps = 1e-10 * (tr / std::max(1, m_setup.nCoarse));
  Eigen::SparseMatrix<double> I(m_setup.nCoarse, m_setup.nCoarse);
  I.setIdentity();
  Ac += eps * I;
  if (!m_analyzed) {
    m_ldlt.analyzePattern(Ac);
    m_analyzed = true;
  }
  m_ldlt.factorize(Ac);
  if (m_ldlt.info() != Eigen::Success) {
    if (error)
      *error = "coarse factorization failed";
    m_ready = false;
    return false;
  }
  m_ready = true;
  return true;
}

bool MgCoarseSolver::Solve(const float* rc, float* ec,
                           std::string* error) const {
  if (!m_ready) {
    if (error)
      *error = "coarse solver not factorized (UpdateScales first)";
    return false;
  }
  Eigen::VectorXd r(m_setup.nCoarse);
  for (int i = 0; i < m_setup.nCoarse; ++i)
    r[i] = rc[i];
  const Eigen::VectorXd e = m_ldlt.solve(r);
  for (int i = 0; i < m_setup.nCoarse; ++i)
    ec[i] = static_cast<float>(e[i]);
  return true;
}

std::shared_ptr<MgGpuData> MakeMgGpuData(const std::vector<double>& nodes,
                                         const SpmvSystem& sys,
                                         const std::vector<int>& dofMap,
                                         std::string* error) {
  MgSetup setup;
  if (!BuildMgSetup(nodes, sys, dofMap, setup, error))
    return nullptr;
  const int nNodes = static_cast<int>(nodes.size() / 3);
  auto mg = std::make_shared<MgGpuData>();
  mg->nNodes = nNodes;
  mg->nCoarse = setup.nCoarse;
  mg->nodeAgg.assign(setup.aggOfNode.begin(), setup.aggOfNode.end());
  mg->nodeOffset.resize(static_cast<size_t>(nNodes) * 3);
  mg->nodeDof.resize(static_cast<size_t>(nNodes) * 3);
  for (int n = 0; n < nNodes; ++n) {
    const int a = setup.aggOfNode[n];
    for (int k = 0; k < 3; ++k) {
      mg->nodeOffset[3 * n + k] =
          a >= 0 ? static_cast<float>(nodes[3 * n + k] -
                                      setup.aggCentroid[3 * a + k])
                 : 0.0f;
      mg->nodeDof[3 * n + k] = dofMap[3 * n + k];
    }
  }
  auto coarse = std::make_shared<MgCoarseSolver>(std::move(setup));
  mg->updateScales = [coarse](const std::vector<float>& s, std::string* e) {
    return coarse->UpdateScales(s, e);
  };
  mg->coarseSolve = [coarse](const float* rc, float* ec, std::string* e) {
    return coarse->Solve(rc, ec, e);
  };
  return mg;
}

bool MgReferencePcg(const SpmvSystem& sys, const std::vector<float>& scales,
                    const MgSetup& mg, const Eigen::VectorXd& f,
                    Eigen::VectorXd& x, double tol, int maxIter,
                    int smootherDegree, MgRefStats* stats, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (static_cast<int>(scales.size()) != sys.nElems)
    return fail("scale count mismatch");

  const Eigen::SparseMatrix<double> A = AssembleA(sys, scales);
  Eigen::VectorXd diag = A.diagonal();
  for (int i = 0; i < diag.size(); ++i)
    if (diag[i] <= 0.0)
      return fail("non-positive diagonal (singular system)");
  const Eigen::VectorXd invDiag = diag.cwiseInverse();
  const double lmax = PowerLambdaMax(A, diag);

  // Coarse operator from the precomputed blocks + Tikhonov regularization
  // (partially-fixed aggregates leave semidefinite rigid modes).
  std::vector<Eigen::Triplet<double>> ct;
  ct.reserve(mg.blockVal.size());
  for (int e = 0; e < sys.nElems; ++e) {
    const double s = scales[e];
    for (int i = mg.elemBlockOffset[e]; i < mg.elemBlockOffset[e + 1]; ++i)
      ct.emplace_back(mg.blockRow[i], mg.blockCol[i], s * mg.blockVal[i]);
  }
  Eigen::SparseMatrix<double> Ac(mg.nCoarse, mg.nCoarse);
  Ac.setFromTriplets(ct.begin(), ct.end());
  double tr = 0.0;
  for (int i = 0; i < mg.nCoarse; ++i)
    tr += Ac.coeff(i, i);
  const double eps = 1e-10 * (tr / std::max(1, mg.nCoarse));
  Eigen::SparseMatrix<double> I(mg.nCoarse, mg.nCoarse);
  I.setIdentity();
  Ac += eps * I;
  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> coarse(Ac);
  if (coarse.info() != Eigen::Success)
    return fail("coarse factorization failed");

  // V(1,1) preconditioner application.
  auto applyM = [&](const Eigen::VectorXd& r) {
    Eigen::VectorXd z = Eigen::VectorXd::Zero(r.size());
    ChebSmooth(A, invDiag, lmax, smootherDegree, r, z);        // pre
    const Eigen::VectorXd rc = mg.P.transpose() * (r - A * z); // restrict
    z += mg.P * coarse.solve(rc);                              // correct
    ChebSmooth(A, invDiag, lmax, smootherDegree, r, z);        // post
    return z;
  };

  // Standard PCG (fp64).
  if (x.size() != sys.nFree)
    x = Eigen::VectorXd::Zero(sys.nFree);
  const double bb = f.squaredNorm();
  if (bb <= 0.0) {
    x.setZero();
    if (stats)
      *stats = {0, 0.0, true};
    return true;
  }
  const double target = tol * tol * bb;
  Eigen::VectorXd r = f - A * x;
  Eigen::VectorXd z = applyM(r);
  Eigen::VectorXd p = z;
  double rz = r.dot(z);
  int it = 0;
  double rr = r.squaredNorm();
  while (rr > target && it < maxIter) {
    const Eigen::VectorXd Ap = A * p;
    const double alpha = rz / p.dot(Ap);
    x += alpha * p;
    r -= alpha * Ap;
    z = applyM(r);
    const double rzNew = r.dot(z);
    p = z + (rzNew / rz) * p;
    rz = rzNew;
    rr = r.squaredNorm();
    ++it;
  }
  if (stats) {
    stats->iterations = it;
    stats->relResidual = std::sqrt(rr / bb);
    stats->converged = rr <= target;
  }
  return true;
}

} // namespace gpu
