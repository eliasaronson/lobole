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
   already reports a face census by surface type — `plane`, `cylinder`, `cone`,
   `sphere`, `torus`, `bezier`, `bspline`, `revolution`, `extrusion`, `other`
   (`SurfaceKind`, line 114). `grade.py` has no check kind
   that reads it. The instrument for BenchCAD's finding is already built; it is
   only unwired.
2. **The app-reported checks are live, and one of the two expressions is
   unchecked.** `run.py:190` implements them (`variable`, `expr_references`),
   `run.py:267` calls them, and `t3-parametric-bushing.json` carries three:
   `bore` == 20, `wall` == 6, and geometry referencing `bore` by name. What is
   missing is the matching reference check on `wall`. The prompt asks for an OD
   driven by `bore + 2 * wall`, so an agent that drives the bore from a variable
   and retypes the outside diameter as the literal 32 passes every check the
   task has — including the one the task exists to ask.
   (A correction: an earlier draft of this plan claimed no task used
   `doc_checks` at all. That was read off the runner's call sites without
   grepping the task files. B4 is a one-line addition, not a wiring job.)
3. Stale counts: `benchmarks/README.md` says "Eight parts, tiers T0–T3, 39
   oracle checks" above a table listing ten tasks. Current truth: 10 tasks, 48
   checks, tiers T0–T5. CLAUDE.md's "48/48" is correct and will move.

Findings 1 and 2 are why this plan is cheap: most of it is wiring, not building.

## B1 — a construction check over the face census

> **Landed, reconciled with the peer-survey round.** W4
> (`peer-survey-plan.md`) independently found the same unread census and wired
> a `faces` check kind for it. This round's parallel `surfaces` kind was folded
> INTO that one on merge rather than shipped beside it — two check kinds over
> one census is how they drift apart, and the note that said so was right.
>
> The corrections in that note were also right. `oracle.cpp` emits lowercase
> short names (`plane`, `cone`, `bspline`, …), not the `GeomAbs_` spellings the
> draft below used, and the JSON examples here are left as originally written
> only so the correction is legible; the shipped tasks use the real names.
>
> What this round added to `faces`, and what B3 depends on: `of` taking a LIST
> summed together (the OR-tuple), the whole census reported in `got`, and a
> refusal for a surface name that is not a census key.

`grade.py`: one check function over `facts["faces_by_kind"]`.

