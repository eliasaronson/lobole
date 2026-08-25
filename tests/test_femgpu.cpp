#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "femgpu/ComputeContext.h"
#include "femgpu/TopoSpmv.h"
#include "gpu_sanity_spv.h"
#include "topo_testmesh.h"

#include <algorithm>
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
  if (!ctx && whyNot)
    *whyNot = err;
  return ctx.get();
}

#define REQUIRE_GPU()                                                          \
  std::string whyNot;                                                          \
  gpu::ComputeContext* ctx = Ctx(&whyNot);                                     \
  if (!ctx)                                                                    \
  SKIP("no usable compute device: " << whyNot)

// Deterministic pseudo-random floats in [-1, 1] (no <random> so values are
// identical across platforms/library versions).
float Rand01(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
}
float RandSym(uint32_t& state) { return 2.0f * Rand01(state) - 1.0f; }

} // namespace

TEST_CASE("femgpu: sanity kernel round-trips through the compute pipeline",
          "[femgpu]") {
  REQUIRE_GPU();
  INFO("device: " << ctx->DeviceName());

  const uint32_t n = 1000;
  gpu::Buffer buf;
  std::string err;
  REQUIRE(ctx->CreateBuffer(n * sizeof(float), buf, &err));
  float* data = static_cast<float*>(buf.mapped);
  for (uint32_t i = 0; i < n; ++i)
    data[i] = static_cast<float>(i);

  gpu::Pipeline pipe;
  REQUIRE(ctx->CreatePipeline(k_gpu_sanity_spv, k_gpu_sanity_spv_size, 1,
                              sizeof(uint32_t), pipe, &err));
  REQUIRE(ctx->Dispatch(pipe, {&buf}, &n, sizeof(n), (n + 63) / 64, 1, &err));

  int wrong = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (data[i] != static_cast<float>(i) * 2.0f + 1.0f)
      ++wrong;
  CHECK(wrong == 0);

  ctx->Destroy(pipe);
  ctx->Destroy(buf);
}

#ifdef HAVE_FEM_SOLVER

#include "core/fem/FemSolverInternal.h"

namespace {

// Packs the reduced system for the GPU exactly the way TopologyOptimize
// assembles it on the CPU: per-tet 30x30 Ke at full modulus (fp32-cast), DOF
// slots node-major with -1 marking fixed DOFs.
gpu::SpmvSystem PackSystem(const fem::FemMesh& mesh,
                           const std::vector<int>& fixedNodes, double E0,
                           double nu) {
  using namespace fem::internal;
  const auto dofs =
      BuildDofMap(static_cast<int>(mesh.nodes.size()), fixedNodes);
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
      if (dof[r] < 0)
        continue;
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

// The same, with the VECTOR in fp64 as well as the accumulation. Iterative
// refinement needs it: its whole mechanism is computing r = F - K u more
// accurately than the solve that produced u, and rounding u back to fp32
// before applying K throws away exactly the digits being bought. (K's own
// entries stay fp32 — `ke` is float — so refinement converges to the solution
// of the fp32-rounded operator, which is the right target: that rounding is
// far below the discretization error.)
std::vector<double> CpuApply64(const gpu::SpmvSystem& sys,
                               const std::vector<float>& scales,
                               const std::vector<double>& p) {
  std::vector<double> Ap(sys.nFree, 0.0);
  for (int e = 0; e < sys.nElems; ++e) {
    const int32_t* dof = &sys.dof[static_cast<size_t>(e) * 30];
    const float* ke = &sys.ke[static_cast<size_t>(e) * 900];
    for (int r = 0; r < 30; ++r) {
      if (dof[r] < 0)
        continue;
      double acc = 0.0;
      for (int c = 0; c < 30; ++c)
        if (dof[c] >= 0)
          acc += static_cast<double>(ke[r * 30 + c]) * p[dof[c]];
      Ap[dof[r]] += static_cast<double>(scales[e]) * acc;
    }
  }
  return Ap;
}

// Relative infinity-norm error of the GPU result against the oracle.
double RelInfError(const std::vector<float>& gpu,
                   const std::vector<double>& ref) {
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
  for (int i = 0; i < sys.nFree; ++i)
    p[i] = RandSym(state);

  std::vector<float> Ap;
  REQUIRE(spmv->Apply(p, Ap, &err));
  const std::vector<double> ref = CpuApply(sys, scales, p);

  const double relErr = RelInfError(Ap, ref);
  INFO("elements: " << sys.nElems << "  free DOFs: " << sys.nFree
                    << "  rel-inf error: " << relErr);
  CHECK(relErr < 1e-4); // fp32 accumulation noise; typically ~1e-6

  // Repeat with uniform full stiffness to also cover the un-scaled path (and
  // prove Apply is repeatable — the scatter zero-init works).
  std::fill(scales.begin(), scales.end(), 1.0f);
  REQUIRE(spmv->SetScales(scales, &err));
  std::vector<float> Ap2;
  REQUIRE(spmv->Apply(p, Ap2, &err));
  const std::vector<double> ref2 = CpuApply(sys, scales, p);
  CHECK(RelInfError(Ap2, ref2) < 1e-4);
}

} // namespace

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
#include "cad/CADKernel.h"
#include "core/fem/FemMesher.h"

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
#endif // HAVE_NETGEN

// ---- GPU1: the on-device CG solver ----------------------------------------

#include "core/fem/FemTopo.h"
#include "femgpu/TopoCgSolver.h"
#include "femgpu/TopoGpuSolver.h"
#include "femgpu/TopoMultigrid.h"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCore>

#include <chrono>

namespace {

// Flat node coordinates for the multigrid builders.
std::vector<double> FlatNodes(const fem::FemMesh& mesh) {
  std::vector<double> out;
  out.reserve(mesh.nodes.size() * 3);
  for (const auto& p : mesh.nodes) {
    out.push_back(p.x);
    out.push_back(p.y);
    out.push_back(p.z);
  }
  return out;
}

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
double ResidualNorm(const gpu::SpmvSystem& sys,
                    const std::vector<float>& scales,
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

} // namespace

TEST_CASE("femgpu: element energies match the CPU oracle (GPU4 GATE)",
          "[femgpu][gate]") {
  REQUIRE_GPU();
  INFO("device: " << ctx->DeviceName());

  auto check = [&](const fem::FemMesh& mesh) {
    const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
    REQUIRE(fixed.size() > 3);
    const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 210000.0, 0.3);

    std::string err;
    auto cg = gpu::TopoCgSolver::Create(*ctx, sys, &err);
    INFO(err);
    REQUIRE(cg);

    uint32_t state = 4242;
    std::vector<float> u(sys.nFree);
    for (int i = 0; i < sys.nFree; ++i)
      u[i] = RandSym(state);

    std::vector<float> se;
    REQUIRE(cg->ElementEnergies(u, se, &err));
    REQUIRE(se.size() == static_cast<size_t>(sys.nElems));

    // fp64 oracle from the same fp32 inputs: se_e = ue^T Ke0 ue.
    double maxRef = 0.0, maxErr = 0.0;
    for (int e = 0; e < sys.nElems; ++e) {
      const int32_t* dof = &sys.dof[static_cast<size_t>(e) * 30];
      const float* ke = &sys.ke[static_cast<size_t>(e) * 900];
      double ue[30];
      for (int s2 = 0; s2 < 30; ++s2)
        ue[s2] = dof[s2] >= 0 ? static_cast<double>(u[dof[s2]]) : 0.0;
      double ref = 0.0;
      for (int a = 0; a < 30; ++a) {
        double row = 0.0;
        for (int b = 0; b < 30; ++b)
          row += static_cast<double>(ke[a * 30 + b]) * ue[b];
        ref += ue[a] * row;
      }
      maxRef = std::max(maxRef, std::abs(ref));
      maxErr = std::max(maxErr, std::abs(static_cast<double>(se[e]) - ref));
    }
    const double rel = maxRef > 0 ? maxErr / maxRef : maxErr;
    INFO("elements: " << sys.nElems << "  rel-inf error: " << rel);
    CHECK(rel < 1e-4); // fp32 accumulation noise; typically ~1e-6
  };

