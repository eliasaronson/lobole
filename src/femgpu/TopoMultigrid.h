// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Geometric multigrid for the topology solver
// (docs/topopt/README.md#the-gpu-solver): CPU-side construction of the
// two-level hierarchy (node aggregation + rigid-body-mode prolongation +
// Galerkin coarse operator) and a pure-fp64 reference V(1,1)-PCG.
//
// The fp64 path is both MG0's gate (does the coarse space kill the void-mode
// tail in exact arithmetic?) and MG1's oracle (the GPU cycle is diffed
// against it). Construction runs once per optimization run — the mesh never
// changes across SIMP iterations; only the per-element scales do, and the
// coarse operator reassembles from precomputed per-element blocks.
//
// Requires Eigen (compiled only when HAVE_FEM_SOLVER) but no Vulkan: MG0 is
// buildable and testable on any machine.

#include "femgpu/TopoSpmv.h" // SpmvSystem (the packed element data)

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>

#include <memory>
#include <string>
#include <vector>

namespace gpu {

// The two-level hierarchy, built once from the packed system + node
// coordinates. Coarse DOFs are 6 per aggregate (rigid body modes: 3
// translations + 3 rotations about the aggregate centroid) — the near-
// nullspace of elasticity, without which an aggregation coarse space cannot
// represent the low-energy modes it exists to correct.
struct MgSetup {
  int nFree = 0;
  int nAgg = 0;
  int nCoarse = 0;                 // 6 * nAgg
  std::vector<int> aggOfNode;      // mesh node -> aggregate (-1 = none)
  std::vector<double> aggCentroid; // 3 per aggregate
  Eigen::SparseMatrix<double> P;   // nFree x nCoarse prolongation
  // Per-element Galerkin blocks: A_c(scales) = sum_e scale_e * block_e,
  // stored as triplet runs so reassembly is a flat weighted accumulation.
  // (elemBlockOffset[e]..elemBlockOffset[e+1]) indexes rows/cols/vals.
  std::vector<int> blockRow, blockCol;
  std::vector<double> blockVal;
  std::vector<int> elemBlockOffset; // size nElems+1
};

// Builds aggregates (greedy BFS over the tet node-clique graph), the
// rigid-body P over the REDUCED system (fixed DOFs dropped), and the
// per-element coarse blocks P^T Ke0 P. `nodes` are the mesh node positions
// (x,y,z per node), `dofMap` the reduced-DOF map (3 per node, -1 fixed).
bool BuildMgSetup(const std::vector<double>& nodes, const SpmvSystem& sys,
                  const std::vector<int>& dofMap, MgSetup& out,
                  std::string* error);

// fp64 reference: V(1,1)-cycle preconditioned CG on the assembled system.
// Chebyshev(smootherDegree) pre/post smoothing on the Jacobi-scaled operator
// (top-of-spectrum interval), exact (LDLT) coarse solve with a tiny Tikhonov
// regularization (partially-fixed aggregates make A_c semidefinite).
struct MgRefStats {
  int iterations = 0;
  double relResidual = 0.0;
  bool converged = false;
};
bool MgReferencePcg(const SpmvSystem& sys, const std::vector<float>& scales,
                    const MgSetup& mg, const Eigen::VectorXd& f,
                    Eigen::VectorXd& x, double tol, int maxIter,
                    int smootherDegree, MgRefStats* stats, std::string* error);

// The coarse-level solver the GPU cycle calls between submits: assembles
// A_c(scales) from the precomputed blocks (+ trace-scaled Tikhonov — see
// MgReferencePcg) and factors it; Solve maps fp32 coarse residual -> fp32
// coarse correction through the fp64 LDLT. The sparsity pattern never
// changes, so refactorizations reuse the symbolic analysis.
class MgCoarseSolver {
public:
  explicit MgCoarseSolver(MgSetup setup) : m_setup(std::move(setup)) {}
  const MgSetup& Setup() const { return m_setup; }
  bool UpdateScales(const std::vector<float>& scales, std::string* error);
  bool Solve(const float* rc, float* ec, std::string* error) const;

private:
  MgSetup m_setup;
  Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> m_ldlt;
  bool m_analyzed = false;
  bool m_ready = false;
};

// One-call packaging for the GPU cycle: builds the hierarchy and returns the
// MgGpuData TopoCgSolver consumes (node tables + coarse-solver callbacks,
// which keep the MgCoarseSolver alive via capture). Null + *error when the
// hierarchy cannot be built.
struct MgGpuData; // femgpu/TopoCgSolver.h
std::shared_ptr<MgGpuData> MakeMgGpuData(const std::vector<double>& nodes,
                                         const SpmvSystem& sys,
                                         const std::vector<int>& dofMap,
                                         std::string* error);

} // namespace gpu
