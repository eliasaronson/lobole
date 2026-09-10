# Benchmark round 2 — can it see HOW the part was built?

Prompted by BenchCAD (arXiv:2605.10865, `~/Documents/CAD/BenchCAD-main`):
17,900 execution-verified CadQuery programs over 106 industrial part families,
graded by voxel IoU against a ground-truth STEP. Its headline finding is about
construction, not shape:

> Common failures include missing fine 3D structure, misinterpreting industrial
> design parameters, and **replacing essential operations such as sweeps, lofts,
> and twist-extrudes with simpler sketch-and-extrude patterns.**

Our oracle grades solid count, bbox, volume, area, centre of mass, holes and
inclined planes. Those are exactly the coarse outer-geometry properties BenchCAD
reports every frontier model already gets. **A stack of extruded discs
approximating a revolve passes every check `t2-stepped-shaft` has.** Our
117/117 ceiling is partly a statement about the tasks and partly a statement
about what the checks can see, and we cannot presently tell which.

This plan closes that, plus two gaps the audit for it turned up.

## What the audit found (before any work)

1. **`faces_by_kind` is emitted and nothing consumes it.** `oracle.cpp:299`
   already reports a face census by surface type — `Plane`, `Cylinder`, `Cone`,
   `Sphere`, `Torus`, `BezierSurface`, `BSplineSurface`, `SurfaceOfRevolution`,
   `SurfaceOfExtrusion` (`SurfaceKind`, line 114). `grade.py` has no check kind
   that reads it. The instrument for BenchCAD's finding is already built; it is
   only unwired.
2. **`doc_grade` is wired but no task uses it.** `run.py:190` implements the
   app-reported checks (`variable`, `expr_references`) and `run.py:267` calls it
   under `if task.get("doc_checks")`. **No task has a `doc_checks` key.**
   `benchmarks/README.md` describes this as a live feature of
   `t3-parametric-bushing` — so the task's central claim, that the geometry is
   *driven by a variable*, is currently verified by nothing at all. An agent that
   deletes the bushing and rebuilds it at bore=20 with no variables scores 5/5.
3. Stale counts: `benchmarks/README.md` says "Eight parts, tiers T0–T3, 39
   oracle checks" above a table listing ten tasks. Current truth: 10 tasks, 48
   checks, tiers T0–T5. CLAUDE.md's "48/48" is correct and will move.

Findings 1 and 2 are why this plan is cheap: most of it is wiring, not building.

## B1 — a `surfaces` check kind

> **Partly landed, and two corrections.** The peer-survey round
> (`peer-survey-plan.md`, W4) independently found the same unread census and
> wired a `faces` check kind for it — `{"kind": "faces", "of": "plane",
> "eq": 14}` — to kill a block that was passing three of `t1-l-bracket`'s four
> checks. So **extend that kind rather than adding a second one over the same
> facts**; two check kinds reading one census is how they drift apart.
>
> And the example JSON below will not match anything as written: `oracle.cpp`
> emits **lowercase short names** (`plane`, `cylinder`, `cone`, `sphere`,
> `torus`, `bezier`, `bspline`, `revolution`, `extrusion`), not the
> `GeomAbs_`/`SurfaceKind` spellings. `"Cone"` is not a census key.
>
> What B1 still adds, and should: the OR-tuple (`of` taking a list summed
> together), reporting the whole census in `got`, and failing loudly on a
> surface name that is not a census key. All three are right and none of them
> exists yet.

`grade.py`: one check function over `facts["faces_by_kind"]`.

```json
{"kind": "surfaces", "of": "Cone",  "min": 1, "why": "a real conical transition"}
{"kind": "surfaces", "of": "Plane", "max": 4, "why": "not a faceted stack"}
{"kind": "surfaces", "of": ["BSplineSurface", "SurfaceOfExtrusion"], "min": 1}
```

`of` takes a name or a list of alternatives summed together — BenchCAD's
OR-tuple (`canonical_ops.py`), which exists because a torus is legitimately
reachable by revolve *or* sweep *or* `makeTorus` and the metric must not pick a
winner. `eq` / `min` / `max` follow the existing `_c_solids` shape. `got` reports
the whole census, not just the queried key, so a miss reads as "you built it out
of 46 planes" rather than "no cones".

Ours is strictly stronger than theirs on the same question: BenchCAD detects
essential ops with **a regex over the model's source code** (`OP_PATTERNS`), so
a program that writes `.sweep(` and produces a prism passes. A surface-type
census on the exported B-rep cannot be written past.

