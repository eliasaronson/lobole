#pragma once

// The fem::TopoLinearSolver implementation backed by the Vulkan matrix-free
// CG (docs/topopt/gpu-plan.md): the bridge between designer_core's GPU-free
// topology loop and fem_gpu's TopoCgSolver. Owns a private headless
// ComputeContext; Setup packs the element matrices once per run, Solve
// converts fp64<->fp32 and runs the on-device PCG warm-started from the
// previous SIMP displacement. Built only when Eigen is available (it uses the
// shared element math to pack Ke); absent a usable GPU, Create returns null
// and callers stay on the CPU path.

#include "core/FemTopo.h"

#include <memory>
#include <string>

namespace gpu {

class TopoGpuSolver : public fem::TopoLinearSolver {
 public:
  // Null + *error when no usable compute device exists. `allowSoftware`
  // admits llvmpipe-style software devices (tests); the app leaves it false —
  // a software "GPU" loses to the CPU Eigen path.
  static std::unique_ptr<TopoGpuSolver> Create(bool allowSoftware,
                                               std::string* error);
  ~TopoGpuSolver() override;

  const std::string& DeviceName() const;

  bool Setup(const fem::FemMesh& mesh, const fem::Material& material,
             const std::vector<int>& dofMap, int nFree,
             std::string* error) override;
  bool Solve(const std::vector<double>& scale, const std::vector<double>& F,
             std::vector<double>& u, std::string* error) override;

 private:
  TopoGpuSolver();
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace gpu