  SECTION("structured tet10 box") {
    check(GenerateBoxMesh(8, 4, 4, 80, 40, 40));
  }
#ifdef HAVE_NETGEN
  SECTION("real Netgen mesh") {
    CADShape box = CADKernel::Instance().CreateBox(60, 30, 30);
    fem::MeshSettings ms;
    ms.maxSize = 10.0;
    std::string err;
    auto mesh = fem::Mesh(box.GetShape(), ms, &err);
    INFO(err);
    REQUIRE(mesh.has_value());
    check(*mesh);
  }
#endif
}

TEST_CASE("femgpu: CG solves the reduced system to tolerance", "[femgpu][cg]") {
  REQUIRE_GPU();
  INFO("device: " << ctx->DeviceName());

  fem::FemMesh mesh = GenerateBoxMesh(6, 3, 3, 60, 30, 30);
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
  const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 210000.0, 0.3);

  // Cover ALL preconditioners with the same correctness bars: Jacobi (the
  // simple default), Chebyshev (GPU2), and the multigrid V-cycle (GPU3).
  gpu::CgOptions opts;
  const std::string precondName = GENERATE("jacobi", "chebyshev", "multigrid");
  INFO("preconditioner: " << precondName);
  std::string err;
  if (precondName == "jacobi") {
    opts.precond = gpu::CgOptions::Precond::Jacobi;
  } else if (precondName == "chebyshev") {
    opts.precond = gpu::CgOptions::Precond::Chebyshev;
  } else {
    const auto dofs =
        fem::internal::BuildDofMap(static_cast<int>(mesh.nodes.size()), fixed);
    opts.mg = gpu::MakeMgGpuData(FlatNodes(mesh), sys, dofs.map, &err);
    INFO(err);
    REQUIRE(opts.mg);
    opts.precond = gpu::CgOptions::Precond::Multigrid;
    opts.chebDegree = 3;
    opts.chebRatio = 15.0f;
  }

  auto cg = gpu::TopoCgSolver::Create(*ctx, sys, &err, opts);
  INFO(err);
  REQUIRE(cg);

  // A load-like right-hand side (deterministic).
  uint32_t state = 42;
  std::vector<float> f(sys.nFree);
  for (int i = 0; i < sys.nFree; ++i)
    f[i] = RandSym(state);

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
    CHECK(res < 1e-3); // the floor is far better than this in practice
    CHECK(res < 2.0 * static_cast<double>(stats.relResidual) + 1e-9);
  }
}

// ---- Where the fp32 solver stops working (diagnostics, not gates) ---------
//
// The 2026-08-23 round found the GPU path returning a ZERO displacement field
// on a beam-shaped part (docs/topopt/plan.md). `TopologyOptimize` falls back
// to the CPU when that happens, so this is a performance limit rather than a
// correctness one — but fixing the numerics needs to start from measurements,
// and these two are them. Hidden ([.]) : run with `femgpu_tests [fp32floor]`.
//
// Measured on an RTX 4060 Laptop, tolerance 1e-5, uniform stiffness, all
// meshes built from 10 mm CUBIC cells so element SHAPE is held constant:
//
//   proportions   tets   MG true residual
//   1 : 1          384   6.6e-06   <- converges
//   2.5 : 1        960   2.6e-05
//   5 : 1         1920   2.8e-04
//   10 : 1         480   1.2e-03
//   10 : 1        3840   1.7e-03
//   20 : 1         240   2.6e-03
//   20 : 1         960   5.1e-02
//   10:1 FLAT      600   3.9e-05   <- thin but not long: nearly fine
//
// The attainable floor tracks the structure's SLENDERNESS, roughly an order
// of magnitude per doubling, and NOT the element count (240 tets at 20:1 is
// worse than 3840 at 10:1). Thinness alone is not the problem — the flat
// plate is one element thick and lands near the chunky box; length-to-depth
// is. Jacobi shows the same ordering ~10x more slowly.
TEST_CASE("femgpu probe: the fp32 residual floor vs slenderness",
          "[.][fp32floor]") {
  REQUIRE_GPU();
  struct Case {
    const char* name;
    int nx, ny, nz;
    double Lx, Ly, Lz;
  };
  const Case cases[] = {
      {"cube    4x4x4   (384 tets, 1:1)", 4, 4, 4, 40, 40, 40},
      {"chunky 10x4x4   (960 tets, 2.5:1)", 10, 4, 4, 100, 40, 40},
      {"beam   20x4x4  (1920 tets, 5:1)", 20, 4, 4, 200, 40, 40},
      {"beam   20x2x2   (480 tets, 10:1)", 20, 2, 2, 200, 20, 20},
      {"beam   40x4x4  (3840 tets, 10:1)", 40, 4, 4, 400, 40, 40},
      {"beam   20x1x2   (240 tets, 20:1)", 20, 1, 2, 200, 10, 20},
      {"beam   40x2x2   (960 tets, 20:1)", 40, 2, 2, 400, 20, 20},
      {"plate  10x1x10  (600 tets, 10:1 flat)", 10, 1, 10, 100, 10, 100},
  };
  for (const Case& c : cases) {
    fem::FemMesh mesh = GenerateBoxMesh(c.nx, c.ny, c.nz, c.Lx, c.Ly, c.Lz);
    const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
    const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 210000.0, 0.3);
    uint32_t state = 42;
    std::vector<float> f(sys.nFree);
    for (int i = 0; i < sys.nFree; ++i)
      f[i] = RandSym(state);
    const std::vector<float> scales(sys.nElems, 1.0f);
    for (const char* pc : {"jacobi", "multigrid"}) {
      std::string err;
      gpu::CgOptions opts;
      if (std::string(pc) == "jacobi") {
        opts.precond = gpu::CgOptions::Precond::Jacobi;
      } else {
        const auto dofs = fem::internal::BuildDofMap(
            static_cast<int>(mesh.nodes.size()), fixed);
        opts.mg = gpu::MakeMgGpuData(FlatNodes(mesh), sys, dofs.map, &err);
        REQUIRE(opts.mg);
        opts.precond = gpu::CgOptions::Precond::Multigrid;
        opts.chebDegree = 3;
        opts.chebRatio = 15.0f;
      }
      auto cg = gpu::TopoCgSolver::Create(*ctx, sys, &err, opts);
      REQUIRE(cg);
      REQUIRE(cg->SetScales(scales, &err));
      std::vector<float> x(sys.nFree, 0.0f);
      gpu::CgStats stats;
      REQUIRE(cg->Solve(f, x, 1e-5f, 20000, &stats, &err));
      std::printf("%-40s %-10s nFree %5d  iters %5d  true %.2e  converged %d\n",
                  c.name, pc, sys.nFree, stats.iterations,
                  ResidualNorm(sys, scales, x, f), (int)stats.converged);
    }
  }
}

