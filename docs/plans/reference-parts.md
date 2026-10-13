# Reference parts: whole parts against closed-form values

## The problem

Every feature has unit tests (`tests/test_<feature>.cpp`), and a few of them
check a closed form: Pappus in `test_revolve`, `test_worm` and `test_thread`,
the box and cylinder in `test_cadkernel`. They test **one feature on a body
the test built in C++**. Nothing checks a part as a user or an agent builds it:
through the RPC, sketch first, edges picked off the geometry the previous
feature made, regenerated, exported and read back.

That path is where the defects are. A fillet on the second piece of a split
fails to build, because the two pieces share one `createdBy`
([Not built](../modeling/README.md#not-built)). A circular corner relief on a
60° fold leaves the flat pattern 1.67 mm thick on 1 mm sheet
([known bugs](../known-bugs.md#a-circular-corner-relief-on-a-fold-that-is-not-square-leaves-the-flat-pattern-thick)).
Both were found by hand; neither has a test that says what the right answer
is, so neither would be noticed the day it is fixed.

The checks that exist are also loose where they compare a part to a number at
all: `sheetmetal_smoke` accepts a normal cut within 5e-3 of πr²t. One kernel
and an exact B-Rep give a box with every edge rounded to 1e-16 of the Steiner
formula; a tolerance of 1e-3 is a hundred times wider than a wrong K-factor
(0.40 against 0.41 moves a 115 mm blank by 2e-4).

What is missing is a corpus of whole parts with **independently known**
volume, area, centre of mass and bounding box, built through the same surface
an agent uses, compared tightly, and a way to keep a known wrong answer
visible until it is right.

## The shape of it

### 1. Where the cases live

```
tests/reference_parts/
  geom.py                 the closed forms (stdlib only), each function's
                          docstring stating its formula
  NN-name/case.json       the recipe, the comparison, an optional known failure
  NN-name/expected.py     the derivation: expected() returns the numbers;
                          its docstring is the `method`
tests/reference_parts.py  the runner
(docs/modeling) reference-parts.md   the generated status page
```

A case is a directory so a later reference file (a STEP of a hand-checked
part) has somewhere to go.

### 2. The recipe is Loböle's own RPC, with picks by point

`case.json` holds `steps`: a list of `{method, params}`, the same shape a
`batch` call takes, run in order against a fresh project. Two placeholders are
resolved by the runner before a step is sent:

- `{"$ref": n}`: step *n*'s result `id` (or `{"$ref": n, "key": k}`),
  as in `batch` ([rpc.md](../scripting/rpc.md#batch)).
- `{"$pick": "edge"|"face", "at": [x, y, z]}`: the one element of the
  current part whose geometry passes through that world point, becoming
  `{feature_id, element}` (its body's `created_by` and its stable name).

The recipe is the RPC dialect rather than a kernel-neutral description
because Loböle has one kernel and one consumer: a neutral format needs a
translator, and the translator is a second implementation of every feature's
semantics, which is exactly where a wrong answer would hide. The RPC dialect
is documented in `describe`, refuses an unknown key by name, and is the path
agents take, so a dialect change breaks a case loudly instead of quietly.

**Picks are by point, never by name or index**, because names and indices are
what is under test. A name like `4/g7;M6` is the toponaming's output, and an
index moves when the kernel splits a face differently. A point on an edge is
a fact of the part's geometry that the recipe author can work out from the
drawing. `sheetmetal_smoke`'s `straight_edge` already picks this way, by an
edge's midpoint; that cannot pick a full circle, whose midpoint depends on
where the kernel put its seam.

So `query_geometry` gains **`on_point`** `[x, y, z]` (mm): it keeps only
elements whose geometry passes within `max_distance` (mm; default 1e-3 with
`on_point`) of the point, measured to the edge or face itself
(`BRepExtrema_DistShapeShape` against a vertex), not to its midpoint or
centroid as `near_point` is. The runner calls it with `expected_count: 1`; a
pick that matches none or several fails the case naming the count (an
ambiguous pick is a recipe bug), and a match without an `element` name fails
the case as "cannot be referenced". This is useful beyond the corpus: an
agent that knows where an edge is no longer has to filter a listing.

Steps may call any modeling RPC (`add_feature`, `set_sheet_metal_rules`, the
sketch `fillet`, `add_constraint`, `set_variable`). Project verbs, exports and
imports are refused in a recipe: the runner owns them. Each step's `regen`
must be `ok`; one that is not fails the case naming the step and the feature's
error.

### 3. Expected values: closed forms only, kept with their derivation

The FreeCAD half of SolidCraft's oracle is not independent here: FreeCAD runs
the same OpenCASCADE, so it would reproduce a kernel defect as the expected
value. Every expected value is **analytic**: prisms, cylinders, cones,
spheres, tori, Pappus–Guldin on an (r, z) profile, Green's theorem over a
region of lines and circular arcs, Cavalieri, Steiner's formula for a ball
rolled round a box, a screw motion's Pappus for a thread, sheet-metal bends
as annular sectors with BA = θ(R + K·t). A part with no closed form is not
in the corpus.

`expected.py` is computed at run time, not copied into a JSON file, so the
number and its derivation cannot drift apart. It returns
`{bodies, volume_mm3, area_mm2, center_of_mass, bbox, faces?}`; its module
docstring is the `method` (the formulas, in words, with the dimensions they
use). It may import only `math` and `geom`: the runner parses its imports
before loading it and refuses anything else, so an expected value can never
be measured off the app.

A derivation is code and is wrong sometimes (the benchmark's tasks were wrong
twice when written). `geom.py` checks itself first, every run: Green's
theorem against the shoelace formula on a polygon and against πr² on a circle
split into arcs, the Pappus routine against a cylinder and a torus, the
frustum against a prism at zero taper. A case whose expected values disagree
with the app is investigated from both ends before either is changed.

Inputs the app derives from a table are pinned in the recipe so the expected
value does not depend on the table: a sheet-metal case sets `bend_radius_mm`
and `k_factor` explicitly, a hole case states its diameter. The one exception
is a value whose table is a published standard (a clearance hole's 6.6 mm
for M6 normal, ISO 273), where the standard is the reference.

### 4. What is compared, and how tightly

From the built part: body count (`get_state`), total volume and summed
surface area and the volume-weighted centre of mass (`mass_properties`), the
union of the bodies' measured bounding boxes (`mass_properties`' `bbox_min` /
`bbox_max`, read off the surfaces), and the face count where the geometry
fixes it (`query_geometry` with `element_type: face`, which counts unnamed
faces too).

Defaults are tight, because the kernel is exact on these surfaces:

| quantity | default |
|---|---|
| volume, area | relative 1e-6 |
| centre of mass | 1e-6 × the bbox diagonal |
| bbox | 1e-4 mm per coordinate (OCC widens a box by edge tolerances) |
| bodies, faces | exact |

A case built from approximated surfaces (a twisted extrude, a helical sweep,
a modelled thread, a loft) states a looser `tolerance` and a `note` saying
why; a case whose geometry fixes only some quantities lists them in
`compare` (a twisted prism's area has no closed form; its volume, centroid
and bbox do).

### 5. Every part is also a STEP round trip

After the comparison the runner exports the part with `export_step` to a
scratch path, opens a new project, `import_step`s it with `keep_origin: true`
(without it the import is recentred and every centroid would move), and runs
the same comparison against the same expected values. That catches an export
that drops a body, flips a solid or writes a shape the reader heals into
something else, which is the round trip a user's part takes.

### 6. Known failures are a ratchet

`case.json` may carry `"known_failure": "docs/<page>.md#<anchor>"`, a link to
the entry that describes the defect: a `known-bugs.md` entry, or an area's
"Not built" when the limit is documented there. The `[docs]` convention test
already scans `tests/**.json`, so the link must resolve.

- A case that fails without the mark fails the run.
- A case with the mark that fails is reported, and passes the run.
- **A case with the mark that passes fails the run**, saying to remove the mark
  and delete the entry it points at. A fix therefore cannot land without the
  case going green on the record, which is also the regression test
  `known-bugs.md` asks for.

### 7. The status page is generated, and checked

`tests/reference_parts.py --write-page` writes
`reference-parts.md` beside the modeling README: a fixed introduction (what
a case is, where the numbers come from, the tolerances), then one row per case: case,
what it exercises, the method's first sentence, and the result (`pass`, or
`known failure` linking the entry). It holds no measured numbers, which would
change across machines in the last digits. Under ctest the runner renders the
page in memory and fails if it differs from the committed one, so a case
that changes state changes the page in the same commit.
`docs/modeling/README.md#tests` links it.

### 8. Running it

One headless instance (`tests/smoke_app.py`, scratch `XDG_STATE_HOME`), a
`new_project` per case, cases in order. `add_test(NAME reference_parts ...)`
with the label **`reference`** (`tools/ctest -L reference`); if the measured
run is over 20 s it also carries **`slow`**, which takes it out of
`test-fast.sh` and leaves it in the headless suite. `-k NN` runs one case
while iterating on it, and prints the expected and measured values side by
side with the relative error.

## The starter cases

Each formula is stated in `expected.py`; the shorthand here is a, b, c for a
box's sides, r for a radius, t for a thickness.

**Sketch and extrude**

| case | part | expected from |
|---|---|---|
| 01-box | Box primitive 40×30×20 | V = abc, A = 2(ab+bc+ca), 6 faces |
| 02-cylinder | Cylinder primitive r15 h50 | πr²h, 2πr² + 2πrh, 3 faces |
| 03-sphere | Sphere primitive r12 (a revolved half disc) | 4/3πr³, 4πr² |
| 04-plate-with-hole | one sketch, rectangle 60×40 with a circle r6 inside, extruded 20 | the circle is an inner loop: abc − πr²c |
| 05-obround | two arcs and two lines, tangent and equal-radius constraints, R6 centres 40 apart, extruded 10 | A₂ = 2rL + πr², perimeter 2L + 2πr |
| 06-solved-triangle | base 50, a 60° angle, the far side dimensioned 45 | apex from the law of cosines (the root nearest the drawn apex); centroid = vertex mean |
| 07-rounded-rectangle | rectangle 50×30, four sketch `fillet`s r5, extruded 8 | A₂ = wh − (4−π)r² |
| 08-two-sided-extrude | rectangle extruded 10 up and 25 down | box z −25..10; the centroid catches a swapped side |
| 09-thin-extrude | an open L polyline, `thin` thickness 2, centred | the offset region's area by Green, with the arc join at the outer corner |
| 10-twisted-extrude | square 20, 30 high, twisted 90° | Cavalieri: V = A₂h, centroid on the axis at h/2, bbox ±10√2 (each corner sweeps through an axis); area not compared |

**Revolve**

| case | part | expected from |
|---|---|---|
| 11-torus | circle r4 at R20 revolved 360° | 2π²Rr², 4π²Rr |
| 12-partial-revolve | square r 4..12, z 0..8 revolved 270° | annular sector: θ/2 (R²−r²)h; centroid from the sector's first moments (R³−r³)/3 (sin θ, 1−cos θ) |
| 13-vase | profile of lines and one arc revolved 360° | Pappus/Green: V = 2π∫x dA; A = Σ 2π∫x ds per edge (disc, frustum, torus zone) |

**Fillet and chamfer**

| case | part | expected from |
|---|---|---|
| 14-box-round-all | box 40×30×20, all 12 edges r3 | Steiner on the inner box a′b′c′: V = a′b′c′ + 2r(a′b′+b′c′+c′a′) + πr²(a′+b′+c′) + 4/3πr³; 26 faces |
| 15-box-chamfer-top | box, the 4 top edges chamfered d=2 | V = ab(c−d) + ∫₀ᵈ(a−2u)(b−2u)du; bevels are trapezoids of slant d√2 |
| 16-concave-fillet | L-bracket, its inner edge filleted r4 | adds the spandrel r²(1−π/4) × width; area trades 2r flat for πr/2 round |
| 17-cylinder-rim-fillet | cylinder R20 h30, top rim r3 | Pappus on the removed spandrel, centroid inset r(10−3π)/(3(4−π)); the round's area from its arc centroid R−r+2r/π |
| 18-hole-rim-chamfer | plate with a through hole, top rim chamfered 1 | removed cone frustum |

**Shell and draft**

| case | part | expected from |
|---|---|---|
| 19-shell-box | box 50×40×30, top open, t2 | box minus the cavity 46×36×28 |
| 20-shell-L | L prism 60×40, legs 8, 20 high, top open, t2 | the cavity is the L eroded by t: sharp at convex corners, an arc of radius t about the reflex corner, so the cavity's section is 352 + 4 − π |
| 21-shell-cup | cylinder r20 h30, top open, t2 | πR²h − π(R−t)²(h−t) |
| 22-draft-box | box 40×40×30, the four sides drafted 5° about the bottom | square frustum h/3 (A₀ + A₁ + √(A₀A₁)), sides of slant h/cos 5° |

**Holes and threads**

| case | part | expected from |
|---|---|---|
| 23-hole-kinds | plate with a counterbore, a countersink and a blind hole with a 118° point | cylinders, a 90° cone frustum, a cone of height r/tan 59° (depth as [holes-and-threads.md](../modeling/holes-and-threads.md#hole) defines it) |
| 24-clearance-pattern | four `fit: {thread: M6, kind: normal}` holes, linear pattern | ISO 273's 6.6 mm; the centroid proves the spacing |
| 25-modelled-thread | cylinder Ø8, modelled M8 over its whole length | removed = A·2πr̄·L/P for the ISO 68-1 basic groove (a trapezoid 7P/8 wide at the major diameter, P/4 at the minor, 5H/8 deep, H = √3/2·P); looser tolerance, a swept B-spline |

**Patterns**

| case | part | expected from |
|---|---|---|
| 26-circular-flange | annulus R50/r15 h10, six holes r5 on R40 by a circular pattern | 6-fold symmetry keeps the centroid on the axis; V, A by subtraction |
| 27-mirror-bosses | a block and one boss mirrored across YZ | abc + 2πr²h |
| 28-fillet-on-instance | three bosses by a linear pattern, the third's top rim filleted | Pappus as 17, on one boss; the pick lands on a pattern instance's edge (`#2`) |

**Sweep and loft**

| case | part | expected from |
|---|---|---|
| 29-bent-rod | circle r3 swept along 30 up, a quarter arc R20, 30 across | two cylinders + a quarter torus: πr²(60 + πR/2), area likewise, plus two end discs |
| 30-coil | circle r1 swept along a 3-turn helix R10 pitch 5 | a tube: V = πr²L, A = 2πrL + 2πr², L = 3√((2πR)² + p²); looser tolerance |
| 31-loft-frustum | squares 40 at z0 and 20 at z30 | frustum as 22 |

**Sheet metal** (rules pinned: aluminium 1.5 mm, `bend_radius_mm`, `k_factor`)

| case | part | expected from |
|---|---|---|
| 32-panel-normal-cut | base flange 100×60, a circle r3 normal-cut | (6000 − 9π)t exactly, where `sheetmetal_smoke` allows 5e-3 |
| 33-edge-flange | base flange, one 90° flange length 15 on a 60 edge, then a flat pattern | folded: panel shortened by R+t, a quarter annulus π/4((R+t)² − R²)w, the wall; flat: (L − (R+t) + π/2(R + Kt) + 15)·w·t, its z span t |
| 34-closed-hem | base flange, a closed hem | a 180° annular sector of inside radius gap/2 (the gap a tenth of t), the return |
| 35-relief-on-60-fold | two 60° flanges meeting at a corner, circular relief, flat pattern | the flat's z span is t (`compare: [bodies, bbox]`) |

**Booleans and split**

| case | part | expected from |
|---|---|---|
| 36-union | box and an overlapping cylinder, `Boolean` union | box + the half cylinder outside + the cylinder above |
| 37-subtract-sphere | box minus a sphere centred on its top face | abc − 2/3πr³, the hemisphere's centroid 3r/8 down |
| 38-intersect | cube [−20, 20]³ ∩ sphere R25 | sphere minus six disjoint caps of height 5: 4/3πR³ − 6·πh²(3R−h)/3 |
| 39-split-then-fillet | box split at mid-length, an edge of the second piece filleted r2 | two bodies; the total less (4−π)r²/4 × the edge length |

**Expected to start as known failures:**

- **35-relief-on-60-fold**: the flat is 1.67 t thick;
  `known-bugs.md#a-circular-corner-relief-on-a-fold-that-is-not-square-leaves-the-flat-pattern-thick`.
- **39-split-then-fillet**: both pieces have the same `created_by`, so the pick
  resolves to a reference the fillet binds to the first piece, and the
  feature fails to build; `modeling/README.md#not-built`, until a known-bugs
  entry is written for it (it is a wrong answer, not a missing feature).

**Most likely to need a tolerance or a fix:** 32 (whatever the 5e-3 in
`sheetmetal_smoke` was covering), 10, 25 and 30 (approximated surfaces, held
to a stated looser tolerance), and 28 if a pattern instance's edge names stop
resolving. Every other case is expected to pass at the defaults.

## Files

- `src/remote/RemoteInspection.cpp`: `query_geometry`'s `on_point`, and its
  `MethodDoc` row (the unit named, for `rpc_units_gate`).
- `tests/query_smoke.py`: `on_point` picks a full circle by any point on it,
  refuses a point off every element, and returns the name `list_elements`
  gives.
- `tests/reference_parts.py` (new): the runner, the import check on
  `expected.py`, the STEP round trip, the ratchet, `--write-page`, `-k`.
- `tests/reference_parts/geom.py` and `tests/reference_parts/NN-*/` (new):
  the closed forms and the 39 cases.
- `tests/CMakeLists.txt`: the `reference_parts` test and its labels.
- `reference-parts.md` in docs/modeling (new, generated).
- `docs/modeling/README.md#tests`: links the page; a feature with a closed
  form lands with a reference case.
- `docs/inspect/README.md#finding-and-naming-geometry`: `on_point`.
- `docs/known-bugs.md`: an entry for 39 if it is a bug rather than a limit.

## Verification

- `tools/ctest -L reference` passes with exactly the known failures above
  reported, and the page it renders matches the committed one.
- `geom.py`'s self-checks run first and fail the run on their own.
- **The comparison discriminates**: by hand, with a case's recipe edited to a
  plausible wrong part (K 0.40 for 0.41 in 33, a chamfer of 2.1 for 2 in 15,
  a fillet on the wrong rim in 28, a sphere 0.1 mm off centre in 37), each
  fails, naming the quantity; and with `expected.py` importing
  `designer_remote`, the runner refuses to load it.
- **The ratchet**: removing the mark from 35 fails the run; marking a passing
  case fails the run and says to remove the mark.
- **The round trip** fails when the export is given one body of a two-body
  part (`export_step {body: 0}` substituted in a scratch copy of the runner),
  showing the second comparison is live.
- A `known_failure` link with a misspelt anchor fails the `[docs]`
  convention test.
- The headless suite and `tools/test-fast.sh` pass; `query_smoke` and
  `rpc_units_gate` pass with the new parameter.
