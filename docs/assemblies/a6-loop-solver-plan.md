# A6 — closing mate loops (the numeric assembly solve)

A3 placed mated occurrences by **direct frame alignment**: anchor the `fixed`
occurrences, walk the mate graph, and let each mate fully determine one side
from the other. Mate *trees* solve exactly, in closed form, with no iteration.
Genuine loops are detected by a consistency check and stamped
`"over-constrained (mate loop conflict)"` — reported, never solved.

A6 closes them. It does not replace the A3 pass; it wraps it.

## Why now

This stopped being theoretical on 2026-09-01. `JoinAcrossInterface` (the
fastener joint mates) has to consult `assembly::MatesConnect` and **drop mates
it knows to be true**:

> bolts two and three add nothing; a mate between an already-connected pair is
> a LOOP, which `AssemblySolve` reports as over-constrained rather than solving

Three M8 bolts through a flange is the ordinary case, not a linkage demo. We
model one bolt's joint and stay quiet about the other two, because the solver
cannot say "redundant and consistent". That is the gap, and it is already
shipping.

## What the model already gives us for free

The decisive fact: **`Mate` is already in reduced coordinates.** Every mate
stores its free DOF as serialized fields — `rotation`, `offset`, `dx`, `dy`,
`ballRotation` (core/Occurrence.h) — and `MateType` is documented as "the
*normal form* a constraint stack would converge to".

So the joint coordinate vector **q** of the whole assembly is already a
persistent part of the document, and `assembly::Solve` is already the forward
map q -> world placements. A6 does not need a new state representation, a
format bump, or a migration. It needs a *residual*, a *Jacobian*, and a
*factorization*.

This is also why the commercial-CAD question "should we add primitive
constraints (coincident + concentric + distance)?" is answered NO. Onshape and
Fusion are joint-only by design; stacked primitive mates are the legacy
SolidWorks model that both deliberately left behind. Adding them would break
reduced coordinates for no capability we lack.

## Prior art, and what we took from each