// ...and what turns "a bad floor" into "no progress at all". Two ingredients,
// and it takes BOTH. Measured on the 20:1 beam above, multigrid, tol 1e-5:
//
//   stiffness field        RHS        true residual   max|x|
//   uniform 1.0            random     2.3e-03         1.4e-02
//   uniform 0.027          random     2.3e-03         5.4e-01
//   uniform 1.0            bending    1.0e-01         7.6e-01
//   uniform 0.027          bending    9.7e-02         2.8e+01
//   frozen skin + 0.027    random     1.7e-02         3.0e-01
//   frozen skin + 0.027    bending    1.00e+00        0.0e+00   <- the bug
//
// The frozen support/load skin is SIMP's own doing: those elements are held
// at density 1 while the bulk starts at the volume fraction cubed, so the
// first solve carries a 37:1 stiffness jump across one element face. With a
// bending load on top of it the CG never finds a single new low and returns
// its initial guess — which is where the zero displacement field came from.
// A uniform scale is irrelevant (the 1.0 and 0.027 rows agree), so this is
// not about magnitudes.
TEST_CASE("femgpu probe: what zeroes the slender solve", "[.][fp32floor]") {
  REQUIRE_GPU();
  fem::FemMesh mesh = GenerateBoxMesh(20, 1, 2, 200, 10, 20);
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
  const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 200000.0, 0.3);
  const auto dofs =
      fem::internal::BuildDofMap(static_cast<int>(mesh.nodes.size()), fixed);

  uint32_t st = 42;
  std::vector<float> fRand(sys.nFree);
  for (int i = 0; i < sys.nFree; ++i)
    fRand[i] = RandSym(st);
  // 100 N over the free-end face, pushing -Y: the load the round placed.
  std::vector<float> fTip(sys.nFree, 0.0f);
  int tipNodes = 0;
  for (const auto& n : mesh.nodes)
    if (std::abs(n.x - 200.0) < 1e-6)
      ++tipNodes;
  for (size_t n = 0; n < mesh.nodes.size(); ++n)
    if (std::abs(mesh.nodes[n].x - 200.0) < 1e-6) {
      const int g = dofs.map[3 * n + 1];
      if (g >= 0)
        fTip[g] = -100.0f / static_cast<float>(tipNodes);
    }

  // SIMP's first solve: the support/load skin frozen solid at 1.0, the bulk
  // at the volume fraction cubed.
  std::vector<float> mixed(sys.nElems, 0.027f);
  int frozen = 0;
  for (int e = 0; e < sys.nElems; ++e) {
    bool touches = false;
    for (int i = 0; i < 10; ++i) {
      const double x = mesh.nodes[mesh.tets[e][i]].x;
      touches = touches || x < 1e-6 || x > 200.0 - 1e-6;
    }
    if (touches) {
      mixed[e] = 1.0f;
      ++frozen;
    }
  }
  std::printf("(frozen elements: %d of %d)\n", frozen, sys.nElems);

  std::vector<float> mixedP1(sys.nElems, 0.3f);
  for (int e = 0; e < sys.nElems; ++e)
    if (mixed[e] == 1.0f)
      mixedP1[e] = 1.0f;
  for (const float sc : {1.0f, 0.027f, -1.0f, -2.0f}) // <0 = the mixed fields
    for (int rhs = 0; rhs < 2; ++rhs) {
      const std::vector<float>& f = rhs == 0 ? fRand : fTip;
      const std::vector<float> scales =
          sc == -1.0f   ? mixed
          : sc == -2.0f ? mixedP1
                        : std::vector<float>(sys.nElems, sc);
      std::string err;
      gpu::CgOptions opts;
      opts.mg = gpu::MakeMgGpuData(FlatNodes(mesh), sys, dofs.map, &err);
      REQUIRE(opts.mg);
      opts.precond = gpu::CgOptions::Precond::Multigrid;
      opts.chebDegree = 3;
      opts.chebRatio = 15.0f;
      auto cg = gpu::TopoCgSolver::Create(*ctx, sys, &err, opts);
      REQUIRE(cg);
      REQUIRE(cg->SetScales(scales, &err));
      std::vector<float> x(sys.nFree, 0.0f);
      gpu::CgStats stats;
      REQUIRE(cg->Solve(f, x, 1e-5f, 20000, &stats, &err));
      double xmax = 0.0;
      for (float v : x)
        xmax = std::max(xmax, (double)std::abs(v));
      std::printf("%-22s %-8s iters %5d  true %.2e  max|x| %.3e\n",
                  sc == -1.0f   ? "frozen skin + 0.027"
                  : sc == -2.0f ? "frozen skin + 0.3 (p=1)"
                  : sc == 1.0f  ? "uniform 1.0"
                                : "uniform 0.027",
                  rhs == 0 ? "random" : "bending", stats.iterations,
                  ResidualNorm(sys, scales, x, f), xmax);
    }
}

