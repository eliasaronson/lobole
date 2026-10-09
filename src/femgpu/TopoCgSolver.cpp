// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "femgpu/TopoCgSolver.h"

#include "cg_copy_spv.h"
#include "cg_diag_spv.h"
#include "cg_dot_spv.h"
#include "cg_precond_dot_spv.h"
#include "cg_residual_spv.h"
#include "cg_update_p_spv.h"
#include "cg_update_xr_spv.h"
#include "cheb_init_spv.h"
#include "cheb_iter_spv.h"
#include "fem_energy_spv.h"
#include "fem_spmv_spv.h"
#include "mg_prolong_spv.h"
#include "mg_restrict_spv.h"
#include "vec_add_spv.h"
#include "vec_divd_spv.h"
#include "vec_scale_spv.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gpu {

namespace {

// Scalar-block slots (one float each). rz alternates between two slots per
// iteration parity so update_p can read both old and new without a copy.
constexpr uint32_t kRzA = 0;
constexpr uint32_t kRzB = 1;
constexpr uint32_t kPAp = 2;
constexpr uint32_t kRr = 3;
constexpr uint32_t kBb = 4;
constexpr uint32_t kNumScalars = 5;

// CG iterations recorded per submit: large enough that per-submit overhead
// vanishes, small enough that convergence is checked promptly.
constexpr int kChunk = 32;

// See kMaxGroupsX in TopoSpmv.cpp — 2D element dispatch past the spec's
// 65535-per-dimension guarantee.
constexpr uint32_t kMaxGroupsX = 32768;

struct ElemPush {
  uint32_t nElems;
  uint32_t groupsX;
};
struct NPush {
  uint32_t n;
};
struct SlotPush {
  uint32_t n;
  uint32_t slot;
};
struct TwoSlotPush {
  uint32_t n;
  uint32_t slotA;
  uint32_t slotB;
};
struct OneFloatPush {
  uint32_t n;
  float c;
};
struct TwoFloatPush {
  uint32_t n;
  float c1;
  float c2;
};

} // namespace

