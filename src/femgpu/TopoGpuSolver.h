#pragma once

// The fem::TopoLinearSolver implementation backed by the Vulkan matrix-free
// CG (docs/topopt/README.md#the-gpu-solver): the bridge between designer_core's
// GPU-free topology loop and fem_gpu's TopoCgSolver. Owns a private headless
// ComputeContext; Setup packs the element matrices once per run (fp32 to the
// device, fp64 kept on the host), Solve refines an fp64 displacement with the
// on-device fp32 PCG as its inner solve, warm-started from the previous SIMP
// displacement (docs/topopt/README.md#mixed-precision-refinement), and the
// element energies are computed in fp64. Built only when Eigen is available (it
// uses the shared element math to pack Ke); absent a usable GPU, Create returns
// null and callers stay on the CPU path.

#include "core/fem/FemTopo.h"

#include <memory>
#include <string>

namespace gpu {

class TopoGpuSolver : public fem::TopoLinearSolver {
public:
  // Null + *error when no usable compute device exists. `allowSoftware`
  // admits llvmpipe-style software devices (tests); the app leaves it false —
  // a software "GPU" loses to the CPU Eigen path.
  // `preferredDevice` is the user's GPU preference (settings.gpu_device),
  // honoured through the same rule the renderer uses. Unlike the viewport,
  // this context is built per run, so a change takes effect immediately.
  static std::unique_ptr<TopoGpuSolver>
  Create(bool allowSoftware, std::string* error,
         const std::string& preferredDevice = "");
  ~TopoGpuSolver() override;

  const std::string& DeviceName() const;

  bool Setup(const fem::FemMesh& mesh, const fem::Material& material,
             const std::vector<int>& dofMap, int nFree,
             std::string* error) override;
  bool Solve(const std::vector<double>& scale, const std::vector<double>& F,
             std::vector<double>& u, std::string* error) override;
  // GPU4: one dispatch over the resident element matrices instead of the
  // caller's per-element Ke0 recompute.
  bool ElementEnergies(const std::vector<double>& u, std::vector<double>& se,
                       std::string* error) override;
  // Static studies: one material per mesh domain, and a solve held to the
  // control's tolerance from a cold start (the same refinement, given more
  // steps, and failing instead of accepting what it reached).
  bool SetupDomains(const fem::FemMesh& mesh,
                    const std::vector<fem::Material>& byDomain,
                    const std::vector<int>& dofMap, int nFree,
                    std::string* error) override;
  bool SolveStrict(const std::vector<double>& F, std::vector<double>& u,
                   fem::LinearSolveControl& control,
                   std::string* error) override;

private:
  TopoGpuSolver();
  bool Refine(const std::vector<double>& scale, const std::vector<double>& F,
              std::vector<double>& u, std::string* error,
              fem::LinearSolveControl* strict);
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace gpu