// Which mechanism actually kills the fp32 solve: the operator's conditioning,
// or the diagonal spread the frozen skin introduces? Dense fp64 eigenvalues of
// the reduced K, small meshes only. Measured 2026-08-24:
//
//   case                              diag spread   kappa     after diag-scale
//   cube 1:1  uniform                     12        3.3e+03      2.2e+03
//   beam 10:1 uniform                     12        3.7e+06      2.4e+06
//   beam 10:1 frozen skin + 0.027        220        8.2e+07      1.6e+07
//   beam 20:1 uniform                     12        1.4e+07      9.4e+06
//   beam 20:1 frozen skin + 0.027        170        2.7e+08      5.3e+07
//   beam 20:1 frozen skin + 0.3 (p=1)     15        3.2e+07      1.2e+07
//
// Both mechanisms are real and they multiply: slenderness costs ~4x in kappa
// per doubling (1:1 -> 10:1 is ~1000x), and SIMP's frozen skin costs another
// ~20x on top. The achieved residual floors track kappa monotonically, and
// the case that returns ZEROS is the one at 2.7e+08 — past what 7 decimal
// digits can do anything with.
//
// Symmetric diagonal scaling (what a Jacobi preconditioner does, but applied
// to the matrix itself so the fp32 recurrence sees unit diagonals) is worth
// only 2-5x. It is not the fix. Dropping the contrast is worth 8.5x, and
// mixed precision is worth ~9 decimal digits — see docs/topopt/plan.md.
// GPU5.0 — the gate for docs/topopt/gpu5-plan.md. No production code: this
// exists to answer one question cheaply, and to KILL the plan if the answer
// is no.
//
// The plan proposes fp64 iterative refinement around the fp32 solve:
//
//     u = 0
//     repeat:  r = F - K u        (fp64, on the host)
//              solve K d = r      (fp32, on the GPU, loosely)
//              u += d
//
// It only works if the inner fp32 solve is a CONTRACTION on the systems that
// matter — if each outer step shrinks the residual by some rho < 1. That is
// not obvious here: the whole reason this plan exists is that on the 20:1
// beam with SIMP's frozen skin the inner solve makes NO progress at all and
// returns its initial guess, and an inner solve that returns its input
// unchanged gives d = 0 and rho = 1 exactly. Refinement would stall.
//
// So: run the loop on that exact system and print rho. Gate (from the plan):
// the outer residual reaches 1e-3 within 4 steps from a warm start that is
// not zero. Cold start is measured too, because the first SIMP solve has no
// warm start and is where the round found the zeros.
//
// RESULT (2026-08-25, RTX 4060 Laptop, 5 runs). The plan SURVIVES, with two
// amendments, and the run-to-run spread is itself the finding — the inner
// solve is nondeterministic on this system, returning 128, 272 or 368
// iterations from byte-identical inputs.
//
//   start                     reached 1e-3 at step   rho once productive
//   warm (non-zero)           4, 4, 4, 5, 5          0.009 - 0.10
//   cold (u = 0)              4, 5, never x3         0.016 - 0.19
//   cold, inner guess seeded  4, 4, 5, 5, 5          0.008 - 0.16
//
// 1. FROM A WARM START, REFINEMENT WORKS — every run converged, and rho in
//    the productive regime is 0.01-0.1, i.e. a decimal digit or two per outer
//    step. That is the gate's own condition and it is met.
//
// 2. FROM ZERO IT CAN STALL FOREVER, AT EXACTLY rho = 1.000. Three runs in
//    five printed this, all six steps identical:
//
//      step 1  ||r||/||F|| 1.000e+00   inner iters 128   max|d| 0.00e+00
//      step 2  ||r||/||F|| 1.000e+00   rho 1.000  ...    max|d| 0.00e+00
//
//    The inner solver's stall policy returns its BEST iterate, which from a
//    zero start it never improves on, so d = 0 -> u unchanged -> r unchanged
//    -> the next step has identical input. Refinement cannot rescue this,
//    because refinement's correction IS the inner solve: zero is a fixed
//    point of the whole loop. More outer steps buy nothing, which is why the
//    plan's gate specified a non-zero warm start rather than trusting the
//    cold case.
//
// 3. PERTURBING THE INNER INITIAL GUESS BREAKS THE FIXED POINT — 5 runs of 5
//    made progress, none stalled. One RNG pass over nFree floats, and it is
//    the cheapest known mitigation for the first (cold) SIMP solve.
//
// The "within 4 steps" budget is too tight to design against: the observed
// range is 4-6 even when it works, and it moves between runs for the reason
// in the header. Cost per outer step here is 200-650 inner iterations.
//
// Rescaling matters and is not incidental. r shrinks by orders of magnitude
// per step, and handing a tiny vector to an fp32 solver wastes its exponent
// range; each step normalises r to unit norm before the solve and scales the
// correction back on the way out.
TEST_CASE("femgpu probe: GPU5.0 does fp64 refinement contract?",
          "[.][fp32floor]") {
  REQUIRE_GPU();
  // The exact case that returns zeros (see "what zeroes the slender solve").
  fem::FemMesh mesh = GenerateBoxMesh(20, 1, 2, 200, 10, 20);
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
  const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 200000.0, 0.3);
  const auto dofs =
      fem::internal::BuildDofMap(static_cast<int>(mesh.nodes.size()), fixed);

  // 100 N over the free-end face, pushing -Y: the load the round placed.
  std::vector<float> f(sys.nFree, 0.0f);
  int tipNodes = 0;
  for (const auto& n : mesh.nodes)
    if (std::abs(n.x - 200.0) < 1e-6)
      ++tipNodes;
  for (size_t n = 0; n < mesh.nodes.size(); ++n)
    if (std::abs(mesh.nodes[n].x - 200.0) < 1e-6) {
      const int g = dofs.map[3 * n + 1];
      if (g >= 0)
        f[g] = -100.0f / static_cast<float>(tipNodes);
    }

  // SIMP's first solve: support/load skin frozen at 1.0, bulk at vf cubed.
  std::vector<float> mixed(sys.nElems, 0.027f);
  for (int e = 0; e < sys.nElems; ++e)
    for (int i = 0; i < 10; ++i) {
      const double x = mesh.nodes[mesh.tets[e][i]].x;
      if (x < 1e-6 || x > 200.0 - 1e-6) {
        mixed[e] = 1.0f;
        break;
      }
    }

  std::string err;
  gpu::CgOptions opts;
  opts.mg = gpu::MakeMgGpuData(FlatNodes(mesh), sys, dofs.map, &err);
  REQUIRE(opts.mg);
  opts.precond = gpu::CgOptions::Precond::Multigrid;
  opts.chebDegree = 3;
  opts.chebRatio = 15.0f;
  auto cg = gpu::TopoCgSolver::Create(*ctx, sys, &err, opts);
  REQUIRE(cg);

  double fnorm = 0.0;
  for (int i = 0; i < sys.nFree; ++i)
    fnorm += static_cast<double>(f[i]) * static_cast<double>(f[i]);
  fnorm = std::sqrt(fnorm);

  // Returns the step at which the outer residual first went below 1e-3, or 0.
  // `seed` != 0 gives each inner solve a small random initial guess instead
  // of zero. The reason is specific: the inner solver's stall policy returns
  // its BEST iterate, and when it cannot improve on a zero start the best
  // iterate IS zero — so d = 0, the outer residual is unchanged, and that
  // whole refinement step is spent for nothing. Breaking the degeneracy costs
  // one RNG pass.
  const auto refine = [&](std::vector<double> u, const char* label, int steps,
                          uint32_t seed = 0) {
    std::printf("\n--- %s ---\n", label);
    double prev = -1.0;
    int hit = 0;
    for (int k = 1; k <= steps; ++k) {
      const std::vector<double> Ku = CpuApply64(sys, mixed, u);
      std::vector<double> r(sys.nFree);
      double rn = 0.0;
      for (int i = 0; i < sys.nFree; ++i) {
        r[i] = static_cast<double>(f[i]) - Ku[i];
        rn += r[i] * r[i];
      }
      rn = std::sqrt(rn);
      const double rel = rn / fnorm;
      const double rho = prev < 0.0 ? 0.0 : rel / prev;
      std::printf("  step %d  ||r||/||F|| %.3e", k, rel);
      if (prev >= 0.0)
        std::printf("   rho %.3f", rho);
      if (rel < 1e-3 && hit == 0)
        hit = k;
      // Normalise into fp32's comfortable range, solve, scale back.
      const double s = rn > 0.0 ? 1.0 / rn : 1.0;
      std::vector<float> rs(sys.nFree);
      for (int i = 0; i < sys.nFree; ++i)
        rs[i] = static_cast<float>(r[i] * s);
      std::vector<float> d(sys.nFree, 0.0f);
      if (seed != 0)
        for (int i = 0; i < sys.nFree; ++i)
          d[i] = 1e-3f * RandSym(seed);
      gpu::CgStats st{};
      REQUIRE(cg->Solve(rs, d, 1e-4f, 4000, &st, &err));
      double dmax = 0.0;
      for (float v : d)
        dmax = std::max(dmax, static_cast<double>(std::abs(v)));
      std::printf("   inner iters %5d  max|d| %.2e\n", st.iterations, dmax);
      for (int i = 0; i < sys.nFree; ++i)
        u[i] += static_cast<double>(d[i]) / s;
      prev = rel;
    }
    return hit;
  };

  REQUIRE(cg->SetScales(mixed, &err));

  // Cold: what the first SIMP solve does. This is the configuration that
  // returned a zero displacement field.
  const int cold = refine(std::vector<double>(sys.nFree, 0.0), "cold start", 6);

  // Warm: what every LATER SIMP solve does — the previous iterate, carried
  // over from a density field one step away. Modelled here by solving the
  // uniform-0.027 system (which converges) and starting from its answer.
  const std::vector<float> uniform(sys.nElems, 0.027f);
  REQUIRE(cg->SetScales(uniform, &err));
  std::vector<float> x0(sys.nFree, 0.0f);
  gpu::CgStats st0{};
  REQUIRE(cg->Solve(f, x0, 1e-5f, 20000, &st0, &err));
  std::vector<double> warmStart(sys.nFree);
  for (int i = 0; i < sys.nFree; ++i)
    warmStart[i] = static_cast<double>(x0[i]);
  double wmax = 0.0;
  for (double v : warmStart)
    wmax = std::max(wmax, std::abs(v));
  std::printf("\n(warm start from the uniform-0.027 solve: max|u| %.3e)\n",
              wmax);
  REQUIRE(wmax > 0.0); // the gate says "a warm start that is not zero"
  REQUIRE(cg->SetScales(mixed, &err));
  const int warm = refine(warmStart, "warm start", 6);

  // ...and the same cold start with the degeneracy broken.
  REQUIRE(cg->SetScales(mixed, &err));
  const int coldSeeded = refine(std::vector<double>(sys.nFree, 0.0),
                                "cold start, inner guess "
                                "perturbed",
                                6, 12345u);

  std::printf("\nGPU5.0 GATE: 1e-3 within 4 steps\n");
  std::printf("  cold                     : %s (step %d)\n",
              cold > 0 && cold <= 4 ? "PASS" : "fail", cold);
  std::printf("  cold, inner guess seeded : %s (step %d)\n",
              coldSeeded > 0 && coldSeeded <= 4 ? "PASS" : "fail", coldSeeded);
  std::printf("  warm                     : %s (step %d)\n",
              warm > 0 && warm <= 4 ? "PASS" : "fail", warm);
  std::printf("(0 = never reached 1e-3 within the steps run)\n");
}

