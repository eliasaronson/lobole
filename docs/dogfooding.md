# Dogfooding rounds — what building real things found

> Consolidated 2026-08-20 from five scattered files (three round backlogs and
> two execution plans). Item-level status was **not** re-verified in that
> pass: each round says what it found and, where a plan followed, what was
> executed. Treat an unmarked item as "last known open", not as proven open.

The rule these rounds exist to enforce: **the only way to find out whether a
CAD app is usable is to build something real with it.** Every round below
built an actual object end-to-end — model, assembly, drawing, simulation —
and wrote down every place the app fought back. Bugs found this way are
different in kind from bugs found by testing: they are about *sequences*, not
functions.

Two rounds were driven by an agent against its own headless-preview instance,
one was hands-on GUI, and the dock round mixed both. That mix matters: the GUI
and the scripting interface fail in different places, and only the dock round
(where the same model was built both ways) surfaced the ones that need both.

---

## 2026-08-23 (c) — a stress analysis and an optimized shape, by clicking

**Why here:** the same countable gap, three rounds running. Of 185 GUI tests,
four touched simulation and all four stopped at *setup* (a gravity checkbox, a
force direction); none ran a study or read a result. Topology optimization
(workstream 15, v1 + GPU0–GPU4) had **zero**. Both are modules whose output a
person acts on without being able to check it by eye, which is what made the
drawings round worth doing.

The object: a steel cantilever, 200 x 10 x 20 mm — three different dimensions,
so an axis mix-up cannot hide — fixed at one end, 100 N on the other. Beam
theory gives the answer in closed form, so every number the app printed had
something to be checked against.

### The solver is right, and it is right to four digits

Worth stating first, because the rest of this round is defects. Tip load 100 N
on the end face: **0.7921 mm** measured against 0.800 mm from `FL^3/3EI`
(-1.0%), peak von Mises **56.86 MPa** where beam theory at that node says
56.83. Distributed over the top face instead: **0.29612 mm** against 0.300,
and **27.10 MPa** where the theory at the sampled node is 26.91 (+0.7%). The
field plot is a clean linear ramp from the clamp to the tip. The FEM core does
not need work; everything below is the panel and the GPU path around it.

### Topology optimization returned a confident wrong answer

Same study, switched to Topology, defaults otherwise. It reported:

```
Iterations: 3 (converged)
Volume fraction: 0.107        <- the target was 0.30
Compliance: 0 -> 0            <- a loaded structure has compliance > 0
```

Every number in that block is wrong, and none of them is flagged. The same
study solved on the CPU path (`DESIGNER_TOPO_GPU=0`) gives 60 iterations,
compliance 4503.5, volume fraction 0.3000000. The GPU path is the DEFAULT on
any machine with a usable device — including the one this app is developed on.

The chain, from the bottom:

1. The fp32 GPU CG **makes no progress at all** on this system. Traced:
   `rel res 1.00e+00` after 128 iterations, with both the multigrid and the
   Jacobi preconditioner (2048 iterations there, same result). A relative
   residual of exactly 1.0 means x never left its initial guess. Per-iteration
   scalars show the residual *growing* — 1848, 2265, 5402, 13074, 28395 — which
   is fp32 CG breaking down on an ill-conditioned system. A slender bending
   problem is exactly that; the femgpu gate's mesh is a chunky 100x40x40 box,
   which is why it never saw this.
2. `TopoCgSolver` **detects it** — the stall/restart machinery gives up after
   two restarts with no gain and honestly returns `converged = false` with the
   best iterate it has, which on a cold solve is a vector of zeros.
3. `TopoGpuSolver::Solve` **threw that away**: it returned `true` and the
   zeros, ignoring `stats.converged`. (A *floored* solve is deliberately
   accepted — SIMP reacts to energies, not to the last digit of u — but a
   solve that made no progress is not a floored solve, it is no solve.)
4. `TopologyOptimize` accepted a zero displacement field: `u.allFinite()` is
   true for zeros. Compliance `F.dot(u)` = 0, every sensitivity 0, the OC
   update with nothing to react to walks the densities down to the bound, the
   max density change falls under `changeTol` on iteration 3, and the loop
   reports **converged**.

Fixed at (3) and (4): a no-progress solve now fails with its residual in the
message, and `TopologyOptimize` **drops the external solver and finishes on
the built-in CPU path** — which its own header already called "the fallback
and the correctness oracle", a role nothing had implemented. The result names
the solver that actually did the work: `cpu (gpu: NVIDIA GeForce RTX 4060
Laptop GPU failed: GPU CG made no progress (relative residual 1.000000 after
128 iterations))`. Wrong-and-fast became slower-and-right.