std::unique_ptr<TopoCgSolver> TopoCgSolver::Create(ComputeContext& ctx,
                                                   const SpmvSystem& sys,
                                                   std::string* error,
                                                   const CgOptions& options) {
  auto fail = [&](const std::string& m) -> std::unique_ptr<TopoCgSolver> {
    if (error)
      *error = m;
    return nullptr;
  };
  if (sys.nFree <= 0 || sys.nElems <= 0)
    return fail("empty CG system");
  if (sys.dof.size() != static_cast<size_t>(sys.nElems) * 30 ||
      sys.ke.size() != static_cast<size_t>(sys.nElems) * 900)
    return fail("CG system arrays have the wrong size");
  if (options.chebDegree < 2 || options.chebRatio <= 1.0f)
    return fail("bad Chebyshev options (degree >= 2, ratio > 1 required)");
  if (options.precond == CgOptions::Precond::Multigrid &&
      (!options.mg || options.mg->nNodes <= 0 || options.mg->nCoarse <= 0 ||
       !options.mg->coarseSolve || !options.mg->updateScales))
    return fail("Multigrid needs a populated CgOptions::mg");

  std::unique_ptr<TopoCgSolver> s(new TopoCgSolver(ctx));
  s->m_nFree = sys.nFree;
  s->m_nElems = sys.nElems;
  s->m_opts = options;
  const uint32_t n = static_cast<uint32_t>(sys.nElems);
  s->m_elemGx = n < kMaxGroupsX ? n : kMaxGroupsX;
  s->m_elemGy = (n + s->m_elemGx - 1) / s->m_elemGx;
  s->m_vecGroups = (static_cast<uint32_t>(sys.nFree) + 255) / 256;

  // Pipelines.
  auto pipe = [&](Pipeline& p, const unsigned char* spv, size_t bytes,
                  uint32_t buffers, uint32_t pushBytes) {
    return ctx.CreatePipeline(spv, bytes, buffers, pushBytes, p, error);
  };
  if (!pipe(s->m_spmv, k_fem_spmv_spv, k_fem_spmv_spv_size, 5,
            sizeof(ElemPush)) ||
      !pipe(s->m_diag, k_cg_diag_spv, k_cg_diag_spv_size, 4,
            sizeof(ElemPush)) ||
      !pipe(s->m_residual, k_cg_residual_spv, k_cg_residual_spv_size, 3,
            sizeof(NPush)) ||
      !pipe(s->m_precondDot, k_cg_precond_dot_spv, k_cg_precond_dot_spv_size, 4,
            sizeof(TwoSlotPush)) ||
      !pipe(s->m_copy, k_cg_copy_spv, k_cg_copy_spv_size, 2, sizeof(NPush)) ||
      !pipe(s->m_dot, k_cg_dot_spv, k_cg_dot_spv_size, 3, sizeof(SlotPush)) ||
      !pipe(s->m_updateXr, k_cg_update_xr_spv, k_cg_update_xr_spv_size, 5,
            sizeof(TwoSlotPush)) ||
      !pipe(s->m_updateP, k_cg_update_p_spv, k_cg_update_p_spv_size, 3,
            sizeof(TwoSlotPush)) ||
      !pipe(s->m_chebInit, k_cheb_init_spv, k_cheb_init_spv_size, 5,
            sizeof(OneFloatPush)) ||
      !pipe(s->m_chebIter, k_cheb_iter_spv, k_cheb_iter_spv_size, 5,
            sizeof(TwoFloatPush)) ||
      !pipe(s->m_vecDivd, k_vec_divd_spv, k_vec_divd_spv_size, 3,
            sizeof(NPush)) ||
      !pipe(s->m_vecScale, k_vec_scale_spv, k_vec_scale_spv_size, 2,
            sizeof(OneFloatPush)) ||
      !pipe(s->m_mgRestrict, k_mg_restrict_spv, k_mg_restrict_spv_size, 5,
            sizeof(NPush)) ||
      !pipe(s->m_mgProlong, k_mg_prolong_spv, k_mg_prolong_spv_size, 5,
            sizeof(NPush)) ||
      !pipe(s->m_vecAdd, k_vec_add_spv, k_vec_add_spv_size, 2, sizeof(NPush)) ||
      !pipe(s->m_energy, k_fem_energy_spv, k_fem_energy_spv_size, 4,
            sizeof(ElemPush)))
    return nullptr;

  // Buffers.
  auto make = [&](Buffer& b, VkDeviceSize size) {
    return ctx.CreateBuffer(size, b, error);
  };
  const VkDeviceSize vec = static_cast<VkDeviceSize>(sys.nFree) * sizeof(float);
  if (!make(s->m_dof, sys.dof.size() * sizeof(int32_t)) ||
      !make(s->m_ke, sys.ke.size() * sizeof(float)) ||
      !make(s->m_scale, sys.nElems * sizeof(float)) || !make(s->m_diagK, vec) ||
      !make(s->m_f, vec) || !make(s->m_x, vec) || !make(s->m_r, vec) ||
      !make(s->m_z, vec) || !make(s->m_p, vec) || !make(s->m_ap, vec) ||
      !make(s->m_chebR, vec) || !make(s->m_chebD, vec) ||
      !make(s->m_scalars, kNumScalars * sizeof(float)) ||
      !make(s->m_se, sys.nElems * sizeof(float)))
    return nullptr;
  std::memcpy(s->m_dof.mapped, sys.dof.data(), s->m_dof.size);
  std::memcpy(s->m_ke.mapped, sys.ke.data(), s->m_ke.size);
  {
    float* sc = static_cast<float*>(s->m_scale.mapped);
    for (int e = 0; e < sys.nElems; ++e)
      sc[e] = 1.0f;
  }
  s->m_scalesCpu.assign(sys.nElems, 1.0f);

  // Multigrid working set + node tables.
  if (options.precond == CgOptions::Precond::Multigrid) {
    const MgGpuData& mg = *options.mg;
    s->m_nodeGroups = (static_cast<uint32_t>(mg.nNodes) + 255) / 256;
    const VkDeviceSize cvec =
        static_cast<VkDeviceSize>(mg.nCoarse) * sizeof(float);
    if (!make(s->m_mgR, vec) || !make(s->m_mgZt, vec) ||
        !make(s->m_mgRc, cvec) || !make(s->m_mgEc, cvec) ||
        !make(s->m_nodeAgg, mg.nodeAgg.size() * sizeof(int32_t)) ||
        !make(s->m_nodeOff, mg.nodeOffset.size() * sizeof(float)) ||
        !make(s->m_nodeDof, mg.nodeDof.size() * sizeof(int32_t)))
      return nullptr;
    std::memcpy(s->m_nodeAgg.mapped, mg.nodeAgg.data(), s->m_nodeAgg.size);
    std::memcpy(s->m_nodeOff.mapped, mg.nodeOffset.data(), s->m_nodeOff.size);
    std::memcpy(s->m_nodeDof.mapped, mg.nodeDof.data(), s->m_nodeDof.size);
  }

  // Static bindings.
  auto bind = [&](BoundSet& set, const Pipeline& p,
                  const std::vector<const Buffer*>& bufs) {
    return ctx.CreateBoundSet(p, bufs, set, error);
  };
  if (!bind(s->m_setSpmvP, s->m_spmv,
            {&s->m_dof, &s->m_ke, &s->m_scale, &s->m_p, &s->m_ap}) ||
      !bind(s->m_setSpmvX, s->m_spmv,
            {&s->m_dof, &s->m_ke, &s->m_scale, &s->m_x, &s->m_ap}) ||
      !bind(s->m_setDiag, s->m_diag,
            {&s->m_dof, &s->m_ke, &s->m_scale, &s->m_diagK}) ||
      !bind(s->m_setResidual, s->m_residual, {&s->m_f, &s->m_ap, &s->m_r}) ||
      !bind(s->m_setPrecond, s->m_precondDot,
            {&s->m_r, &s->m_diagK, &s->m_z, &s->m_scalars}) ||
      !bind(s->m_setCopyZP, s->m_copy, {&s->m_z, &s->m_p}) ||
      !bind(s->m_setDotFF, s->m_dot, {&s->m_f, &s->m_f, &s->m_scalars}) ||
      !bind(s->m_setDotPAp, s->m_dot, {&s->m_p, &s->m_ap, &s->m_scalars}) ||
      !bind(s->m_setUpdateXr, s->m_updateXr,
            {&s->m_p, &s->m_ap, &s->m_x, &s->m_r, &s->m_scalars}) ||
      !bind(s->m_setUpdateP, s->m_updateP, {&s->m_z, &s->m_p, &s->m_scalars}) ||
      // Chebyshev preconditioner + power iteration (chebD doubles as the
      // power vector v, chebR as its work vector w).
      !bind(s->m_setSpmvChebD, s->m_spmv,
            {&s->m_dof, &s->m_ke, &s->m_scale, &s->m_chebD, &s->m_ap}) ||
      !bind(s->m_setChebInit, s->m_chebInit,
            {&s->m_r, &s->m_diagK, &s->m_chebR, &s->m_chebD, &s->m_z}) ||
      !bind(s->m_setChebIter, s->m_chebIter,
            {&s->m_ap, &s->m_diagK, &s->m_chebR, &s->m_chebD, &s->m_z}) ||
      !bind(s->m_setDotRZ, s->m_dot, {&s->m_r, &s->m_z, &s->m_scalars}) ||
      !bind(s->m_setDotRR, s->m_dot, {&s->m_r, &s->m_r, &s->m_scalars}) ||
      !bind(s->m_setDivdAp, s->m_vecDivd,
            {&s->m_ap, &s->m_diagK, &s->m_chebR}) ||
      !bind(s->m_setScaleRD, s->m_vecScale, {&s->m_chebR, &s->m_chebD}) ||
      !bind(s->m_setDotDR, s->m_dot,
            {&s->m_chebD, &s->m_chebR, &s->m_scalars}) ||
      !bind(s->m_setDotDD, s->m_dot,
            {&s->m_chebD, &s->m_chebD, &s->m_scalars}) ||
      !bind(s->m_setDotRRcheb, s->m_dot,
            {&s->m_chebR, &s->m_chebR, &s->m_scalars}) ||
      // Energies read u from the p buffer — CG scratch, dead between solves,
      // so ElementEnergies never touches the warm-start x.
      !bind(s->m_setEnergy, s->m_energy,
            {&s->m_dof, &s->m_ke, &s->m_p, &s->m_se}))
    return nullptr;

  if (options.precond == CgOptions::Precond::Multigrid) {
    if (!bind(s->m_setSpmvZ, s->m_spmv,
              {&s->m_dof, &s->m_ke, &s->m_scale, &s->m_z, &s->m_ap}) ||
        !bind(s->m_setResidualMg, s->m_residual,
              {&s->m_r, &s->m_ap, &s->m_mgR}) || // r2 = r - K z
        !bind(s->m_setMgRestrict, s->m_mgRestrict,
              {&s->m_mgR, &s->m_nodeAgg, &s->m_nodeOff, &s->m_nodeDof,
               &s->m_mgRc}) ||
        !bind(s->m_setMgProlong, s->m_mgProlong,
              {&s->m_mgEc, &s->m_nodeAgg, &s->m_nodeOff, &s->m_nodeDof,
               &s->m_z}) ||
        !bind(s->m_setChebInitMg, s->m_chebInit,
              {&s->m_mgR, &s->m_diagK, &s->m_chebR, &s->m_chebD, &s->m_mgZt}) ||
        !bind(s->m_setChebIterMg, s->m_chebIter,
              {&s->m_ap, &s->m_diagK, &s->m_chebR, &s->m_chebD, &s->m_mgZt}) ||
        !bind(s->m_setVecAddZt, s->m_vecAdd, {&s->m_mgZt, &s->m_z}))
      return nullptr;
  }

  return s;
}

