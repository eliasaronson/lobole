# GPU5 — fp64 iterative refinement around the fp32 solve (sub-plan)

> Status: **DESIGNED, NOT BUILT.** Written 2026-08-24 after the dogfooding
> round found the GPU path returning a zero displacement field on a beam
> ([plan.md](plan.md), `docs/dogfooding.md` 2026-08-23 (c)). That failure is
> *fixed* — `TopologyOptimize` drops the external solver and finishes on the
> CPU — so this plan buys **speed on slender parts, not correctness**. Nothing
> here is urgent, and phase 0 exists to kill the plan cheaply if its one
> assumption does not hold.

## Why (measured, not projected)

The fp32 matrix-free CG cannot reach its 1e-5 target on a slender part, and on
one particular system it makes no progress at all. Dense fp64 eigenvalues of
the reduced K (`femgpu_tests [fp32floor]`):

| case | diagonal spread | κ(K) | κ after diagonal scaling |
|---|---|---|---|
| cube 1:1, uniform | 12 | 3.3e+03 | 2.2e+03 |
| beam 10:1, uniform | 12 | 3.7e+06 | 2.4e+06 |
| beam 10:1, frozen skin + 0.027 | 220 | 8.2e+07 | 1.6e+07 |
| beam 20:1, uniform | 12 | 1.4e+07 | 9.4e+06 |
| beam 20:1, frozen skin + 0.027 | 170 | **2.7e+08** | 5.3e+07 |

Two mechanisms multiply: slenderness costs ~4x in κ per doubling, and the SIMP
frozen support/load skin (density 1 against a bulk at the volume fraction
cubed) another ~20x. Achieved residual floors track κ monotonically, and the
case at 2.7e+08 is past what seven decimal digits can work with — the CG
returns its zero initial guess. **fp32 is the binding constraint, not the
preconditioner**: Jacobi and multigrid fail identically on it.

Everything cheaper than this plan has been tried and measured:

- **A different mesh does not help and refining hurts.** κ grows like h⁻², and
  the floors show it: at 20:1, 240 → 960 tets moved the floor 2.6e-03 →
  5.2e-02. Element *shape* is not the issue — every probe mesh is perfect
  cubes. Linear tets would condition better and lock in bending, which is the
  load case that hurts.
- **Symmetric diagonal scaling** (unit diagonals, so the fp32 recurrence sees
  well-scaled numbers): worth 2-5x. Not a fix.
- **Penalty continuation** (`penaltyRampIters`): takes κ to 3.2e+07 and
  un-zeroes the first solve, but does not rescue the run, and converged to a
  design **54% softer and greyer** on the chunky mesh. Tried and rejected —
  see plan.md.
- **fp64 scalars inside the CG** (fp64 reductions and atomics): four shaders
  bind the scalars buffer, plus a `shaderFloat64` +
  `shaderBufferFloat64AtomicAdd` feature gate and a second compiled variant
  for devices without them. It fixes accumulation in the dot products while
  `x`, `r` and `p` stay fp32, so `r -= alpha*Ap` still cancels: ~1-2 digits
  where about 8 are needed. Worse ratio of blast radius to payoff than this
  plan, and it is not a prerequisite for it.

## The idea

Refinement moves the precision where it is needed without moving the work.
For `K u = F`:

```
u = 0
repeat:
    r = F - K·u                (fp64, exact-ish)
    d = fp32_solve(K, r)       (the existing GPU CG, unchanged)
    u = u + d                  (fp64)
```

Each outer step multiplies the error by the inner solver's relative residual
ρ. The inner solve stays exactly as it is — same kernels, same preconditioner,
same warm-start machinery — and every part that needs digits (the residual and
the accumulation of `u`) happens in fp64 on data that is 240 KB, not 49 MB.
The expensive half, the element SpMV over the whole mesh, never leaves fp32.

Measured ρ on the 20:1 beam today: **1.8e-01** once the frozen-skin contrast
is out of the way, **1.00** with it. That is the whole risk of this plan, and
phase 0 is about nothing else.

### The two details that decide whether it works

**Rescaling.** `r` shrinks geometrically — by the third outer step it is ~1e-3
of `F`. Handing that to an fp32 solver wastes its exponent range and then its
mantissa. The RHS must be normalised (`r / ‖r‖`) before the inner solve and
the correction scaled back afterwards. This is the step implementations get
wrong; it is two lines and it is load-bearing.

**Contraction.** Refinement converges only if ρ < 1, and the pathological case
is exactly ρ = 1. Note *which* solve that is: the **first** one, cold-started
from zero with the worst contrast the run will ever have (frozen skin at 1.0
against a bulk at 0.027). Later solves are warm-started from a design that has
already moved. So the proposed seed is to solve iteration 1 on the CPU — the
built-in Eigen CG-IC, already there, already the oracle — and hand its result
to the GPU as the warm start. One CPU solve per optimization run, against
however many iterations follow. Whether that is enough is **not measured**;
phase 2 measures it, and the fallback catches it if not.

