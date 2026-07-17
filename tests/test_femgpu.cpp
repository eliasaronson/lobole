#include <catch2/catch_test_macros.hpp>

#include "femgpu/ComputeContext.h"
#include "femgpu/TopoSpmv.h"
#include "gpu_sanity_spv.h"
#include "topo_testmesh.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

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

#endif  // HAVE_FEM_SOLVER