TopoCgSolver::~TopoCgSolver() {
  for (BoundSet* set :
       {&m_setSpmvP,      &m_setSpmvX,      &m_setDiag,       &m_setResidual,
        &m_setPrecond,    &m_setCopyZP,     &m_setDotFF,      &m_setDotPAp,
        &m_setUpdateXr,   &m_setUpdateP,    &m_setSpmvChebD,  &m_setChebInit,
        &m_setChebIter,   &m_setDotRZ,      &m_setDotRR,      &m_setDivdAp,
        &m_setScaleRD,    &m_setDotDR,      &m_setDotDD,      &m_setDotRRcheb,
        &m_setSpmvZ,      &m_setResidualMg, &m_setMgRestrict, &m_setMgProlong,
        &m_setChebInitMg, &m_setChebIterMg, &m_setVecAddZt})
    m_ctx.Destroy(*set);
  for (Buffer* b :
       {&m_dof,  &m_ke,   &m_scale, &m_diagK,   &m_f,       &m_x,       &m_r,
        &m_z,    &m_p,    &m_ap,    &m_chebR,   &m_chebD,   &m_scalars, &m_mgR,
        &m_mgZt, &m_mgRc, &m_mgEc,  &m_nodeAgg, &m_nodeOff, &m_nodeDof, &m_se})
    m_ctx.Destroy(*b);
  for (Pipeline* p :
       {&m_spmv, &m_diag, &m_residual, &m_precondDot, &m_copy, &m_dot,
        &m_updateXr, &m_updateP, &m_chebInit, &m_chebIter, &m_vecDivd,
        &m_vecScale, &m_mgRestrict, &m_mgProlong, &m_vecAdd, &m_energy})
    m_ctx.Destroy(*p);
}

bool TopoCgSolver::SetScales(const std::vector<float>& scales,
                             std::string* error) {
  if (scales.size() != static_cast<size_t>(m_nElems)) {
    if (error)
      *error = "scale count != element count";
    return false;
  }
  std::memcpy(m_scale.mapped, scales.data(), m_scale.size);
  m_scalesCpu = scales; // the coarse-rebuild callback needs them on the CPU
  m_scalesDirty = true;
  return true;
}

// Records z = M^-1 r plus the two reductions PCG needs from it
// (s[rzSlot] = r.z, s[kRr] = r.r; both slots zeroed here).
// Jacobi: one fused kernel. Chebyshev: a degree-m polynomial of the
// Jacobi-scaled operator (Saad Alg. 12.1) — m-1 SpMVs with host-precomputed
// coefficients as push constants, then the two dots. No reductions inside
// the polynomial, so the whole application records into the same batch.
void TopoCgSolver::RecordPrecond(uint32_t rzSlot) {
  const uint32_t n = static_cast<uint32_t>(m_nFree);
  const NPush np{n};
  const ElemPush ep{static_cast<uint32_t>(m_nElems), m_elemGx};
  m_ctx.BatchFill(m_scalars, rzSlot * sizeof(float), sizeof(float), 0);
  m_ctx.BatchFill(m_scalars, kRr * sizeof(float), sizeof(float), 0);
  if (m_opts.precond == CgOptions::Precond::Jacobi) {
    const TwoSlotPush pz{n, rzSlot, kRr};
    m_ctx.BatchDispatch(m_precondDot, m_setPrecond, &pz, sizeof(pz),
                        m_vecGroups, 1);
    return;
  }
  const OneFloatPush init{n, m_chebInvTheta};
  m_ctx.BatchDispatch(m_chebInit, m_setChebInit, &init, sizeof(init),
                      m_vecGroups, 1);
  for (size_t k = 0; k < m_chebC1.size(); ++k) {
    m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_spmv, m_setSpmvChebD, &ep, sizeof(ep), m_elemGx,
                        m_elemGy);
    const TwoFloatPush cf{n, m_chebC1[k], m_chebC2[k]};
    m_ctx.BatchDispatch(m_chebIter, m_setChebIter, &cf, sizeof(cf), m_vecGroups,
                        1);
  }
  const SlotPush rz{n, rzSlot};
  m_ctx.BatchDispatch(m_dot, m_setDotRZ, &rz, sizeof(rz), m_vecGroups, 1);
  const SlotPush rr{n, kRr};
  m_ctx.BatchDispatch(m_dot, m_setDotRR, &rr, sizeof(rr), m_vecGroups, 1);
}

