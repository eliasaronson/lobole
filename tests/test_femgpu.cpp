#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "femgpu/ComputeContext.h"
#include "femgpu/TopoSpmv.h"
#include "gpu_sanity_spv.h"
#include "topo_testmesh.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// GPU0 — the GPU solver gate (docs/topopt/gpu-plan.md). Proves the one real
// correctness risk of the matrix-free CG: an element-wise K(rho)·p with float
// atomicAdd scatter matches the CPU assembly, before any CG is built on it.
// Every case self-SKIPs when no usable compute device exists (the fem_gpu
// requirement is a compute queue + VK_EXT_shader_atomic_float), so the suite
// stays green on machines without Vulkan.

namespace {

// One shared context across cases (device creation is ~100 ms); null+reason
// when the machine can't run these tests.
gpu::ComputeContext* Ctx(std::string* whyNot) {
  static std::string err;
  static std::unique_ptr<gpu::ComputeContext> ctx =
      gpu::ComputeContext::Create(&err);
  if (!ctx && whyNot) *whyNot = err;
  return ctx.get();
}

#define REQUIRE_GPU()                                    \
  std::string whyNot;                                    \
  gpu::ComputeContext* ctx = Ctx(&whyNot);               \
  if (!ctx) SKIP("no usable compute device: " << whyNot)

// Deterministic pseudo-random floats in [-1, 1] (no <random> so values are
// identical across platforms/library versions).
float Rand01(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
}
float RandSym(uint32_t& state) { return 2.0f * Rand01(state) - 1.0f; }

}  // namespace

TEST_CASE("femgpu: sanity kernel round-trips through the compute pipeline",
          "[femgpu]") {
  REQUIRE_GPU();
  INFO("device: " << ctx->DeviceName());

  const uint32_t n = 1000;
  gpu::Buffer buf;
  std::string err;
  REQUIRE(ctx->CreateBuffer(n * sizeof(float), buf, &err));
  float* data = static_cast<float*>(buf.mapped);
  for (uint32_t i = 0; i < n; ++i) data[i] = static_cast<float>(i);

  gpu::Pipeline pipe;
  REQUIRE(ctx->CreatePipeline(k_gpu_sanity_spv, k_gpu_sanity_spv_size, 1,
                              sizeof(uint32_t), pipe, &err));
  REQUIRE(ctx->Dispatch(pipe, {&buf}, &n, sizeof(n), (n + 63) / 64, 1, &err));

  int wrong = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (data[i] != static_cast<float>(i) * 2.0f + 1.0f) ++wrong;
  CHECK(wrong == 0);

  ctx->Destroy(pipe);
  ctx->Destroy(buf);
}

#ifdef HAVE_FEM_SOLVER

#include "core/FemSolverInternal.h"

