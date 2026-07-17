#include "femgpu/TopoCgSolver.h"

#include "cg_copy_spv.h"
#include "cg_diag_spv.h"
#include "cg_dot_spv.h"
#include "cg_precond_dot_spv.h"
#include "cg_residual_spv.h"
#include "cg_update_p_spv.h"
#include "cg_update_xr_spv.h"
#include "fem_spmv_spv.h"

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

}  // namespace

std::unique_ptr<TopoCgSolver> TopoCgSolver::Create(ComputeContext& ctx,
                                                   const SpmvSystem& sys,
                                                   std::string* error) {
  auto fail = [&](const std::string& m) -> std::unique_ptr<TopoCgSolver> {
    if (error) *error = m;
    return nullptr;
  };
  if (sys.nFree <= 0 || sys.nElems <= 0) return fail("empty CG system");
  if (sys.dof.size() != static_cast<size_t>(sys.nElems) * 30 ||
      sys.ke.size() != static_cast<size_t>(sys.nElems) * 900)
    return fail("CG system arrays have the wrong size");

  std::unique_ptr<TopoCgSolver> s(new TopoCgSolver(ctx));
  s->m_nFree = sys.nFree;
  s->m_nElems = sys.nElems;
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
      !pipe(s->m_precondDot, k_cg_precond_dot_spv, k_cg_precond_dot_spv_size,
            4, sizeof(TwoSlotPush)) ||
      !pipe(s->m_copy, k_cg_copy_spv, k_cg_copy_spv_size, 2, sizeof(NPush)) ||
      !pipe(s->m_dot, k_cg_dot_spv, k_cg_dot_spv_size, 3, sizeof(SlotPush)) ||
      !pipe(s->m_updateXr, k_cg_update_xr_spv, k_cg_update_xr_spv_size, 5,
            sizeof(TwoSlotPush)) ||
      !pipe(s->m_updateP, k_cg_update_p_spv, k_cg_update_p_spv_size, 3,
            sizeof(TwoSlotPush)))
    return nullptr;

  // Buffers.
  auto make = [&](Buffer& b, VkDeviceSize size) {
    return ctx.CreateBuffer(size, b, error);
  };
  const VkDeviceSize vec = static_cast<VkDeviceSize>(sys.nFree) * sizeof(float);
  if (!make(s->m_dof, sys.dof.size() * sizeof(int32_t)) ||
      !make(s->m_ke, sys.ke.size() * sizeof(float)) ||
      !make(s->m_scale, sys.nElems * sizeof(float)) ||
      !make(s->m_diagK, vec) || !make(s->m_f, vec) || !make(s->m_x, vec) ||
      !make(s->m_r, vec) || !make(s->m_z, vec) || !make(s->m_p, vec) ||
      !make(s->m_ap, vec) ||
      !make(s->m_scalars, kNumScalars * sizeof(float)))
    return nullptr;
  std::memcpy(s->m_dof.mapped, sys.dof.data(), s->m_dof.size);
  std::memcpy(s->m_ke.mapped, sys.ke.data(), s->m_ke.size);
  {
    float* sc = static_cast<float*>(s->m_scale.mapped);
    for (int e = 0; e < sys.nElems; ++e) sc[e] = 1.0f;
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
      !bind(s->m_setUpdateP, s->m_updateP,
            {&s->m_z, &s->m_p, &s->m_scalars}))
    return nullptr;

  return s;
}

TopoCgSolver::~TopoCgSolver() {
  for (BoundSet* set :
       {&m_setSpmvP, &m_setSpmvX, &m_setDiag, &m_setResidual, &m_setPrecond,
        &m_setCopyZP, &m_setDotFF, &m_setDotPAp, &m_setUpdateXr, &m_setUpdateP})
    m_ctx.Destroy(*set);
  for (Buffer* b : {&m_dof, &m_ke, &m_scale, &m_diagK, &m_f, &m_x, &m_r, &m_z,
                    &m_p, &m_ap, &m_scalars})
    m_ctx.Destroy(*b);
  for (Pipeline* p : {&m_spmv, &m_diag, &m_residual, &m_precondDot, &m_copy,
                      &m_dot, &m_updateXr, &m_updateP})
    m_ctx.Destroy(*p);
}

bool TopoCgSolver::SetScales(const std::vector<float>& scales,
                             std::string* error) {
  if (scales.size() != static_cast<size_t>(m_nElems)) {
    if (error) *error = "scale count != element count";
    return false;
  }
  std::memcpy(m_scale.mapped, scales.data(), m_scale.size);
  m_scalesDirty = true;
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
  m_ctx.BatchFill(m_scalars, rzNew * sizeof(float), sizeof(float), 0);
  m_ctx.BatchFill(m_scalars, kRr * sizeof(float), sizeof(float), 0);
  const TwoSlotPush pz{n, rzNew, kRr};
  m_ctx.BatchDispatch(m_precondDot, m_setPrecond, &pz, sizeof(pz), m_vecGroups,
                      1);
  // beta = rzNew/rzOld; p = z + beta p
  const TwoSlotPush up{n, rzNew, rzOld};
  m_ctx.BatchDispatch(m_updateP, m_setUpdateP, &up, sizeof(up), m_vecGroups, 1);
}