// Power iteration for lambda_max(D^-1 K), needed to aim the Chebyshev
// interval. Runs on-device (chebD = v, chebR = w); a handful of submits with
// scalar readbacks. The estimate is inflated 10% — Chebyshev must bound the
// spectrum from ABOVE (the polynomial blows up past its interval), and power
// iteration converges from below.
bool TopoCgSolver::EstimateLambdaMax(std::string* error) {
  const uint32_t n = static_cast<uint32_t>(m_nFree);
  const ElemPush ep{static_cast<uint32_t>(m_nElems), m_elemGx};
  const NPush np{n};
  const float* scal = static_cast<const float*>(m_scalars.mapped);

  { // v0: a fixed pseudo-random field, NOT ones. Ones is the smoothest
    // vector there is, nearly orthogonal to the top eigenvector this is
    // after: on the pan/tilt yoke's third SIMP system it read 3.35 after 16
    // iterations against a true 5.52, the 1.1 margin did not cover it, the
    // Chebyshev smoother AMPLIFIED the top of the spectrum, and the
    // multigrid PCG made no progress in 3000 iterations. From a random
    // start the same 16 iterations read 5.47.
    float* v = static_cast<float*>(m_chebD.mapped);
    uint32_t state = 0x2545f491u;
    for (int i = 0; i < m_nFree; ++i) {
      state = state * 1664525u + 1013904223u;
      v[i] =
          static_cast<float>(state >> 8) / static_cast<float>(1u << 23) - 1.0f;
    }
  }
  double lambda = 0.0, prev = 0.0;
  for (int it = 0; it < 16; ++it) {
    if (!m_ctx.BeginBatch(error))
      return false;
    m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_spmv, m_setSpmvChebD, &ep, sizeof(ep), m_elemGx,
                        m_elemGy);
    m_ctx.BatchDispatch(m_vecDivd, m_setDivdAp, &np, sizeof(np), m_vecGroups,
                        1); // w = D^-1 K v
    m_ctx.BatchFill(m_scalars, kRzA * sizeof(float), 3 * sizeof(float), 0);
    const SlotPush dvw{n, kRzA}; // v.w
    m_ctx.BatchDispatch(m_dot, m_setDotDR, &dvw, sizeof(dvw), m_vecGroups, 1);
    const SlotPush dvv{n, kRzB}; // v.v
    m_ctx.BatchDispatch(m_dot, m_setDotDD, &dvv, sizeof(dvv), m_vecGroups, 1);
    const SlotPush dww{n, kPAp}; // w.w
    m_ctx.BatchDispatch(m_dot, m_setDotRRcheb, &dww, sizeof(dww), m_vecGroups,
                        1);
    if (!m_ctx.SubmitBatch(error))
      return false;

    const float vw = scal[kRzA], vv = scal[kRzB], ww = scal[kPAp];
    if (!std::isfinite(vw) || !std::isfinite(ww) || vv <= 0.0f || ww <= 0.0f) {
      if (error)
        *error = "power iteration broke down (singular diagonal?)";
      return false;
    }
    lambda = static_cast<double>(vw) / static_cast<double>(vv);
    // v = w / |w|
    if (!m_ctx.BeginBatch(error))
      return false;
    const OneFloatPush sc{n, 1.0f / std::sqrt(ww)};
    m_ctx.BatchDispatch(m_vecScale, m_setScaleRD, &sc, sizeof(sc), m_vecGroups,
                        1);
    if (!m_ctx.SubmitBatch(error))
      return false;

    if (it >= 5 && std::abs(lambda - prev) < 0.01 * lambda)
      break;
    prev = lambda;
  }
  m_lambdaMax = static_cast<float>(1.1 * lambda);

  // Host-side Chebyshev coefficients over [lambdaMax/ratio, lambdaMax].
  const double lmax = m_lambdaMax;
  const double lmin = lmax / m_opts.chebRatio;
  const double theta = 0.5 * (lmax + lmin), delta = 0.5 * (lmax - lmin);
  const double sigma1 = theta / delta;
  m_chebInvTheta = static_cast<float>(1.0 / theta);
  m_chebC1.clear();
  m_chebC2.clear();
  double rhoPrev = 1.0 / sigma1;
  for (int k = 1; k < m_opts.chebDegree; ++k) {
    const double rho = 1.0 / (2.0 * sigma1 - rhoPrev);
    m_chebC1.push_back(static_cast<float>(rho * rhoPrev));
    m_chebC2.push_back(static_cast<float>(2.0 * rho / delta));
    rhoPrev = rho;
  }
  return true;
}