namespace {

// Packs the reduced system for the GPU exactly the way TopologyOptimize
// assembles it on the CPU: per-tet 30x30 Ke at full modulus (fp32-cast), DOF
// slots node-major with -1 marking fixed DOFs.
gpu::SpmvSystem PackSystem(const fem::FemMesh& mesh,
                           const std::vector<int>& fixedNodes, double E0,
                           double nu) {
  using namespace fem::internal;
  const auto dofs = BuildDofMap(static_cast<int>(mesh.nodes.size()), fixedNodes);
  const auto D0 = Elasticity(E0, nu);

  gpu::SpmvSystem sys;
  sys.nFree = dofs.nFree;
  sys.nElems = static_cast<int>(mesh.tets.size());
  sys.dof.reserve(sys.nElems * 30);
  sys.ke.reserve(static_cast<size_t>(sys.nElems) * 900);
  for (const auto& tet : mesh.tets) {
    for (int i = 0; i < 10; ++i)
      for (int k = 0; k < 3; ++k)
        sys.dof.push_back(dofs.map[3 * tet[i] + k]);
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
  return sys;
}

// CPU oracle: the same product in fp64 FROM THE SAME fp32 inputs (ke, scale,
// p), so the diff isolates the GPU's fp32 accumulation + atomic scatter — not
// the fp64->fp32 cast, which both sides share.
std::vector<double> CpuApply(const gpu::SpmvSystem& sys,
                             const std::vector<float>& scales,
                             const std::vector<float>& p) {
  std::vector<double> Ap(sys.nFree, 0.0);
  for (int e = 0; e < sys.nElems; ++e) {
    const int32_t* dof = &sys.dof[static_cast<size_t>(e) * 30];
    const float* ke = &sys.ke[static_cast<size_t>(e) * 900];
    for (int r = 0; r < 30; ++r) {
      if (dof[r] < 0) continue;
      double acc = 0.0;
      for (int c = 0; c < 30; ++c)
        if (dof[c] >= 0)
          acc += static_cast<double>(ke[r * 30 + c]) *
                 static_cast<double>(p[dof[c]]);
      Ap[dof[r]] += static_cast<double>(scales[e]) * acc;
    }
  }
  return Ap;
}

// Relative infinity-norm error of the GPU result against the oracle.
double RelInfError(const std::vector<float>& gpu, const std::vector<double>& ref) {
  double maxRef = 0.0, maxErr = 0.0;
  for (size_t i = 0; i < ref.size(); ++i) {
    maxRef = std::max(maxRef, std::abs(ref[i]));
    maxErr = std::max(maxErr, std::abs(static_cast<double>(gpu[i]) - ref[i]));
  }
  return maxRef > 0 ? maxErr / maxRef : maxErr;
}

// Runs the gate diff on one mesh: SIMP-like scales spanning the full Emin..1
// range, deterministic p, GPU vs oracle.
void CheckSpmvAgainstCpu(gpu::ComputeContext& ctx, const fem::FemMesh& mesh,
                         const std::vector<int>& fixedNodes) {
  const gpu::SpmvSystem sys = PackSystem(mesh, fixedNodes, 210000.0, 0.3);
  REQUIRE(sys.nFree > 0);

  std::string err;
  auto spmv = gpu::TopoSpmv::Create(ctx, sys, &err);
  INFO(err);
  REQUIRE(spmv);

  // SIMP-like scales: mostly intermediate, some solid, some at the 1e-6
  // ersatz floor — the magnitude spread the real solver produces.
  uint32_t state = 12345;
  std::vector<float> scales(sys.nElems);
  for (int e = 0; e < sys.nElems; ++e) {
    const float r = Rand01(state);
    scales[e] = r < 0.2f ? 1e-6f : (r > 0.8f ? 1.0f : r);
  }
  REQUIRE(spmv->SetScales(scales, &err));

  std::vector<float> p(sys.nFree);
  for (int i = 0; i < sys.nFree; ++i) p[i] = RandSym(state);

  std::vector<float> Ap;
  REQUIRE(spmv->Apply(p, Ap, &err));
  const std::vector<double> ref = CpuApply(sys, scales, p);

  const double relErr = RelInfError(Ap, ref);
  INFO("elements: " << sys.nElems << "  free DOFs: " << sys.nFree
                    << "  rel-inf error: " << relErr);
  CHECK(relErr < 1e-4);  // fp32 accumulation noise; typically ~1e-6

  // Repeat with uniform full stiffness to also cover the un-scaled path (and
  // prove Apply is repeatable — the scatter zero-init works).
  std::fill(scales.begin(), scales.end(), 1.0f);
  REQUIRE(spmv->SetScales(scales, &err));
  std::vector<float> Ap2;
  REQUIRE(spmv->Apply(p, Ap2, &err));
  const std::vector<double> ref2 = CpuApply(sys, scales, p);
  CHECK(RelInfError(Ap2, ref2) < 1e-4);
}

}  // namespace

TEST_CASE("femgpu: matrix-free SpMV matches the CPU assembly (GATE)",
          "[femgpu][gate]") {
  REQUIRE_GPU();
  INFO("device: " << ctx->DeviceName());

  // Structured mesh (deterministic, Netgen-free) — the primary gate.
  fem::FemMesh mesh = GenerateBoxMesh(8, 4, 4, 80, 40, 40);
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
  REQUIRE(fixed.size() > 3);
  CheckSpmvAgainstCpu(*ctx, mesh, fixed);
}

#ifdef HAVE_NETGEN
#include "CADKernel.h"
#include "core/FemMesher.h"

TEST_CASE("femgpu: SpMV matches the CPU on a real Netgen mesh",
          "[femgpu][gate]") {
  REQUIRE_GPU();
  INFO("device: " << ctx->DeviceName());

  // Unstructured mesh — catches scatter bugs a regular grid can't (irregular
  // node valences, varied element volumes).
  CADShape box = CADKernel::Instance().CreateBox(60, 30, 30);
  fem::MeshSettings ms;
  ms.maxSize = 10.0;
  std::string err;
  auto mesh = fem::Mesh(box.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  const std::vector<int> fixed = NodesAtPlane(*mesh, 0, 0.0, 1e-6);
  REQUIRE(fixed.size() > 3);
  CheckSpmvAgainstCpu(*ctx, *mesh, fixed);
}
#endif  // HAVE_NETGEN

// ---- GPU1: the on-device CG solver ----------------------------------------

#include "core/FemTopo.h"
#include "femgpu/TopoCgSolver.h"
#include "femgpu/TopoGpuSolver.h"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCore>

#include <chrono>

namespace {

// SIMP-like scales with a solid load path (nothing fully disconnected).
std::vector<float> SimpScales(int nElems, uint32_t seed) {
  uint32_t state = seed;
  std::vector<float> scales(nElems);
  for (int e = 0; e < nElems; ++e) {
    const float r = Rand01(state);
    scales[e] = r < 0.2f ? 1e-6f : (r > 0.6f ? 1.0f : r);
  }
  return scales;
}

// fp64 relative L2 residual |f - Kx| / |f| of a GPU solution, on the same
// fp32 element data the GPU used.
double ResidualNorm(const gpu::SpmvSystem& sys, const std::vector<float>& scales,
                    const std::vector<float>& x, const std::vector<float>& f) {
  const std::vector<double> Kx = CpuApply(sys, scales, x);
  double rr = 0.0, ff = 0.0;
  for (int i = 0; i < sys.nFree; ++i) {
    const double ri = static_cast<double>(f[i]) - Kx[i];
    rr += ri * ri;
    ff += static_cast<double>(f[i]) * static_cast<double>(f[i]);
  }
  return std::sqrt(rr / ff);
}

}  // namespace

TEST_CASE("femgpu: CG solves the reduced system to tolerance", "[femgpu][cg]") {
  REQUIRE_GPU();
  INFO("device: " << ctx->DeviceName());

  fem::FemMesh mesh = GenerateBoxMesh(6, 3, 3, 60, 30, 30);
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
  const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 210000.0, 0.3);

  std::string err;
  auto cg = gpu::TopoCgSolver::Create(*ctx, sys, &err);
  INFO(err);
  REQUIRE(cg);

  // A load-like right-hand side (deterministic).
  uint32_t state = 42;
  std::vector<float> f(sys.nFree);
  for (int i = 0; i < sys.nFree; ++i) f[i] = RandSym(state);

  SECTION("uniform stiffness, achievable tolerance") {
    // tol above the measured fp32 floor (~3e-5 on this size): the solver must
    // genuinely converge, and its claimed residual must be honest (the
    // chunk-boundary residual replacement measures TRUE residuals).
    const std::vector<float> scales(sys.nElems, 1.0f);
    REQUIRE(cg->SetScales(scales, &err));
    std::vector<float> x(sys.nFree, 0.0f);
    gpu::CgStats stats;
    REQUIRE(cg->Solve(f, x, 1e-4f, 20000, &stats, &err));
    const double res = ResidualNorm(sys, scales, x, f);
    INFO("iterations: " << stats.iterations << "  claimed: "
                        << stats.relResidual << "  fp64-verified: " << res);
    CHECK(stats.converged);
    CHECK(res < 2e-4);
    CHECK(stats.iterations > 0);
    // The claim matches reality (no fp32 recurrence lies).
    CHECK(res < 2.0 * static_cast<double>(stats.relResidual) + 1e-9);

    // Warm start: re-solving the same system starts at the solution.
    gpu::CgStats warm;
    REQUIRE(cg->Solve(f, x, 1e-4f, 20000, &warm, &err));
    INFO("warm-start iterations: " << warm.iterations);
    CHECK(warm.iterations == 0);
  }

  SECTION("SIMP void/solid scales, tolerance below the fp32 floor") {
    // Scales spanning 1e-6..1 make the system brutally ill-conditioned and
    // 1e-6 unreachable in fp32. The solver must return its best (floored)
    // iterate — not a past-the-floor walk-away — and report honestly.
    const std::vector<float> scales = SimpScales(sys.nElems, 777);
    REQUIRE(cg->SetScales(scales, &err));
    std::vector<float> x(sys.nFree, 0.0f);
    gpu::CgStats stats;
    REQUIRE(cg->Solve(f, x, 1e-6f, 20000, &stats, &err));
    const double res = ResidualNorm(sys, scales, x, f);
    INFO("iterations: " << stats.iterations << "  claimed: "
                        << stats.relResidual << "  fp64-verified: " << res);
    CHECK(res < 1e-3);  // the floor is far better than this in practice
    CHECK(res < 2.0 * static_cast<double>(stats.relResidual) + 1e-9);
  }
}

TEST_CASE("femgpu: TopologyOptimize lands on the CPU design via the GPU solver",
          "[femgpu][cg]") {
  // The GPU1 verification: the same small cantilever optimized twice — the
  // built-in Eigen CG-IC vs the injected GPU solver — must land on the same
  // optimum (fp32 + Jacobi vs fp64 + IC changes the path negligibly since
  // the OC update reacts to energies, not raw displacements).
  std::string gerr;
  auto solver = gpu::TopoGpuSolver::Create(/*allowSoftware=*/true, &gerr);
  if (!solver) SKIP("no usable compute device: " << gerr);
  INFO("device: " << solver->DeviceName());

  fem::FemMesh mesh = GenerateBoxMesh(10, 4, 4, 100, 40, 40);
  fem::Material steel{210000.0, 0.3};
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(mesh, 0, 100.0, 1e-6);
  load.magnitude = 1000.0;
  load.direction = glm::dvec3(0, 0, -1);
  fem::TopoParams params;
  params.filterRadius = 25.0;
  params.maxIterations = 30;

  std::string err;
  auto cpuRes = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err);
  REQUIRE(cpuRes.has_value());
  auto gpuRes = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err,
                                      {}, {}, solver.get());
  INFO(err);
  REQUIRE(gpuRes.has_value());

  INFO("cpu compliance " << cpuRes->complianceHistory.back() << " ("
                         << cpuRes->iterations << " it), gpu compliance "
                         << gpuRes->complianceHistory.back() << " ("
                         << gpuRes->iterations << " it)");
  // SIMP is non-convex: solver differences (fp32+Jacobi vs fp64+IC) steer
  // gray boundary elements differently, so element-wise equality is the
  // wrong bar. Equivalence means: same objective quality (compliance within
  // a few %), same volume, and the same solid/void classification for the
  // overwhelming majority of elements.
  CHECK_THAT(gpuRes->complianceHistory.back(),
             WithinRel(cpuRes->complianceHistory.back(), 0.03));
  CHECK_THAT(gpuRes->volumeFractionAchieved,
             WithinAbs(cpuRes->volumeFractionAchieved, 0.01));
  REQUIRE(gpuRes->density.size() == cpuRes->density.size());
  int flipped = 0;
  double meanDiff = 0.0;
  for (size_t e = 0; e < cpuRes->density.size(); ++e) {
    const double d = std::abs(gpuRes->density[e] - cpuRes->density[e]);
    meanDiff += d;
    if (d > 0.5) ++flipped;  // solid on one side, void on the other
  }
  meanDiff /= cpuRes->density.size();
  const double flippedFrac = double(flipped) / cpuRes->density.size();
  INFO("mean |density diff|: " << meanDiff
                               << "  solid/void flips: " << flippedFrac);
  CHECK(flippedFrac < 0.10);
}

