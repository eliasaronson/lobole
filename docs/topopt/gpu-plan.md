# GPU topology solver — plan

> Status: **GPU0 + GPU1 + GPU2 SHIPPED** (GPU2 = a measured negative:
> Jacobi stays the default). The linear solve is the
> topology wall: CPU CG-IC already cut a bulky 10k-tet cube from 42 s/iter
> (direct) to 2.8 s/iter, but fine meshes want another order of magnitude. This
> plan is the GPU matrix-free CG that gets it.
>
> GPU0 result: `fem_gpu` static lib (`src/femgpu/` — `gpu::ComputeContext` +
> `gpu::TopoSpmv`), `fem_spmv.comp` matrix-free element SpMV with float
> `atomicAdd` scatter, embedded SPIR-V (no runtime shader path), `femgpu_tests`
> self-skipping without a device. Gate diff vs the CPU fp64 oracle on the same
> fp32 inputs: **rel-∞ error ~1e-7** (bar 1e-4) on both the structured tet10
> box and a real Netgen mesh, with SIMP-like scales spanning 1e-6..1. The
> scatter/atomics risk is retired.
>
> GPU1 result: on-device Jacobi-PCG (`gpu::TopoCgSolver` — all kernels
> recorded in 32-iteration chunks, one scalar readback per chunk), injected
> into `TopologyOptimize` via the `fem::TopoLinearSolver` seam
> (`gpu::TopoGpuSolver`), auto-selected for app topology runs when a hardware
> device exists (`DESIGNER_TOPO_GPU=0/1` overrides). Full SIMP loop on the
> 10368-tet cube: **1.32 s/iter GPU vs 3.57 s/iter CPU CG-IC (2.7x)**,
> compliance matching to 0.15%. fp32 lessons baked in (measured, not
> theoretical): the recurrence residual reads ~100x optimistic → true-residual
> replacement at chunk boundaries; fp32 PCG walks AWAY past its round-off
> floor → best-iterate snapshot is what's returned; the L2 residual
> legitimately grows ~20x early (CG minimizes A-norm error, identical in
> fp64) → no short-term divergence guards, only long-window stall + restart
> from best. Jacobi needs ~25x the iterations of IC — that gap is the
> preconditioner headroom, now the throughput lever since per-iteration cost
> is already near memory bandwidth.
>
> GPU2 result — **negative, by the numbers**: Chebyshev-Jacobi preconditioning
> (degree-m polynomial of D⁻¹K per outer iteration; on-device power-iteration
> λmax estimate ×1.1; host-computed coefficients as push constants, zero
> inner-loop reductions) cuts outer iterations ~2.3x but costs m SpMVs per
> outer — full-SIMP wall time LOSES 2-2.5x vs Jacobi at every degree/ratio
> tried (d8/r30, d4/r10, d3/r6). Cause: CG is already the optimal polynomial
> method, and the convergence-gating void-mode tail (scales ~1e-6) sits far
> below any usable Chebyshev interval — polynomial preconditioning cannot
> reach what actually gates SIMP systems. Jacobi stays the app default;
> Chebyshev remains selectable (FEMGPU_PRECOND=cheb, FEMGPU_CHEB_DEGREE/
> RATIO) and its kernels + eigenvalue estimator are the GPU3 multigrid
> smoother, where a top-of-spectrum polynomial is exactly right and coarse
> grids handle the tail. **The remaining ~10x lives in GPU3.**

## Feasibility (settled first, because it was the whole risk)

The concern was verification: the assistant can't launch windowed Vulkan
(device-lost), and GPU bugs are miserable to debug by proxy. Two probes
(`scratchpad/vkprobe.cpp`, `vkcompute.cpp`) settled it:

- **A GPU is visible headlessly** from the build/test environment — the RTX
  4060 (discrete), an Intel Arc iGPU, and llvmpipe (software), all with compute
  queues. No window/swapchain needed.
- **A full compute round-trip works and is CPU-verifiable**: instance → pick
  discrete GPU → device + compute queue → storage buffer → compute pipeline →
  `vkCmdDispatch` → readback → assert against the CPU result. `COMPUTE OK`.

So the GPU solver is buildable and testable in-tree like any other component
(a headless Catch2 test that runs a kernel and diffs against the CPU path).
Software fallback (llvmpipe) means it can even run where no real GPU exists.

## Why matrix-free CG (not GPU direct, not CPU-assembled SpMV)

- **Direct factorization is the wrong GPU target** — inherently sequential
  (elimination tree, fill-in). Its fill-in blow-up is exactly what we're
  escaping.