// One PCG iteration, alpha/beta computed on-device. Parity picks which rz
// slot is "old" (read by alpha/beta) vs "new" (written by the reduction).
void TopoCgSolver::RecordIteration(int parity) {
  const uint32_t n = static_cast<uint32_t>(m_nFree);
  const uint32_t rzOld = parity == 0 ? kRzA : kRzB;
  const uint32_t rzNew = parity == 0 ? kRzB : kRzA;

  // Ap = K p; pAp = dot(p, Ap)
  m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
  m_ctx.BatchFill(m_scalars, kPAp * sizeof(float), sizeof(float), 0);
  const ElemPush ep{static_cast<uint32_t>(m_nElems), m_elemGx};
  m_ctx.BatchDispatch(m_spmv, m_setSpmvP, &ep, sizeof(ep), m_elemGx, m_elemGy);
  const SlotPush dp{n, kPAp};
  m_ctx.BatchDispatch(m_dot, m_setDotPAp, &dp, sizeof(dp), m_vecGroups, 1);
  // alpha = rzOld/pAp; x += alpha p; r -= alpha Ap
  const TwoSlotPush ux{n, rzOld, kPAp};
  m_ctx.BatchDispatch(m_updateXr, m_setUpdateXr, &ux, sizeof(ux), m_vecGroups,
                      1);
  // z = M^-1 r; rzNew = dot(r,z); rr = dot(r,r)
  RecordPrecond(rzNew);
  // beta = rzNew/rzOld; p = z + beta p
  const TwoSlotPush up{n, rzNew, rzOld};
  m_ctx.BatchDispatch(m_updateP, m_setUpdateP, &up, sizeof(up), m_vecGroups, 1);
}

bool TopoCgSolver::Solve(const std::vector<float>& f, std::vector<float>& x,
                         float tol, int maxIter, CgStats* stats,
                         std::string* error, bool lastIterate) {
  if (f.size() != static_cast<size_t>(m_nFree)) {
    if (error)
      *error = "f size != nFree";
    return false;
  }
  if (m_opts.precond == CgOptions::Precond::Multigrid)
    return SolveMg(f, x, tol, maxIter, stats, error, lastIterate);
  x.resize(m_nFree, 0.0f);
  std::memcpy(m_f.mapped, f.data(), m_f.size);
  std::memcpy(m_x.mapped, x.data(), m_x.size);

  const uint32_t n = static_cast<uint32_t>(m_nFree);
  const ElemPush ep{static_cast<uint32_t>(m_nElems), m_elemGx};
  const NPush np{n};
  const float* scal = static_cast<const float*>(m_scalars.mapped);

  // ---- Init: preconditioner diagonal + |f|^2 ------------------------------
  const bool rebuildPrecond = m_scalesDirty;
  if (!m_ctx.BeginBatch(error))
    return false;
  if (m_scalesDirty) {
    m_ctx.BatchFill(m_diagK, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_diag, m_setDiag, &ep, sizeof(ep), m_elemGx, m_elemGy);
    m_scalesDirty = false;
  }
  m_ctx.BatchFill(m_scalars, kBb * sizeof(float), sizeof(float), 0);
  const SlotPush bbp{n, kBb};
  m_ctx.BatchDispatch(m_dot, m_setDotFF, &bbp, sizeof(bbp), m_vecGroups, 1);
  if (!m_ctx.SubmitBatch(error))
    return false;
  const float bb = scal[kBb];
  // Chebyshev needs the spectrum's top edge; the diagonal (and so the scaled
  // operator) just changed with the SIMP scales.
  if (m_opts.precond != CgOptions::Precond::Jacobi && rebuildPrecond &&
      !EstimateLambdaMax(error))
    return false;
  if (!std::isfinite(bb)) {
    if (error)
      *error = "CG breakdown (non-finite |f| — bad load vector?)";
    return false;
  }
  if (bb <= 0.0f) { // zero load: x = 0 is exact
    std::fill(x.begin(), x.end(), 0.0f);
    if (stats)
      *stats = {0, 0.0f, true};
    return true;
  }
  const float target = tol * tol * bb;

  // (Re)starts the CG from the current device x: true residual r = f - K x,
  // z = M^-1 r, p = z, rz -> slot A, rr refreshed.
  auto startCg = [&]() -> bool {
    if (!m_ctx.BeginBatch(error))
      return false;
    m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_spmv, m_setSpmvX, &ep, sizeof(ep), m_elemGx,
                        m_elemGy);
    m_ctx.BatchDispatch(m_residual, m_setResidual, &np, sizeof(np), m_vecGroups,
                        1);
    RecordPrecond(kRzA);
    m_ctx.BatchDispatch(m_copy, m_setCopyZP, &np, sizeof(np), m_vecGroups, 1);
    return m_ctx.SubmitBatch(error);
  };
  if (!startCg())
    return false;
  float rr = scal[kRr];
  if (!std::isfinite(rr)) {
    if (error)
      *error = "CG breakdown (non-finite residual)";
    return false;
  }

  // ---- Chunked iteration loop with restarts -------------------------------
  // Each chunk ends with RESIDUAL REPLACEMENT: the fp32 recurrence r drifts
  // away from the true residual (observed ~100x optimistic on FEM systems),
  // so r is recomputed as f - K x on-device — keeping p, which preserves the
  // conjugate direction history — and rz/rr refreshed. Convergence is decided
  // on the TRUE residual, and the best iterate is snapshotted at every new
  // low (fp32 PCG walks AWAY past its round-off floor — measured — so the
  // final iterate is not trustworthy; the best one is what gets returned).
  //
  // Judging progress needs care: CG minimizes the A-norm error, and the L2
  // residual is NOT monotone — it legitimately grows by orders of magnitude
  // in the early phase (measured 20x on a SIMP skin/void system, identical
  // in fp64) before falling. So there is no short-term divergence test; the
  // only signals used are long-window ones. No new low across kStallChunks
  // chunks means either fp32 conjugacy rot or the round-off floor: a full
  // RESTART from the best iterate (fresh r, p = z) distinguishes them — rot
  // recovers and progress resumes; at the floor the restart gains nothing
  // twice in a row and the loop returns the best, mirroring how the CPU path
  // accepts Eigen's NoConvergence. SIMP is tolerant of a floored solve; the
  // femgpu tests validate the whole loop.
  constexpr int kStallChunks = 32;
  // FEMGPU_CG_TRACE=1: single-iteration chunks + scalar dump (debug aid).
  const bool trace = std::getenv("FEMGPU_CG_TRACE") != nullptr;
  int iter = 0, parity = 0, chunksSinceBest = 0, restartsNoGain = 0;
  float bestRr = rr;
  float bestAtRestart = rr;
  std::vector<float> bestX(x); // x0 (already uploaded) is the initial best
  while (rr > target && iter < maxIter && restartsNoGain < 2) {
    const int chunk = std::min(trace ? 1 : kChunk, maxIter - iter);
    if (!m_ctx.BeginBatch(error))
      return false;
    for (int k = 0; k < chunk; ++k) {
      RecordIteration(parity);
      parity ^= 1;
    }
    // Residual replacement + rz/rr refresh (into the slot the NEXT iteration
    // reads as rzOld, i.e. the current parity's slot).
    m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_spmv, m_setSpmvX, &ep, sizeof(ep), m_elemGx,
                        m_elemGy);
    m_ctx.BatchDispatch(m_residual, m_setResidual, &np, sizeof(np), m_vecGroups,
                        1);
    RecordPrecond(parity == 0 ? kRzA : kRzB);
    if (!m_ctx.SubmitBatch(error))
      return false;
    iter += chunk;
    rr = scal[kRr];
    if (trace)
      std::fprintf(stderr, "cg it=%d rr=%g rzA=%g rzB=%g pAp=%g\n", iter,
                   scal[kRr], scal[kRzA], scal[kRzB], scal[kPAp]);
    // (In lastIterate mode "best" is the latest finite iterate, and nothing
    // restarts: a NaN ends the solve on the iterate before it.)
    if (std::isfinite(rr) && (lastIterate || rr < bestRr)) {
      bestRr = rr;
      std::memcpy(bestX.data(), m_x.mapped, m_x.size);
      chunksSinceBest = 0;
    } else {
      ++chunksSinceBest;
    }
    if (lastIterate) {
      if (!std::isfinite(rr))
        break;
      continue;
    }
    if (!std::isfinite(rr) || chunksSinceBest >= kStallChunks) {
      // Restart from the best iterate (a NaN — alpha noise at the floor —
      // also lands here and is cleared by the fresh residual). No gain since
      // the previous restart, twice in a row, means the floor: stop there.
      restartsNoGain = bestRr < 0.999f * bestAtRestart ? 0 : restartsNoGain + 1;
      bestAtRestart = bestRr;
      std::memcpy(m_x.mapped, bestX.data(), m_x.size);
      if (!startCg())
        return false;
      rr = scal[kRr];
      if (!std::isfinite(rr)) { // a fresh true residual is NaN: real trouble
        if (error)
          *error = "CG breakdown (non-finite residual)";
        return false;
      }
      parity = 0;
      chunksSinceBest = 0;
    }
  }

  // Leave the best iterate on the device too, so the next warm start (and the
  // caller) see the best solution rather than a past-the-floor walk-away.
  std::memcpy(m_x.mapped, bestX.data(), m_x.size);
  x = std::move(bestX);
  if (stats) {
    stats->iterations = iter;
    stats->relResidual = std::sqrt(bestRr / bb);
    stats->converged = bestRr <= target;
    stats->lambdaMax = m_lambdaMax;
  }
  return true;
}