TEST_CASE("femgpu bench: GPU CG vs CPU CG-IC on the bulky cube",
          "[.][gpubench]") {
  // Mirror of [topobench] (12x12x12 cube, 10368 tets): three SIMP-like solves
  // through each path. Run with: femgpu_tests "[gpubench]"
  REQUIRE_GPU();
  WARN("device: " << ctx->DeviceName());

  const double L = 80.0;
  fem::FemMesh mesh = GenerateBoxMesh(12, 12, 12, L, L, L);
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
  const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 210000.0, 0.3);
  WARN("system: " << sys.nElems << " tets, " << sys.nFree << " free DOFs");

  std::string err;
  auto cg = gpu::TopoCgSolver::Create(*ctx, sys, &err);
  REQUIRE(cg);
  uint32_t state = 99;
  std::vector<float> f(sys.nFree);
  for (int i = 0; i < sys.nFree; ++i) f[i] = RandSym(state);

  using clk = std::chrono::steady_clock;
  std::vector<float> x(sys.nFree, 0.0f);
  for (int pass = 0; pass < 3; ++pass) {
    // Slightly different scales each pass, like consecutive SIMP iterations
    // (so the warm start helps but is not exact).
    std::vector<float> scales = SimpScales(sys.nElems, 1000 + pass);
    REQUIRE(cg->SetScales(scales, &err));
    gpu::CgStats stats;
    const auto t0 = clk::now();
    REQUIRE(cg->Solve(f, x, 1e-5f, 40000, &stats, &err));
    const double ms =
        std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    WARN("gpu solve " << pass << ": " << ms << " ms, " << stats.iterations
                      << " CG iters, rel res " << stats.relResidual
                      << " (fp64 check "
                      << ResidualNorm(sys, scales, x, f) << ")");
  }

  // CPU reference: Eigen CG-IC (the shipping CPU path) on the same system.
  {
    const std::vector<float> scales = SimpScales(sys.nElems, 1000);
    std::vector<Eigen::Triplet<double>> trip;
    for (int e = 0; e < sys.nElems; ++e) {
      const int32_t* dof = &sys.dof[static_cast<size_t>(e) * 30];
      const float* ke = &sys.ke[static_cast<size_t>(e) * 900];
      for (int a = 0; a < 30; ++a) {
        if (dof[a] < 0) continue;
        for (int b = 0; b < 30; ++b)
          if (dof[b] >= 0)
            trip.emplace_back(dof[a], dof[b],
                              static_cast<double>(scales[e]) *
                                  static_cast<double>(ke[a * 30 + b]));
      }
    }
    Eigen::SparseMatrix<double> K(sys.nFree, sys.nFree);
    K.setFromTriplets(trip.begin(), trip.end());
    Eigen::VectorXd F(sys.nFree);
    for (int i = 0; i < sys.nFree; ++i) F(i) = f[i];
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>,
                             Eigen::Lower | Eigen::Upper,
                             Eigen::IncompleteCholesky<double>>
        cpu;
    cpu.setTolerance(1e-6);
    const auto t0 = clk::now();
    cpu.compute(K);
    const Eigen::VectorXd u = cpu.solve(F);
    const double ms =
        std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    WARN("cpu CG-IC solve: " << ms << " ms, " << cpu.iterations()
                             << " CG iters (cold, incl. assembly+IC setup)");
  }
}