Gated by `femgpu: a slender cantilever solves on the GPU (GATE)`, which
reproduces the failure deterministically on a 20x1x2-cell beam and passes
either way — the GPU solving it, or saying it could not and the CPU finishing.
**Not fixed: the fp32 numerics themselves.** Making the GPU path solve slender
bending is its own project (fp64 sections, or a preconditioner that survives
the conditioning); this round makes the failure visible and harmless.

### The panel could not be clicked, and that is why none of this was known

Every defect above sat behind a panel that no test could drive:

- **`Run` is unreachable by name.** A widget scrolled out of a panel reports an
  EMPTY label to `ui_items` — it is still listed, with its position and its
  numeric id, but nameless — and arming any BC tool adds two rows, which is
  enough to push Run below the fold. There is no RPC that scrolls a panel
  (the dock round's open P5), so nothing could bring it back into view.
- **The Analysis combo had no `###id`**, so it gathered with an empty label
  (the drawings-round rule, in a second panel). It is the ONLY way to reach a
  topology study from the GUI — which is the whole explanation for workstream
  15 having no clicked coverage.
- **Study rows, load rows and keep-face rows sat in `PushID(int)` scopes** —
  the configurations-round rule, in a third panel.

All of them carry stable ids now, and `//###Model/###study_analysis` clicks a
combo that is scrolled off the top of the panel: `ItemInfo` resolves a path by
hash and never consults the label. Both rules are in `docs/ui/gui-coverage.md`.

### Three more the round found

- **Clicking Run froze the whole app.** The static path called the blocking
  `RunStudyAndCache` straight from the draw, while the topology path next door
  ran on a worker thread with a progress bar and a Cancel — and the `run_study`
  RPC has an async mode too. So the same action, done by clicking, behaved
  worse than done by scripting, on a panel where the wait is measured in
  seconds to minutes. Both Run buttons (panel and toolbar) go through
  `StartStudyRun` / `StartTopologyRun` now, and the panel says `Solving... 4s`.
- **Two controls could not be clicked at any width the panel ships at.**
  `SameLine()` does not wrap, so the third item of a row drew past the panel
  edge: the **Pressure** boundary condition and the **Displacement** result
  field, gone. `ui::WrapRow` breaks the line when one will not fit.
- **The labels were clipped**, the exact defect commit 7e6a426 had just fixed
  everywhere else — "Analysi", "Mesh s", "Magn", "Direct". `LeftLabel` existed
  and this panel had not been converted. Hovering anything then scrolled the
  panel sideways, because the overflow made it horizontally scrollable.
- **A stale legend.** Switching a study from Static to Topology left the von
  Mises colorbar on screen reading `0 MPa` — a legend for a field the study
  does not have. The displayed field resets with the analysis type now, and
  the legend refuses to draw one that does not belong to the study.

### Two refusals that were right, and one that could not be acted on

Running topology on the auto mesh refuses with "volume fraction target is
below the frozen support/load volume". That is correct — the elements touching
a support or load face are frozen solid — but the message named no number, and
on a part meshed one element thick the frozen skin is **100% of the part**, so
the advice it implied (raise the target) was impossible. It now says which it
is: below 85% it names the fraction and the target that would work; at or
above, it says the mesh is too coarse to carry a shape at all. A topology
result under 2000 elements also carries a warning now, because a coarse mesh
returns a coarse design and that looks like a bad optimizer rather than a
setting.

### New gates

- `sim/a_static_run_matches_beam_theory` — builds the cantilever by clicking,
  places both boundary conditions by clicking faces in the viewport, runs, and
  checks the displacement and stress against Euler-Bernoulli.
- `sim/topology_holds_its_volume_target` — the same by clicking, switched to
  Topology through the combo that had no id, and checks the two things a zero
  displacement field destroys: the volume constraint and a compliance > 0.
- `femgpu: a slender cantilever solves on the GPU (GATE)` — the deterministic
  reproduction, at the solver level.

### Still open

- The fp32 GPU CG cannot solve slender bending problems. It falls back now, so
  it is a performance limit rather than a correctness one.
- The auto mesh size is chosen for a stress field and is far too coarse for a
  topology design; the warning says so rather than the sizing being fixed.
- `describe` still lists no parameter schema per method (the dock round's P4).
  Third agent to reach for `describe {method: ...}` and find it absent.
- The box tool's own dimension fields have no ids, so a test still has to set
  a primitive's size through the command layer.

---

## 2026-08-23 (b) — a parametric family, by clicking

**Why here:** the same countable gap the drawings round used. Configurations
(workstream 14) and Variables (workstream 4) are both v1-complete, both have
a smoke test through the RPC, and neither had a single GUI test touching it.

### The flow dead-ended on the first thing anyone would try

Add a variable, make a configuration of it, override the value. That is what
a parametric family *is*. It answered:

```
No variables to override — add some in Variables.
```

...immediately after adding one. The Variables panel's only button creates a
variable at **project** scope (deliberately: "define a driving dimension
once"), and the Configurations panel listed **part**-scoped variables only.
There is no control anywhere in the Variables panel to create a part-scoped
one, so through the GUI the two features could never meet. Every
configuration could suppress features and nothing else.

`Document::Regenerate` has handled the project case all along — there is a
block that injects an overridden project variable as a synthetic local so it
shadows the project value in that part alone, written for the C-clamp
configuration bug. Only the panel never listed them. One place, and the whole
parametric half of a workstream came back.

### Why neither panel had ever been clicked

They could not be. Every row of the Variables table and every configuration
row sat in an **`ImGui::PushID(int)`** scope, which leaves a widget reachable
only by numeric id (`docs/ui/gui-coverage.md` already says so) — no test and
no agent could name a cell. The tables were `##`-hidden too, which made even
a wildcard path an ID-scope crawl. Both now carry stable `###` names, and
`configs/a_parametric_family_by_clicking` walks the whole story: a variable, a
dimension driven by it, a configuration overriding it, and the geometry
following in both directions.

### Left open

- ~~**The primitives are not parametric.**~~ **FIXED (v61).** `BoxFeature`,
  `SphereFeature` and `CylinderFeature` carried no expression fields at all,
  so no variable could drive a box and a part built from primitives could
  never be configured — the Configurations panel would offer overrides that
  changed nothing. They take `*_expr` fields now, the editors give them the
  same `fx` button every other feature has, and
  `tests/golden/v61-parametric-primitives.json` is a box and a cylinder
  driven by variables with a configuration that changes both.
- ~~**The dock tabs are unreadable**~~ **FIXED.** Five panels in the 210 px
  left dock rendered as `M`, `V…`, `C…`, `H…`, `C…` — Configurations and CAM
  the same string. Each title now leads with an icon, so the characters that
  survive truncation are the distinguishing ones.
- **A `**` wildcard ref can hang the ImGui test engine** — a crawl through
  the variables table ran past the 300 s watchdog. Fully-qualified paths
  only, which is now the reason both tables are named.

---

## 2026-08-23 — a drawing of a plate, by clicking

**Why here:** of 158 GUI tests, exactly one opened a drawing tab, and all it
asserted was that the tab appeared. The whole D0-D4 drawing workspace — place
a view, pick an orientation, dimension it — had never been clicked. Drawings
were well covered by `drawing_smoke.py` and `test_drawing_hlr.cpp`, which is
the RPC half: structurally the half that cannot find "this control is dead on
this tab". The object was deliberately dull — an 80 x 8 x 60 mounting plate
with two holes — because the point was the *sequence*, not the geometry.

### The one that matters: every drawing view was mislabelled

The front view of a plate 80 wide and 8 thick came out **80 x 60**. Measured
against a part whose three dimensions are all different:

| named view | showed | should show |
|---|---|---|
| front | 80 x 60 | 80 x 8 |
| top | 80 x 8 | 80 x 60 |
| right | 8 x 60 | 60 x 8 |

Every one matches a **Z-up** convention exactly, and this app is **Y-up**
(the view cube's Top is +Y). `OrientationAxes` in `DrawingDocument.cpp` was a
Z-up table — its Iso case says so in a comment — so a "front" view was really
the top, on every drawing anyone had ever made. Fixed as a pure rotation of
the basis, which preserves handedness and therefore cannot mirror anything.

**Why 753 tests missed it:** the HLR tests pass direction and up vectors in
by hand (`kFrontDir{0,1,0}, kFrontUp{0,0,1}`) and never go through a named
orientation. The projector was right; only the table naming the directions
was wrong, and nothing crossed the two. This is the third module to get the
axis convention wrong (CAM's Z, simulation's gravity) — the pattern is that
a module written against the CAD habit meets an app that chose Y-up.

### The rest

- **The Hole tool's default diameter was a literal 0.5 mm** — on an 80 x 60
  plate, holes you cannot see. Straight against the rule U10 left behind
  (`docs/ui/u10-defaults-and-refusals.md`), and the `defaults/*` gate misses
  it because the tool *does* change the document: it drills two invisible
  holes rather than refusing. Now a tenth of the narrower side of the face
  being drilled (6 mm on this plate), with blind depth following it.
- **Six RPCs were part-gated with no cause**: `get_settings`/`set_settings`
  (the user's preferences, not the document), `reset_layout` (the dock
  layout — and the recovery for a wedged one, so gating it is backwards),
  `checkpoint_create`/`checkpoint_restore`/`history_restore` and `rt_build`.
  The last four carry comments saying they were *already* widened to cover
  the whole project. **The gate is a list kept apart from the handlers**, so
  widening a handler never widened its gate.
- **A typo'd method on a non-part tab answered "switch to a part tab"** —
  the gate ran before the unknown-method check, so any misspelling sent you
  hunting a tab problem that was not there. It is how this round discovered
  `list_versions` is called `checkpoint_list`.
- **Scripted views and clicked views were placed by different rules.**
  `add_drawing_view` dropped every view at the sheet centre at 1:1; the Add
  view button fits a standard scale and places clear of what is there. Three
  scripted views landed in one pile. The RPC uses the button's rule now when
  the caller names neither position nor scale.
- **Views of one part each chose their own scale** — exposed by the fix
  above: the right view is the smallest face, so it was blown up to 2:1
  beside its 1:1 siblings until it collided with the top view. A sheet's
  views of one part share a scale now. This one was in the *shared* rule, so
  clicking Add view three times had always done it.
- **The ui actions answered a missing parameter with raw nlohmann**
  (`[json.exception.out_of_range.403] key 'chord' not found`) where the rest
  of the surface says "missing required parameter". Now they match, and name
  the key you probably reached for instead: *missing required parameter
  'chord' for key_press — you passed 'key'*.

### Why the drawing workspace had no clicked coverage

It could not have had any. `ImGui::Combo("Orientation", ...)` gathers with an
**empty label** in the test engine, so no ref could address the two combos
that drive the panel — `ui_items` reports them, and reports nothing you can
click. They carry `###ids` now, and
`drawing/build_a_drawing_by_clicking` is the first test to build a drawing
the way a person does. It asserts what each named view shows, that the views
share a scale, and that the sheet validator finds nothing — so all three
defects above are now caught by clicks.

### Left open

- **Left-panel labels truncate** in the default 210 px dock: "Docume",
  "Orienta", "^ or click a plane / flat face in the". Systemic (three panels
  seen in one round), and parked by Elias until he is back at the machine.
- **The prompt pill and the Tool window overlap** at the top of the viewport:
  the Hole tool's "Step 2/3" pill runs underneath its own dialog.

---

## 2026-07-19 — fidget spinner

Collected from modeling a three-lobe 608-bearing fidget spinner end-to-end
through the MCP interface, plus Elias's hands-on GUI round, plus a comparison
against [flowful-ai/cad-skill](https://github.com/flowful-ai/cad-skill)
(CadQuery-driving Claude skill). Owner areas link to the plan docs.

### Sketching / dimensions (GUI) — the big UX cluster

- **Center rectangle tool** (rectangle from center + corner; today only
  corner-corner). Small; SketchToolbar + AddRectangle variant.
- **Dimension tool rework — make it a real tool** (docs/sketching/plan.md):
  today's flow fights every CAD habit. Wanted: arm a Dimension tool, then
  click targets, tool infers the dimension kind (traditional CAD order):
  - one line → its length; two parallel lines → the perpendicular distance
    (today two lines yield the DIAGONAL endpoint-to-endpoint distance);
  - point+line → perpendicular distance; two points → distance;
  - circle/arc → radius/diameter (already good).
  Selection precision underneath it: lines are currently hard to pick, and
  clicking a point grabs both the line and the point (need pick priority +
  disambiguation like the model-space same-spot cycling).
- **Dimension input = expression field**: arithmetic ("22/2 + 0.15") and
  variable references must evaluate in the inline dimension editor — today it
  apparently only takes plain numbers (the constraint model supports exprs;
  the GUI editor is the gap).
- **Dimension-on-create** (SolidWorks/Onshape habit): while a drawing tool is
  active, typing a number applies it as a driving dimension to the entity
  just drawn (line length, circle radius) — "sketch by numbers". Big
  ergonomic win; pairs with the U-backlog "dimension-first sketching" item.
- **Dimension label placement must stay on the dimension's own axis**: the
  witness/extension-line geometry should constrain the label drag —
  perpendicular offset only — instead of free 2D dragging that shears the
  indicator away from what it measures.

### Patterns

- **Linear sketch pattern shows no preview** (circular does). Verify whether
  the op even applies or silently no-ops; fix the preview path either way.
- **SEGFAULT: circular pattern on a part body** (dev-watch log 14:54:54):
  repeated per-frame "Document synced" lines with fresh OutlineRenderer mesh
  ids (597/598/599…) then SIGSEGV — looks like a per-frame version bump
  (preview param drag?) racing the outline resync. Repro attempt + fix in
  flight; if it survives this session it is the top bug here.

### Rendering (fold into S3, docs/rendering/ssao-soft-shadows.md)

- **Dark dielectrics wash out**: plastic_black (0.05 albedo) renders nearly
  identical to the unassigned palette grey — the IBL specular term
  (albedo-independent for dielectrics) dominates. Gold/brass/white fine.
  Screenshots in the session record. Likely needs specular-AO/horizon
  attenuation or prefilter energy check.
- **SSAO imperceptible** at default strength in concave junctions
  (plastic_white spinner: lobe/hub creases show no visible darkening).
- **Expose the new View toggles over RPC/MCP** (Shadows, Ambient Occlusion)
  so agents can A/B them for visual verification.

### MCP / scripting interface

- **Expression-driven sketch geometry**: entity coordinates/radii are
  literal-only through add_feature; only constraints carry exprs. Either
  accept `*_expr` on sketch entities or provide an "anchor + distance
  constraint" helper — without it, in-plane parameters (arm_len, lobe_r)
  don't actually drive.
- **list_elements should return edge kind/length/radius (and face kind/area)**
  inline — identifying the 8 bore rims took measure-probing opaque names
  ("6/g1;U0;M6" turned out to be a cylinder seam, the real rims split across
  two naming families).
- **set_material → renderer**: confirmed working (early confusion was the
  plastic_black wash-out above), but while probing: consider returning the
  applied baseColor in the reply for agent-side sanity checks.
- **Headless preview**: screenshot needs a GUI instance; an STL-based or
  offscreen render fallback would make agent/CI flows self-sufficient
  (cad-skill renders 6-view orthographic PNGs from the STL headlessly).
- **Watertightness/manifold check on export_stl** (cheap with OCC; report
  shells/free-edges count in the reply).
- **Cross-part element queries**: while assembling, resolving the spinner's
  face names required set_active_part round-trips (list_elements only reads
  the active tab). Let list_elements/measure take an optional part name/id.

### Workflow prompt (tools/designer_mcp.py WORKFLOW) — cad-skill adoptions

- **Manufacturing section**: FDM minimums (walls ≥1.2mm, hole clearance
  0.2–0.3mm, min feature 0.4mm), print-orientation note in the final answer,
  one-line print recipe convention (material/layer/walls/infill + why).
- **"Research real dimensions first"** rule: when interfacing with real
  products (bearings, inserts, connectors), web-search the datasheet numbers
  before sketching; never guess.
- **Phased delivery cadence**: base shape → show the user → features → show →
  finish (chamfers/fillets) → deliver STL + parameter table + "want me to
  adjust anything?" closing offer.

### Assemblies (full-path exercise: spinner + 4 bearings, fastened mates)

The path WORKS end-to-end (part → assembly → occurrences → face-connector
mates → solve → save → per-part STL export). Findings:

- **Assembly-level interference is missing**: the `interference` RPC (and the
  underlying I4 check) is part-tab only — "the active document is an
  assembly" error. Clash checks matter MOST in assemblies; extend it to
  occurrence pairs (transform the part bodies like assembly measure does).
- **Stale-toponame trap on mates**: bore faces listed BEFORE a chamfer kept
  their old names ("6/g1;M6"); after the chamfer they are "6/g1;M6;M7", and
  the mate errored "connector reference could not be resolved". Correct
  behavior (the history really changed the face), but the error should name
  WHICH connector and ideally suggest near-miss names ("6/g1;M6;M7 exists —
  face was modified by feature 7").
- add_mate flipped=true default seats a bearing in a bore flush and
  coaxial — good default, worked first try once names were fresh.

### Toolchain

- **C++23 bump** (from C++20): flip CMAKE_CXX_STANDARD, link stdc++exp (GCC),
  swap the glibc crash-handler backtrace for std::stacktrace (symbolized
  frames, no addr2line step). Also unlocks std::expected (feature-error/RPC
  result plumbing), std::print, deducing this, std::mdspan (FEM matrices).
  Compiler ready (nixpkgs GCC >= 13); risk = OCC/ImGui/spdlog headers under
  -std=c++23 — verify with a full build + both suites, one chore commit.

### Crash-debugging infrastructure (added this session)

- Fatal-signal backtraces (3284986): SIGSEGV/SIGABRT now print a stack to
  stderr → dev-watch log. glibc backtrace now; std::stacktrace when C++23.
- Circular-pattern segfault: headless param sweep (count 0..100, angle
  -90..720, per-frame modify) does NOT reproduce — core exonerated, crash is
  GUI/render-side (log shows per-frame resyncs then SIGSEGV). Next GUI repro
  will carry a backtrace.

### What was done about it

The execution plan that followed is folded in here; its phase-by-phase
detail (estimates, sign-off shape, per-phase design) lived in a separate
file and is in git history — what survives is the outcome.

> Status: PHASES 0–4 EXECUTED 2026-07-19 (same day as approval; commits
> cd81d31, e792b51, 934e76b, b76c891, 18e3310, f700f62, eadf0c6). Format now
> v38. Phase 5 (headless preview) remains the open stretch item; the
> circular-pattern segfault stayed unreproduced (regression-netted, next
> occurrence self-diagnoses via the crash handler). Decisions honored:
> dimension tool = full traditional inference (1A); sketch-entity `*_expr`
> fields with format bump (2A).

Phases, in the order they landed:

- Phase 0 — C++23 toolchain bump
- Phase 1 — Crashers & pattern correctness
- Phase 2 — S3 rendering tuning
- Phase 3 — Dimension & sketch UX cluster
- Phase 4 — MCP/interface round
- Phase 5 — Headless preview (Tier 1 done, 1438ee2)


---

## 2026-07-20 — swivel caster

Second self-driven dogfooding round: an agent built a genuine 5-part swivel
caster (wheel/axle/fork/kingpin/plate) + assembly + drawing + FEM against its
OWN headless-preview instance (Tier-1 render loop, screenshot-verified every
step). Model at `CAD_files/swivel-caster.dsgn`, round-trip verified. **No
crashes.** Exercised the surfaces the fidget-spinner round never touched:
revolve, sweep, loft, moving mates (revolute DOF driven), drawings + PDF/SVG,
FEM, assembly interference/BOM/exploded.

Verified against source where load-bearing: `SymbolTable m_symbols` is a
`Document` member (#1 confirmed — variables are genuinely per-part).

### P1 — correctness / blocks parametric workflow

- **#1 Variables are PART-SCOPED, not document-global** (blocks-workflow +
  wrong-geometry). `set_variable` writes the active part's `Document::m_symbols`;
  each part has an isolated set. A part B sketch can't see part A's
  `axle_dia` — the whole point of named variables in an assembly (one
  `bearing_bore` driving axle + wheel bore + fork holes) doesn't work. The
  workflow prompt even calls them "document variables." **Compounding: expr
  fields silently fall back to the literal twin on unknown-variable, no error
  on the feature** — wrong-but-plausible geometry passes unnoticed (only
  Hole surfaced it, via a zero value). Fix is a design call: project-level
  shared symbol table (assembly-scoped or document-tree-scoped) vs. keeping
  per-part + an explicit "link/publish variable" mechanism. The silent-fallback
  half should error regardless (feature carries an error on unresolved ref).
- **#2 Loft twists/pinches when section vertex counts differ**
  (wrong-geometry). square(20)→circle(r5)→circle(r5) gives an hourglass with a
  pinched waist (vol < the top cylinder alone). Two identical coaxial circles
  loft to an EXACT cylinder, so the engine is fine for matched sections — the
  bug is section-pairing / start-vertex correspondence / anti-twist when edge
  counts differ (4 verts vs 1 curve). Lives in the loft section-alignment.
- **#3 Hole through:true silently drills the 1 mm default** when
  `diameter_expr`'s variable is out of scope (removed 6 mm³ not 420, no error);
  a variable resolving to 0 DOES error. Same root as #1's silent fallback +
  a bad default. through:true itself works.

### P2 — feature gaps / inconsistency

- **#4 Extrude hard-requires literal `distance` even with `distance_expr`**
  (friction/inconsistent). `{distance_expr:"plate_thk"}` → "key 'distance'
  not found", no body. Revolve defaults `angle` to 360 with only `angle_expr`
  — Extrude should mirror that (schema marks `distance_expr` optional).
  Deserializer reads `distance` as required.
- **#5 mass_properties on an assembly tab returns total: null**
  (missing). No world-space aggregation of occurrence masses/CoM; `bom` is
  the workaround (it gives total mass). Assembly should report combined
  mass + CoM (transform part inertia by OccurrenceMatrix, like assembly
  interference already transforms bodies).
- **#6 Mate posing is the roughest workflow** (friction — the assembly-motion
  path). The DOF itself is perfect (drove `set_mate(swivel, rotation)` and the
  sub-assembly rotated exactly about the kingpin axis). But building a clean
  pose from face mates is hard: revolute fixes axial position at connector
  CENTROID COINCIDENCE with no `offset` to slide along the axis (offset is
  slider/cylindrical only) → stacks collapse, wheel intersects plate; `flipped`
  on a vertical-axis revolute rotates azimuthally, can't flip up↔down; fastened
  face-mates centroid-snap with no lateral centering (wheel landed on a fork
  leg); and the `offset` SIGN is inverted (offset −16 → +16 world). Wants: an
  axial-offset DOF on revolute, an auto-center/align option, and a sign fix.

### P3 — ergonomics / messages / docs

- **#7 auto_explode with no view** → raw "key 'view' not found" json
  exception; must add_exploded_view first. MCP description reads like it
  creates one. Auto-create or give a real error.
- **#8 list_elements face `normal` is always [0,0,0]** — faces have
  kind/area/centroid/name but no usable normal; agents must infer orientation
  from centroids to place holes/mates on "the +X face." Populate it.
- **#9 `part=<int>` is stable-id, not slot index** — but list_parts shows
  both `index` and `id` prominently; passing the index silently returns a
  different part's geometry. Accept index too, or error on unknown id.
- **#10 No rename_part RPC** — the default first part is stuck "Part 1"
  (leaks into BOM + add_occurrence). rename_occurrence exists; add rename_part.
- **#11 Raw RPC screenshot(view=…) ignores view** — reorientation is
  client-side in the MCP wrapper (view then screenshot). Raw callers get
  byte-identical shots. Align the raw RPC or note it (the MCP tool is fine).

### Solid (confirmed working well)
Revolve (exact volumes, intuitive axis refs); Sweep (auto-mitered corner
fillets, <0.4% vol error); Hole/Extrude (with literal), multi-point sketches;
**FEM the standout** — full mesh→run→von Mises/FoS/colormap headless, believable
numbers (FoS 1.63, max at the fixed bore rim), load glyphs + deformed shape;
Drawing (3 HLR views + truly-driven dims + title block + PDF/SVG); BOM
(per-part + total mass); exploded view; assembly interference (caught every
posing error); measure/section/checkpoints/history; git-backed round-trip.
Minor cosmetic: set_material updates mass but doesn't recolor the shaded body
in the viewport.

### What was done about it

The execution plan that followed is folded in here; its phase-by-phase
detail (estimates, sign-off shape, per-phase design) lived in a separate
file and is in git history — what survives is the outcome.

> Status: ALL PHASES EXECUTED 2026-07-21. V (project-wide vars, v41, 691991d),
> G1 (loft twist / extrude / hole, eae1a36), A1 (assembly mass_properties + mate
> docs, ca24d15), E1 (ergonomics batch, 2707394). Suites ended 475 headless +
> 68/68 GUI.

Phases, in the order they landed:

- Phase V — Project-wide variables + fail-loud expressions
- Phase G1 — Geometry correctness
- Phase A1 — Assembly usability
- Phase E1 — Ergonomics batch


---

## 2026-08-18 — Marsliden floating dock

Source: the Marsliden floating-dock session (`scratch/build_dock.py`, six
design iterations, ~15 script runs, all scripted against a headless
instance with Xvfb preview renders). Two app bugs were found and one was
fixed in-session (the dockspace/part-tabs overlay). The rest, ordered by
how much of the session they cost:

### P1 — Contact is unverifiable (caused both structural review rounds)

The two worst design defects — the v1 cradle that only *kissed* the frame
at a plane, and the v2 barrel stops left floating in mid-air when the
rails moved — passed every scripted check, **because the checks passed
exactly when nothing touched**. `interference` answers "is intersection
volume > 0" and nothing else, so a script can prove not-clashing but
never prove connected. Both defects were caught by human eyes on renders.

- `interference` should return `distance` (min gap between the pair;
  0 = touching, optionally with contact area). Scripts then assert
  `gap == 0` for every bolted joint and `gap == 5` for designed
  clearances — the whole defect class becomes assertable.
- Bigger hammer: an assembly-level `contact_report` that builds the
  touch graph (bbox prefilter, then exact min-distance) and returns
  disconnected components ("these 8 occurrences touch nothing") plus
  near-misses (0 < gap < 1 mm, usually an arithmetic slip). One call
  gates the "floating member" class; ~1.6k pairs at 57 occurrences is
  cheap after the prefilter.

### P2 — Feature JSON accepts unknown fields silently

`add_feature {type: Box, ..., mode: "add"}` was accepted, `mode` ignored,
and a second body silently created — surfacing three calls later as BOM
material `mixed` with `null` masses and a null total. The RPC layer
validates *param* names strictly ("unknown parameter 'topic'"), but the
feature-JSON path does not validate *field* names.

- Validate feature fields strictly on the `add_feature`/`modify_feature`
  path (the file deserializer stays tolerant for old documents).
- `bom` should carry `warnings: ["body 2 of 'Corner mooring' has no
  material"]` when masses go null — and the GUI table should show it.

### P3 — Scripted placement ergonomics

- Occurrence orientation is quaternion-only. Hand-deriving rotated-box
  positions for the ±90° end posts produced a real error (first attempt
  put them inside the end joist; caught by re-derivation, not by the
  tool). `add_occurrence`/`set_occurrence_transform` should accept
  `rotate: {axis: [0,1,0], angle_deg: 90}` alongside quaternions.
- No body/part mirror (`mirror` is sketch-entities only), so the chiral
  corner-mooring bracket needed a second hand-mirrored part. A Mirror
  body feature would collapse "Corner mooring M" into one part.
- `set_camera` `direction` points **target → camera**; undocumented, and
  the intuitive reading (view direction) aims the camera backwards —
  cost three renders from inside a barrel. One doc sentence, or accept
  an `eye` position param.

### P4 — Introspection gaps

- `describe` lists feature types but not their parameter schemas — Box
  semantics (position = min corner; width/height/depth = X/Y/Z) needed
  an empirical probe part. Attach a params list per feature type.
- `mass_properties` nests the summary under `"total"`; the shape is
  undocumented and was guessed wrong twice. Document result shapes in
  the method docs (or `describe {method: ...}` filtered output — which
  was instinctively tried twice and doesn't exist).

### P5 — Remote/render papercuts

- `render_still` on an assembly tab returns ok:true and renders only the
  HDRI environment (known papercut, now cost a second session) — error
  out or support assemblies.
- No way to scroll a *panel* remotely: `scroll` is viewport zoom and
  `key_press` has no PageDown/Home/End tokens. Verifying the BOM fix
  required abusing the test engine's hover-scrolls-into-view. Add
  `scroll {window, dy}` or the missing key tokens.
- FIXED in-session: docked panels ran underneath the bottom part-tabs
  bar (dockspace host covered the full work area; the bar was an
  overlay), which made the BOM section unreachable — the view cube's
  manual "sit above the bar" offset was the tell. The dockspace now
  reserves the strip (`PartTabsBarHeight()`).

### Round 7-8 addendum (gangway + ladder, first rounds through the loop)

What worked: rotate{axis,angle_deg} (rotated ladder placed first-try),
set_camera eye (zero inside-geometry shots), decision-complete errors
(each of three gate-caught hinge mistakes was one edit from its
message), body-level contact_report (shipped mid-round after Elias found
the floating mount plate; caught pin-float, pin-clash, and leaf-reach in
succession before any render).

The floating-plate post-mortem, in layers: (1) the occurrence-level gate
structurally could not see an intra-part break — no tool, so nothing was
asserted, so nothing was caught; (2) the "interesting" contact (arm-tube
tangency) got hand-verified while the "obvious" one (arm-plate, 35 mm
apart) got skipped; (3) at render distance a 35 mm depth gap is
invisible and confirmation bias filled it in. The durable fix was the
tool, not a resolution to look harder.

The render_sheet betrayal: bend direction was misjudged from a small
tile ("arches" that were dips) while the add_feature DIGEST already
carried the answer (the bends body's bbox sat below the axis). Measure
before render applies to orientation too.

New items:
- Revolve params row: document the sweep direction convention
  (right-hand rule about the axis from the sketch plane) — the 50/50
  should not exist.
- render_sheet {focus: occurrence|body}: auto-frame one thing large;
  the close-up should be automatic, not a camera dance.
- query_geometry: add "torus" (+ radii) to the face classifier — bends
  currently classify "other" and cannot be asserted.
- check_geometry: warn by default on disconnected multi-body parts
  (make the floating-part class opt-out, not opt-in).
- Convention, documented: intra-part overlaps are welds — body-scope
  contact_report counts them as connecting "clash" edges; never assert
  clashes == [] at body scope on welded-look parts.

### Standing gap (planned elsewhere)

Barrels render white: no blue material, and color-per-slug was rejected —
per-part appearance override designed in docs/rendering/appearance-plan.md
(its acceptance test is recoloring these barrels).

### What worked — keep doing

Regenerable build script as the single master (parameters at top, re-run
per iteration); interference spot-checks + BOM + mass + buoyancy printed
every run; headless build instance + separate Xvfb preview instance;
`describe` for API discovery; `ui_items`/`item_click` to verify GUI fixes
from the same script surface humans use.
