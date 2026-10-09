// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Matrix-free FEM stiffness product on the GPU
// (docs/topopt/README.md#the-gpu-solver): Ap = K(rho)·p without ever assembling
// K, computed element-by-element with an atomicAdd scatter. fem_gpu stays
// Vulkan-pure — the caller (tests now; the GPU1 solver adapter later) packs the
// element data from the FemMesh + DOF map, so this library needs neither Eigen
// nor designer_core.

#include "femgpu/ComputeContext.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gpu {

// The reduced system, packed per element. Layout mirrors the CPU assembly in
// TopologyOptimize: 30 DOF slots per tet10 (3 per node, node-major), each
// holding the compacted free-DOF index or -1 when fixed; Ke at full modulus
// E0, row-major 30x30, fp32.
struct SpmvSystem {
  int nFree = 0;
  int nElems = 0;
  std::vector<int32_t> dof; // 30 * nElems
  std::vector<float> ke;    // 900 * nElems
};

class TopoSpmv {
public:
  // Uploads the element data once. The context must outlive the TopoSpmv.
  static std::unique_ptr<TopoSpmv>
  Create(ComputeContext& ctx, const SpmvSystem& sys, std::string* error);
  ~TopoSpmv();

  TopoSpmv(const TopoSpmv&) = delete;
  TopoSpmv& operator=(const TopoSpmv&) = delete;

  // Per-element SIMP stiffness scale E(rho)/E0; size nElems. Re-set between
  // SIMP iterations, cheap next to the solve.
  bool SetScales(const std::vector<float>& scales, std::string* error);

  // Ap = K·p. p and Ap are reduced vectors (size nFree); Ap is overwritten.
  bool Apply(const std::vector<float>& p, std::vector<float>& Ap,
             std::string* error);

private:
  TopoSpmv(ComputeContext& ctx) : m_ctx(ctx) {}

  ComputeContext& m_ctx;
  int m_nFree = 0;
  int m_nElems = 0;
  Pipeline m_pipeline;
  Buffer m_dof, m_ke, m_scale, m_p, m_ap;
};

} // namespace gpu
