# What four peer projects are worth to us — plan

Companion to `ai-cad-survey-2026-08-18.md`, which surveyed the agent-CAD
landscape. This one reads four checkouts in `~/Documents/CAD`: **vcad**
(ecto/vcad — the only true peer: own Rust BRep kernel, ~700K LOC, shipped web +
desktop + cloud), **forgent3d-desktop** (Electron over build123d),
**CADAM** (browser text-to-OpenSCAD), **NURBGen** (AAAI'26; a fine-tuned 4B
emitting NURBS control nets).

Three of the four have less CAD depth than we do, so there is little *feature*
to take. What crosses a kernel boundary is discipline, interface design, and
measurement — and on those, three ideas are worth building and one is worth
knowing about.

Ordered by what it costs to leave alone.

**Standing, 2026-09-10.** W1 and W4's villain arm, `document` arm and pass^k
SHIPPED and merged (`723014c3`). On `wtpeer2`: **W2 COMPLETE** — all eleven
verdict RPCs carry a receipt (the twelfth, `cam_simulate`, was on the list by
its name and is a scrubber, not a check) — and **W3 SHIPPED** at v100: four
verbs, five standings, a content-keyed staleness test, a golden, and the
drone's own `verify()` reading its clearances from the document. W5 largely
fell out of W1 — the drone's derived layout composes its Z-stack as a formula
without a new syntax. A second reading of vcad, done after working inside the
app, added the findings in "Second pass" below and **demoted W6**: a
verdict-naming fix I had ranked first turned out to have been diagnosed and
half-fixed here already, and the rest of the manufacturing material is
contingent on W6, which is now the weakest item on this plan.

The drone's `verify()` now reads its clearances from the document instead of
measuring them, which is the proof W3 owed — and the conversion turned up the
next thing W1 owes (below).

W4(c) is done: `f1-cap-tube` grades a candidate against a host part the agent
never sees, and both its negative controls are dimensionally perfect parts
that the surface census cannot catch. W6 was investigated and is **not being
built** — the evidence and the condition that would reverse it are in its
section.

**Nothing on this plan is outstanding.** What is left is the open question the
drone conversion raised, which belongs to a modelling round rather than to
this one: whether the drone should mate its standoffs, now that
`solve_assembly` reports 64 of 105 occurrences placed by a typed transform.

### What the drone conversion found: parts are parametric, placements are not

W1 made `fpv_drone_spec.py`'s 119 dimensions document variables, and they
drive part GEOMETRY: `standoff_len` 15 → 9 takes the standoff part from
577 mm3 to 352. It does not move the top plate that stands on them. After the
edit and a `solve_assembly` reporting **dof 0, 0 errored mates, loops closed**,
every occurrence is where it was, and all seven declared clearances still read
`holds` — correctly, because nothing moved.

