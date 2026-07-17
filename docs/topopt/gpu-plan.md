# GPU topology solver — plan

> Status: **GPU0 SHIPPED — gate PASS.** The linear solve is the
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
> scatter/atomics risk is retired → GPU1 (on-device CG) is a go.

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
1. **GPU1 — on-device CG.** Reduction (dot), axpy, and the CG driver as
   kernels; Jacobi preconditioner (element-assembled diagonal); warm-start from
   the previous SIMP `u`. Wire into `TopologyOptimize` via the injection seam.
   Benchmark vs CPU CG-IC on the `[.][topobench]` cube; verify the optimized
   density matches the CPU result (same gate metrics).
2. **GPU2 — Chebyshev preconditioner** (needs only a cheap eigenvalue estimate;
   matrix-free) if Jacobi-CG iteration counts gate throughput.
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
