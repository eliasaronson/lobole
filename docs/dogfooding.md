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
