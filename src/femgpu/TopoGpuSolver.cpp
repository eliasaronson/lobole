#include "femgpu/TopoGpuSolver.h"

#include "core/fem/FemSolverInternal.h" // element math for packing Ke (Eigen)
#include "femgpu/ComputeContext.h"
#include "femgpu/TopoCgSolver.h"
#include "femgpu/TopoMultigrid.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace gpu {

struct TopoGpuSolver::Impl {
  std::unique_ptr<ComputeContext> ctx;
  std::unique_ptr<TopoCgSolver> cg; // keeps the MG hierarchy alive via opts
  int nFree = 0;
  std::vector<float> scaleF, fF, xF; // fp32 staging
  bool haveWarmStart = false;
};

TopoGpuSolver::TopoGpuSolver() : m_impl(new Impl) {}
TopoGpuSolver::~TopoGpuSolver() = default;

const std::string& TopoGpuSolver::DeviceName() const {
  return m_impl->ctx->DeviceName();
}

std::unique_ptr<TopoGpuSolver> TopoGpuSolver::Create(bool allowSoftware,
                                                     std::string* error) {
  auto ctx = ComputeContext::Create(error);
  if (!ctx)
    return nullptr;
  if (ctx->IsSoftwareDevice() && !allowSoftware) {
    if (error)
      *error = "only a software Vulkan device (" + ctx->DeviceName() +
               ") is available; the CPU solver is faster";
    return nullptr;
  }
  std::unique_ptr<TopoGpuSolver> s(new TopoGpuSolver());
  s->m_impl->ctx = std::move(ctx);
  return s;
}

bool TopoGpuSolver::Setup(const fem::FemMesh& mesh,
                          const fem::Material& material,
                          const std::vector<int>& dofMap, int nFree,
                          std::string* error) {
  using namespace fem::internal;
  Impl& im = *m_impl;

  // Pack the reduced system exactly the way the CPU assembly builds it:
  // per-tet Ke at full modulus (fp32-cast), DOF slots node-major, -1 = fixed.
  const auto D0 = Elasticity(material.youngMPa, material.poisson);
  SpmvSystem sys;
  sys.nFree = nFree;
  sys.nElems = static_cast<int>(mesh.tets.size());
  sys.dof.reserve(static_cast<size_t>(sys.nElems) * 30);
  sys.ke.reserve(static_cast<size_t>(sys.nElems) * 900);
  for (const auto& tet : mesh.tets) {
    for (int i = 0; i < 10; ++i)
      for (int k = 0; k < 3; ++k)
        sys.dof.push_back(dofMap[3 * tet[i] + k]);
    Eigen::Vector3d X[10];
    for (int i = 0; i < 10; ++i) {
      const auto& p = mesh.nodes[tet[i]];
      X[i] = Eigen::Vector3d(p.x, p.y, p.z);
    }
    Eigen::Matrix<double, 30, 30> Ke = Eigen::Matrix<double, 30, 30>::Zero();
    for (const auto& g : kTetGauss) {
      Eigen::Matrix<double, 6, 30> B;
      const double detJ = StrainDisp(X, g[0], g[1], g[2], B);
      Ke += (kTetW * std::abs(detJ)) * (B.transpose() * D0 * B);
    }
    for (int r = 0; r < 30; ++r)
      for (int c = 0; c < 30; ++c)
        sys.ke.push_back(static_cast<float>(Ke(r, c)));
  }

  // Preconditioner selection (measured verdicts in gpu-plan.md): MULTIGRID
  // by default when its hierarchy builds — the fp64 gate cut outer
  // iterations 19-32x vs Jacobi and the GPU bench confirms the win; plain
  // Jacobi is the fallback (standalone Chebyshev measured 2-2.5x slower).
  // Env overrides for experiments: FEMGPU_PRECOND=mg|jacobi|cheb,
  // FEMGPU_CHEB_DEGREE=<int>, FEMGPU_CHEB_RATIO=<float>.
  std::string precond = "mg";
  if (const char* p = std::getenv("FEMGPU_PRECOND"))
    precond = p;
  CgOptions opts;
  if (precond == "cheb") {
    opts.precond = CgOptions::Precond::Chebyshev;
  } else if (precond == "mg") {
    std::vector<double> flat;
    flat.reserve(mesh.nodes.size() * 3);
    for (const auto& p : mesh.nodes) {
      flat.push_back(p.x);
      flat.push_back(p.y);
      flat.push_back(p.z);
    }
    std::string mgErr;
    if (auto mg = MakeMgGpuData(flat, sys, dofMap, &mgErr)) {
      opts.precond = CgOptions::Precond::Multigrid;
      opts.chebDegree = 3;    // the SMOOTHER polynomial
      opts.chebRatio = 15.0f; // top-of-spectrum smoothing interval
      opts.mg = std::move(mg);
    } // else: fall through to Jacobi
  }
  if (const char* d = std::getenv("FEMGPU_CHEB_DEGREE"))
    opts.chebDegree = std::max(2, std::atoi(d));
  if (const char* r = std::getenv("FEMGPU_CHEB_RATIO"))
    opts.chebRatio = std::max(1.5f, static_cast<float>(std::atof(r)));

  im.cg = TopoCgSolver::Create(*im.ctx, sys, error, opts);
  if (!im.cg)
    return false;
  im.nFree = nFree;
  im.scaleF.resize(sys.nElems);
  im.fF.resize(nFree);
  im.xF.assign(nFree, 0.0f);
  im.haveWarmStart = false;
  return true;
}

