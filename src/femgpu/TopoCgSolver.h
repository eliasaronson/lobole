#pragma once

// On-device preconditioned Conjugate Gradient over the matrix-free stiffness
// product (docs/topopt/README.md#the-gpu-solver). The whole CG
// loop — SpMV, reductions, alpha/beta, vector updates — runs in compute shaders
// recorded in chunks, so vectors never cross PCIe between iterations; the host
// only reads one scalar (the residual norm) per chunk to decide convergence.
// The preconditioner is chosen by CgOptions: Jacobi (element-assembled
// diagonal) is this class's default, while TopoGpuSolver asks for multigrid
// whenever its hierarchy builds and falls back to Jacobi otherwise. Warm-
// started from the previous SIMP iteration's displacement. fp32 throughout:
// the SIMP loop is validated against the CPU fp64 path by the femgpu tests.

#include "femgpu/ComputeContext.h"
#include "femgpu/TopoSpmv.h" // SpmvSystem (the packed element data)

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gpu {

struct CgStats {
  int iterations = 0;       // outer PCG iterations (not inner SpMVs)
  float relResidual = 0.0f; // achieved |r|/|f|
  bool converged = false;   // hit tol (false = maxIter or fp32 stagnation)
  float lambdaMax = 0.0f;   // Chebyshev only: estimated lambda_max(D^-1 K)
};

// The multigrid hierarchy handed to the GPU cycle, packed by the
// caller (TopoGpuSolver via TopoMultigrid) so this header stays Vulkan-pure:
// per-node aggregate membership + rigid-mode geometry for the restrict/
// prolong kernels, and two CPU callbacks — the coarse solve that runs
// between submits, and the per-SIMP-iteration coarse reassembly.
struct MgGpuData {
  int nNodes = 0;
  int nCoarse = 0;               // 6 * nAgg
  std::vector<int32_t> nodeAgg;  // per node (-1 = none)
  std::vector<float> nodeOffset; // 3/node: position - aggregate centroid
  std::vector<int32_t> nodeDof;  // 3/node: reduced DOF ids (-1 = fixed)
  std::function<bool(const std::vector<float>& scales, std::string*)>
      updateScales; // reassemble + refactor A_c
  std::function<bool(const float* rc, float* ec, std::string*)> coarseSolve;
};

// Preconditioner selection. Chebyshev applies a degree-
// `chebDegree` polynomial of the Jacobi-scaled operator per outer iteration
// (each outer step then costs `chebDegree` SpMVs instead of 1).
//
// MEASURED VERDICTS (gpu-plan.md): standalone Chebyshev LOSES 2-2.5x wall
// time on real SIMP systems — CG is already the optimal polynomial method
// and the void-mode tail (element scales ~1e-6) sits below any usable
// interval. As the MULTIGRID smoother the same polynomial is exactly right:
// it scrubs the top of the spectrum while the rigid-body coarse space (mg)
// handles the tail (fp64 gate: 19-32x fewer outer iterations than Jacobi).
// For Multigrid, chebDegree/chebRatio parameterize the SMOOTHER (typical:
// degree 3, ratio 15) and `mg` must be set.
struct CgOptions {
  enum class Precond { Jacobi, Chebyshev, Multigrid };
  Precond precond = Precond::Jacobi;
  int chebDegree = 8;            // polynomial degree (SpMVs per application)
  float chebRatio = 30.0f;       // targets [lambda_max/ratio, lambda_max]
  std::shared_ptr<MgGpuData> mg; // required for Multigrid
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

  // Per-element strain energies at FULL modulus: se[e] = ue^T Ke0 ue for the
  // reduced displacement `u` (GPU4 — the SIMP sensitivity kernel). One
  // dispatch over the resident element matrices; `u` is uploaded into CG
  // scratch (never the warm-start state), so this is safe to call with any
  // vector at any time between solves.
  bool ElementEnergies(const std::vector<float>& u, std::vector<float>& se,
                       std::string* error);

private:
  explicit TopoCgSolver(ComputeContext& ctx) : m_ctx(ctx) {}
  void RecordPrecond(uint32_t rzSlot); // z = M^-1 r + refresh rz/rr slots
  void RecordIteration(int parity);
  bool EstimateLambdaMax(std::string* error); // power iteration on D^-1 K
  // Multigrid path (GPU3): the coarse solve is a CPU call between submits,
  // so the cycle cannot be chunk-recorded — its own per-iteration driver.
  bool SolveMg(const std::vector<float>& f, std::vector<float>& x, float tol,
               int maxIter, CgStats* stats, std::string* error);
  void RecordChebInto(const BoundSet& initSet, const BoundSet& iterSet);
  bool ApplyMgPrecond(uint32_t rzSlot, std::string* error);

  ComputeContext& m_ctx;
  int m_nFree = 0;
  int m_nElems = 0;
  uint32_t m_elemGx = 0, m_elemGy = 0; // element-grid dispatch (spmv/diag)
  uint32_t m_vecGroups = 0;            // ceil(nFree/256) for vector kernels
  uint32_t m_nodeGroups = 0;           // ceil(nNodes/256) for mg kernels
  CgOptions m_opts;

  // Pipelines (all embedded SPIR-V).
  Pipeline m_spmv, m_diag, m_residual, m_precondDot, m_copy, m_dot, m_updateXr,
      m_updateP, m_chebInit, m_chebIter, m_vecDivd, m_vecScale, m_mgRestrict,
      m_mgProlong, m_vecAdd, m_energy;
  // Buffers: element data + the CG working set + the scalar block. chebR/
  // chebD are the Chebyshev inner residual/direction (doubling as the power-
  // iteration work vectors). mg*: multigrid working set (fine residual r2,
  // post-smoothing correction, coarse residual/correction, node tables).
  Buffer m_dof, m_ke, m_scale, m_diagK, m_f, m_x, m_r, m_z, m_p, m_ap, m_chebR,
      m_chebD, m_scalars, m_mgR, m_mgZt, m_mgRc, m_mgEc, m_nodeAgg, m_nodeOff,
      m_nodeDof, m_se;
  // Static buffer bindings, created once.
  BoundSet m_setSpmvP, m_setSpmvX, m_setDiag, m_setResidual, m_setPrecond,
      m_setCopyZP, m_setDotFF, m_setDotPAp, m_setUpdateXr, m_setUpdateP,
      m_setSpmvChebD, m_setChebInit, m_setChebIter, m_setDotRZ, m_setDotRR,
      m_setDivdAp, m_setScaleRD, m_setDotDR, m_setDotDD, m_setDotRRcheb,
      m_setSpmvZ, m_setResidualMg, m_setMgRestrict, m_setMgProlong,
      m_setChebInitMg, m_setChebIterMg, m_setVecAddZt, m_setEnergy;

  bool m_scalesDirty = true;
  std::vector<float> m_scalesCpu; // for the coarse-rebuild callback
  float m_lambdaMax = 0.0f;
  std::vector<float> m_chebC1, m_chebC2; // per-step coefficients (host)
  float m_chebInvTheta = 0.0f;
};

} // namespace gpu