TEST_CASE("femgpu probe: condition numbers", "[.][fp32floor]") {
  struct Case {
    const char* name;
    int nx, ny, nz;
    double Lx, Ly, Lz;
    bool frozenSkin;
  };
  const Case cases[] = {
      {"cube  4x4x4  1:1     uniform", 4, 4, 4, 40, 40, 40, false},
      {"beam 20x1x2  20:1    uniform", 20, 1, 2, 200, 10, 20, false},
      {"beam 20x1x2  20:1    frozen skin + 0.027", 20, 1, 2, 200, 10, 20, true},
      {"beam 20x1x2  20:1    frozen skin + 0.3 (p=1)", 20, 1, 2, 200, 10, 20,
       true},
      {"beam 20x2x2  10:1    uniform", 20, 2, 2, 200, 20, 20, false},
      {"beam 20x2x2  10:1    frozen skin + 0.027", 20, 2, 2, 200, 20, 20, true},
  };
  for (const Case& c : cases) {
    fem::FemMesh mesh = GenerateBoxMesh(c.nx, c.ny, c.nz, c.Lx, c.Ly, c.Lz);
    const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
    const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 200000.0, 0.3);
    const bool p1 = std::string(c.name).find("p=1") != std::string::npos;
    std::vector<double> scale(sys.nElems,
                              c.frozenSkin ? (p1 ? 0.3 : 0.027) : 1.0);
    if (c.frozenSkin)
      for (int e = 0; e < sys.nElems; ++e)
        for (int i = 0; i < 10; ++i) {
          const double x = mesh.nodes[mesh.tets[e][i]].x;
          if (x < 1e-6 || x > c.Lx - 1e-6)
            scale[e] = 1.0;
        }

    Eigen::MatrixXd K = Eigen::MatrixXd::Zero(sys.nFree, sys.nFree);
    for (int e = 0; e < sys.nElems; ++e) {
      const int32_t* dof = &sys.dof[static_cast<size_t>(e) * 30];
      const float* ke = &sys.ke[static_cast<size_t>(e) * 900];
      for (int r = 0; r < 30; ++r) {
        if (dof[r] < 0)
          continue;
        for (int cc = 0; cc < 30; ++cc)
          if (dof[cc] >= 0)
            K(dof[r], dof[cc]) += scale[e] * (double)ke[r * 30 + cc];
      }
    }
    // Symmetric diagonal (Jacobi) scaling: D^-1/2 K D^-1/2, unit diagonal.
    Eigen::VectorXd d = K.diagonal();
    Eigen::VectorXd s = d.array().rsqrt();
    Eigen::MatrixXd Ks = s.asDiagonal() * K * s.asDiagonal();

    auto kappa = [](const Eigen::MatrixXd& M) {
      Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(M,
                                                        Eigen::EigenvaluesOnly);
      const auto& ev = es.eigenvalues();
      return ev(ev.size() - 1) / ev(0);
    };
    std::printf("%-38s n %4d  diag spread %.1e  kappa %.3e  after diag-scale "
                "%.3e\n",
                c.name, sys.nFree, d.maxCoeff() / d.minCoeff(), kappa(K),
                kappa(Ks));
  }
}

