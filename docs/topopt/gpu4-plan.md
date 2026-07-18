# GPU4 — element strain energies on the GPU (sub-plan)

> Status: **SHIPPED** — with a plot twist the profiler forced (below).
>
> Results: the energies kernel gate PASSED (rel-inf 5-8e-8 vs the fp64
> oracle, bar 1e-4) and the CPU sensitivity step went 0.25 s -> 0.01 s per
> SIMP run segment, exactly as designed. But the full-SIMP bench barely
> moved — the new TOPO_PROFILE instrumentation showed the earlier "solve is
> only ~0.1 s" attribution was wrong: mid-SIMP solves were spending 500-1100
> outer iterations grinding at the fp32 residual floor (~1e-2 on real
> concentrated-load/frozen-skin systems) through the stall/restart
> machinery, while their ENERGY (compliance) converged in the first ~100.
> Fix: warm-started SIMP solves are capped at 128 outer iterations (the
> best iterate is returned regardless; cold first solves keep the generous
> cap). A compliance-plateau stop was tried first and REJECTED: at the fp32
> floor the iterate itself wanders (the GPU1 walk-away), so instantaneous
> f^T x never stabilizes — measuring a wandering iterate is the wrong
> concept.
>
> Combined GPU4 outcome (13.6k-tet bench cube): **0.98 -> 0.55 s/iter**
> (CPU 3.47 — 6.3x end-to-end); the bracket demo runs 50 s -> **18 s** in
> the live app, identical design to 4 digits. Diagnostics kept:
> TOPO_PROFILE=1 (per-phase SIMP timing) and FEMGPU_SOLVE_TRACE=1
> (per-solve iterations/residual).

## Why (measured, not projected)

After GPU3, the full SIMP iteration at 13.6k tets costs ~0.98 s of which the
linear solve is only ~0.1 s. The dominant remainder is the **sensitivity
loop** in `TopologyOptimize` (FemTopo.cpp): for every element, every
iteration, it rebuilds the 30×30 element stiffness `Ke0` from scratch
(4 Gauss points of dense Eigen math, ~0.26 MFLOP + allocation churn per
element) only to evaluate the single quadratic form `se = ueᵀ·Ke0·ue`.
That is ~0.5–0.8 s/iter at bracket scale and grows linearly: ~4–6 s/iter at
100k tets — it would re-become the wall at exactly the fine meshes GPU3
unlocked.

The GPU already holds everything the computation needs, resident: the fp32
`Ke0` array (read by every SpMV), the reduced DOF map, and the fresh
displacement (the solve's own output). The energies are one dispatch away.

## Design

**One kernel, one seam method, CPU fallback preserved.**

- **`fem_energy.comp`** — same workgroup-per-element shape as the proven
  `fem_spmv.comp`: stage the element's 30 DOF of `u` in shared memory
  (gather via the dof table; fixed slots = 0, exactly the CPU convention),
  thread `t < 30` computes `ue[t] · (Ke0 row t · ue)`, workgroup tree-reduce,
  single write `se[e]`. **No atomics** (one element, one writer). Bindings:
  dof, ke, u, se; push `{nElems, groupsX}` (2D dispatch, as everywhere).
  Note the resident `ke` buffer is `Ke0` at FULL modulus — the SIMP scale is
  applied separately in the SpMV — which is precisely what the sensitivity
  formula wants. Cost ≈ one extra SpMV-sized read per SIMP iteration
  (~1–2 ms at 100k tets) vs ~seconds on the CPU.

- **Seam extension** (`fem::TopoLinearSolver`, FemTopo.h):

  ```cpp
  // Optional capability: per-element strain energies se_e = ue^T Ke0 ue at
  // full modulus, for the reduced displacement `u` (size nFree). Default
  // false = caller uses its own CPU loop (which remains the oracle).
  virtual bool ElementEnergies(const std::vector<double>& u,
                               std::vector<double>& se, std::string* error) {
    return false;
  }
  ```

  `TopologyOptimize`'s sensitivity loop becomes: try the solver's
  `ElementEnergies`; on `false`, run the existing per-element CPU loop
  unchanged. A hard error is never needed — the fallback is always correct.

- **Plumbing**: `TopoCgSolver::ElementEnergies` (uploads the passed `u` —
  240 KB at 200k DOF, negligible; deliberately NOT reusing the resident x
  buffer so the method has no hidden coupling to call order) →
  `TopoGpuSolver::ElementEnergies` does the fp64↔fp32 staging. New bound
  set + pipeline; ~150 lines total across the stack.

- **fp32 discipline**: `se` comes from the same fp32 `Ke0` the solve used
  (a consistency *improvement* over today's mixed fp64-sensitivities/
  fp32-solve). Sensitivities feed the OC update through the sensitivity
  filter — tolerant by construction, and the design-equivalence tests bound
  the effect.

## Gate & verification

1. **Unit gate (GPU0 style)**: GPU energies vs a CPU fp64 oracle on the
   same fp32 inputs — structured tet10 box + real Netgen mesh, deterministic
   pseudo-random `u`. Bar: rel-∞ error < 1e-4 (expect ~1e-6).
2. **Design equivalence**: the existing adapter-vs-CPU test exercises the
   new path automatically once wired — same bars (compliance within 5%,
   solid/void flips < 10%).
3. **Bench**: full-SIMP at 13.6k tets, expect **0.98 → ~0.3–0.4 s/iter**
   (solve 0.1 + filter/OC/expansion remainder); `[.]` fine-mesh case where
   the sensitivity loop currently costs seconds.
4. **Bar**: sensitivity step ≥10x faster with unchanged gate metrics. If
   fp32 energies push designs past the equivalence bars, escalation path:
   fp64 accumulation in-kernel (the RTX 4060 has shaderFloat64; energies
   need no atomics, so no float64-atomics dependency) before any retreat to
   a CPU cache.

## What it deliberately does NOT touch

- **Filter, OC bisection, displacement expansion stay on the CPU** — O(n)
  milliseconds; porting them buys nothing and costs the SIMP loop its
  headless testability.
- **The CPU-only path is unchanged** (still recomputes `Ke0`). An optional
  fp32 `Ke0` cache for the CPU fallback (~360 MB at 100k tets) is a
  separate decision, not part of GPU4.
- No serialization, RPC, or GUI surface; no change to solver defaults.

## Risks

- **fp32 energy accuracy** — gated (1) and bounded (2), with an in-kernel
  fp64 escape hatch. Low: a 30-term quadratic form is far gentler than the
  CG recurrences that needed the GPU1 lessons.
- **Seam churn** — one defaulted virtual on an internal interface; the CPU
  fallback keeps every existing caller and test working untouched.
- **Memory** — `se` is 4 bytes/element; nothing else new.