bool TopoGpuSolver::Solve(const std::vector<double>& scale,
                          const std::vector<double>& F, std::vector<double>& u,
                          std::string* error) {
  Impl& im = *m_impl;
  if (!im.cg) {
    if (error)
      *error = "Setup was not called";
    return false;
  }
  for (size_t e = 0; e < scale.size(); ++e)
    im.scaleF[e] = static_cast<float>(scale[e]);
  if (!im.cg->SetScales(im.scaleF, error))
    return false;
  for (int i = 0; i < im.nFree; ++i)
    im.fF[i] = static_cast<float>(F[i]);
  // Warm start: the device-side x from the previous SIMP iteration (kept in
  // xF); the incoming `u` matches it, so either would do.
  if (!im.haveWarmStart)
    std::fill(im.xF.begin(), im.xF.end(), 0.0f);

  // 1e-5 relative residual — the fp32-realistic target (the measured MG/
  // Jacobi floor sits just under it). Asking for the CPU path's 1e-6 made
  // EVERY solve grind hundreds of floor iterations through the stall/restart
  // machinery before accepting (measured: 300-460 iters/solve, ~1 s, where
  // convergent solves take 25-60). Compliance error scales as tol^2 (energy
  // norm), so 1e-5 is far below anything the OC update can sense; the
  // stagnation guard still catches fields whose floor sits above it.
  const float tol = 1e-5f;
  // Warm-started mid-SIMP solves are capped hard: on real topology systems
  // the fp32 residual floors near 1e-2 and the useful (energy) work is done
  // within the first ~100 iterations — everything after was measured to be
  // floor-grinding through the stall machinery (560-1100 iters/solve, ~2/3
  // of the whole SIMP runtime) with no effect on the design. The cold first
  // solve keeps the generous cap for its legitimate slow phases; capped
  // solves still return the best iterate found.
  const int maxIter = im.haveWarmStart ? 128 : std::min(2 * im.nFree, 20000);
  CgStats stats;
  const auto t0 = std::chrono::steady_clock::now();
  if (!im.cg->Solve(im.fF, im.xF, tol, maxIter, &stats, error))
    return false;
  im.haveWarmStart = true;
  // FEMGPU_SOLVE_TRACE=1: one stderr line per SIMP linear solve.
  if (std::getenv("FEMGPU_SOLVE_TRACE"))
    std::fprintf(stderr,
                 "FEMGPU_SOLVE: %.0f ms, %d outer iters, rel res %.2e, "
                 "converged=%d\n",
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t0)
                     .count(),
                 stats.iterations, stats.relResidual, (int)stats.converged);

  u.resize(im.nFree);
  for (int i = 0; i < im.nFree; ++i)
    u[i] = static_cast<double>(im.xF[i]);
  return true;
}

bool TopoGpuSolver::ElementEnergies(const std::vector<double>& u,
                                    std::vector<double>& se,
                                    std::string* error) {
  Impl& im = *m_impl;
  if (!im.cg || static_cast<int>(u.size()) != im.nFree) {
    if (error)
      *error = "ElementEnergies before Setup / size mismatch";
    return false;
  }
  for (int i = 0; i < im.nFree; ++i)
    im.fF[i] = static_cast<float>(u[i]);
  std::vector<float> seF;
  if (!im.cg->ElementEnergies(im.fF, seF, error))
    return false;
  se.assign(seF.begin(), seF.end());
  return true;
}

} // namespace gpu