// Does the existing (disabled) penalty continuation make the GPU path usable
// on a beam? ANSWER: no, and it is not free either. Measured 2026-08-24,
// 60 iterations, ramp over the first 15:
//
//   mesh            ramp   CPU compliance   Mnd (grey)   GPU fell back?
//   beam 20x1x2      0        4178          60.7%        yes (res 1.00)
//   beam 20x1x2     15        4176          60.7%        yes (res 1.15)
//   chunky 10x4x4    0       12.65          33.3%        no
//   chunky 10x4x4   15       19.46          34.7%        no
//
// `penaltyRampIters` starts p at 1, so the bulk sits at the volume fraction
// itself (0.3) rather than its cube (0.027): kappa falls 2.7e+08 -> 3.2e+07
// and the FIRST solve goes from zero-progress to a normal floored one
// (1.8e-01, u sane). But the run still falls back later, on a warm-started
// solve that ends up worse than its own starting point — and on the chunky
// mesh, where the GPU works fine, continuation converges to a design 54%
// SOFTER and greyer. Both runs stop on changeTol well inside the budget, so
// that is not iteration starvation; it is a worse local optimum.
//
// Which is what FemTopo.h already says ("sensitivity filtering already yields
// a discrete design at constant p"), now with a number attached. Do not turn
// it on for the solver's sake.
TEST_CASE("femgpu probe: penalty ramp on a slender part", "[.][fp32floor]") {
  REQUIRE_GPU();
  struct M {
    const char* name;
    int nx, ny, nz;
    double Lx, Ly, Lz;
  };
  // The slender one (where the GPU fails) AND a chunky one (where it works),
  // because the design question and the solver question are separate: on a
  // 240-element beam the result is grey mush whichever way, so discreteness
  // has to be judged on a mesh that can hold a shape.
  const M meshes[] = {{"beam   20x1x2", 20, 1, 2, 200.0, 10.0, 20.0},
                      {"chunky 10x4x4", 10, 4, 4, 100.0, 40.0, 40.0}};
  for (const M& m : meshes) {
    fem::FemMesh mesh = GenerateBoxMesh(m.nx, m.ny, m.nz, m.Lx, m.Ly, m.Lz);
    fem::Material steel{200000.0, 0.3};
    const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
    fem::FaceLoad load;
    load.type = fem::LoadType::Force;
    load.tris = TrisAtPlane(mesh, 0, m.Lx, 1e-6);
    load.magnitude = 100.0;
    load.direction = glm::dvec3(0, -1, 0);

    for (const double ramp : {0.0, 15.0}) {
      fem::TopoParams params;
      // 60 iterations ramping over the first 15 — a quarter of the budget,
      // the usual schedule. (30 gives the same numbers to four figures: both
      // schedules stop on changeTol first, so the gap below is not iteration
      // starvation.)
      params.maxIterations = 60;
      params.penaltyRampIters = ramp;
      std::string err;
      auto cpu =
          fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err);
      REQUIRE(cpu.has_value());
      std::string gerr;
      auto solver = gpu::TopoGpuSolver::Create(true, &gerr);
      REQUIRE(solver);
      auto gpu = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err,
                                       {}, {}, solver.get());
      REQUIRE(gpu.has_value());
      int flips = 0;
      for (size_t e = 0; e < cpu->density.size(); ++e)
        if (std::abs(gpu->density[e] - cpu->density[e]) > 0.5)
          ++flips;
      // Is the design still black-and-white? Mnd = mean 4x(1-x), the standard
      // SIMP discreteness measure: 0% = fully solid/void, 100% = all grey. The
      // ramp's whole point is a better basin, but it spends iterations at a
      // penalty that does NOT punish grey, so this is the thing to check.
      double mnd = 0.0;
      int grey = 0;
      for (double x : cpu->density) {
        mnd += 4.0 * x * (1.0 - x);
        if (x > 0.1 && x < 0.9)
          ++grey;
      }
      mnd = 100.0 * mnd / cpu->density.size();
      std::printf("              cpu discreteness Mnd %.1f%%  grey elements "
                  "%d/%zu\n",
                  mnd, grey, cpu->density.size());
      std::printf("%s ramp %4.0f  cpu C %.4g vf %.3f | gpu C %.4g vf %.3f  "
                  "flips %d/%zu  fellback: %s\n",
                  m.name, ramp, cpu->complianceHistory.back(),
                  cpu->volumeFractionAchieved, gpu->complianceHistory.back(),
                  gpu->volumeFractionAchieved, flips, cpu->density.size(),
                  gpu->solverFallback.empty() ? "no"
                                              : gpu->solverFallback.c_str());
    }
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
  if (!solver)
    SKIP("no usable compute device: " << gerr);
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

  const auto best = [](const std::vector<double>& hist) {
    return *std::min_element(hist.begin(), hist.end());
  };
  const double cpuBest = best(cpuRes->complianceHistory);
  const double gpuBest = best(gpuRes->complianceHistory);
  INFO("cpu compliance last "
       << cpuRes->complianceHistory.back() << " best " << cpuBest << " ("
       << cpuRes->iterations << " it), gpu last "
       << gpuRes->complianceHistory.back() << " best " << gpuBest << " ("
       << gpuRes->iterations << " it)");
  // SIMP is non-convex and the GPU solve is run-to-run NONDETERMINISTIC
  // (fp32 atomic reduction order), so what can this test assert? Measured
  // over dozens of back-to-back runs against the deterministic CPU result:
  // the DESIGN is the sharp invariant — solid/void flips were 0 and the
  // volume matched in every run, including the bad ones. The compliance
  // readout is NOT sharp: most runs land within ~10% of the CPU value, but
  // ~1 run in 15 reports ~55% high across its WHOLE history while the flips
  // check still passes — same topology, inflated objective readout (grayer
  // boundary elements and/or an under-converged warm-capped CG). A tight
  // compliance band therefore flakes wherever it's set (the old 5%
  // back()-vs-back() check failed ~1 run in 3). Gate on the design; keep
  // compliance as a divergence backstop only (catches NaN / a genuinely
  // broken solve, not readout spread).
  CHECK(gpuBest > 0.3 * cpuBest);
  CHECK(gpuBest < 2.5 * cpuBest);
  CHECK_THAT(gpuRes->volumeFractionAchieved,
             WithinAbs(cpuRes->volumeFractionAchieved, 0.01));
  REQUIRE(gpuRes->density.size() == cpuRes->density.size());
  int flipped = 0;
  double meanDiff = 0.0;
  for (size_t e = 0; e < cpuRes->density.size(); ++e) {
    const double d = std::abs(gpuRes->density[e] - cpuRes->density[e]);
    meanDiff += d;
    if (d > 0.5)
      ++flipped; // solid on one side, void on the other
  }
  meanDiff /= cpuRes->density.size();
  const double flippedFrac = double(flipped) / cpuRes->density.size();
  INFO("mean |density diff|: " << meanDiff
                               << "  solid/void flips: " << flippedFrac);
  CHECK(flippedFrac < 0.10);
  // Gray closeness: catches wholesale density-field divergence that never
  // crosses the 0.5 flip threshold (measured <= 0.096 across all runs).
  CHECK(meanDiff < 0.15);
}

