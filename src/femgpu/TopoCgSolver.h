#pragma once

// On-device preconditioned Conjugate Gradient over the matrix-free stiffness
// product (GPU1 of docs/topopt/gpu-plan.md). The whole CG loop — SpMV,
// reductions, alpha/beta, vector updates — runs in compute shaders recorded
// in chunks, so vectors never cross PCIe between iterations; the host only
// reads one scalar (the residual norm) per chunk to decide convergence.
// Jacobi (element-assembled diagonal) preconditioning, warm-started from the
// previous SIMP iteration's displacement. fp32 throughout: the SIMP loop is
// validated against the CPU fp64 path by the femgpu tests.

#include "femgpu/ComputeContext.h"
#include "femgpu/TopoSpmv.h"  // SpmvSystem (the packed element data)

#include <memory>
#include <string>
#include <vector>

namespace gpu {

struct CgStats {
  int iterations = 0;        // outer PCG iterations (not inner SpMVs)
  float relResidual = 0.0f;  // achieved |r|/|f|
  bool converged = false;    // hit tol (false = maxIter or fp32 stagnation)
  float lambdaMax = 0.0f;    // Chebyshev only: estimated lambda_max(D^-1 K)
};

// Preconditioner selection (GPU2). Chebyshev applies a degree-`chebDegree`
// polynomial of the Jacobi-scaled operator per outer iteration (each outer
// step then costs `chebDegree` SpMVs instead of 1).
//
// MEASURED VERDICT (gpu-plan.md): Jacobi is the default because Chebyshev
// LOSES 2-2.5x wall time on real SIMP systems at every degree/ratio tried —
// it does cut outer iterations ~2.3x, but CG is already the optimal
// polynomial method, and the convergence-gating void-mode tail (element
// scales ~1e-6) sits far below any usable Chebyshev interval, so the extra
// SpMVs buy less than they cost. The machinery stays because a Chebyshev
// polynomial over the TOP of the spectrum is exactly the multigrid smoother
// (GPU3), where coarse grids handle the tail it cannot reach.
struct CgOptions {
  enum class Precond { Jacobi, Chebyshev };
  Precond precond = Precond::Jacobi;
  int chebDegree = 8;       // polynomial degree (SpMVs per application)
  float chebRatio = 30.0f;  // targets [lambda_max/ratio, lambda_max]
};

class TopoCgSolver {
 public:
  // Uploads the element data and builds all pipelines/bindings once. The
  // context must outlive the solver.
  static std::unique_ptr<TopoCgSolver> Create(ComputeContext& ctx,
                                              const SpmvSystem& sys,
                                              std::string* error,
                                              const CgOptions& options = {});
  ~TopoCgSolver();

  TopoCgSolver(const TopoCgSolver&) = delete;
  TopoCgSolver& operator=(const TopoCgSolver&) = delete;

  // Per-element SIMP stiffness scale E(rho)/E0; triggers a preconditioner
  // (diagonal) rebuild on the next Solve.
  bool SetScales(const std::vector<float>& scales, std::string* error);

  // Solves K x = f. `x` carries the warm start in and the solution out.
  // Runs until |r|/|f| <= tol, maxIter, or fp32 stagnation (no residual
  // progress over several chunks — the solution is then still the best
  // available, mirroring Eigen's accept-on-NoConvergence behavior that the
  // CPU topology path relies on). Returns false only on a hard failure
  // (device error, NaN breakdown).
  bool Solve(const std::vector<float>& f, std::vector<float>& x, float tol,
             int maxIter, CgStats* stats, std::string* error);

 private:
  explicit TopoCgSolver(ComputeContext& ctx) : m_ctx(ctx) {}
  void RecordPrecond(uint32_t rzSlot);  // z = M^-1 r + refresh rz/rr slots
  void RecordIteration(int parity);
  bool EstimateLambdaMax(std::string* error);  // power iteration on D^-1 K

  ComputeContext& m_ctx;
  int m_nFree = 0;
  int m_nElems = 0;
  uint32_t m_elemGx = 0, m_elemGy = 0;  // element-grid dispatch (spmv/diag)
  uint32_t m_vecGroups = 0;             // ceil(nFree/256) for vector kernels
  CgOptions m_opts;

  // Pipelines (all embedded SPIR-V).
  Pipeline m_spmv, m_diag, m_residual, m_precondDot, m_copy, m_dot, m_updateXr,
      m_updateP, m_chebInit, m_chebIter, m_vecDivd, m_vecScale;
  // Buffers: element data + the CG working set + the scalar block. chebR/
  // chebD are the Chebyshev inner residual/direction (doubling as the power-
  // iteration work vectors).
  Buffer m_dof, m_ke, m_scale, m_diagK, m_f, m_x, m_r, m_z, m_p, m_ap,
      m_chebR, m_chebD, m_scalars;
  // Static buffer bindings, created once.
  BoundSet m_setSpmvP, m_setSpmvX, m_setDiag, m_setResidual, m_setPrecond,
      m_setCopyZP, m_setDotFF, m_setDotPAp, m_setUpdateXr, m_setUpdateP,
      m_setSpmvChebD, m_setChebInit, m_setChebIter, m_setDotRZ, m_setDotRR,
      m_setDivdAp, m_setScaleRD, m_setDotDR, m_setDotDD, m_setDotRRcheb;

  bool m_scalesDirty = true;
  float m_lambdaMax = 0.0f;
  std::vector<float> m_chebC1, m_chebC2;  // per-step coefficients (host)
  float m_chebInvTheta = 0.0f;
};

}  // namespace gpu