```json
{"kind": "faces", "of": "cone",  "min": 1, "why": "a real conical transition"}
{"kind": "faces", "of": "plane", "max": 4, "why": "not a faceted stack"}
{"kind": "faces", "of": ["bspline", "extrusion"], "min": 1}
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

- A `Revolve` of a straight line should give `cone` — but may give
  `revolution`.
- A `Loft` between two circles is probably `bspline`, **not** `cone`, which is
  precisely the OR-tuple case.
- A `Sweep` along a `Curve3D` helix — `bspline` or `extrusion`, unknown which.
- A `Pipe` bend is `torus` only if the path segment is a true arc.
- `Refine` heals the B-Rep and may convert between these. STEP export may too.

So: build one probe part per operation with `reference.py`'s helpers, export,
run the oracle, and **write the observed census into the plan** before writing a
single task check. Cost: zero dollars, one `nix-shell`, about twenty minutes.

**Result — measured 2026-09-09** (`benchmarks/runners/surface_probe.py`, ten
probes, build → `export_step` → oracle, so it is the whole round trip and not
just what the kernel held in memory):

| probe | built | census |
|---|---|---|
| `extrude_cylinder` | circle extruded 30 | `cylinder:1 plane:2` |
| **`stack_taper`** | **6 stacked discs r20→r12.5** | **`cylinder:6 plane:7`** |
| `revolve_frustum` | trapezoid revolved 360° about Z | `cone:1 plane:2` |
| `revolve_torus` | circle at R30 r8 revolved 360° | `torus:1` |
| `loft_circles` | loft r20 → r12 over 30 | `cone:1 plane:2` |
| `sweep_helix` | r2 wire swept along helix R20 p10 ×3 | `bspline:1 plane:2` |
| `pipe_helix` | OD4 wall1 pipe along the same helix | `bspline:2 plane:2` |
| `pipe_spline` | OD12 wall2 tube along a 4-point fit spline | `bspline:2 plane:2` |
| `sweep_sketch_arc` | r6 profile swept along a 90° sketch arc R40 | `torus:1 plane:2` |
| `shell_box` | 60×40×30 box shelled 3 through its top | `plane:11` |

Four things this changed:

1. **The discrimination is real and wide.** A frustum built the right way is
   three faces (`cone:1 plane:2`); the same shape faked with stacked discs is
   thirteen (`cylinder:6 plane:7`), with **zero** cones. Same bbox, same
   envelope, same centre of mass — the census separates them outright.
2. **A loft between two circles is a true `cone`, not a `bspline`.** The plan
   predicted otherwise. This is better than expected: both legitimate ways to
   build a taper (revolve and loft) land on the same surface kind, so
   `t3-taper-adapter` can check `cone` tightly instead of accepting a vague
   OR-tuple.
3. **A bend has two legitimate spellings with DIFFERENT surface kinds.**
   Sweeping along a sketch arc gives a real `torus`; a `Pipe` along a
   `fit_spline` gives `bspline`. `Curve3D` has no arc kind at all — only
   `fit_spline`, `control_poly` and `helix` — so a true torus bend is reachable
   *only* through a sketch path. `t3-bent-tube` must therefore accept
   `[torus, bspline]`, or it stops being a check on construction and becomes a
   check on which of two correct methods the agent picked. This is the case the
   OR-tuple exists for, found by measuring rather than by taste.
4. **Shell is not census-checkable as a wall, but is as a count.** Shelling
   changes no surface *kind* — a shelled box is `plane:11` against a solid
   box's `plane:6`. So a hollow part is distinguishable from a solid one by
   planar face count, but nothing about the census says "3 mm wall". Wall
   thickness stays a volume check.

| feature | predicted | observed | usable as a check? |
|---|---|---|---|
| Revolve (line profile) | `cone` | `cone` | **yes** — tight |
| Revolve (circle offset from axis) | `revolution`/`torus` | `torus` | **yes** — tight |
| Loft (circle → circle) | `bspline` | `cone` | **yes**, and same kind as revolve |
| Sweep (along helix) | `bspline`/`extrusion` | `bspline` | **yes** |
| Pipe (along helix) | — | `bspline` | **yes**, same kind as Sweep |
| Pipe (along fit spline) | `torus`+`cylinder` | `bspline` | yes, but NOT `torus` |
| Sweep (along sketch arc) | — | `torus` | yes — the only route to `torus` |
| Shell | kinds unchanged, counts doubled | `plane:6` → `plane:11` | count only, not kind |
| stacked Extrudes (the wrong way) | `plane`+`cylinder` only | `cylinder:6 plane:7` | **the control** |

Two pieces of API friction turned up on the way, both from the probe getting
refused (each refusal was honest and arrived in the mutating RPC's own
response, which is the behaviour `regen_honesty_smoke.py` exists to protect):

- `Shell.open_faces[].element` wants the **full** name `list_elements` reports
  (`"2/cap1"`), feature-id prefix included, even though `feature_id` is passed
  in the same object. The bare tag is refused.
- That refusal reads `open face 'cap1' no longer exists` — but it never
  existed under that name. "Refuse, don't substitute" asks a refusal to name
  what IS accepted; this one names a history that did not happen. Worth a
  message fix, not scheduled here.

## B3 — three tasks that cannot be done with sketch-and-extrude

Each maps to a real BenchCAD family, and each to a Designer feature with **zero
agent-facing measurement today** (the 3D-curves workstream, v42–v45, has never
been in a sweep).

| id | feature under test | BenchCAD family | the check that bites (calibrated) |
|---|---|---|---|
| `t3-coil-spring` | `Curve3D` (helix) + `Sweep`/`Pipe` | `coil_spring` | `bspline` ≥ 1 and `plane` ≤ 2 (the cut ends) — stacked rings give `cylinder:N plane:N+1` and no bspline |
| `t3-taper-adapter` | `Revolve` or `Loft` | `taper_pin` | `cone` ≥ 1 — both right methods give exactly `cone:1`, the disc stack gives 0 |
| `t3-bent-tube` | `Sweep` along a sketch arc, or `Pipe` along a spline | `u_bolt` / `j_hook` | `[torus, bspline]` ≥ 1 — straight segments give neither |

All three are tier 3: single part, no assembly, non-prismatic construction
required. Prompts follow the existing template exactly — millimetres, pinned
axes and origin, no unrequested fillets — so a failed check is a failure to
model, not a failure to guess. **Every one keeps its ordinary dimensional checks
too**; the surface check is added evidence, never the only evidence. A part with
a beautiful cone and the wrong diameter must still fail.

`runners/reference.py` gets a builder for each and must pass 48 + N of N.
CLAUDE.md's "48/48" and `benchmarks/README.md`'s stale counts move here.

**Result — built and measured 2026-09-09.** `reference.py` is green on all
thirteen tasks, 68/68. The three new parts measured:

| task | census | bbox | volume |
|---|---|---|---|
| `t3-coil-spring` | `bspline:1 plane:2` | 44 × 44 × 53.98 | 7895.69 |
| `t3-taper-adapter` | `cone:1 cylinder:1 plane:2` | 40 × 40 × 30 | 21237.17 |
| `t3-bent-tube` | `torus:1 plane:2` | 50 × 50 × 20 | 19739.21 |

`runners/negative_controls.py` is the other half, and the half that proves
anything: it builds each part the way an agent reaches for when it cannot do
the real operation, and fails the run if a wrong build passes. All four are
rejected. The one that matters is the fine staircase:

| wrong build | passes | caught by |
|---|---|---|
| spring as 5 flat rings | 0/6 | everything |
| bend as 6 straight chords | 2/6 | solids, bbox, volume, census (`extrusion:6`) |
| taper as 6 discs | 4/7 | bbox **and** census |
| **taper as 60 discs** | **5/7** | **the census, and nothing else** |

Sixty discs at the step midpoints pass `solids`, `bbox`, `volume`, the `Ø12`
bore and the centre of mass — every dimensional check this benchmark had
before today — with a census of `cylinder:120 plane:61` and not one cone in
it. That is the failure mode BenchCAD names, reproduced here on purpose and
now detected. Six discs are caught by `bbox` as well, which is why the coarse
version alone would have been a misleading demonstration.

Two things fell out of building them:

- **A sketch arc's `start_angle`/`end_angle` are RADIANS**, there is no `_deg`
  spelling, and `describe` lists sketch entity types by NAME ONLY
  (`kSketchEntityTypes`) with no field documentation at all — so nothing tells
  an agent the unit. `end_angle: 90` is accepted and quietly sweeps 116.6°
  (90 mod 2π): a plausible arc, the wrong part, no refusal. This is the same
  trap as `set_mate`'s `rotation`, which was misdiagnosed twice as a solver
  bug. Not fixed here — changing the tool surface mid-plan changes what the
  sweep measures — but it is the strongest candidate for the next round, and
  the benchmark now has a task (`t3-bent-tube`) that an agent will hit it on.
- The bent-tube negative control exported **zero solids** while still
  reporting a bbox and a volume. Investigated and fixed — see below.

## The polyline sweep, investigated

The negative control that swept a circle along six straight chords produced a
body the app was happy with and a STEP with no solids in it. It is not what it
looked like, and the chase is worth recording because two guesses were wrong.

**Not** the chord count: one chord works, and so do two collinear lines and two
lines meeting at 170 degrees. **Not** the profile size or the corner angle
either — every combination of six/three/two chords against a 10, 2 or 0.5 mm
profile failed identically.

**It is the profile being 7.5 degrees off perpendicular to the path start.**
The chord path leaves (40,0,0) heading 7.5 degrees away from +Y, and the
control's profile plane was squared to +Y. Aligned to the chord's own
direction, the same six-chord sweep is a clean single solid.

What made it a silent wrong answer rather than an error is a chain of four:

1. OCC's `MakePipeShell` reports `IsDone()`, and `MakeSolid()` **returns
   true**, while handing back a solid whose faces are not connected to one
   another — one loose piece per path edge.
2. `SweepFeature` checked `IsValid()` and a non-zero volume. The shape passes
   both: it is not null and it has a plausible volume.
3. `regen` therefore reported `ok: true`, and the body's digest counted one
   solid.
4. `check_geometry` DID say `valid: false` — and named nothing. `bad_faces`,
   `bad_wires` and `bad_edges` were all 0 and `reasons` was empty, because
   `CheckGeometry` scanned only the FACE, WIRE and EDGE levels. Every face,
   wire and edge here is individually fine; the fault is the SHELL they form.

So the one component that detected the problem could not say what it was, and
everything else said it was fine. The STEP then wrote a solid that no reader
can read back as one.

**Two fixes.** `CheckGeometry` now scans the shell and solid levels too, so
the verdict names its reason — this shape reports `not connected`. And
`SweepFeature` runs that check on its result and refuses, in the mutating
RPC's own response, naming both the reason and the cause: *the profile must be
perpendicular to the path where the path STARTS; a few degrees off is enough.*
A grossly misaligned profile was already refused; only the near miss lied.

The lesson is one this repo has written down twice already — the GPU topology
solver that returned zero displacement and called it converged, and the OCC
fuse that came back valid, smaller and empty. **A boolean from the kernel is a
claim, not a result.** `MakeSolid() == true` is the third instance.

The negative control now squares its profile to the first chord, so it builds
a valid solid of the wrong shape and goes on testing the census (`extrusion:6
plane:2 revolution:5`, no torus) rather than testing the new refusal.

## B4 — the bushing's unchecked expression

`t3-parametric-bushing.json` gains the fourth app-reported check its prompt
already implies:

```json
{"kind": "expr_references", "variable": "wall",
 "why": "the OD is still the EXPRESSION bore + 2*wall, not the number 32"}