// A dogfooding round (2026-08-23) ran the FIRST thing anyone would try —
// a steel cantilever, fixed at one end, 100 N on the other — and the GPU
// solver returned a ZERO displacement field: compliance 0, every sensitivity
// 0, the volume constraint abandoned (0.107 against a 0.30 target), and the
// SIMP loop stopping after 3 iterations reporting "converged". The CPU path
// on the same study was exact. The gate above missed it because its mesh is
// a CHUNKY box (100x40x40, 10x4x4 cells); the failure needs a SLENDER one,
// which is what a beam-shaped part meshes to.
TEST_CASE("femgpu: a slender cantilever solves on the GPU (GATE)",
          "[femgpu][cg]") {
  std::string gerr;
  auto solver = gpu::TopoGpuSolver::Create(/*allowSoftware=*/true, &gerr);
  if (!solver)
    SKIP("no usable compute device: " << gerr);
  INFO("device: " << solver->DeviceName());

  // 200 x 10 x 20 mm, the proportions of a real beam coupon.
  fem::FemMesh mesh = GenerateBoxMesh(20, 1, 2, 200.0, 10.0, 20.0);
  fem::Material steel{200000.0, 0.3};
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-6);
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(mesh, 0, 200.0, 1e-6);
  load.magnitude = 100.0;
  load.direction = glm::dvec3(0, -1, 0);
  fem::TopoParams params;
  params.maxIterations = 10;

  std::string err;
  auto cpuRes = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err);
  REQUIRE(cpuRes.has_value());
  auto gpuRes = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err,
                                      {}, {}, solver.get());
  INFO(err);
  REQUIRE(gpuRes.has_value());
  INFO("cpu compliance " << cpuRes->complianceHistory.back() << " vf "
                         << cpuRes->volumeFractionAchieved << " ("
                         << cpuRes->iterations << " it), gpu "
                         << gpuRes->complianceHistory.back() << " vf "
                         << gpuRes->volumeFractionAchieved << " ("
                         << gpuRes->iterations << " it)");
  // Either the GPU solves it or it says it could not and the CPU finishes the
  // run; both are pass. Returning zeros and calling them an answer is not.
  INFO("solver fallback: '" << gpuRes->solverFallback << "'");
  // A structure carrying a load has compliance > 0. Zero means the linear
  // solve never moved off its zero initial guess.
  CHECK(gpuRes->complianceHistory.back() > 0.0);
  // The volume constraint is enforced by the OC bisection every iteration,
  // so it holds whatever the compliance does — unless the sensitivities are
  // all zero, which is exactly what a zero displacement field produces.
  CHECK_THAT(gpuRes->volumeFractionAchieved,
             WithinAbs(params.volumeFraction, 0.01));
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
  uint32_t state = 99;
  std::vector<float> f(sys.nFree);
  for (int i = 0; i < sys.nFree; ++i)
    f[i] = RandSym(state);

  using clk = std::chrono::steady_clock;
  for (const char* name : {"jacobi", "multigrid"}) {
    gpu::CgOptions opts;
    if (std::string(name) == "jacobi") {
      opts.precond = gpu::CgOptions::Precond::Jacobi;
    } else {
      const auto dofs = fem::internal::BuildDofMap(
          static_cast<int>(mesh.nodes.size()), fixed);
      opts.mg = gpu::MakeMgGpuData(FlatNodes(mesh), sys, dofs.map, &err);
      REQUIRE(opts.mg);
      opts.precond = gpu::CgOptions::Precond::Multigrid;
      opts.chebDegree = 3;
      opts.chebRatio = 15.0f;
    }
    auto cg = gpu::TopoCgSolver::Create(*ctx, sys, &err, opts);
    REQUIRE(cg);
    std::vector<float> x(sys.nFree, 0.0f);
    for (int pass = 0; pass < 3; ++pass) {
      // Slightly different scales each pass, like consecutive SIMP
      // iterations (so the warm start helps but is not exact).
      std::vector<float> scales = SimpScales(sys.nElems, 1000 + pass);
      REQUIRE(cg->SetScales(scales, &err));
      gpu::CgStats stats;
      const auto t0 = clk::now();
      REQUIRE(cg->Solve(f, x, 1e-5f, 40000, &stats, &err));
      const double ms =
          std::chrono::duration<double, std::milli>(clk::now() - t0).count();
      WARN(name << " solve " << pass << ": " << ms << " ms, "
                << stats.iterations << " outer iters, rel res "
                << stats.relResidual << " (fp64 check "
                << ResidualNorm(sys, scales, x, f) << ")");
    }
  }

  // CPU reference: Eigen CG-IC (the shipping CPU path) on the same system.
  {
    const std::vector<float> scales = SimpScales(sys.nElems, 1000);
    std::vector<Eigen::Triplet<double>> trip;
    for (int e = 0; e < sys.nElems; ++e) {
      const int32_t* dof = &sys.dof[static_cast<size_t>(e) * 30];
      const float* ke = &sys.ke[static_cast<size_t>(e) * 900];
      for (int a = 0; a < 30; ++a) {
        if (dof[a] < 0)
          continue;
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
    for (int i = 0; i < sys.nFree; ++i)
      F(i) = f[i];
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

// ---- GPU3 / MG0: the multigrid coarse space (pure CPU, no device) ---------

namespace {

// Eigen Jacobi-PCG (DiagonalPreconditioner) iteration count — the baseline
// the MG0 gate is measured against.
int JacobiIters(const gpu::SpmvSystem& sys, const std::vector<float>& scales,
                const Eigen::VectorXd& f, double tol, int maxIter) {
  std::vector<Eigen::Triplet<double>> trip;
  for (int e = 0; e < sys.nElems; ++e) {
    const int32_t* dof = &sys.dof[static_cast<size_t>(e) * 30];
    const float* ke = &sys.ke[static_cast<size_t>(e) * 900];
    for (int a = 0; a < 30; ++a) {
      if (dof[a] < 0)
        continue;
      for (int b = 0; b < 30; ++b)
        if (dof[b] >= 0)
          trip.emplace_back(dof[a], dof[b],
                            static_cast<double>(scales[e]) *
                                static_cast<double>(ke[a * 30 + b]));
    }
  }
  Eigen::SparseMatrix<double> A(sys.nFree, sys.nFree);
  A.setFromTriplets(trip.begin(), trip.end());
  Eigen::ConjugateGradient<Eigen::SparseMatrix<double>,
                           Eigen::Lower | Eigen::Upper>
      cg; // default preconditioner = Jacobi
  cg.setTolerance(tol);
  cg.setMaxIterations(maxIter);
  cg.compute(A);
  const Eigen::VectorXd x = cg.solve(f); // assignment forces evaluation
  (void)x;
  return static_cast<int>(cg.iterations());
}

} // namespace

TEST_CASE("femgpu MG0: rigid-body coarse space accelerates PCG",
          "[femgpu][mg0]") {
  // Pure CPU (fp64) — no compute device needed. The small always-on version
  // of the MG0 gate: the V(1,1) cycle must converge and beat Jacobi-PCG
  // clearly even on a small system.
  fem::FemMesh mesh = GenerateBoxMesh(8, 4, 4, 80, 40, 40);
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
  const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 210000.0, 0.3);
  const auto dofs =
      fem::internal::BuildDofMap(static_cast<int>(mesh.nodes.size()), fixed);

  gpu::MgSetup mg;
  std::string err;
  REQUIRE(gpu::BuildMgSetup(FlatNodes(mesh), sys, dofs.map, mg, &err));
  INFO("aggregates: " << mg.nAgg << "  coarse DOFs: " << mg.nCoarse
                      << "  fine DOFs: " << mg.nFree);
  CHECK(mg.nCoarse < mg.nFree / 4); // a real coarsening, not a copy

  // P must reproduce rigid translation exactly: a coarse vector holding
  // t=(1,0,0) for every aggregate prolongs to u_x=1 on every free DOF.
  Eigen::VectorXd tc = Eigen::VectorXd::Zero(mg.nCoarse);
  for (int a = 0; a < mg.nAgg; ++a)
    tc[6 * a + 0] = 1.0;
  const Eigen::VectorXd u = mg.P * tc;
  for (int n = 0; n < static_cast<int>(mesh.nodes.size()); ++n) {
    if (dofs.map[3 * n + 0] >= 0)
      REQUIRE_THAT(u[dofs.map[3 * n + 0]], WithinAbs(1.0, 1e-12));
    if (dofs.map[3 * n + 1] >= 0)
      REQUIRE_THAT(u[dofs.map[3 * n + 1]], WithinAbs(0.0, 1e-12));
  }

  // Solve with SIMP-like scales: MG-PCG converges, and in far fewer
  // iterations than Jacobi-PCG on the identical system.
  const std::vector<float> scales = SimpScales(sys.nElems, 777);
  uint32_t state = 42;
  Eigen::VectorXd f(sys.nFree);
  for (int i = 0; i < sys.nFree; ++i)
    f[i] = RandSym(state);

  Eigen::VectorXd x;
  gpu::MgRefStats stats;
  REQUIRE(
      gpu::MgReferencePcg(sys, scales, mg, f, x, 1e-6, 2000, 3, &stats, &err));
  const int jac = JacobiIters(sys, scales, f, 1e-6, 40000);
  INFO("MG iters: " << stats.iterations << "  Jacobi iters: " << jac);
  CHECK(stats.converged);
  CHECK(stats.iterations * 4 < jac); // conservative; the gate bar is 10x
}

TEST_CASE("femgpu MG0 gate: bulky cube, uniform / harsh / real SIMP fields",
          "[.][mg0gate]") {
  // THE GPU3 GATE (docs/topopt/gpu3-plan.md): on the 45k-DOF bench cube the
  // coarse space must cut outer iterations >=10x vs Jacobi-PCG in fp64 —
  // including on a REAL late-iteration SIMP field (crisp void/solid
  // contrast), the case that defeated Chebyshev. Manual: femgpu_tests
  // "[mg0gate]" (runs minutes — it optimizes the cube on the CPU first).
  const double L = 80.0;
  fem::FemMesh mesh = GenerateBoxMesh(12, 12, 12, L, L, L);
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
  const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 210000.0, 0.3);
  const auto dofs =
      fem::internal::BuildDofMap(static_cast<int>(mesh.nodes.size()), fixed);

  gpu::MgSetup mg;
  std::string err;
  REQUIRE(gpu::BuildMgSetup(FlatNodes(mesh), sys, dofs.map, mg, &err));
  WARN("system: " << sys.nFree << " fine DOFs -> " << mg.nCoarse << " coarse ("
                  << mg.nAgg << " aggregates)");

  uint32_t state = 42;
  Eigen::VectorXd f(sys.nFree);
  for (int i = 0; i < sys.nFree; ++i)
    f[i] = RandSym(state);

  auto compare = [&](const char* name, const std::vector<float>& scales) {
    Eigen::VectorXd x;
    gpu::MgRefStats stats;
    REQUIRE(gpu::MgReferencePcg(sys, scales, mg, f, x, 1e-6, 4000, 3, &stats,
                                &err));
    const int jac = JacobiIters(sys, scales, f, 1e-6, 40000);
    WARN(name << ": MG " << stats.iterations << " iters (converged="
              << stats.converged << ") vs Jacobi " << jac << " -> "
              << (stats.iterations > 0
                      ? static_cast<double>(jac) / stats.iterations
                      : 0.0)
              << "x");
  };

  compare("uniform", std::vector<float>(sys.nElems, 1.0f));
  compare("harsh-random", SimpScales(sys.nElems, 1000));

  // A REAL late-iteration SIMP field: optimize the cube (CPU path, exact)
  // and penalize the final densities.
  fem::Material steel{210000.0, 0.3};
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAtPlane(mesh, 0, L, 1e-6);
  load.magnitude = 2000.0;
  load.direction = glm::dvec3(0, 0, -1);
  fem::TopoParams params;
  params.filterRadius = 18.0;
  params.maxIterations = 25;
  auto res = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err);
  REQUIRE(res.has_value());
  std::vector<float> simpScales(sys.nElems);
  for (int e = 0; e < sys.nElems; ++e) {
    const double xr = res->density[e];
    simpScales[e] = static_cast<float>(1e-6 + xr * xr * xr * (1.0 - 1e-6));
  }
  compare("real-late-SIMP", simpScales);
}