- ~25 lines in `grade.py`, plus a case in `benchmarks/tests` if one exists.
- No C++ change. `oracle.cpp` is untouched by this whole plan.

**Acceptance:** an unknown surface name is a *failing* check with a note naming
the census keys — not a silent pass. `grade.py` already fails unknown check
kinds loudly; the same rule one level down.

## B2 — calibrate before anything depends on it

**This is a measurement step, not a formality.** Two of the first eight tasks had
wrong expected numbers when written, and T4's were wrong twice. The surface type
OCC reports after build → export → import is a *prediction* until measured:

- A `Revolve` of a straight line should give `Cone` — but may give
  `SurfaceOfRevolution`.
- A `Loft` between two circles is probably `BSplineSurface`, **not** `Cone`,
  which is precisely the OR-tuple case.
- A `Sweep` along a `Curve3D` helix — `BSplineSurface` or `SurfaceOfExtrusion`,
  unknown which.
- A `Pipe` bend is `Torus` only if the path segment is a true arc.
- `Refine` heals the B-Rep and may convert between these. STEP export may too.

So: build one probe part per operation with `reference.py`'s helpers, export,
run the oracle, and **write the observed census into the plan** before writing a
single task check. Cost: zero dollars, one `nix-shell`, about twenty minutes.

**Acceptance:** a table in this document, filled in from real output, mapping
each Designer feature to the surface kinds it actually leaves in an exported
STEP. If an operation turns out to be indistinguishable from an extrude at the
B-rep level, that operation gets no task in B3 — and that fact is the finding.

| feature | expected | observed | usable as a check? |
|---|---|---|---|
| Revolve (line profile) | `Cone` | _tbd_ | |
| Revolve (arc profile) | `SurfaceOfRevolution` / `Torus` | _tbd_ | |
| Loft (circle → circle) | `BSplineSurface` | _tbd_ | |
| Sweep (along helix) | `BSpline` / `SurfaceOfExtrusion` | _tbd_ | |
| Pipe (along arc) | `Torus` + `Cylinder` | _tbd_ | |
| Shell | unchanged kinds, doubled counts | _tbd_ | |
| stacked Extrudes (the wrong way) | `Plane` + `Cylinder` only | _tbd_ | |

The last row is the one that matters. Every check in B3 is really a claim about
the *gap* between it and the row above it.

## B3 — three tasks that cannot be done with sketch-and-extrude

Each maps to a real BenchCAD family, and each to a Designer feature with **zero
agent-facing measurement today** (the 3D-curves workstream, v42–v45, has never
been in a sweep).

| id | feature under test | BenchCAD family | the check that bites |
|---|---|---|---|
| `t3-coil-spring` | `Curve3D` (helix) + `Sweep`/`Pipe` | `coil_spring` | free-length bbox, wire-diameter volume, and a `Plane` budget of 2 (the cut ends) — a stack of rings blows it |
| `t3-taper-adapter` | `Revolve` or `Loft` | `taper_pin` | `Cone`/`BSpline` present, both bore diameters as `cyl_group`, volume — a stepped approximation has neither the cone nor the volume |
| `t3-bent-tube` | `Pipe` along a `Curve3D` | `u_bolt` / `j_hook` | `Torus` at the bend, one solid, CoM off both axes — straight segments give zero torus faces |

All three are tier 3: single part, no assembly, non-prismatic construction
required. Prompts follow the existing template exactly — millimetres, pinned
axes and origin, no unrequested fillets — so a failed check is a failure to
model, not a failure to guess. **Every one keeps its ordinary dimensional checks
too**; the surface check is added evidence, never the only evidence. A part with
a beautiful cone and the wrong diameter must still fail.

`runners/reference.py` gets a builder for each and must pass 48 + N of N.
CLAUDE.md's "48/48" and `benchmarks/README.md`'s stale counts move here.

**Acceptance:** `reference.py` green on all 13; and a deliberately-wrong build
of each (the stacked-extrude version, ~10 lines each, kept in the reference file
or a sibling) **fails** the surface check while passing bbox and volume. That
second half is the whole experiment — an assertion that the new check
discriminates. Without it we have only added checks that everything passes.

## B4 — turn on the bushing's `doc_checks` (free)

`t3-parametric-bushing` gets the key the machinery has been waiting for:

```json
"doc_checks": [
  {"kind": "variable", "name": "bore", "value": 20},
  {"kind": "variable", "name": "wall", "value": 6},
  {"kind": "expr_references", "variable": "bore"},
  {"kind": "expr_references", "variable": "wall"}
]
```

These stay app-reported and out of the score, exactly as the README already
says. The point is that the run record will finally *contain* the answer to the
question the task was written to ask. Also fold `t3-taper-adapter` into it if
its prompt names a variable.