bool TopoCgSolver::ElementEnergies(const std::vector<float>& u,
                                   std::vector<float>& se, std::string* error) {
  if (u.size() != static_cast<size_t>(m_nFree)) {
    if (error)
      *error = "u size != nFree";
    return false;
  }
  std::memcpy(m_p.mapped, u.data(), m_p.size);
  const ElemPush ep{static_cast<uint32_t>(m_nElems), m_elemGx};
  if (!m_ctx.BeginBatch(error))
    return false;
  m_ctx.BatchDispatch(m_energy, m_setEnergy, &ep, sizeof(ep), m_elemGx,
                      m_elemGy);
  if (!m_ctx.SubmitBatch(error))
    return false;
  se.resize(m_nElems);
  std::memcpy(se.data(), m_se.mapped, m_se.size);
  return true;
}

// ---- GPU3: multigrid-preconditioned PCG -----------------------------------

// Records a Chebyshev polynomial application into the CURRENT batch: the
// output vector is whatever `initSet`/`iterSet` bind (z for pre-smoothing,
// the zt correction for post-smoothing). Coefficients are the smoother's
// (m_chebC1/C2 from EstimateLambdaMax with the MG smoother degree/ratio).
void TopoCgSolver::RecordChebInto(const BoundSet& initSet,
                                  const BoundSet& iterSet) {
  const uint32_t n = static_cast<uint32_t>(m_nFree);
  const ElemPush ep{static_cast<uint32_t>(m_nElems), m_elemGx};
  const OneFloatPush init{n, m_chebInvTheta};
  m_ctx.BatchDispatch(m_chebInit, initSet, &init, sizeof(init), m_vecGroups, 1);
  for (size_t k = 0; k < m_chebC1.size(); ++k) {
    m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_spmv, m_setSpmvChebD, &ep, sizeof(ep), m_elemGx,
                        m_elemGy);
    const TwoFloatPush cf{n, m_chebC1[k], m_chebC2[k]};
    m_ctx.BatchDispatch(m_chebIter, iterSet, &cf, sizeof(cf), m_vecGroups, 1);
  }
}

