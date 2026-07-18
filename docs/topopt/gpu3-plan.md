# GPU3 — geometric multigrid preconditioner (sub-plan)

> Status: **design for sign-off.** The last order of magnitude for the GPU
> topology solver (docs/topopt/gpu-plan.md). Everything here builds on
> measured GPU1/GPU2 facts, not hope.

## Why multigrid, and why nothing else

The measured position after GPU1+GPU2 (45k-DOF cube, harsh SIMP scales):

| solver | outer iterations | per-iteration | note |
|---|---|---|---|
| GPU Jacobi-PCG (shipped) | ~4700–6800 | ~0.21 ms (~70% of memory BW) | the 2.7x baseline |
| GPU Chebyshev-PCG (GPU2) | ~2300 | ×degree SpMVs | net 2–2.5x SLOWER |
| CPU CG-IC | 225 | — | the iteration-count target |

Per-iteration cost is already near the RTX 4060's bandwidth — **iteration
count is the only lever left**, and GPU2 proved polynomials cannot pull it:
CG is already the optimal polynomial method, and the void-mode tail (element
scales ~1e-6) sits far below any usable polynomial interval. Killing the
tail requires a COARSE SPACE that represents those low-energy modes
directly. That is multigrid, and there is no cheaper substitute.

**Target:** ≥5x over Jacobi-PCG at 45k DOF (outer iterations to the low
hundreds), growing with mesh size — the point is ~100k-tet SIMP runs in
about the wall time today's 13k-tet runs take.

## Design

A **V(1,1)-cycle used as the PCG preconditioner**:

- **Fixed and symmetric** per solve (same cycle every application, equal
  pre/post smoothing) — required for CG validity, same rule the Chebyshev
  preconditioner obeyed.
- **Two levels first.** Fine level = the existing matrix-free operator.
  One coarse level already captures the tail; more levels only if the
  coarse solve itself becomes the bottleneck (MG2).
- **Coarsening — geometric node aggregation** (CPU, once per Setup; the
  mesh never changes across SIMP iterations): greedy seeded BFS over the
  node adjacency graph, aggregates of ~2-edge radius. No mesh hierarchy,
  no re-meshing — works on any unstructured tet10 mesh Netgen emits.
- **Prolongation P — rigid-body modes per aggregate**: 6 coarse DOF per
  aggregate (3 translations + 3 rotations about the aggregate centroid),
  entries from node positions. This is the part that makes elasticity MG
  work at all — piecewise-constant P (3 DOF) cannot represent rotation and
  converges poorly. P is built once, stays fixed.
- **Galerkin coarse operator, incrementally reassembled**:
  `A_c = Pᵀ A(ρ) P = Σ_e scale_e · (Pᵀ Ke0 P)_e`. The per-element coarse
  blocks `(Pᵀ Ke0 P)_e` are precomputed once at Setup (small dense blocks —
  an element touches only a few aggregates); each SIMP iteration's new
  scales reduce coarse assembly to a weighted block sum on the CPU
  (~tens of ms at 100k elements), then one Eigen LDLT factorization of the
  coarse system (n_c ≈ nFree/20 → a few thousand DOF → milliseconds).
- **Smoother — Chebyshev(2–4) on the Jacobi-scaled operator**: the GPU2
  kernels (cheb_init/cheb_iter, zero inner reductions) and the on-device
  power-iteration λmax estimator are exactly this smoother. A smoother only
  needs the TOP of the spectrum — precisely where GPU2 measured Chebyshev
  to be effective; the coarse grid handles the tail it could not reach.
- **Cycle placement**: everything on-device except the coarse solve —
  restrict `r_c = Pᵀ r` (scatter kernel, same atomicAdd pattern as the
  SpMV), read back the small coarse vector (~kB), CPU LDLT solve, upload,
  prolong-and-correct kernel, post-smooth. Each preconditioner application
  costs one extra submit + a small PCIe roundtrip; with outer iterations in
  the low hundreds this overhead is noise (~0.1 ms against a saved ~0.2 ms
  × dozens of avoided iterations).
- **fp32 discipline carries over unchanged**: true-residual replacement at
  chunk boundaries, best-iterate snapshot, restart-on-stall — all shipped
  in GPU1 and preconditioner-agnostic.

## Risks (ranked)

1. **High-contrast degradation** — aggregates straddling the void/solid
   boundary weaken the coarse correction; the "right" aggregation depends
   on the density field, which changes every SIMP iteration. Escalation
   path: (a) measure the fixed geometric aggregation first — literature
   says rigid-mode geometric MG holds up acceptably for SIMP; (b) if late
   iterations degrade, re-aggregate every N SIMP iterations with
   scale-weighted edges (Setup is cheap); (c) smoothed aggregation (one
   Jacobi pass on P) as the heavyweight fallback.
2. **Coarse solve on the CPU** — a per-application sync. Bounded by making
   the coarse system small (n/20) and reusing the LDLT analyze pattern; if
   it still gates at scale, MG2 moves it to the GPU or adds a third level.
3. **Implementation surface** — aggregation + P construction is the largest
   new CPU code (several hundred lines with real geometry subtleties).
   Mitigated by MG0: prove it in fp64 Eigen before any kernel exists.

## Phases

- **MG0 — CPU prototype (the gate).** The entire 2-level scheme in Eigen
  fp64 — aggregation, rigid-mode P, Galerkin coarse blocks, Chebyshev
  smoother, V(1,1)-PCG — run against the `[.][topobench]` systems,
  including LATE-iteration SIMP fields (crisp void/solid contrast, the
  hard case). **Gate: outer iterations must drop ≥10x vs Jacobi-PCG in
  fp64.** If the coarse space cannot deliver that with exact arithmetic,
  no GPU kernel will save it — stop and rethink (scale-aware aggregation)
  before writing any Vulkan. Fast to build; kills the dominant risk first.
- **MG1 — GPU integration.** Restrict/prolong kernels, coarse CPU solve in
  the chunk loop, `CgOptions::Precond::Multigrid`, the same honesty checks
  as GPU1 (fp64-verified residuals, design-equivalence vs CPU), full-SIMP
  bench. **Success: ≥3x net over Jacobi on the full-SIMP bench, else the
  GPU2 rule applies — keep Jacobi default, document the numbers.**
- **MG2 — only if measurements demand it**: 3+ levels, smoothed
  aggregation, GPU coarse solve, aggregation refresh cadence.

## What GPU3 does NOT touch

The TopoLinearSolver seam, the SIMP loop, the app wiring, and the CPU path
are all frozen — multigrid is purely a new `CgOptions::Precond` mode inside
`TopoCgSolver` plus Setup-time CPU construction in `TopoGpuSolver`. A
failed gate leaves everything shipped exactly as it is today.