```

Still app-reported, still out of the oracle's score, exactly as
`benchmarks/README.md` describes. It closes the case where a part with a
variable-driven bore and a hand-typed outside diameter reads as parametric all
the way through.

## B5 — a floor (built, not yet run)

`arms/no-inspect.json`: every tool that lets the agent ask an INDEPENDENT
question is unregistered — `get_state`, `check_geometry`, `measure`,
`mass_properties`, `list_elements`, `list_variables`, and the
screenshot/render/view family — and `rpc` and `execute_script` go with them,
because either one reaches every RPC there is and would put the whole
inspection surface back. `describe` and `help` stay, so the arm measures
blindness rather than undiscoverability. Eight tools remain. The names
validate against `designer_mcp.py --only-tools`, which refuses an unknown one
loudly and lists all 27.

**The deprivation is partial by construction, and that is itself a finding.**
Every mutating call returns its own digest — bodies, bbox, volume — and its
regen summary. An agent cannot be blinded to the result of the edit it just
made, because this API answers with the consequence. What `no-inspect` removes
is the ability to ask about anything it did not just cause. So it is a floor on
*deliberate inspection*, not on feedback, and the write-up has to say so or the
number will be over-read.

Unlike `rpc-only`, this arm **does** lose capability, on purpose.

It has not been run: a fourth arm takes the default sweep from 117 runs to 156
($312 ceiling), and spending that is the user's call. `--arms full,rpc-only,
script-first` keeps the old three.

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
| before this round | 90 = 10 × 3 × 3 | $180 | ~$25 (last 8-task sweep: $13.35 / 72 runs; T4/T5 never swept) |
| **B3 (landed)** | **117 = 13 × 3 × 3** | **$234** | ~$30 |
| **B5 too (landed, unrun)** | **156 = 13 × 4 × 3** | **$312** | ~$40 |

Both numbers are confirmed by `run.py --dry-run`. Note the second row is now
what a bare `run.py --reps 3` costs, because the runner takes every arm in the
directory: `--arms full,rpc-only,script-first` is the old three.

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