// One V(1,1) application z = M^-1 r (r lives in m_r), split around the CPU
// coarse solve: [pre-smooth, fine residual, restrict] -> CPU LDLT ->
// [prolong-correct, fine residual, post-smooth, fold, rz/rr dots]. On
// return the rz/rr scalars are refreshed (rz into `rzSlot`).
bool TopoCgSolver::ApplyMgPrecond(uint32_t rzSlot, std::string* error) {
  const uint32_t n = static_cast<uint32_t>(m_nFree);
  const ElemPush ep{static_cast<uint32_t>(m_nElems), m_elemGx};
  const NPush np{n};
  const NPush nodes{static_cast<uint32_t>(m_opts.mg->nNodes)};

  // Part 1: z = Cheb(r); r2 = r - K z; rc = P^T r2.
  if (!m_ctx.BeginBatch(error))
    return false;
  RecordChebInto(m_setChebInit, m_setChebIter); // writes m_z from m_r
  m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
  m_ctx.BatchDispatch(m_spmv, m_setSpmvZ, &ep, sizeof(ep), m_elemGx, m_elemGy);
  m_ctx.BatchDispatch(m_residual, m_setResidualMg, &np, sizeof(np), m_vecGroups,
                      1);
  m_ctx.BatchFill(m_mgRc, 0, VK_WHOLE_SIZE, 0);
  m_ctx.BatchDispatch(m_mgRestrict, m_setMgRestrict, &nodes, sizeof(nodes),
                      m_nodeGroups, 1);
  if (!m_ctx.SubmitBatch(error))
    return false;

  // CPU coarse solve between the submits.
  if (!m_opts.mg->coarseSolve(static_cast<const float*>(m_mgRc.mapped),
                              static_cast<float*>(m_mgEc.mapped), error))
    return false;

  // Part 2: z += P ec; r2 = r - K z; z += Cheb(r2); refresh rz/rr.
  if (!m_ctx.BeginBatch(error))
    return false;
  m_ctx.BatchDispatch(m_mgProlong, m_setMgProlong, &nodes, sizeof(nodes),
                      m_nodeGroups, 1);
  m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
  m_ctx.BatchDispatch(m_spmv, m_setSpmvZ, &ep, sizeof(ep), m_elemGx, m_elemGy);
  m_ctx.BatchDispatch(m_residual, m_setResidualMg, &np, sizeof(np), m_vecGroups,
                      1);
  RecordChebInto(m_setChebInitMg, m_setChebIterMg); // zt = Cheb(r2)
  m_ctx.BatchDispatch(m_vecAdd, m_setVecAddZt, &np, sizeof(np), m_vecGroups, 1);
  m_ctx.BatchFill(m_scalars, rzSlot * sizeof(float), sizeof(float), 0);
  m_ctx.BatchFill(m_scalars, kRr * sizeof(float), sizeof(float), 0);
  const SlotPush rz{n, rzSlot};
  m_ctx.BatchDispatch(m_dot, m_setDotRZ, &rz, sizeof(rz), m_vecGroups, 1);
  const SlotPush rr{n, kRr};
  m_ctx.BatchDispatch(m_dot, m_setDotRR, &rr, sizeof(rr), m_vecGroups, 1);
  return m_ctx.SubmitBatch(error);
}