## B5 — a floor

BenchCAD publishes a blank-image baseline (0.375) against a best Vision-QA of
0.587 — two thirds of the score is available **without seeing the part**. It is
the single most useful row on their board, because it converts "0.587" into
"0.212 of headroom actually earned".

We have a ceiling (`reference.py`, by-hand, 48/48) and **no floor**. Nothing
tells us how much of 117/117 is task difficulty versus free. Add one arm:

- `no-inspect` — every build tool, but `get_state`, `measure`, `check_geometry`
  and `screenshot` unregistered. The agent can act and cannot look.

That is a floor on the *feedback loop* rather than on the input, which is the
right analogue for an agentic benchmark: it asks how much of our score comes
from the inspection surface we keep investing in. A clear prediction either way
is a result. If `no-inspect` also scores 117/117, either the checks are loose or
the tasks are too easy, and we learn it for about $5.

`arms/*.json` + `designer_mcp.py --only-tools` already support this; the arm
narrows the surface for real and refuses loudly if it cannot, so it cannot
silently be a duplicate of `full`.

## B6 — deferred: an edit tier scored on headroom

BenchCAD's CodeEdit is its most decisive result — **best 0.865 against a
no-change baseline of exactly 0.0**, while Vision2Code tops out at 0.289.
Editing existing parametric state is close to solved; reconstructing it from
geometry is not. That is a direct endorsement of the `get_state` →
`modify_feature` loop, and it is the tier we do not have: `t3-parametric-bushing`
grades the final state only, so "edited the right literal" and "deleted it and
rebuilt" are the same score.

Doing it properly needs `run.py` to seed a document before the agent starts
(the harness builds the part, the agent gets one instruction), plus
headroom-normalised scoring — `clip((model − baseline)/(1 − baseline), 0, 1)`
over the check-pass rate rather than a binary. That is a change to the runner's
shape, not to a JSON file, and it should wait until B1–B5 have run once.

Recorded here so it is not re-derived later. Their `CodeEdit/pipeline/modes.py`
minimal-edit system prompt is worth reading when we get to it.

## Order, cost, and what to ask before spending

B1 → B2 → B3 → B4 are one sequence; B5 is independent and can land alongside.

| after | runs (tasks × arms × reps) | ceiling | expected real |
|---|---|---|---|
| today | 90 = 10 × 3 × 3 | $180 | ~$25 (last 8-task sweep: $13.35 / 72 runs; T4/T5 never swept) |
| B3 | 117 = 13 × 3 × 3 | $234 | ~$30 |
| B3 + B5 | 156 = 13 × 4 × 3 | $312 | ~$40 |

Validation along the way is nearly free: `reference.py` costs nothing, and
`run.py --tasks t3-coil-spring --arms full --reps 1` is about $0.20. **Ask
before the full sweep** — CLAUDE.md's rule, and the ceiling has moved.

The sweep this earns is the first one that can answer a question the previous
two could not. Both existing results end the same way: every arm at 1.000, "the
benchmark cannot presently detect a regression", and then "it cannot detect an
improvement either, because no task exercises the surface that improved". B3
gives it three tasks that exercise a workstream it has never seen, and B1 gives
it a class of failure it has never been able to name.

## Rejected

- **Voxel IoU as a score.** BenchCAD grades on a normalized 64³ grid — ~2 mm
  voxels on a 120 mm plate. It cannot see a bolt circle 1 mm out, and it cannot
  say *what* was wrong. Their leaderboard living at 0.27–0.29 is close to
  uninterpretable. Our per-check facts (CoM proving hole spacing, internal vs.
  external decided by the face's outward normal) are sharper and diagnostic.
  Keep binary checks.
- **Source-code inspection of the agent's transcript** to see which RPCs it
  called. `tools_used` is already recorded and is useful *diagnosis*, but making
  it a check grades the method instead of the part, and there is always more
  than one right method. The B-rep is the verdict.
- **Retro-fitting construction checks onto T0–T2.** A stepped shaft built from
  stacked extrudes is a *legitimate* way to build a stepped shaft. Adding a
  surface check there would fail correct work. Construction checks go only where
  a wrong construction is a real defect.
- **Generating task instances from parametric families** (BenchCAD's 106
  families × ~170 instances). It would kill the ceiling problem at no authoring
  cost per instance, and it is the right idea for a *single-shot* benchmark.
  Ours is agentic at ~$0.19 a run: 17,900 instances is $3,400. Revisit only as
  "sample 2 instances per family per sweep", and not before B6.