- **Matrix-free** — never assemble K. Each CG iteration's `A·p` is computed
  element-by-element in a compute shader (one workgroup per tet: load the 30
  DOF of `p`, apply the element's `scale·Ke0`, scatter the 30 results back).
  No global matrix to store or upload; the arithmetic is where the GPU wins.
- **Everything stays on-device** — the CG loop (SpMV, dot products via parallel
  reduction, axpy) runs in compute shaders so vectors never cross PCIe between
  iterations. Only the final `u` is read back per SIMP step.
- **Preconditioner** is the crux (CPU proved diagonal is ~4x worse than IC).
  Options in escalation order: Jacobi (trivial, weak), Chebyshev/polynomial
  (matrix-free, no setup, moderate), geometric multigrid (strong, the real
  answer, large). Start Jacobi to get the pipeline correct, then Chebyshev,
  then multigrid only if convergence still gates.

## Architecture — a separate `fem_gpu` library, injected

`designer_core` is deliberately GPU-free (headless, unit-tested — see
`CMakeLists.txt:162`). Do **not** pull Vulkan into it. Instead:

- **`fem_gpu` static lib** (links `Vulkan::Vulkan`): a headless
  `gpu::ComputeContext` (instance/device/compute-queue, buffer + pipeline +
  dispatch helpers, no surface) and `gpu::TopoCgSolver` (the matrix-free CG).
- **Injection seam in `designer_core`**: `TopologyOptimize` gains an optional
  `LinearSolve` callback (or a tiny abstract solver) — default null = the
  existing Eigen CG-IC. The GPU solver implements it. So `designer_core` stays
  GPU-free and the CPU path is always the fallback (and the correctness oracle).
- The app constructs the `fem_gpu` solver once (reusing its own Vulkan device
  is possible later; a private headless context is simplest first) and passes
  it into topology runs. Absent a GPU, topology silently uses the CPU path.
- Tests link `fem_gpu` and verify each kernel against the CPU (`test_femgpu`),
  self-skipping (exit 77 / SKIP) if no compute device is present.

## Phasing

0. **DONE — GPU0 — compute infra + matrix-free SpMV (gate).** `gpu::ComputeContext`
   (mirrors the proven probe: headless device, host-visible + device-local
   buffers, SPIR-V pipeline, dispatch, fence). `fem_spmv.comp`: element-wise
   `Ap = K(ρ)·p` over the reduced DOF map, scatter via `atomicAdd` (float
   atomics — RTX 4060 supports `VK_EXT_shader_atomic_float`; a node-gather
   fallback if unavailable). **Gate:** GPU `A·p` matches the CPU element
   assembly to fp tolerance on a hand mesh + a Netgen mesh. If float atomics or
   the diff don't hold up, stop and rethink (gather-based scatter) before
   building CG.
1. **DONE — GPU1 — on-device CG.** Reduction (dot), axpy, and the CG driver as
   kernels; Jacobi preconditioner (element-assembled diagonal); warm-start from
   the previous SIMP `u`. Wire into `TopologyOptimize` via the injection seam.
   Benchmark vs CPU CG-IC on the `[.][topobench]` cube; verify the optimized
   density matches the CPU result (same gate metrics).
2. **DONE — GPU2 — Chebyshev preconditioner** (needs only a cheap eigenvalue
   estimate; matrix-free) if Jacobi-CG iteration counts gate throughput.
   Built, benched, and rejected as the default (see status above) — kept as
   the GPU3 smoother.
3. **GPU3 — geometric multigrid V-cycle** only if still needed for the finest
   meshes — the strong-but-large preconditioner. Its own sub-plan.

Order rationale: 0 gates on the only correctness risk (the scatter/atomics) and
reuses the proven probe; CG before preconditioner tuning; the CPU path stays
the oracle throughout, so every GPU result is diffed against a known-good
answer.

## Risks

- **Scatter correctness/perf** — float `atomicAdd` contention on shared nodes.
  Mitigation: the gate diffs against CPU; if atomics are slow, switch to a
  node-centric gather (precomputed node→incident-element lists).
- **fp32 vs fp64** — GPUs are fp32-fast, fp64-slow. Elasticity CG usually
  tolerates fp32 with a good preconditioner; if accuracy/convergence suffers,
  mixed precision (fp32 SpMV, fp64 residual) is the escape hatch. The CPU
  oracle catches drift.
- **Device sharing** — a private headless context is simplest but means a
  second Vulkan device alongside the app's. Fine for v1; unifying with the
  app's device is a later optimization.
- **Effort** — this is a multi-milestone workstream, not a bolt-on. GPU0 is the
  committable first step and is self-contained (no `TopologyOptimize` change).
