#include "femgpu/TopoGpuSolver.h"

#include "core/Parallel.h"
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

namespace {

// The fp64 side of the solve: K·u for the residual, and the element
// energies, from the element matrices in fp64 — NOT the fp32 copies the GPU
// holds. Rounding Ke to fp32 breaks its rigid-body null space by ~1e-7, and
// a slender part's displacement is mostly rigid motion of each element (a
// cantilever's tip translates and turns far more than it strains): ue^T Ke ue
// is then the small difference of large terms, and from fp32 Ke the
// sensitivities came out wrong enough to walk SIMP to a different design.
// Stored as the upper triangle (465 of 900 entries), about the size of the
// fp32 full matrix. Gathered by row rather than scattered by element: each
// free DOF sums its own (element, local row) contributions, so the product
// needs no atomics, splits across threads by fixed row blocks, and is
// bit-identical run to run.
class HostOperator {
public:
  static constexpr int kPacked = 465; // 30 * 31 / 2

  HostOperator() {
    for (int a = 0, k = 0; a < 30; ++a)
      for (int c = a; c < 30; ++c, ++k)
        m_idx[a][c] = m_idx[c][a] = static_cast<int16_t>(k);
  }

  // `dof`: 30 slots per element (-1 = fixed); `ke`: kPacked per element.
  void Build(int nFree, std::vector<int32_t> dof, std::vector<double> ke) {
    m_nFree = nFree;
    m_nElems = static_cast<int>(dof.size() / 30);
    m_dof = std::move(dof);
    m_ke = std::move(ke);
    m_rowStart.assign(static_cast<size_t>(nFree) + 1, 0);
    for (int32_t g : m_dof)
      if (g >= 0)
        ++m_rowStart[static_cast<size_t>(g) + 1];
    for (int i = 0; i < nFree; ++i)
      m_rowStart[i + 1] += m_rowStart[i];
    m_rows.resize(m_rowStart[nFree]);
    std::vector<int32_t> fill(m_rowStart.begin(), m_rowStart.end() - 1);
    for (size_t slot = 0; slot < m_dof.size(); ++slot)
      if (m_dof[slot] >= 0)
        m_rows[fill[m_dof[slot]]++] = static_cast<int32_t>(slot);
  }

  // out = K(scale)·u, resized to nFree.
  void Apply(const std::vector<double>& scale, const std::vector<double>& u,
             std::vector<double>& out) const {
    const int n = m_nFree;
    out.resize(n);
    constexpr int kBlock = 2048;
    const size_t blocks = (static_cast<size_t>(n) + kBlock - 1) / kBlock;
    parallel::For(blocks, [&](size_t b) {
      const int lo = static_cast<int>(b) * kBlock;
      const int hi = std::min(n, lo + kBlock);
      for (int g = lo; g < hi; ++g) {
        double acc = 0.0;
        for (int32_t k = m_rowStart[g]; k < m_rowStart[g + 1]; ++k) {
          const int32_t slot = m_rows[k];
          const int e = slot / 30, a = slot % 30;
          const int32_t* dof = &m_dof[static_cast<size_t>(e) * 30];
          const double* ke = &m_ke[static_cast<size_t>(e) * kPacked];
          const int16_t* idx = m_idx[a];
          double row = 0.0;
          for (int c = 0; c < 30; ++c)
            if (dof[c] >= 0)
              row += ke[idx[c]] * u[dof[c]];
          acc += scale[e] * row;
        }
        out[g] = acc;
      }
    });
  }

  // se[e] = ue^T Ke0 ue at full modulus (fixed DOFs are zero).
  void Energies(const std::vector<double>& u, std::vector<double>& se) const {
    const int nE = m_nElems;
    se.resize(nE);
    constexpr int kBlock = 512;
    const size_t blocks = (static_cast<size_t>(nE) + kBlock - 1) / kBlock;
    parallel::For(blocks, [&](size_t b) {
      const int lo = static_cast<int>(b) * kBlock;
      const int hi = std::min(nE, lo + kBlock);
      double ue[30];
      for (int e = lo; e < hi; ++e) {
        const int32_t* dof = &m_dof[static_cast<size_t>(e) * 30];
        const double* ke = &m_ke[static_cast<size_t>(e) * kPacked];
        for (int a = 0; a < 30; ++a)
          ue[a] = dof[a] >= 0 ? u[dof[a]] : 0.0;
        double diag = 0.0, off = 0.0;
        for (int a = 0, k = 0; a < 30; ++a) {
          diag += ke[k++] * ue[a] * ue[a];
          for (int c = a + 1; c < 30; ++c)
            off += ke[k++] * ue[a] * ue[c];
        }
        se[e] = diag + 2.0 * off;
      }
    });
  }

private:
  int m_nFree = 0, m_nElems = 0;
  std::vector<int32_t> m_dof;
  std::vector<double> m_ke;        // kPacked per element, upper triangle
  std::vector<int32_t> m_rowStart; // nFree + 1
  std::vector<int32_t> m_rows;     // element slot (e * 30 + a) per entry
  int16_t m_idx[30][30];           // (row, col) -> packed index
};

double Dot(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
    s += a[i] * b[i];
  return s;
}

} // namespace