bool TopoCgSolver::Solve(const std::vector<float>& f, std::vector<float>& x,
                         float tol, int maxIter, CgStats* stats,
                         std::string* error) {
  if (f.size() != static_cast<size_t>(m_nFree)) {
    if (error) *error = "f size != nFree";
    return false;
  }
  x.resize(m_nFree, 0.0f);
  std::memcpy(m_f.mapped, f.data(), m_f.size);
  std::memcpy(m_x.mapped, x.data(), m_x.size);

  const uint32_t n = static_cast<uint32_t>(m_nFree);
  const ElemPush ep{static_cast<uint32_t>(m_nElems), m_elemGx};
  const NPush np{n};
  const float* scal = static_cast<const float*>(m_scalars.mapped);

  // ---- Init: preconditioner diagonal + |f|^2 ------------------------------
  if (!m_ctx.BeginBatch(error)) return false;
  if (m_scalesDirty) {
    m_ctx.BatchFill(m_diagK, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_diag, m_setDiag, &ep, sizeof(ep), m_elemGx,
                        m_elemGy);
    m_scalesDirty = false;
  }
  m_ctx.BatchFill(m_scalars, kBb * sizeof(float), sizeof(float), 0);
  const SlotPush bbp{n, kBb};
  m_ctx.BatchDispatch(m_dot, m_setDotFF, &bbp, sizeof(bbp), m_vecGroups, 1);
  if (!m_ctx.SubmitBatch(error)) return false;
  const float bb = scal[kBb];
  if (!std::isfinite(bb)) {
    if (error) *error = "CG breakdown (non-finite |f| — bad load vector?)";
    return false;
  }
  if (bb <= 0.0f) {  // zero load: x = 0 is exact
    std::fill(x.begin(), x.end(), 0.0f);
    if (stats) *stats = {0, 0.0f, true};
    return true;
  }
  const float target = tol * tol * bb;

  // (Re)starts the CG from the current device x: true residual r = f - K x,
  // z = M^-1 r, p = z, rz -> slot A, rr refreshed.
  auto startCg = [&]() -> bool {
    if (!m_ctx.BeginBatch(error)) return false;
    m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_spmv, m_setSpmvX, &ep, sizeof(ep), m_elemGx,
                        m_elemGy);
    m_ctx.BatchDispatch(m_residual, m_setResidual, &np, sizeof(np),
                        m_vecGroups, 1);
    m_ctx.BatchFill(m_scalars, kRzA * sizeof(float), sizeof(float), 0);
    m_ctx.BatchFill(m_scalars, kRr * sizeof(float), sizeof(float), 0);
    const TwoSlotPush pz{n, kRzA, kRr};
    m_ctx.BatchDispatch(m_precondDot, m_setPrecond, &pz, sizeof(pz),
                        m_vecGroups, 1);
    m_ctx.BatchDispatch(m_copy, m_setCopyZP, &np, sizeof(np), m_vecGroups, 1);
    return m_ctx.SubmitBatch(error);
  };
  if (!startCg()) return false;
  float rr = scal[kRr];
  if (!std::isfinite(rr)) {
    if (error) *error = "CG breakdown (non-finite residual)";
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
  std::vector<float> bestX(x);  // x0 (already uploaded) is the initial best
  while (rr > target && iter < maxIter && restartsNoGain < 2) {
    const int chunk = std::min(trace ? 1 : kChunk, maxIter - iter);
    if (!m_ctx.BeginBatch(error)) return false;
    for (int k = 0; k < chunk; ++k) {
      RecordIteration(parity);
      parity ^= 1;
    }
    // Residual replacement + rz/rr refresh (into the slot the NEXT iteration
    // reads as rzOld, i.e. the current parity's slot).
    m_ctx.BatchFill(m_ap, 0, VK_WHOLE_SIZE, 0);
    m_ctx.BatchDispatch(m_spmv, m_setSpmvX, &ep, sizeof(ep), m_elemGx,
                        m_elemGy);
    m_ctx.BatchDispatch(m_residual, m_setResidual, &np, sizeof(np),
                        m_vecGroups, 1);
    const uint32_t rzSlot = parity == 0 ? kRzA : kRzB;
    m_ctx.BatchFill(m_scalars, rzSlot * sizeof(float), sizeof(float), 0);
    m_ctx.BatchFill(m_scalars, kRr * sizeof(float), sizeof(float), 0);
    const TwoSlotPush rp{n, rzSlot, kRr};
    m_ctx.BatchDispatch(m_precondDot, m_setPrecond, &rp, sizeof(rp),
                        m_vecGroups, 1);
    if (!m_ctx.SubmitBatch(error)) return false;
    iter += chunk;
    rr = scal[kRr];
    if (trace)
      std::fprintf(stderr, "cg it=%d rr=%g rzA=%g rzB=%g pAp=%g\n", iter,
                   scal[kRr], scal[kRzA], scal[kRzB], scal[kPAp]);
    if (std::isfinite(rr) && rr < bestRr) {
      bestRr = rr;
      std::memcpy(bestX.data(), m_x.mapped, m_x.size);
      chunksSinceBest = 0;
    } else {
      ++chunksSinceBest;
    }
    if (!std::isfinite(rr) || chunksSinceBest >= kStallChunks) {
      // Restart from the best iterate (a NaN — alpha noise at the floor —
      // also lands here and is cleared by the fresh residual). No gain since
      // the previous restart, twice in a row, means the floor: stop there.
      restartsNoGain = bestRr < 0.999f * bestAtRestart ? 0 : restartsNoGain + 1;
      bestAtRestart = bestRr;
      std::memcpy(m_x.mapped, bestX.data(), m_x.size);
      if (!startCg()) return false;
      rr = scal[kRr];
      if (!std::isfinite(rr)) {  // a fresh true residual is NaN: real trouble
        if (error) *error = "CG breakdown (non-finite residual)";
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
  }
  return true;
}

}  // namespace gpu