TEST_CASE("femgpu bench: full SIMP loop GPU vs CPU on the bulky cube",
          "[.][gpubench]") {
  // The number that matters to the user: wall time of TopologyOptimize on
  // the [topobench] cube through each solver, on the REAL SIMP systems
  // (smooth filtered fields, warm starts) rather than harsh random scales.
  std::string gerr;
  auto solver = gpu::TopoGpuSolver::Create(/*allowSoftware=*/true, &gerr);
  if (!solver) SKIP("no usable compute device: " << gerr);
  WARN("device: " << solver->DeviceName());

  const double L = 80.0;
  fem::FemMesh mesh = GenerateBoxMesh(12, 12, 12, L, L, L);
  fem::Material steel{210000.0, 0.3};
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(mesh, 0, L, 1e-6);
  load.magnitude = 2000.0;
  load.direction = glm::dvec3(0, 0, -1);
  fem::TopoParams params;
  params.filterRadius = 18.0;
  params.maxIterations = 6;  // enough to see steady-state per-iteration cost

  using clk = std::chrono::steady_clock;
  std::string err;
  const auto t0 = clk::now();
  auto gpuRes = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err,
                                      {}, {}, solver.get());
  const double gpuS = std::chrono::duration<double>(clk::now() - t0).count();
  REQUIRE(gpuRes.has_value());
  WARN("gpu SIMP: " << gpuS << " s for " << gpuRes->iterations << " iters ("
                    << gpuS / gpuRes->iterations << " s/iter), compliance "
                    << gpuRes->complianceHistory.back());

  const auto t1 = clk::now();
  auto cpuRes = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err);
  const double cpuS = std::chrono::duration<double>(clk::now() - t1).count();
  REQUIRE(cpuRes.has_value());
  WARN("cpu SIMP: " << cpuS << " s for " << cpuRes->iterations << " iters ("
                    << cpuS / cpuRes->iterations << " s/iter), compliance "
                    << cpuRes->complianceHistory.back());
}

#endif  // HAVE_FEM_SOLVER