struct TopoGpuSolver::Impl {
  std::unique_ptr<ComputeContext> ctx;
  std::unique_ptr<TopoCgSolver> cg; // keeps the MG hierarchy alive via opts
  HostOperator K;                   // the fp64 residual's operator
  int nFree = 0;
  std::vector<float> scaleF, rF, dF; // fp32 staging
  bool haveWarmStart = false;
};

TopoGpuSolver::TopoGpuSolver() : m_impl(new Impl) {}
TopoGpuSolver::~TopoGpuSolver() = default;

const std::string& TopoGpuSolver::DeviceName() const {
  return m_impl->ctx->DeviceName();
}

std::unique_ptr<TopoGpuSolver>
TopoGpuSolver::Create(bool allowSoftware, std::string* error,
                      const std::string& preferredDevice) {
  auto ctx = ComputeContext::Create(error, preferredDevice);
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
  return SetupDomains(mesh, {material}, dofMap, nFree, error);
}

bool TopoGpuSolver::SetupDomains(const fem::FemMesh& mesh,
                                 const std::vector<fem::Material>& byDomain,
                                 const std::vector<int>& dofMap, int nFree,
                                 std::string* error) {
  using namespace fem::internal;
  Impl& im = *m_impl;
  if (byDomain.empty()) {
    if (error)
      *error = "no material";
    return false;
  }

  // Pack the reduced system exactly the way the CPU assembly builds it:
  // per-tet Ke at full modulus (fp32-cast), DOF slots node-major, -1 = fixed.
  // Each tet's elasticity is its domain's, as SolveCases picks it; topology
  // passes one material, so every tet shares it.
  std::vector<Eigen::Matrix<double, 6, 6>> Ds;
  for (const fem::Material& m : byDomain)
    Ds.push_back(Elasticity(m.youngMPa, m.poisson));
  SpmvSystem sys;
  sys.nFree = nFree;
  sys.nElems = static_cast<int>(mesh.tets.size());
  sys.dof.reserve(static_cast<size_t>(sys.nElems) * 30);
  sys.ke.reserve(static_cast<size_t>(sys.nElems) * 900);
  std::vector<double> ke64; // the host's fp64 copy, upper triangle
  ke64.reserve(static_cast<size_t>(sys.nElems) * HostOperator::kPacked);
  for (size_t ti = 0; ti < mesh.tets.size(); ++ti) {
    const auto& tet = mesh.tets[ti];
    const size_t dom =
        mesh.tetDomain.empty() ? 0 : static_cast<size_t>(mesh.tetDomain[ti]);
    const Eigen::Matrix<double, 6, 6>& D0 = Ds[std::min(dom, Ds.size() - 1)];
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
    for (int r = 0; r < 30; ++r)
      for (int c = r; c < 30; ++c)
        ke64.push_back(Ke(r, c));
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
  // The device has its fp32 copy; the host keeps fp64 for the residual and
  // the energies.
  im.K.Build(nFree, std::move(sys.dof), std::move(ke64));
  im.nFree = nFree;
  im.scaleF.resize(static_cast<size_t>(mesh.tets.size()));
  im.rF.resize(nFree);
  im.dF.assign(nFree, 0.0f);
  im.haveWarmStart = false;
  return true;
}

// Mixed-precision refinement (docs/topopt/README.md#mixed-precision-refinement)
//
// fp32 cannot SOLVE a slender bending part (kappa to ~3e8), but it can still
// IMPROVE an answer: each outer step measures the true residual r = F - K u in
// fp64, hands r/|r| to the fp32 GPU CG loosely, and folds the correction back
// into the fp64 u. The steps are combined as a flexible CG (Notay's FCG(1)):
// the correction is K-orthogonalized against the previous one and taken with
// the energy-optimal step length, both in fp64. An exact inner solve makes
// that plain refinement (step length 1, nothing to orthogonalize); an inexact
// one — fp32 on these systems always is — loses nothing for it, and NO inner
// result can raise the energy error, because a step along any direction is
// chosen to minimize it.
bool TopoGpuSolver::Solve(const std::vector<double>& scale,
                          const std::vector<double>& F, std::vector<double>& u,
                          std::string* error) {
  return Refine(scale, F, u, error, nullptr);
}

bool TopoGpuSolver::SolveStrict(const std::vector<double>& F,
                                std::vector<double>& u,
                                fem::LinearSolveControl& control,
                                std::string* error) {
  // Full modulus everywhere: a static study's K.
  const std::vector<double> ones(m_impl->scaleF.size(), 1.0);
  return Refine(ones, F, u, error, &control);
}

bool TopoGpuSolver::Refine(const std::vector<double>& scale,
                           const std::vector<double>& F, std::vector<double>& u,
                           std::string* error,
                           fem::LinearSolveControl* strict) {
  using clk = std::chrono::steady_clock;
  Impl& im = *m_impl;
  if (!im.cg) {
    if (error)
      *error = "Setup was not called";
    return false;
  }
  const int n = im.nFree;
  for (size_t e = 0; e < scale.size(); ++e)
    im.scaleF[e] = static_cast<float>(scale[e]);
  if (!im.cg->SetScales(im.scaleF, error))
    return false;

  const auto t0 = clk::now();
  // FEMGPU_SOLVE_TRACE=1: one stderr line per refinement step and per solve.
  const bool trace = std::getenv("FEMGPU_SOLVE_TRACE") != nullptr;
  const double fNorm = std::sqrt(Dot(F, F));
  if (!(fNorm > 0.0)) {
    u.assign(n, 0.0);
    return true;
  }

  // The target, as an fp64 relative residual: the old fp32 one, now
  // reachable. Compliance error goes as its square. A strict (static) solve
  // names its own.
  const double kTol = strict ? strict->tolerance : 1e-5;
  // The inner fp32 solve only has to contract, and a loose one is cheaper
  // per digit: 1e-1 a step measured fastest overall (1e-2 spent its
  // iterations solving the fp32 operator more exactly than it is worth, and
  // on a 40:1 beam did not finish), with 0.2 and 0.05 close behind.
  constexpr float kInnerTol = 1e-1f;
  // Iterations per inner solve. The first solve of a run starts from
  // nothing and gets more; past that the warm start leaves less to do.
  const int innerMax = im.haveWarmStart && !strict ? 128 : 256;
  // A static solve starts from nothing and wants two more decades than
  // SIMP: more steps, still ended early by the stall test below.
  const int kMaxSteps = strict ? 400 : 40;

  // Warm start: the previous SIMP iteration's displacement (the caller hands
  // back what this returned last time), scaled by the energy-optimal factor
  // for the NEW stiffness. After a large density change the old field can be
  // a poor fit — the pan/tilt yoke's had a residual 13x |F| — and scaling is
  // never worse than either it or zero.
  std::vector<double> Ku, r(n);
  // A strict solve is a different right-hand side from whatever came before:
  // no warm start.
  if (!im.haveWarmStart || strict || static_cast<int>(u.size()) != n)
    u.assign(n, 0.0);
  double hostMs = 0.0; // the fp64 products, for the trace
  auto th = clk::now();
  im.K.Apply(scale, u, Ku);
  hostMs += std::chrono::duration<double, std::milli>(clk::now() - th).count();
  // The potential energy Pi(u) = u.K.u/2 - F.u: zero at u = 0, minimal at
  // the solution, and lowered by every step below. It is the measure of
  // progress; the residual norm is not monotone.
  double energy = 0.0;
  {
    const double uKu = Dot(u, Ku), uF = Dot(u, F);
    const double beta = uKu > 0.0 && uF > 0.0 ? uF / uKu : 0.0;
    for (int i = 0; i < n; ++i) {
      u[i] *= beta;
      r[i] = F[i] - beta * Ku[i];
    }
    energy = -0.5 * beta * uF;
  }
  double rel = std::sqrt(Dot(r, r)) / fNorm;
  const double relStart = rel;

  std::vector<double> d(n), Kd, p, Kp;
  double pKp = 0.0;
  int steps = 0, innerIters = 0, stalls = 0;
  while (rel > kTol && steps < kMaxSteps) {
    if (strict && strict->cancelled && strict->cancelled()) {
      if (error)
        *error = "cancelled";
      return false;
    }
    ++steps;
    // Rescale into fp32's comfortable range: r shrinks by orders of
    // magnitude over the run, and a tiny right-hand side wastes the
    // exponent range the solve needs.
    const double rNorm = rel * fNorm;
    for (int i = 0; i < n; ++i)
      im.rF[i] = static_cast<float>(r[i] / rNorm);
    std::fill(im.dF.begin(), im.dF.end(), 0.0f);
    // The inner solve returns where CG got to, not its best iterate by
    // residual norm: on a bending load that norm rises long before it falls,
    // so the best iterate from a zero start was zero and refinement stepped
    // by nothing, forever (TopoCgSolver::Solve, lastIterate).
    CgStats st;
    if (!im.cg->Solve(im.rF, im.dF, kInnerTol, innerMax, &st, error,
                      /*lastIterate=*/true))
      return false;
    innerIters += st.iterations;
    for (int i = 0; i < n; ++i)
      d[i] = static_cast<double>(im.dF[i]) * rNorm;
    th = clk::now();
    im.K.Apply(scale, d, Kd);
    hostMs +=
        std::chrono::duration<double, std::milli>(clk::now() - th).count();

    // FCG(1): K-orthogonalize against the previous direction, then the
    // energy-optimal step.
    if (pKp > 0.0) {
      const double gamma = Dot(d, Kp) / pKp;
      for (int i = 0; i < n; ++i) {
        p[i] = d[i] - gamma * p[i];
        Kp[i] = Kd[i] - gamma * Kp[i];
      }
    } else {
      p = d;
      Kp = Kd;
    }
    pKp = Dot(p, Kp);
    if (!(pKp > 0.0) || !std::isfinite(pKp))
      break; // a zero (or broken) direction: nothing left to gain
    const double pr = Dot(p, r);
    const double alpha = pr / pKp;
    for (int i = 0; i < n; ++i) {
      u[i] += alpha * p[i];
      r[i] -= alpha * Kp[i];
    }
    const double gain = 0.5 * pr * alpha; // Pi drops by exactly this
    energy -= gain;
    const double relNew = std::sqrt(Dot(r, r)) / fNorm;
    if (trace)
      std::fprintf(stderr,
                   "FEMGPU_REFINE: step %d  rel %.3e -> %.3e  energy gain "
                   "%.2e of %.2e  inner %d iters to %.2e  alpha %.3f\n",
                   steps, rel, relNew, gain, -energy, st.iterations,
                   st.relResidual, alpha);
    rel = relNew;
    // Short of the target: three steps in a row that lower the energy by
    // less than fp64 can tell from rounding are the floor of what the inner
    // solve can contribute.
    if (gain > 1e-13 * std::abs(energy))
      stalls = 0;
    else if (++stalls >= 3)
      break;
  }

  if (trace)
    std::fprintf(
        stderr,
        "FEMGPU_SOLVE: %.0f ms (fp64 products %.0f ms), %d steps, "
        "%d inner iters, rel res %.2e -> %.2e\n",
        std::chrono::duration<double, std::milli>(clk::now() - t0).count(),
        hostMs, steps, innerIters, relStart, rel);

  if (strict) {
    strict->achieved = rel;
    strict->iterations = innerIters;
    if (!(rel <= kTol)) {
      if (error)
        *error = "GPU solve reached a relative residual of " +
                 std::to_string(rel) + ", not the " + std::to_string(kTol) +
                 " asked for, in " + std::to_string(steps) +
                 " refinement steps";
      return false;
    }
    return true;
  }
  // An unconverged answer is accepted: SIMP reacts to element energies, not
  // to the last digit of u. One no better than u = 0 is not an answer — it
  // reads downstream as a structure carrying no load — so say so and let
  // the caller finish on the CPU.
  if (!(rel < 0.99) || !std::isfinite(rel)) {
    if (error)
      *error = "GPU solve made no progress (relative residual " +
               std::to_string(rel) + " after " + std::to_string(steps) +
               " refinement steps, " + std::to_string(innerIters) +
               " inner iterations)";
    return false;
  }
  im.haveWarmStart = true;
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
  // In fp64 on the host, from the fp64 element matrices — not the GPU
  // kernel, whose fp32 Ke cannot resolve a slender part's energies (see
  // HostOperator). The cost is one pass over Ke, as for one residual.
  im.K.Energies(u, se);
  return true;
}

} // namespace gpu
