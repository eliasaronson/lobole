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

## What a contact check is actually made of

Instrumented per pass on the standing sweep (800 body-pair booleans):

```
boolean  800 calls  132298.5 ms total (165.4 ms each, 799 empty)
distance 797 calls  238631.9 ms total (299.4 ms each)
pairs    799, of which 506 separated, 293 box-nested
```

Two surprises, both of which killed a plan:

* **The DISTANCE pass is the expensive half** — 299 ms against the boolean's
  165, and 64% of the whole sweep. `BRepExtrema_DistShapeShape` on two
  complex solids costs more than intersecting them. So "run the cheap
  distance first and skip the boolean" would have made it SLOWER.
* **799 of 800 booleans find nothing**, and 37% of the pairs that reach one
  are box-NESTED (one box inside the other). Nesting is why a distance test
  can never replace the boolean: one solid entirely inside another has its
  boundaries far apart, so a distance query calls it clear. That is exactly
  the "no part is inside another" case the gate exists for.

## What to do, in order

0. **DONE — ask only for what you read.** A motion study wants "does the
   motion make these collide" and uses only volume and centroid; it was
   paying two thirds of its time for a separation it discarded, 20,403 times.
   `ContactBetween(..., wantDistance)` skips that pass, and reports
   `distanceKnown = false` rather than a 0.0 that would read as contact.
   429.7 s -> 198.4 s.
1. **DONE — outer CPU parallelism across pairs.** The pairs are independent. The
   catch is that OCC caches lazily *on the shape* (`TShape` bounding boxes,
   tolerances), and one body appears in many pairs, so concurrent booleans
   over shared shapes can race even though neither "modifies" anything.
   Pre-warm single-threaded, then treat shapes as read-only, and prove it
   with a determinism test rather than assuming it.
2. **MEASURED AND DROPPED — temporal coherence across poses.** The idea: a
   pair 20 mm apart at pose *k*, given a bound on relative speed, cannot
   touch at *k+1*. Measured, the parts move a median 11.2 mm and a p90 of
   20.4 mm per 15-degree step, while only 8% of pairs are more than 20 mm
   apart — so almost nothing is provably safe for even one step. Worth
   ~5-10% here. It pays off as sampling gets DENSER (the per-step bound
   shrinks), which is when you would want it; revisit then.
3. **MEASURED AND DROPPED — an oriented-box (OBB) prune.** The idea: world
   AABBs are loose for rotated parts, so an OBB would retire pairs before
   the boolean. Measured over 49 poses and 19,878 moving pairs, it retires
   **4.4%**. These parts are compact rather than long-and-angled, so the
   AABB is already nearly as tight as the OBB.
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

## The GPU idea — now aimed at the DISTANCE pass

*(Revised once the per-pass measurement landed. It was parked as a filter in
front of the boolean, which is the smaller half.)*

You cannot run an exact NURBS boolean on a GPU — it *constructs* topology:
Newton-iterated surface intersection, pave blocks, tolerance-driven rebuild,
dynamic allocation throughout. The opposite of what a GPU wants.

A minimum-DISTANCE query is a different animal. It is a search, not a
construction: on triangle meshes it is BVH traversal plus triangle-triangle
distance — uniform inner loop, no allocation, embarrassingly parallel, and
what every collision library already does on a GPU. Since the distance pass
is 64% of a sweep and the boolean only 36%, this is where a GPU would pay.

The app already has that shape of thing — `clash_mode: mesh` — and we
already know it is unusable **as the verdict** at engine scale: the
collision mesh's deflection is 1.17 mm on a part this size, and every
running fit in this engine is a deliberate half-millimetre, so every one of
them reads as contact.

But deflection is a choice, not a law. A much finer GPU-tessellated pass
could be a sound **conservative filter** rather than a verdict:

> if two meshes are separated by more than twice the tessellation
> deflection, the true solids are provably clear.

Only the ambiguous band would need OCC's exact query, and `near_misses` —
the only reason a sweep computes distances at all — has a 1 mm tolerance
that a filter can answer directly for everything clearly outside it.

Two things make this cheaper than it sounds. The **tessellation already
exists**: `collide::Mesh` builds these meshes today with a known deflection,
and the 1.17 mm that makes mesh mode useless as a VERDICT is fine for a
filter, which only ever answers "definitely clear" or "ask OCC". And the
**BVH machinery already exists** — the still renderer builds ray-tracing
acceleration structures, and a BLAS is a BVH over triangles. The honest
caveat is that RT hardware answers ray-triangle, not point-triangle-distance,
so a closest-point query needs its own compute-shader traversal rather than
riding `rayQuery` directly. Real work, but well-trodden rather than novel.

What it cannot do is replace the boolean, for the nesting reason above: 37%
of the pairs that reach a boolean are box-nested, and no distance query of
any kind sees one solid sitting inside another.
