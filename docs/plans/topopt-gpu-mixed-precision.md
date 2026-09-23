# Topology on the GPU for slender parts: fp64 refinement around the fp32 solve

## The problem

The GPU topology solver is fp32 matrix-free CG. On a slender part under
bending it cannot reach its 1e-5 target, and on the worst system it makes no
progress at all. `TopologyOptimize` then drops the GPU solver and finishes on
the CPU ([../topopt/README.md](../topopt/README.md), "The fp32 limit on
slender parts"), so the result is correct but runs at CPU speed.

The cause is conditioning beyond fp32. Dense fp64 eigenvalues of the reduced
K (`femgpu_tests [fp32floor]`, "condition numbers"):

| case | diagonal spread | κ(K) | κ after diagonal scaling |
|---|---|---|---|
| cube 1:1, uniform | 12 | 3.3e+03 | 2.2e+03 |
| beam 10:1, uniform | 12 | 3.7e+06 | 2.4e+06 |
| beam 10:1, frozen skin + 0.027 | 220 | 8.2e+07 | 1.6e+07 |
| beam 20:1, uniform | 12 | 1.4e+07 | 9.4e+06 |
| beam 20:1, frozen skin + 0.027 | 170 | 2.7e+08 | 5.3e+07 |

Slenderness costs ~4× in κ per doubling and SIMP's frozen skin another ~20×;
the 2.7e+08 case is the one whose first solve returns zeros. Jacobi and
multigrid fail identically. Cheaper levers do not fix it: a finer mesh makes
κ worse (h⁻²); symmetric diagonal scaling is worth 2–5×; ramping the penalty
from p = 1 lowers κ but converges to a softer, greyer design; fp64 scalars
inside the CG keep `x`, `r`, `p` in fp32 and recover only 1–2 of the ~8
digits needed.

## The fix

Iterative refinement moves precision to where it is lost without moving the
work:

```
u = 0
repeat:
    r = F - K·u            (fp64)
    d = fp32_solve(K, r)   (the existing GPU CG, unchanged)
    u = u + d              (fp64)
```

Each outer step multiplies the error by the inner solve's relative residual
ρ. The element SpMV over the whole mesh stays fp32; only the residual and
the accumulation of u (vectors of nFree doubles) are fp64.

Three details decide whether it works:

- **Rescale the right-hand side.** `r` shrinks geometrically; hand the inner
  solver `r / ‖r‖` and scale the correction back, or fp32 wastes its range.
- **Zero is a fixed point.** From `u = 0` on the worst system the inner
  solver returns its initial guess, so `d = 0` and the loop never moves. The
  inner initial guess must be perturbed (one RNG pass broke the fixed point in
  every probe run), or the first SIMP iteration seeded by one CPU solve
  (`fem::Solve`, fp64) with the GPU warm-started from it.
- **Budget for spread.** The probe (`femgpu probe: GPU5.0 does fp64
  refinement contract?`) reached 1e-3 in 4–6 outer steps from a warm start,
  ρ ≈ 0.01–0.1 once productive, but the inner solve is nondeterministic
  (128, 272 or 368 iterations from identical inputs) and one outer step costs
  200–650 inner iterations. SIMP needs perhaps 1e-3, not 1e-5, since
  compliance error goes as tol².

The fp64 residual needs K: either retain the packed system (`dof` + `ke`) on
the host (~360 MB at 100k tets, no new kernel), or an fp64 variant of
`fem_spmv.comp` behind a `shaderFloat64` feature check. Start with the host
and measure. The warm-solve cap of 128 iterations was tuned for the current
behaviour and must be re-measured, since refinement changes what a warm start
is.

Not touched: the inner CG, its preconditioners, the SIMP loop (filter, OC,
constant penalty), the CPU path and the fallback, which stays in place for
whatever this does not cover. No serialization, RPC or GUI surface.

## Files

- `src/femgpu/TopoGpuSolver.{h,cpp}`: the outer loop, rescaling, stopping
  rule (`‖r‖/‖F‖ < tol`, or a step that fails to reduce `‖r‖`, which is where
  the existing no-progress check moves), the retained system or fp64 SpMV,
  the perturbed or CPU-seeded first solve.
- `src/shaders/fem_spmv.comp` (only if the device fp64 SpMV wins).
- `tests/test_femgpu.cpp`: the gates below.

## Verification

- `femgpu: a slender cantilever solves on the GPU (GATE)` passes with
  `solverFallback` empty — the GPU finishes the run itself.
- A full SIMP run on a 200 × 10 × 20 mm cantilever completes on the GPU and
  its design matches the CPU within the bars `femgpu: TopologyOptimize lands
  on the CPU design via the GPU solver` uses (solid/void flips, volume,
  compliance).
- `femgpu bench: full SIMP loop GPU vs CPU on the bulky cube` is unchanged
  within noise, and the slender case beats the CPU. If refinement costs more
  than it saves, it ships off by default with the fallback unchanged.
