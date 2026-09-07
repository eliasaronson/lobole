# Making the clash sweep scale

Measured on the OM615 (76 parts, 241 occurrences) on 2026-09-07, an Intel
Core Ultra 9 185H (16 cores / 22 threads, hybrid).

## Where the time is

A full generator run was ~63 minutes. The build was 130 s of it. The rest
was clash checking:

```
standing check   821 of 821 pairs, exactly            ~210 s
motion study     20403 pair checks in 3292.4 s        (161 ms each)
                 433209 pairs skipped as rigid to each other
```

161 ms per `Measure::ContactBetween`, and **both loops are serial**:
`RunSweep` (RemoteInspection.cpp) and `motion::Run` contain no `ParallelFor`,
no `std::execution`, no thread — and their callbacks append to shared json,
so they could not simply be wrapped in one. The app also never sets
`BOPAlgo_Options::SetRunParallel`, so each individual boolean is
single-threaded too. Two untapped axes, not one.

The cost is `49 poses x the pairs that can move relative to each other`.
The box prune is already doing its job — 433,209 pairs skipped — so what
survives is genuinely close.

Distances across the 821 pairs that survive box pruning at one pose:

| gap (mm) | pairs | |
|---|---|---|
| 0 .. 0.001 | 193 | 23.5% |
| 0.001 .. 0.5 | 194 | 23.6% |
| 0.5 .. 2 | 92 | 11.2% |
| 2 .. 5 | 102 | 12.4% |
| 5 .. 20 | 174 | 21.2% |
| 20+ | 66 | 8.0% |

## What to do, in order

1. **Outer CPU parallelism across pairs.** The pairs are independent. The
   catch is that OCC caches lazily *on the shape* (`TShape` bounding boxes,
   tolerances), and one body appears in many pairs, so concurrent booleans
   over shared shapes can race even though neither "modifies" anything.
   Pre-warm single-threaded, then treat shapes as read-only, and prove it
   with a determinism test rather than assuming it.
2. **Temporal coherence across poses.** A pair 20 mm apart at pose *k*, with
   a bound on the two occurrences' relative speed, provably cannot touch at
   pose *k+1*. Exact, not approximate; it removes work instead of spreading
   it, so it composes with (1). The 49x multiplier is the real structure of
   the cost.
3. **A conservative distance pre-filter.** Retires the 29% of pairs beyond
   5 mm; worth roughly 1.4x on the table above.
4. **`SetRunParallel(true)` for the single-pair path only.** With 20k small
   booleans the outer axis dominates and the two compete for the same cores.

## Not SIMD

A B-Rep boolean is pointer-chasing over a topology DAG: surface/surface
intersection by Newton iteration, pave-block bookkeeping, tolerance-driven
reconstruction, thousands of small allocations in `NCollection` containers.
The control flow is data-dependent and there is no wide uniform inner loop.
OCC's `gp_*` arithmetic already gets whatever the compiler auto-vectorises,
and that is a rounding error of 161 ms. Hand-SIMD would mean writing a new
intersector, not speeding up this one.

## The GPU idea, parked

You cannot run an exact NURBS boolean on a GPU — the algorithm and its
dynamic data structures are the opposite of what a GPU wants. What maps to a
GPU is a **different question**: mesh or voxel/SDF overlap.

The app already has that shape of thing — `clash_mode: mesh` — and we
already know it is unusable **as the verdict** at engine scale: the
collision mesh's deflection is 1.17 mm on a part this size, and every
running fit in this engine is a deliberate half-millimetre, so every one of
them reads as contact.

But deflection is a choice, not a law. A much finer GPU-tessellated pass
could be a sound **conservative filter** rather than a verdict:

> if two meshes are separated by more than twice the tessellation
> deflection, the true solids are provably clear.

Only the pairs that survive that test would need the 161 ms exact boolean.
Per the distance table this retires the 29% beyond 5 mm and some of the
2-5 mm band — call it 1.4x, which is why it ranks *below* items 1-3 despite
being the most interesting of them. It is also the largest piece of work:
it needs a GPU tessellation path, a conservative deflection bound carried
with the mesh, and a proof that the bound is respected. Worth doing when
assemblies get big enough that 1.4x on the remainder matters; not before.