TEST_CASE("femgpu bench: multigrid scaling on a fine mesh", "[.][mgscale]") {
  // The GPU3 claim that matters: at FINE meshes the solve dominates the SIMP
  // iteration and multigrid's iteration-count advantage becomes wall time.
  // One representative mid-optimization solve on a ~190k-DOF cube.
  REQUIRE_GPU();
  WARN("device: " << ctx->DeviceName());

  const double L = 80.0;
  fem::FemMesh mesh = GenerateBoxMesh(20, 20, 20, L, L, L);
  const std::vector<int> fixed = NodesAtPlane(mesh, 0, 0.0, 1e-9);
  const gpu::SpmvSystem sys = PackSystem(mesh, fixed, 210000.0, 0.3);
  WARN("system: " << sys.nElems << " tets, " << sys.nFree << " free DOFs");

  // A REAL mid-optimization field at this mesh size (random voids create
  // isolated-island pathologies no real SIMP field has): run a few SIMP
  // iterations through the shipping GPU adapter, then penalize the result.
  std::vector<float> scales(sys.nElems, 1.0f);
  {
    std::string gerr;
    auto solver = gpu::TopoGpuSolver::Create(/*allowSoftware=*/true, &gerr);
    REQUIRE(solver);
    fem::Material steel{210000.0, 0.3};
    fem::FaceLoad load;
    load.type = fem::LoadType::Force;
    load.tris = TrisAtPlane(mesh, 0, L, 1e-6);
    load.magnitude = 2000.0;
    load.direction = glm::dvec3(0, 0, -1);
    fem::TopoParams params;
    params.filterRadius = 12.0;
    params.maxIterations = 8;
    std::string err;
    auto res = fem::TopologyOptimize(mesh, steel, fixed, {load}, params, &err,
                                     {}, {}, solver.get());
    REQUIRE(res.has_value());
    for (int e = 0; e < sys.nElems; ++e) {
      const double xr = res->density[e];
      scales[e] = static_cast<float>(1e-6 + xr * xr * xr * (1.0 - 1e-6));
    }
  }
  uint32_t state = 42;
  std::vector<float> f(sys.nFree);
  for (int i = 0; i < sys.nFree; ++i)
    f[i] = RandSym(state);

  using clk = std::chrono::steady_clock;
  for (const char* name : {"jacobi", "multigrid"}) {
    gpu::CgOptions opts;
    if (std::string(name) == "jacobi") {
      opts.precond = gpu::CgOptions::Precond::Jacobi;
    } else {
      const auto dofs = fem::internal::BuildDofMap(
          static_cast<int>(mesh.nodes.size()), fixed);
      std::string err2;
      opts.mg = gpu::MakeMgGpuData(FlatNodes(mesh), sys, dofs.map, &err2);
      REQUIRE(opts.mg);
      opts.precond = gpu::CgOptions::Precond::Multigrid;
      opts.chebDegree = 3;
      opts.chebRatio = 15.0f;
    }
    std::string err;
    const auto t0 = clk::now();
    auto cg = gpu::TopoCgSolver::Create(*ctx, sys, &err, opts);
    REQUIRE(cg);
    REQUIRE(cg->SetScales(scales, &err));
    std::vector<float> x(sys.nFree, 0.0f);
    gpu::CgStats stats;
    const auto t1 = clk::now();
    REQUIRE(cg->Solve(f, x, 1e-5f, 40000, &stats, &err));
    const double setupMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double solveMs =
        std::chrono::duration<double, std::milli>(clk::now() - t1).count();
    WARN(name << ": solve " << solveMs << " ms (" << stats.iterations
              << " outer iters, rel res " << stats.relResidual
              << ", fp64 check " << ResidualNorm(sys, scales, x, f)
              << "), setup " << setupMs << " ms");
  }
}

TEST_CASE("femgpu bench: full SIMP loop GPU vs CPU on the bulky cube",
          "[.][gpubench]") {
  // The number that matters to the user: wall time of TopologyOptimize on
  // the [topobench] cube through each solver, on the REAL SIMP systems
  // (smooth filtered fields, warm starts) rather than harsh random scales.
  std::string gerr;
  auto solver = gpu::TopoGpuSolver::Create(/*allowSoftware=*/true, &gerr);
  if (!solver)
    SKIP("no usable compute device: " << gerr);
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
  params.maxIterations = 6; // enough to see steady-state per-iteration cost

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

#endif // HAVE_FEM_SOLVER