// The multigrid PCG driver. Structurally the GPU1 loop (true-residual
// replacement, best-iterate snapshot, restart-on-stall — the fp32 lessons
// carry over verbatim), but per-iteration submits instead of 32-iteration
// chunks: the CPU coarse solve sits inside every preconditioner application.
// At ~10^2 outer iterations (the fp64 gate measured 19-32x fewer than
// Jacobi's ~10^3-10^4) the extra submit overhead is noise.
bool TopoCgSolver::SolveMg(const std::vector<float>& f, std::vector<float>& x,
                           float tol, int maxIter, CgStats* stats,
                           std::string* error, bool lastIterate) {
  x.resize(m_nFree, 0.0f);
  std::memcpy(m_f.mapped, f.data(), m_f.size);
  std::memcpy(m_x.mapped, x.data(), m_x.size);

  const uint32_t n = static_cast<uint32_t>(m_nFree);
  const ElemPush ep{static_cast<uint32_t>(m_nElems), m_elemGx};
  const NPush np{n};
  const float* scal = static_cast<const float*>(m_scalars.mapped);

  // Init: diagonal + |f|^2 (+ coarse operator + smoother spectrum on a
  // scales change).
  const bool rebuildPrecond = m_scalesDirty;
  if (!m_ctx.BeginBatch(error))
    return false;
  if (m_scalesDirty) {
    m_ctx.BatchFill(m_diagK, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_diag, m_setDiag, &ep, sizeof(ep), m_elemGx, m_elemGy);
    m_scalesDirty = false;
  }
  m_ctx.BatchFill(m_scalars, kBb * sizeof(float), sizeof(float), 0);
  const SlotPush bbp{n, kBb};
  m_ctx.BatchDispatch(m_dot, m_setDotFF, &bbp, sizeof(bbp), m_vecGroups, 1);
  if (!m_ctx.SubmitBatch(error))
    return false;
  if (rebuildPrecond) {
    if (!EstimateLambdaMax(error))
      return false;
    if (!m_opts.mg->updateScales(m_scalesCpu, error))
      return false;
  }
  const float bb = scal[kBb];
  if (!std::isfinite(bb)) {
    if (error)
      *error = "CG breakdown (non-finite |f|)";
    return false;
  }
  if (bb <= 0.0f) {
    std::fill(x.begin(), x.end(), 0.0f);
    if (stats)
      *stats = {0, 0.0f, true, m_lambdaMax};
    return true;
  }
  const float target = tol * tol * bb;

  // (Re)start from the current device x: true residual, precondition, p = z.
  auto startCg = [&]() -> bool {
    if (!m_ctx.BeginBatch(error))
      return false;
    m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_spmv, m_setSpmvX, &ep, sizeof(ep), m_elemGx,
                        m_elemGy);
    m_ctx.BatchDispatch(m_residual, m_setResidual, &np, sizeof(np), m_vecGroups,
                        1);
    if (!m_ctx.SubmitBatch(error))
      return false;
    if (!ApplyMgPrecond(kRzA, error))
      return false;
    if (!m_ctx.BeginBatch(error))
      return false;
    m_ctx.BatchDispatch(m_copy, m_setCopyZP, &np, sizeof(np), m_vecGroups, 1);
    return m_ctx.SubmitBatch(error);
  };
  if (!startCg())
    return false;
  float rr = scal[kRr];
  if (!std::isfinite(rr)) {
    if (error)
      *error = "CG breakdown (non-finite residual)";
    return false;
  }

  // Counted in true-residual REFRESHES (one per 16 iterations): four
  // refreshes (~64 iterations) with no new low reads as the fp32 floor.
  // Cold solves on hard systems get their legitimate slow phases via the
  // caller's generous maxIter; warm SIMP solves are capped tight there
  // instead (see TopoGpuSolver) — floor-grinding measured as pure waste.
  constexpr int kStallIters = 4;
  int iter = 0, parity = 0, sinceBest = 0, restartsNoGain = 0;
  float bestRr = rr;
  float bestAtRestart = rr;
  std::vector<float> bestX(x);
  while (rr > target && iter < maxIter && restartsNoGain < 2) {
    const uint32_t rzOld = parity == 0 ? kRzA : kRzB;
    const uint32_t rzNew = parity == 0 ? kRzB : kRzA;
    // CG step: Ap = K p; pAp; alpha (on-device from slots); x += ap, r -= aAp.
    if (!m_ctx.BeginBatch(error))
      return false;
    m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchFill(m_scalars, kPAp * sizeof(float), sizeof(float), 0);
    m_ctx.BatchDispatch(m_spmv, m_setSpmvP, &ep, sizeof(ep), m_elemGx,
                        m_elemGy);
    const SlotPush dp{n, kPAp};
    m_ctx.BatchDispatch(m_dot, m_setDotPAp, &dp, sizeof(dp), m_vecGroups, 1);
    const TwoSlotPush ux{n, rzOld, kPAp};
    m_ctx.BatchDispatch(m_updateXr, m_setUpdateXr, &ux, sizeof(ux), m_vecGroups,
                        1);
    if (!m_ctx.SubmitBatch(error))
      return false;
    // z = M^-1 r (2 submits + CPU coarse), refreshing rzNew/rr.
    if (!ApplyMgPrecond(rzNew, error))
      return false;
    // p = z + beta p.
    if (!m_ctx.BeginBatch(error))
      return false;
    const TwoSlotPush up{n, rzNew, rzOld};
    m_ctx.BatchDispatch(m_updateP, m_setUpdateP, &up, sizeof(up), m_vecGroups,
                        1);
    if (!m_ctx.SubmitBatch(error))
      return false;
    parity ^= 1;
    ++iter;
    rr = scal[kRr];

    // TRUE-residual replacement (GPU1 lesson: the fp32 recurrence residual
    // reads optimistic — measured ~25x after only 30 MG iterations):
    // periodically, and ALWAYS before believing convergence, refresh
    // r = f - K x and re-precondition into the slot the next iteration reads
    // as rzOld (p kept). Exit decisions AND the best-iterate bookkeeping run
    // only on these fresh measurements — a drifted recurrence value in
    // between must neither exit the loop nor be snapshotted as "best".
    bool fresh = false;
    if (std::isfinite(rr) && (iter % 16 == 0 || rr <= target)) {
      if (!m_ctx.BeginBatch(error))
        return false;
      m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
      m_ctx.BatchDispatch(m_spmv, m_setSpmvX, &ep, sizeof(ep), m_elemGx,
                          m_elemGy);
      m_ctx.BatchDispatch(m_residual, m_setResidual, &np, sizeof(np),
                          m_vecGroups, 1);
      if (!m_ctx.SubmitBatch(error))
        return false;
      if (!ApplyMgPrecond(parity == 0 ? kRzA : kRzB, error))
        return false;
      rr = scal[kRr];
      fresh = true;
    }
    // lastIterate: "best" is the latest fresh finite iterate; no restarts.
    if (fresh && std::isfinite(rr) && (lastIterate || rr < bestRr)) {
      bestRr = rr;
      std::memcpy(bestX.data(), m_x.mapped, m_x.size);
      sinceBest = 0;
    } else if (fresh) {
      ++sinceBest;
    }
    if (lastIterate) {
      if (!std::isfinite(rr))
        break;
      continue;
    }
    if (!std::isfinite(rr) || sinceBest >= kStallIters) {
      restartsNoGain = bestRr < 0.999f * bestAtRestart ? 0 : restartsNoGain + 1;
      bestAtRestart = bestRr;
      std::memcpy(m_x.mapped, bestX.data(), m_x.size);
      if (!startCg())
        return false;
      rr = scal[kRr];
      if (!std::isfinite(rr)) {
        if (error)
          *error = "CG breakdown (non-finite residual)";
        return false;
      }
      parity = 0;
      sinceBest = 0;
    }
  }

  std::memcpy(m_x.mapped, bestX.data(), m_x.size);
  x = std::move(bestX);
  if (stats) {
    stats->iterations = iter;
    stats->relResidual = std::sqrt(bestRr / bb);
    stats->converged = bestRr <= target;
    stats->lambdaMax = m_lambdaMax;
  }
  return true;
}

} // namespace gpu