| Project | Coordinates | What we took |
|---|---|---|
| **MuJoCo** (Apache-2.0) | reduced | Architecture confirmation: a kinematic tree in generalized coordinates, with closed loops added back as `mjEQ_CONNECT`/`mjEQ_WELD` equality constraints. That is exactly spanning-tree + cut-set. Also: soft constraints (`solref`/`solimp`) — violations are distributed, not rejected. |
| **Pinocchio** (BSD-2) | reduced | `log6`/`exp6` on SE(3) as the loop residual; `computeJointJacobians` as the shape of the analytic Jacobian. Read, not linked. |
| **OndselSolver** (LGPL-2.1, FreeCAD's) | maximal | The redundancy algorithm, below. 41k lines across 639 files — far too heavy to vendor for kinematics. |
| **SolveSpace** (GPLv3) | maximal | `CalculateRank`/`TestRank`: DOF = columns - rank(J). 1.7k lines for a complete 3D solver, which is the honest size estimate for this work. |

**The redundancy algorithm, converged on independently three times.**
`GESpMatFullPvPosIC` is Gaussian elimination with **full pivoting**; when the
best available pivot falls below tolerance, the rows that could not find one
*are* the redundant equations. Ondsel throws them out of the factorization,
deactivates those constraint rows, and restarts Newton from the previous
configuration. SolveSpace does the same thing with rank-revealing elimination.
We do it with a rank-revealing QR.

The part worth stating explicitly: **full pivoting tells you WHICH rows are
redundant, not merely how many.** That is what lets the UI name the two bolts
rather than say "3 redundant equations".

## Design

### Spanning forest, cut set

Nodes are occurrences, edges are usable mates. BFS from each component's
anchor (`fixed`, else the lowest-id occurrence, exactly as today). Tree edges
place, precisely as A3 does now. The remaining `m - (n - 1)` edges per
component are **cut mates**, and they are the only source of residuals.

A component with no cut mates never enters the numeric path at all.

### Unknowns

**q** = the concatenated free DOF of *every* mate in a component that has at
least one cut mate — tree mates and cut mates alike. A cut revolute
contributes 1 unknown and 6 residual rows: net 5 constraints, which is exactly
the DOF a revolute removes.

The anchor is pinned, not free. That gauge-fixes each component; without it
every floating component is rank-deficient by exactly 6.

### Residual

For a cut mate with connector frames `Fa`, `Fb` on occurrences A, B:

```
Wa = T(A) * Fa                      // a's connector, world
Wb = T(B) * Fb                      // b's connector, world
E  = inverse(Wa * MateAdjust(m)) * Wb
r  = log6(E)                        // 6-vector: 3 translation, 3 rotation
```

`T(A)` and `T(B)` come from running the existing A3 propagation over the tree
at the current q. The A3 pass IS the residual evaluation — it is not
rewritten, wrapped, or duplicated.

**Weighting.** Translation rows are mm, rotation rows are radians; mixing them
un-scaled makes the solver's notion of "close" depend on the units. Rotation
rows are scaled by a characteristic length L (half the assembly bbox
diagonal), so every row is in mm and the tolerance means one thing.

### Jacobian

Numeric forward differences to start: column i is one A3 propagation at
`q + h*e_i`. Systems are tiny — a four-bar is **one** unknown — so this is
cheap and it cannot disagree with the forward map. Analytic Jacobians only if
profiling asks for them.

### Solve

Levenberg-Marquardt, seeded from the stored q. LM rather than Gauss-Newton
specifically because the damping term is also what keeps drag (a later stage)
stable through singular configurations, e.g. a linkage at full extension.

### DOF and redundancy: why NOT Grübler

The obvious cheap answer — Grübler/Kutzbach mobility from the graph — is
wrong, and wrong in the most common case. A planar four-bar in 3D:

```
Grübler:  6*(4-1) - 5*4  =  18 - 20  =  -2     (says "over-constrained")
Truth:    mobility 1                            (it is a crank rocker)
```

Grübler cannot see that the four parallel axes make 3 of the 6 loop equations
identically zero. Rank can:

```
unknowns = 4 angles,  residual rows = 6,  rank(J) = 3
DOF = 4 - 3 = 1                                 (correct)
```

So DOF reporting is not a cheap graph pass that lands before the solver — it
*is* the solver's rank analysis. There is no useful Stage 0 that skips the
Jacobian, and the plan below reflects that.

### Classification

After LM converges, with `rank = rank(J)` from a column-pivoted QR:

| residual | rank | verdict |
|---|---|---|
| small | full row rank | **solved**, DOF = cols - rank |
| small | deficient | **redundant, consistent** — name the mates owning the rows that found no pivot (the three-bolt flange) |
| large | any | **cannot close** — report the residual in mm, name the cut mates |

The third row matters for honesty: today a 3.2 mm loop conflict and a 0.02 mm
one are both "over-constrained (mate loop conflict)". Real parts have
tolerance. Saying the number is the difference between "your model is wrong"
and "your model is fine".

## Determinism

A3's guarantee is that a file re-solves to identical placements. A6 must not
lose it.

- **No cut mates -> no solver.** The closed-form path runs untouched, so every
  existing assembly and every existing test keeps its exact behaviour.
- q is seeded from the *stored* mate DOF, which are serialized, so a reopened
  file starts where it converged.
- Fixed iteration cap, fixed tolerance, deterministic pivoting.

## The one real behaviour change

Today mate DOF are pure **inputs**. For a mate inside a loop they become
**outputs** — that is what a mechanism is: turn the crank, the rocker follows.
So `Solve` now writes `rotation`/`offset`/`dx`/`dy`/`ballRotation` back for
loop members, and those fields are serialized, so a solve can dirty the
document.

Guarded: write back only when the change exceeds tolerance, and only for
occurrences in a component that actually has a cut set. Non-loop mates keep
today's read-only semantics exactly.

## Staging

1. **Extract the forward map.** Split `Solve` into forest construction + a
   pure `Evaluate(q) -> world transforms`. No behaviour change; existing tests
   are the gate.
2. **Residual + numeric Jacobian + LM.** Loops close.
3. **Rank analysis.** DOF count and the three-way classification above.
4. **Report it.** `SolveReport` grows DOF, residual, and named redundant
   mates; `solve_assembly` RPC returns them.
5. **Un-suppress the fastener mates** that `MatesConnect` currently drops.

Out of scope for A6, in rough order of what should come next: joint limits
(a box bound on a scalar of q), ~~gear/screw/rack relations (a linear equality
between two entries of q, i.e. variable elimination)~~ — **built 2026-09-03**
as one residual row per relation rather than elimination, plus propagation
in a mate tree ([relations-plan.md](relations-plan.md), v83); drag-solve (an
IK residual on the existing A2.5 gizmo), motion study.

Explicitly not planned: primitive constraint stacking, dynamics, contact-driven
motion.

## Files

- `src/core/AssemblyLoopSolve.{h,cpp}` — new; forest, residual, LM, rank.
  Kept out of `AssemblySolve.cpp` (703 lines already).
- `src/core/AssemblySolve.cpp` — `Solve` grows the cut-set branch.
- Eigen is already `PUBLIC` on `designer_core` when found; A6 adds its own
  `HAVE_EIGEN` define rather than borrowing `HAVE_FEM_SOLVER`, and degrades to
  today's behaviour without it.

---

## What shipped (2026-09-01), and the two things it taught

Stages 1-4 are in: `src/core/AssemblyLoopSolve.{h,cpp}`, the cut-set branch in
`assembly::Solve`, `SolveReport`'s loop fields, and `solve_assembly`'s
response. Gated by `tests/test_assembly_loops.cpp` (11 cases) and
`tests/assembly_loops_smoke.py` (the wire). Stage 5 — un-suppressing the
fastener mates in `JoinAcrossInterface` — is deliberately NOT done: it is a
judgement call about mate-list noise (three bolts would become three mates
where one used to appear), not a bug fix, and it wants a human.

**Float32 forward kinematics cannot feed a finite-difference Jacobian.**
The first working version closed the four-bar to 0.064 mm and stopped, looking
for all the world like a tolerance to loosen. It was not. Accumulating a chain
of `glm::mat4` leaves ~1e-3 mm of noise at 100 mm scale; the difference
quotient `(r(q+h) - r(q))/h` at `h = 1e-6` then amplifies that noise by 1e6 and
the Jacobian is garbage. The fix is precision at the source, not a bigger `h`:
the whole evaluation runs in `double` and narrows once, at the end, because the
document stores floats. `MateAdjustT<T>` is templated for exactly this — the
placement pass keeps its float arithmetic (and its goldens) while the solver
gets double, from ONE definition.

**"Some dependent rows" is not "redundant".** The first redundancy rule marked
a mate whenever any of its six rows failed to find a pivot. That is almost
every cut mate: a planar four-bar's closing joint has three structurally-zero
rows, because a loop of parallel axes can say nothing about z or tilt. It would
have reported the one joint that makes the linkage a linkage as doing nothing.
A mate is redundant only when **all six** rows are dependent — remove it and
the assembly is unchanged. That is the three-bolt flange, and nothing else.

Both were found by a test asserting something specific (the parallelogram
branch, then `redundant.empty()`) rather than by asserting the solve
"succeeded".

## Correction to the survey above

The four-bar test also settled a question the prior-art table left open. LM
converged to the **crossed** assembly mode rather than the parallelogram one,
from a seed that was on neither. Both are correct; which one you land on is a
property of the seed, not the mechanism. So the branch is a *user-visible*
thing a mechanism has, and any future drag stage must track the branch it was
given rather than re-solving from scratch — `test_assembly_loops.cpp`'s "A
four-bar seeded on a branch stays on it" is the guard on that.

## Partitioning, and what it costs (measured 2026-09-01)

`SolveLoops` splits the mate graph into connected components and solves each
looped one on its own. This is not a micro-optimisation. Without it, one small
loop anywhere in a document puts a column in the Jacobian for EVERY mate in
that document, and each column costs a full tree walk — quadratic in the size
of the model for a loop four parts wide. Independent loops also stay
independent: ten of them are ten small solves rather than one system ten times
as wide.

Measured over the wire (`solve_assembly`, headless, sliders on origin anchors):

| assembly | solve |
|---|---|
| 200 occurrences, 199 mates, no loop | 2.5 ms (closed-form path, no solver) |
| 200-mate tree + a **disjoint** 2-mate loop | 2.6 ms, `dof=1` |
| 200 occurrences chained, 1 loop **in that chain** | 8.2 ms, `dof=199` |
| 50 occurrences chained, 1 loop in the chain | 2.5 ms, `dof=49` |

The third row is honest rather than disappointing: a chain of 200 sliders
closed by one mate genuinely is a 200-unknown problem with 199 degrees of
freedom, and partitioning cannot help because it is one component. The second
row is the shape partitioning exists for, and it costs the same as no loop at
all.

A consequence worth stating: because the loop pass returns placements only for
the components it solved, `assembly::Solve` MERGES them into the walk's result
instead of replacing it. Replacing would strand every occurrence in every other
component at its stored transform — a silent wrong answer, guarded by "A loop
leaves a mate tree elsewhere in the document alone".

## Stage 5 done: the fastener mates are no longer suppressed (2026-09-02)

`JoinAcrossInterface` consulted `assembly::MatesConnect` and skipped any
interface an earlier bolt had already joined, because that mate would have
closed a loop A3 refused. That guard is gone: every bolt now mates every
interface it spans, anchored at ITS hole, so deleting one bolt leaves the joint
held by the others — which is what a bolt pattern means.

`MatesConnect` itself is deleted. It existed only for that guard, and with the
guard gone it had no caller and no test.

On `scratch/build_alternator.py` (three M8 through-bolts, SRE housing + stator
+ DE housing):

| | before | after |
|---|---|---|
| mates | 2 | **6** (3 bolts x 2 interfaces) |
| errored | 0 | 0 |
| loop mates | — | 4, all `redundant_mates` |
| residual | — | 0.0 mm |
| solve | — | 3.5 ms |

## Performance (measured 2026-09-02)

The shape that matters: **a bolt pattern has zero unknowns.** Every mate in it
is Fastened, so `PackDofs` yields no columns, and `SolveComponent` skips LM and
the QR entirely — it walks the tree once, evaluates one residual per cut mate,
and takes the `cols == 0` branch straight to "every row is dependent". Linear,
and barely distinguishable from a loop-free solve.

| assembly | solve |
|---|---|
| 3 bolts (2 loops) | 2.13 ms |
| 12 bolts (11 loops) | 2.23 ms |
| 48 bolts (47 loops) | 2.32 ms |
| **192 bolts (191 loops)** | **2.88 ms** |
| 4-link closed chain (4 unknowns) | 2.10 ms, residual 7e-9 |
| 16-link closed chain (16 unknowns) | 2.24 ms, residual 1e-7 |
| 64-link closed chain (64 unknowns) | 3.66 ms, residual 5e-6 |
| 200-slider chain, loop in it (199 unknowns) | 8.2 ms |
| 200-mate tree + a disjoint 2-mate loop | 2.6 ms |
| 200 occurrences, no loop at all | 2.5 ms |

~2.1 ms is the floor — the RPC round trip plus the placement walk, not the
solver. So un-suppressing the fastener mates costs nothing measurable, and the
only case that climbs is a genuinely large mechanism, where the cost is the
numeric Jacobian: one tree walk per unknown per LM iteration.

If that ever matters, the fix is analytic Jacobians (a revolute's column is a
cross product, not a re-walk), not a cheaper solver. Nothing yet asks for it.
