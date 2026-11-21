// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace gpu {

// When Refine's outer loop has stopped gaining. A step is progress when it
// lowers the energy by more than fp64 can tell from rounding (1e-13 of it),
// OR when it at least halves the best residual before it; three steps in a
// row that are neither end the solve. The energy alone is not enough: near
// the answer a step's gain goes as the square of the residual, and on a
// slender part it falls under 1e-13 of the energy while the residual still
// drops tenfold a step. Judged on the energy alone, a static solve stopped
// three steps after the first such step, just short of its 1e-7 or just
// under it as the fp32 inner solves fell (the tea alarm's 0.5 mm clip: 9
// steps at 6e-7 one run in three, 11 at 5e-8 otherwise), and the CPU that
// finished it was refused as stalled one run in a dozen.
struct RefineStall {
  explicit RefineStall(double startRel) : best(startRel) {}
  // After a step that gained `gain` with the energy at `energy` and left the
  // residual at `rel`: true when the solve should stop.
  bool Step(double gain, double energy, double rel) {
    const bool progress = gain > 1e-13 * std::abs(energy) || rel < 0.5 * best;
    best = std::min(best, rel);
    stalls = progress ? 0 : stalls + 1;
    return stalls >= 3;
  }
  double best;
  int stalls = 0;
};

class TopoGpuSolver : public fem::TopoLinearSolver {
public:
  // Null + *error when no usable compute device exists. `allowSoftware`
  // admits llvmpipe-style software devices (tests); the app leaves it false —
  // a software "GPU" loses to the CPU Eigen path.
  // `preferredDevice` is the user's GPU preference (settings.gpu_device),
  // honoured through the same rule the renderer uses. This context is
  // built per run, so a change applies on the next run.
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
  bool DeviceLost() const override;
  void SetCancel(std::function<bool()> cancelled) override;
  // Tests: the submit `after` submits from now on this solver's device
  // answers VK_ERROR_DEVICE_LOST (ComputeContext::InjectLossForTest).
  void InjectDeviceLossForTest(int after = 0);

private:
  TopoGpuSolver();
  bool Refine(const std::vector<double>& scale, const std::vector<double>& F,
              std::vector<double>& u, std::string* error,
              fem::LinearSolveControl* strict);
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace gpu