The cause is visible in the generator: `place()` does `[float(v) for v in
position]`, so a Param reaching an occurrence transform is flattened to a
literal on the way in. `Occurrence`'s own header says the intended mechanism
is mates ("A3 mates will re-solve over non-`fixed` occurrences on load — the
literal-plus-`_expr` pattern applied to placement"), and the drone does have
56 of them at dof 0. So one of two things is true and this has not yet
established which:

* the bolted mates constrain the axes but not the stack height, and the Z came
  from the explicit transform all along — in which case the assembly is
  under-constrained in a way `dof: 0` is not reporting; or
* the solver is not re-solving on a geometry change, only on demand.

**ANSWERED, and it was the first one.** Exactly one mate touches the top
plate — a `fastened` to the battery pad that rides on it. The four bolts
through it into the standoffs produced no mate at all, and neither did much
else: **64 of the drone's 105 occurrences have no mate whatsoever**, including
all four standoffs, both camera plates, the camera and every nut. They hold
their position because someone typed it. Nothing says the top plate sits on
the standoffs, so shortening one cannot move it.

`dof: 0` was never wrong. `AssemblySolve.h` says what it is — "degrees of
freedom left in the looped components", zero on a loop-free assembly because
that pass never runs. It is silent about every occurrence no mate touches, and
the drone's report line printed it as "56 mates, dof 0, loops True", which
reads as a full bill of health. I read it that way twice.

So this is the W2 shape once more, in the solver's own reporting rather than
in a check: a number that reads clean while a large fact goes unstated.
Placing a nut by transform is legitimate and refusing it would be wrong, so
the fix is to STATE it. `solve_assembly` now returns `occurrences`,
`placed_by_mates` and `placed_by_transform` beside `dof`, its `describe` row
says `dof` is the looped components only, and the drone prints the split:

    56 mates, dof 0 (looped), loops True; 41 of 105 occurrences placed by
    mates, 64 by transform

Whether the drone SHOULD mate its standoffs is a modelling question for a
future round, and now it is one somebody can see to ask.

Note none of this was an assertion bug: the clearances were right to stay
`holds`. The same assertions do go stale, and selectively, when something
really moves — raising the top plate 20 mm marks exactly the three that
involve it (prop to top plate, prop to battery, battery to camera) and leaves
the other four alone, across 91 occurrences.

---

## W1 — a parameter must survive the script boundary

**The problem, in our own repo.** Counting only the raw `set_variable` RPC
says thirteen of sixteen generators declare no variables at all; that count is
wrong, because the SDK's `var()` is how most of them do it. The honest picture
is worse in one place and fine in others:

    generator              module constants   document variables
    fpv_drone_spec.py            103                  0
    fpv_drone_parts.py             0                  0
    om615_top.py                  15                  2
    single_cylinder.py             9                  7
    slider_crank.py                0                  4
    toolmakers_clamp.py            —                  2 + a full table

`slider_crank.py` and `toolmakers_clamp.py` already do the right thing — the
clamp declares its whole `D` / `CHOICES` / `SPACER` table as project variables
by hand and passes expression strings like `"jaw_l-taper_l"` into sketches. So
the parametric path is reachable today. It is just verbose enough that the
flagship skipped it.

And the flagship is the case that matters. `fpv_drone_spec.py` is a **103-entry
dimension table**, deliberately classified into CLASS (what a 5" freestyle quad
IS), CATALOGUE (bought-part dimensions that are not free) and DESIGN (this
frame's own choices), with the derived layout computed as "arithmetic, never
transcription". It is exactly what a document variable table is for, and not
one entry of it reaches the document. The drone — 91 occurrences, 42 derived
fasteners — opens in the GUI with nothing to turn.

vcad names this exactly, in `crates/vcad-loon/src/params.rs`:

> *"Loon's own `let` bindings are lexical and vanish at evaluation — the
> emitted document keeps only the arithmetic result, so design intent is
> unrecoverable."*

Their fix is a small set of **declaration forms resolved statically before the
program runs**, so the name lands in `Document::parameters` and derived values
stay formulas:

    [defparam pitch_axis_x 310.0]
    [defparam femur_len "pitch_axis_x - 40"]

**We already have the hard half.** `set_variable {name, expression, scope:
project|part}` takes a real expression and regenerates; `Expression` is in the
document model and `VisitExprs` makes it hash-visible. Nothing is missing from
the kernel. What is missing is that the SDK makes a literal the path of least
resistance and a variable a deliberate extra call, so no generator makes it.

**The fix.** `Param`: a float subclass that carries the expression which
produced it. Arithmetic between Params composes the expression, so the script
does its arithmetic in ordinary Python and the document receives the formula:

    L = part.param("arm_len", 110.0)     # Param(110.0, "arm_len")
    part.box(w=L, h=L / 2, d=W * 2)      # height_expr "(arm_len / 2.0)"

Because a Param IS a float, every existing path keeps working untouched —
face filtering by diameter, `V(...)`, comparisons, `math.*`. `_dim`/`_dim3`
are the only places that need to know, and they are where every
expression-capable field already funnels.

**That last claim was wrong**, and being a float subclass is exactly why it
was hard to see: two places tested only for `str` and so took the literal
branch in silence — `SketchBuilder.circle`, which is every hole a generator
drills, and `_uv`, which is every line, point and polyline coordinate. A
document built entirely from Params reported `cached, executed 0` when its
table changed. Both route Param now and `tests/sdk_smoke.py` gates the class
(docs/porting-friction-plan.md, A). A derived param names itself
(`part.param("bolt_out", "arm_len - 18")` has expression `bolt_out`), so the
formula lives once, in the document.

`app.params(table)` declares a whole dict at once and hands back
`{name: Param}` — which is the shape generators already keep their dimensions
in, so converting one changes the table's binding and nothing else.

**Gate.** Two tests. (1) Open the converted project, assert the expected
variables exist, `set_variable` one of them, assert the bbox moved. (2) The
perturbation test, stolen from vcad's `recover.rs`: nudge a variable, diff the
document field by field, and assert *exactly* the expected fields moved. That
catches a field that is silently a literal where it should be expression-driven
— the same class as the config-could-never-override-a-variable scope bug.

Size: small-to-medium. One SDK addition, one generator conversion, two tests.

---

## W2 — every verdict says how it knows

**The hole.** We have a strict honesty doctrine for *mutation* — refuse rather
than substitute; a broken regen says so in its own response — and none at all
for *analysis*. These twelve RPCs all return a verdict:

    check_geometry   check_fasteners   check_drawing    check_layout
    interference     clash             contact_report   compare_geometry
    cam_verify       cam_simulate      study_results    run_motion_study

Every one of them reports a clean result and a result it could not compute the
same way. The drone round recorded *"EMPTY exact clash sweep"* with no way to
tell **swept, nothing hit** from **could not sweep three occurrences**. An FEM
factor of safety is a *prediction* and is quoted downstream as a fact.

**vcad's answer** (`crates/vcad-receipt/src/lib.rs`) is two orthogonal enums,
and it is better than it first looks:

- `ClaimVerdict` = `Pass` | `Fail` | **`Unverifiable`** — *"the oracle could not
  check the claim. Not clean, not failed — unknown."*
- `ClaimBasis` = `Predicted` | `Verified` | `Measured` — the evidentiary weight.

Plus roll-up rules that do the work: any unverifiable claim and the receipt can
never read as verified; passes resting on predictions roll up **`Provisional`,
never `Pass`**. Each claim carries the oracle id *and version*, and predicted vs
measured values with units.

**The fix.** One receipt struct in core, emitted by those twelve handlers.
`Predicted` is the honest basis for FEM and for CAM sim; `Verified` for
kernel-computed clash and mass properties; `Measured` stays reserved for a
caliper. Their milestone ladder docs are self-labelled `predicted → Provisional`
across every exotic domain — they built the mechanism *because* they needed it,
which is the strongest recommendation available.

**Gate.** A `test_conventions.cpp` rule: every RPC in the verdict list returns a
receipt. Plus a case per handler where the oracle genuinely cannot run
(unmeshable study, occurrence with no geometry, CAM op with no stock) asserting
`Unverifiable` — never `Pass`, never a silent empty result.

Size: medium. The schema is small; the twelve call sites are the work.

---

## W3 — assertions that live in the document

**Declared, not detected.** The one design rule to take from vcad's second
reading, and it belongs here rather than in W6. Their snap-fit rules ship with
a scope boundary written into the pack:

> *"Automatic detection of a snap beam from geometry alone is not attempted;
> the designer declares the beam and the arithmetic is enforced."*

A violated limit then REFUSES at export rather than handing a slicer a latch
that cannot work — our own refusal doctrine, applied to manufacturability.

That is the shape an assertion should take here. The alternative — infer what
the designer meant from the geometry and check that — is a geometry-inference
layer that is wrong at exactly the edges where it matters, and it is how a
checker starts disagreeing with the person using it. An assertion is something
someone SAID, in their own terms, that the document then holds and re-checks.
It also makes the honest answer cheap: an assertion whose subject no longer
resolves is `Stale`, not a silent pass, which is the W2 rule one level up.

Every check we have is one-shot. vcad's `check_clearance` stores a **labelled**
assertion on the document, and `verify_receipt` re-reports it later as
`Holds` / `Stale` / `Violated`.

"These two parts never touch" should be a regression test that ships with the
part and re-runs on regen, not a number in a transcript nobody re-derives. The
machinery exists — chain hashes for derived features, regen tracking, the
dirty/stale plumbing — and W2's receipt is the natural payload.

Depends on W2. Size: medium.

---

## W4 — the benchmark cannot see W1 or W2

`REPORT.md` already says the instrument is saturated: 117/117 checks, every
arm, zero over-claims, *"cannot detect a correctness regression … cannot detect
an improvement either."* W1 and W2 both change the agent-facing surface, so
"no benchmark, no claim" applies to both — and neither is visible to any of the
ten tasks. This is a prerequisite, not a nice-to-have.

Four things from `vcad/mecheval` (146 tasks, 1887 committed run blobs):

**(a) A direct-emission arm.** Their own numbers, same model, same tiers:

    tier   claude-direct (emit the doc format)   claude-mcp (tool loop)
    a1              88%                                82%
    a2             100%                                72%
    a3              56%                                56%
    a4              82%                                42%

**Their MCP surface makes the model worse than not having it.** That is the
question `benchmarks/` exists to ask and we have never asked it — `rpc-only`
and `script-first` both still go through the API. The harness runs
`--tools ""` so the arm needs exactly one MCP tool, `write_document(json)`,
that saves a project file and opens it. Nothing else. If direct emission wins
here too, it reorders the roadmap.

**(b) A lower control.** They keep `DEFAULT_CUBE` (a cube for every task) and
`FLOOD_ZONE` in CI as standing proof the checks bite — *"if any task accepts
this output, a check is broken."* `reference.py` is our upper control at 48/48;
there is no lower one. A runner that emits a box of the task's bbox and must
score ~0 is an afternoon.

**(c) A task with parameters, and a Fit task.** A task whose prompt asks for a
part *and* for the dimensions to remain adjustable is the only way W1 becomes
measurable. A Fit task is the only way assemblies, mates and exact clash become
measurable: host geometry as an input marked `agent_visible: false` — **the
harness strips it from the prompt** — the agent designs the accessory alone,
and the grader mates the two and checks clearance, interference volume and
contact area.

**(d) pass^k and per-task limits.** They take the last k runs and pass only if
*all* passed; `--reps 3` averages, which reports a one-in-three failure as 67%.
And `max_tokens` / `max_wallclock_sec` / `max_tool_calls` belong in the task,
not in a global `--budget`, because t0-plate and t5-crank-motion are not the
same problem.

Size: (b) small, (a) and (d) small-to-medium, (c) medium.

---

## W5 — `stack`: named clearances instead of transcribed coordinates

After W1, not before. vcad's declarative packing form:

    [stack y "leg" 131.0
      [lane "femur_inner" 5.0]
      [gap  "idler_clr"  1.0]
      [lane "idler_boss" 3.0]]

Every lane boundary becomes a named datum plane **and** a derived parameter;
gaps are named clearances; geometry reads `[datum+ "leg_idler_boss_hi" 3.0]`
where a literal coordinate would go. Widening one gap slides everything
outboard of it, and — their words — *"two parts that reference one datum cannot
disagree about where it is."*

We shipped named datum frames in G0 and the SDK took the gripper from twelve
transcribed coordinates to none. Same disease; `stack` is a sharper cure for
the stacked-plate / boss / clearance case, which is most of a bracket, a
gearbox wall, and every one of the drone's arm joints.

Size: medium, and worth re-scoping once W1 has landed and we can see how much
of it `d.param` already bought.

---

## W6 — a DFM check that returns fixable issues — RE-DECIDED

**Verdict: still not building W6 as scoped — and two of its four reasons are
now false, so the verdict rests on different ground and points somewhere
else.** The re-decision is first; the original follows it unchanged.

### What the grill round did to the evidence

Reason 1 said "no defect any dogfood round has found was DFM-shaped." The
grill's did. Every hem on the grease tray failed the minimum-radius check;
bend relief relieved nothing on the default mould line; a hole sat too close
to a bend. Reason 4 said "nothing here measures wall thickness — and nothing
has needed it": sheet metal now measures the thing that matters in its place,
a bend radius against what the MATERIAL takes, and it found fifteen defects
over three passes.

But those defects were found BY a check of exactly this kind, not missed for
want of one. `check_sheet_metal` is a per-material rule pack in all but name,
and it is the strongest evidence in the repo that rule packs pay. So the grill
does not meet the stated reversal condition — "a part that passes every check
here and cannot be made" — with one exception worth naming: finding 9, the
circular corner relief whose flat pattern comes out 1.67 mm thick on 1 mm
sheet. That part passes every check and cannot be cut. It is a defect in the
unfolder, though, not a rule nobody wrote; a rule pack would not have caught
it either.

Reason 3 stands untouched: there is still no printing pipeline, so the FDM
families would still feed nothing. Reason 2 stands and generalises.

### The rule this replaces it with

Generalising reason 2 — the two milling rules "are better in CAM, computed
with the ACTUAL tool in the ACTUAL job" — with what sheet metal has since
shown:

**A manufacturability check belongs to a declared process, beside the feature
that declares it. A check that has to GUESS the process is the one not worth
building.**

Sheet metal declares its process and its material (`sheet::SheetMetalRules` —
material, gauge, resolved thickness, bend radius, K with a provenance string),
so `check_sheet_metal` can state a minimum radius rather than assume one. CAM
declares the tool and the machine, so `cam_verify` can cut the real part in
the real fixture. Both work for the same reason. A cross-process rule pack
guessing a 6 mm cutter and a 3 mm wall is the version that does not.

That reading has an immediate consumer, and it is why this was re-opened:
**tube bending declares neither.** There is no rules object for a tube and no
bender profile, and the only manufacturability rule in the build is a
hard-coded `radius < 1.5 × diameter` flag on `tube_schedule` — a literal, in a
project that derives its defaults. So the work W6 would have gone into goes
into `docs/tube/plan.md` instead, as `tube::TubeRules` and `check_tube` built
in the shape `SheetMetalRules` and `check_sheet_metal` already proved, and W6
stays closed.

If a printing pipeline ever lands, reason 3 expires and the FDM half of the
original is worth the same week — built per-process, declared the same way.

### The original verdict, for whoever revisits it

**Verdict: not worth building now.** Four pieces of evidence, and a condition
that would reverse it.

**1. No defect any dogfood round has found was DFM-shaped.** The drone's eight
(docs/modeling/fpv-drone.md) were a hole check sampling over a bounding-box
diagonal, a joint reading blindness off the axis, a tapped-hole lookup asking
the wrong part, a thread table that started at M3, a screw not declaring its
own thread, thread relief depending on argument order, and two drawing bugs.
Not one is "this part cannot be made".

**2. For milling, the two rules that matter are already in CAM, and they are
better there.** `internal_radius_too_small` is `cam_verify`'s
`leftover_regions` — docs/cam/router-round.md says it exactly: "a corner
radius is exactly where it did not [go]". `tool_inaccessibility` is the lead
refusal: "refused, not shortened, where the opening is too small for it — a
lead into the wall is a gouge". Both are computed with the ACTUAL tool in the
ACTUAL job, which a rule pack guessing a 6 mm cutter at design time cannot
match.

**3. The families CAM does not cover are the PRINTING ones — and they would
have no consumer.** `steep_overhang`, `floating_floor`, FDM `min_wall`: there
is no printing pipeline here. `nozzle`, `filament` and `layer_height` do not
appear anywhere in `src/`. `export_stl` is where the road ends. vcad has DFM
packs because it has `vcad-slicer` and the rules feed something; ours would
feed nothing.

**4. Nothing here measures wall thickness — and nothing has needed it.** That
is a real gap in the abstract. In practice the parts that exist are a 3 mm
printed antenna mount, a 3 mm battery pad and 3–5 mm carbon plate. No rule
pack fires on any of them.

**What would reverse this**, stated so it can actually happen rather than
being a hedge:

* a printing pipeline lands (a slicer, or a printer profile that
  `export_stl` answers to) — then the FDM rules have a consumer and W6 is
  worth building the same week; or
* a dogfood round produces a part that passes every check here and cannot be
  made. Eight rounds have not.

If it is ever built, it takes the rule-pack shape and the face naming from
"Second pass" with it from the start — both are cheap then and expensive to
retrofit.

The original plan follows, for whoever revisits it.

**Demoted after the second pass, and it should be decided on its own merits.**
CAM already tells us whether a toolpath can cut the part, so DFM's distinct
value is narrow: catching a thin wall BEFORE CAM, and covering processes we
generate no toolpaths for. It ranks behind W3 and behind W2's remaining seven
handlers. If it happens, it takes the rule-pack shape and the face naming from
"Second pass" with it from the start.

forgent3d's `print_check` runs on the **real BREP, never the tessellation**:
wall thickness by inward ray casting (`IntCurvesFace_ShapeIntersector` — our
kernel, same call), overhang by per-facet normal against the build axis, small
features by edge length and face area. It returns `issues[].code` the agent
reads and fixes in the parametric source, plus a 0–100 score with
error/warning penalties, and the profile comes from the printer rather than a
literal — which is our defaults doctrine restated.

We have CAM and FEM and nothing that says *"this wall is 0.6 mm, under two
perimeters."* Independent of everything above.

Size: medium.

---

## Second pass — what a reading of vcad found after working in the app

Five more findings, and a correction that matters more than any of them.

### The correction: our checks already learned this lesson

vcad's `DfmIssue` carries `face_indices` plus `origin_op` — the node that made
the face — and a machine-applicable `DfmFix` (`SetParam{node, path, value}` /
`WrapOp` / `ReplaceOp`), so an agent gets a patch rather than a complaint.
Their provenance is a stub (`ProvenanceMap` "ships the data model only"),
while ours has been real all along: `NamePrimitiveElements(shape,
std::to_string(GetId()), map)` means **the element-name prefix IS the feature
id**, so `1/face3` already says "face 3 of feature 1".

The obvious conclusion — make `check_geometry` report offending element names
instead of `int badFaces` — is weaker than it looks, and
`benchmark-construction-plan.md` is why. The real incident that motivated
naming had **`bad_faces`, `bad_wires` and `bad_edges` all zero**: the fault was
at the SHELL level, which has no element name. That was fixed by naming the
REASON, not the face. Face identity would have added nothing.

So it stays worth doing, but only where something needs it — a DFM issue IS
about a specific face, B-Rep validity apparently is not. Filed under W6, not
ahead of it.

One trap for whoever writes it: `CheckGeometry` counts with a
`TopExp_Explorer` while element names index off a
`TopTools_IndexedMapOfShape`. Those orders usually agree and are not
guaranteed to — resolve with `FindIndex`, never by counting explorer steps.

### Worth taking, inside W6 and only there

**DFM is a versioned rule pack, not code.** Twelve TOML packs (`fdm`, `sla`,
`cnc-3axis`, `hobby-3axis-mill`, `sheet-metal`, two castings, injection
moulding, four PCB houses), each `schema = "vcad.dfm/1"` with a version, and
every rule carrying `severity` plus a `fix` that names the repair
(`thicken_wall`, `raise_fillet_radius`, `reorient_or_support`, `manual`). The
same wall thickness is an error for CNC and a warning elsewhere, because the
process is DATA. That is our defaults doctrine — a number carrying a unit
derived from what it acts on — with the process as the thing it derives from,
and it joins naturally to CAM machine profiles, which are the same kind of
object. Cheap at design time, expensive to retrofit: if W6 happens, it starts
here.

**`floating_floor`**, which is a different check from overhang angle and
sharper than the per-facet-normal test forgent3d uses: *"A flat downward face
above first-layer height with no geometry below its footprint starts printing
in mid-air. Distinct from steep_overhang: 45-degree walls self-support; flat
shelves cannot."*

### Not now

**Print-then-measure** (`vcad-kernel-calibration`) is what would make
`Basis::Measured` more than a reserved enumerator: `predict_print` snapshots
the claim, calipers and a scale supply readings, `record_measurement` binds
them, and the report yields per-feature deltas plus aggregates a profile can
act on — axis scale factors, hole undersize, wall/flow offset, which then feed
defaults. Their framing is the insight: *"the user's own printer is the one fab
rail with zero vendor dependency, so every print can capture a (predicted,
measured) pair for near-zero cost."*

It is a real build, and its value depends on a physical loop there is no
evidence of here — the dogfood rounds end at G-code and STEP, not at a printed
part and a caliper. `Measured` sitting reserved is honest; a calibration engine
with nothing measuring into it would not be.

**A shared `CostEstimate` shape** across slicer / DFM / quote panel exists there
because three surfaces were disagreeing. We have `cutting_minutes` and a BOM
and no third consumer. Revisit when one appears.

### One-line change, worth making

CLAUDE.md quotes counts that drift: the benchmark line has already moved 8 to
13 tasks, 48/48 to 68/68, $180 to $390. The file's own rule about ports covers
it — *"a number repeated in a doc goes stale while the registry cannot"* — so
point at `reference.py`, which prints the true count, rather than restating it.
(vcad solves the same class differently: one JSON file per changelog entry with
the rollup gitignored, "so concurrent PRs never collide on it". We have seven
worktrees and hit exactly that this round — two plans written the same evening,
both editing `docs/README.md`, both independently finding the unread
`faces_by_kind`.)

## Known and deliberately not doing

- **The differentiable seam** (`vcad-kernel-diff`) is the one capability they
  have that we have no answer to: d(mass properties)/d(named parameter), so
  "get the FoS to 2.0 by moving the fillet radius and the rib thickness" is a
  solve rather than a search over rebuilds. The architecture is portable — cut
  the chain at the mesh, never push derivatives through boolean combinatorics;
  freeze topology on the primal and make any topology change under perturbation
  a hard error; validate every path against central differences. It is also
  large, and we do not yet have the optimization problem that would pay for it.
  Revisit after W1, which is its prerequisite anyway (there must be parameters
  to differentiate with respect to).
- **A second kernel, ECAD, and their exotic analysis domains** (photonics,
  neutronics, antenna MoM, thermal, acoustics). Their own receipt schema files
  all of them as unvalidated predictions.
- **CADAM's multi-view loop** — *"do not finalize just because it compiled;
  finalize because the views look right"* — is `render_sheet`, shipped in V3
  with the kernel digest footer. `ai-cad-survey-2026-08-18.md` found the same
  idea in flowful-ai/cad-skill and it is the reason V3 exists. The only delta
  left is attaching a sheet to a mutating call's own response, which costs a
  projection per edit; not obviously worth it.
- **NURBGen** is single-shot surface generation with no history and no edit —
  the opposite bet. The only portable piece is its wire encoding: delta-encode
  against the first element, run-length uniform runs, single-character enums,
  and drop anything recomputable (they strip `bbox_area`). That is a
  `get_state` / `list_elements` bandwidth playbook we already applied once when
  `bodies[].mesh` came out.

## How advanced is vcad, for the record

~700K LOC (462K Rust + 238K TS) and 2209 commits since **2026-01-27** — 1816
human, ~390 agent — against our 235K and 1252. Three times the code in half the
time, mostly agent-written. They ship: vcad.io, installers for three platforms,
`npx @vcad/mcp`, Supabase auth and sync, a public leaderboard. We ship a
nix-shell on one machine.

They are behind on geometric trustworthiness, and their own benchmark is the
evidence — A2 (cube with a pocket, bolt-circle block) at 72%, A3 (hex nut,
crossed cylinders) at 47%, and the headline Suite C mech tier at **0/27**.
Those are tier-0 parts to us because OCCT does them without comment; their
kernel is seven months old. The rates confound model ability with kernel
ability, but "hex nut, 47%" is a smell.

Wider, shallower, shipped — against deeper, narrower, better-verified, with no
users. Not the same axis.