## Phases

**GPU5.0 — measure ρ. No production code.** Extend the `[fp32floor]` probes to
drive a refinement loop from the host around the existing `TopoCgSolver`, on
the 20:1 beam with the real SIMP scale field (frozen skin + 0.027) and a
bending RHS, reporting the outer residual and ρ per step.

> **Gate: the outer residual reaches 1e-3 within 4 steps, from a warm start
> that is not zero.** If it does not, the inner solve is not a contraction on
> the systems that matter and **this plan is wrong** — stop here, having spent
> a day and no production code. That is the point of doing it first.

**GPU5.1 — refinement inside `TopoGpuSolver::Solve`.** The loop, the
rescaling, the stopping rule (`‖r‖/‖F‖ < tol`, or a step that fails to reduce
`‖r‖` — which is also where the existing no-progress bail moves to). The fp64
residual needs `K` on the host or an fp64 device SpMV; decide by measurement,
starting with the host:

- *Host*: retain the packed system in `Impl` (`dof` + `ke`). 49 MB at 13.6k
  tets, ~360 MB at 100k — the same figure GPU4 flagged for a CPU `Ke0` cache.
  ~30 lines, no new kernel, no feature gate.
- *Device*: an fp64 variant of `fem_spmv.comp`. Needs `shaderFloat64` (the
  RTX 4060 has it) and a fallback path for devices without it. One dispatch
  per outer step, so the 1/64 fp64 rate is irrelevant. No host memory.

> Gate: `femgpu: a slender cantilever solves on the GPU (GATE)` passes with
> `solverFallback.empty()` — i.e. the GPU finishes the run itself.

**GPU5.2 — the cold-solve seed.** When the first GPU solve fails to contract,
solve that iteration with `fem::Solve` (fp64, CPU) and warm-start the GPU from
it, rather than failing over for the whole run. Triggered by the measurement,
not by a guess about the geometry.

> Gate: a full SIMP run on the round's 200x10x20 cantilever completes on the
> GPU, and its design matches the CPU within the bars the existing
> adapter-vs-CPU test already uses (solid/void flips < 10%, volume within
> 0.01, compliance within the documented band).

**GPU5.3 — bench, then choose the default.** s/iter on a slender part, GPU
with refinement vs CPU, alongside the existing 13.6k-tet chunky bench to prove
no regression where the GPU already works. Then decide: refinement always on,
or only when the first solve floors above a threshold.

> Gate: the chunky bench is unchanged within noise, and the slender case beats
> the CPU. If refinement costs more than it saves, ship it **off by default**
> with the fallback unchanged — the honest outcome, not a failure.

## What it deliberately does NOT touch

- **The inner solver.** No new preconditioner, no fp64 scalars, no kernel
  changes to the CG itself. If refinement works, the fp32 CG is fine as it is.
- **The CPU path and the fallback stay exactly as they are.** They are the
  correctness floor; this plan is allowed to fail into them.
- **The SIMP loop** — filter, OC bisection, sensitivities — is untouched. In
  particular the penalty schedule stays at constant p (see plan.md for why).
- **No serialization, RPC or GUI surface.** Nothing about this is a user
  decision; a user should never learn the words "iterative refinement".

## Risks

- **ρ ≥ 1 even from a good seed** — the plan dies. Phase 0 costs a day and
  finds this before anything is built. Highest risk, cheapest to test.
- **Cost.** k outer steps multiply the solve cost, and GPU4 measured the solve
  at only ~0.1 s of a 0.55 s iteration — but that was on a *chunky* mesh where
  the solve converges; on a beam it is the whole iteration. SIMP needs perhaps
  1e-3, not 1e-5 (compliance error goes as tol² in the energy norm), so 2-3
  steps rather than 6. Phase 3 decides on numbers.
- **Warm-start interaction.** GPU4's "warm solves capped at 128 iterations"
  rule was worth two thirds of the runtime and was tuned against the current
  behaviour; refinement changes what a warm start *is*. Re-measure it in phase
  1 rather than assuming it carries over. A compliance-plateau stop was tried
  and rejected once (GPU4) — do not retry it.
- **Memory**, if the host-residual option wins: 360 MB at 100k tets. The
  device fp64 SpMV avoids it entirely and is the escape hatch.

## Success

A beam-shaped part optimizes on the GPU, at GPU speed, with a design matching
the CPU within the existing bars — and the fallback added in 90ec721 stops
firing on the round's cantilever while staying in place for whatever it has
not seen yet.
