# Kernel case corpus

Hold the modeller to a large body of small cases whose answers were worked
out by hand, most of them at a coincidence, a tangency or a geometric limit,
and to the invariants a correct kernel keeps whatever the case.

## The problem

Loböle's independent answers are few and comfortable. The reference parts
(`tests/reference_parts/`, docs/modeling/reference-parts.md) are whole parts
with closed-form values, but each is a part its author expected to build.
The action matrix (`tools/action_matrix.py`) tries every action everywhere
at sizes a tenth of what it acts on, far from any limit. The random designs
(`tools/fuzz_design.py`) snap to a 2.5 mm grid, which makes coincidences,
and check them with three boolean identities, a replay, undo and a reopen.
None of them asks what OpenCASCADE is weakest at:

- **The limit itself.** A round as wide as its face, as tall as its wall or
  as large as the cylinder it ends; a cut tangent to a face along a line; a
  tool 1e-6 mm into or out of the stock; holes touching at one point; a
  shell wall equal to an inner round.
- **Dependence on where and in what order.** OpenCASCADE's tolerances are
  absolute (1e-7 confusion, a fuzzy value in mm), so a design that builds at
  the origin can lose a face 1000 mm away; a blend can depend on the order
  its edges were given; a body minus itself can leave a sliver.
- **One value in a range.** A feature that builds at 2.00 and 2.02 and not
  at 2.01. The grid never lands there.
- **Files other programs wrote.** `tests/fixtures/import/` is our own box
  under foreign headers (docs/interchange/README.md#tests). No fixture
  carries an exporter's quirk, and an imported body is never checked: a
  body that came in open or invalid is handed to the next feature as it is.
- **Tolerance.** Nothing reads a result's tolerances. A fuzzy retry
  (docs/modeling/README.md#a-refused-boolean-gets-one-fuzzy-retry) or a
  sewing can leave a vertex whose tolerance ball swallows a neighbouring
  edge, and every later feature inherits it.
- **Scaling.** `boolean/plate-holes` is the one modelling row in
  `tools/bench_workloads.py`. A union of many separate bodies, a pattern of
  many copies and a blend of many edges are not timed.

SolidCraft's kernel rig holds such cases as data: a command script
(`recipe.json`) and its right answer (`expected.json`: `source: analytic`
with the derivation in `method`, or `source: invariants`), by area, with a
graded ladder on one 60×40×20 stock. Its rule is ours: the answer never
comes from a kernel. About half of its recipes use only commands Loböle
has.

## The shape of the fix

### 1. The cases are copied unchanged, with their licence

`tests/kernel_cases/solidcraft/<area>/<id>/` holds each case's
`recipe.json`, `expected.json` and, for an import case, its `input.step` or
`input.igs`, byte for byte. `tools/kernel_cases.py vendor --from CHECKOUT`
copies them and writes `tests/kernel_cases/SOURCE` (the repository, the
commit, the areas taken); nobody edits a copied file, and the runner
refuses a tree whose files differ from the hashes `SOURCE` lists. Taken:
`ladder`, `booleans`, `fillets`, `shell`, `sweeps`, `solids`, `threads`,
`import`, `tessellation` and `crashes`. Not taken: the areas that test
SolidCraft's own tools (`assemblies`, `sheet-metal`, `surfaces`, `plastic`,
`frames`, `construction`, `history`, `inspect`, `continuity`).

SolidCraft is MIT OR Apache-2.0; the cases are taken under MIT.
`tests/kernel_cases/LICENSES.txt` says what was copied, from where and at
which commit, and carries the MIT text with its copyright line, as
`tests/fixtures/step/LICENSES.txt` does for the BenchCAD files. A case
solidcore added after SolidCraft's folder froze has no recipe: its
`expected` block is copied to `tests/kernel_cases/solidcore/<area>/<id>/`
beside a recipe written here, under Apache-2.0, with solidcore's `NOTICE`
reproduced in `LICENSES.txt` and the statement that the recipe is ours
(`LICENSES/Apache-2.0.txt` already holds the text). `THIRD_PARTY.md` gains
a "Test data" section naming both and the BenchCAD files, since the source
tarball carries them. Every file written here keeps the repository's SPDX
header; copied files keep theirs and gain none.

### 2. A translator, at run time

`tools/kernel_cases.py` turns a recipe (with its `base` chain) into the
step dialect `tests/reference_parts.py` already runs: `{method, params}`
with `{"$ref": n}` and `{"$pick": "edge"|"face", "at": [x, y, z]}`. Nothing
translated is committed; the recipe is the source.

| recipe command | becomes |
|---|---|
| `solid.box`, `solid.cylinder` | a Sketch on a plane given as `{origin, normal, uAxis}` at the corner or base, and an Extrude |
| `solid.sphere`, `solid.cone`, `solid.torus` | a section Sketch and a Revolve about a `custom` axis |
| `sketch.create` with lines, rectangles, circles, arcs, slots, rounded rectangles | one Sketch; SolidCraft's named planes are written out as frames, never left to our `Top`/`Front`/`Right` |
| `solid.extrude` | Extrude: `distance`, `direction`, `taper` as `taper_deg`, `through_all`, a `profiles` pick as `regions` |
| `operation` `new` / `join` / `cut` | `mode` `new_body` / `add` / `cut`; with `targets` in a part of several bodies, a new body and a Boolean against the named one |
| `solid.combine` | a Boolean per tool, chained on the result |
| `solid.fillet`, `solid.chamfer` | Fillet, Chamfer, each edge a `$pick` at its point |
| `solid.shell`, `solid.offset_face`, `face.delete`, `solid.draft` | Shell, OffsetFace, DeleteFace, Draft, faces by `$pick` |
| `solid.hole` | a point Sketch on the face at `position` and a Hole |
| `solid.revolve`, `solid.loft`, `solid.sweep`, `solid.thread` | Revolve, Loft, Sweep, Thread |
| `solid.pattern.*`, `solid.mirror`, `solid.split_body` | Pattern (a total `angle` divided into the step between copies), Split |
| `file.insert_step`, `file.export` | `import_step` with `keep_origin`, `export_step` to a scratch directory (`$CASE`, `$OUT`) |

A command or a parameter with no counterpart is not approximated: the case
is `untranslated`, naming it. That covers `type: g2`, `corner`, `setbacks`,
a second radius and `fit` on a fillet, a two-distance chamfer, an outward
shell, `keep_tools`, a twist or scale on a sweep, a cut with several
targets, and every command outside the table. A `$pick` that finds no
element or more than one (`query_geometry` `on_point`, `expected_count: 1`)
makes the case `untranslated: ambiguous pick`, never the nearest one. A
recipe names bodies; the translator maps each name to the `created_by` of
the feature that made it.

### 3. What a case is held to

- `body_count`, `volume_mm3`, and `area_mm2`, `center_of_mass` and `bbox`
  where given, from `mass_properties`; every body valid and closed by
  `check_geometry`. A number is held to half a unit in the last digit the
  case states, never tighter than 1e-9 relative, or to the case's own
  `tolerance`; a `[min, max]` is inclusive. SolidCraft's blanket 1e-3 is
  not used: the near-tangent ladder's answers differ from the stock's by
  2e-7 of it.
- `faces`, `edges`, `vertices` where given, as their own class
  (`topology`), so a right volume on faces a union left unmerged is told
  apart from a wrong volume.
- `outcome: refused`: the call is refused with a message that states a
  condition (`fuzz_design.measured`) and the document is unchanged. The
  wording is ours, so `refusal` is not matched as text; a size it states is
  compared with the size we report once
  [fillet-size-that-fits.md](fillet-size-that-fits.md) lands.
  `solid_or_refused` takes either.
- Never a crash, a hang (a budget per case, the instance killed by pid),
  an `internal` error or a non-finite number.

### 4. A failure stays visible, and fails when it stops

`tests/kernel_cases/status.json` holds one sorted line per case: `pass`,
`untranslated: <what>`, or `known: <class>`. `tools/kernel_cases.py bless`
writes all of it except the `known` rows, which are edited by hand and each
name their entry. A run fails on:

- a `pass` that no longer passes;
- a `known` case that passes ("remove the row and its entry"), or fails
  with another class;
- an `untranslated` case that now translates, until it is blessed or given
  a `known` row;
- a failing case with no row, and a vendored case missing from the file.

`bless` refuses while any failing case lacks a `known` row. A page,
`kernel-cases.md` beside reference-parts.md in the modeling docs, is
generated from the same run (`--write-page`, checked by the test as that
page is): per area, how many pass, are known and are untranslated, each
untranslated reason with its count, and every `known` row with its class
and entry. No count is written by hand anywhere.

### 5. What a found failure becomes

The corpus will find more defects than one round fixes. Each becomes, in
the commit that adds its rows:

- **a wrong answer** (class `volume`, `topology`, `invalid`, `accepted` for
  a case that must be refused, `crash`, `hang`, `internal`): one
  [known-bugs](../known-bugs.md) entry per defect, not per case, naming its
  smallest case, with every case that shows it pointing at that entry;
- **a clean refusal of a case that has an answer** (class `refused`): not a
  bug but something not built. One line per kind (a round run past its
  wall, a shell round a rib thinner than two walls) under "Not built" in
  docs/modeling/README.md, with its cases pointing there.

The case stays in the corpus, failing, under its row. Fixing the defect
removes the entry and the rows in one commit, and the ratchet says so if
either is forgotten.

### 6. Import cases, and imported bodies are checked

The `import` area's files reproduce exporters' quirks (B-spline edges over
1e-7 knot spans, a vertex off its edge's end, a cylinder written as a cone
of 8e-12 rad, a void whose shell is oriented `.F.`, IGES trims written end
to start or with gaps) and run through section 3 like any case.

Every body an Import makes is then checked where it is made
(`CADKernel::LoadSTEPStructured`, `LoadIGESStructured`, `ModelFromXcaf`):
`Measure::InvalidReasons`, free edges, a positive volume. A body that fails
still comes in, since the user needs the geometry, and the Import's warning
names it and why ("body 'Housing' came in open: 3 free edges"), as a
skipped surface is named today; `import_step` already answers `warning`. A
body too large to check inside the import's budget is named as not
checked.

`tools/step-bakeoff.py` is an occasional audit like `bench.py`: it fetches
pinned public STEP corpora into `~/.cache/lobole/` (never the repository;
some are not ours to redistribute), imports each file in a headless
instance, and reports opened, valid, closed, volume against a reference
keyed by the file's SHA-256, and seconds. It is how an OpenCASCADE upgrade
or reader patch is measured.

### 7. Metamorphic checks in the random designs

New end checks in `fuzz_design.py`, each a finding class of its own. They
draw from a second `random.Random(seed)`, so no seed builds another design
and `GENERATOR_VERSION` stays.

- **`moved`.** The design replayed with every world literal (plane frames,
  axes, `$face`/`$edge`/`$body` points and normals) carried through a
  translation of (1000, −700, 300) mm and a 37° turn about (1, 2, 3). A
  step naming a world plane takes a DatumPlane instead. Every step builds
  or is refused as it was; volumes agree to 1e-9 relative, solid and face
  counts exactly.
- **`mirror`.** A mirror Pattern of the final body: the same volume, and
  the same count of faces of each kind.
- **`order`.** Each Fillet or Chamfer of two or more edges rebuilt with its
  edges reversed: the same outcome, volume and element names.
- **`self`.** The design replayed twice into one part under two id ranges,
  then A − A′ refused as empty, A ∪ A′ and A ∩ A′ of A's volume in one
  solid; and A ∪ A″ with A″ moved 1e-6 mm, between V(A) and V(A) plus its
  area times 1e-6.
- **`split`.** A Split at the mid-plane of the largest extent: the pieces'
  volumes sum to the body's to 1e-9 relative, each valid. Joining them back
  waits for a Boolean that can name a split's pieces.
- **`step`.** `export_step` and an Import with `keep_origin`: volume, solid
  and face counts and validity equal. Then the design's last Fillet or
  Chamfer, picked by position, on the body as built and on the re-import of
  the body before it: both build to the same volume, or both are refused.
- **`scan`.** The base's height, and the first later step's size, stepped
  over 50 values 0.01 mm apart. The base alone matches its closed form at
  each (a box, a prism on the profile's area, an annular sector). With the
  step, a value is not refused between two that build, and the volume has
  no jump.

`fuzz.py run --metamorphic` turns them on; `fuzz_seeds_smoke` runs them on
its seeds. A seed they fail leaves the list for `tests/fuzz/open/` with its
entry, as the fuzzer's rule already says. The self-test plants one failure
per class.

### 8. A tolerance watch and a deep check

`check_geometry` gains, per body, `max_tolerance_mm`
(`ShapeAnalysis_ShapeTolerance` over vertices, edges and faces) and a
`loose` list: each vertex or edge whose tolerance exceeds 100 × confusion
and reaches a face or edge it does not bound (`BRepExtrema` from it to
the non-incident ones, run only for those few). A `loose` entry is reported
with its reason ("a tolerance of X mm at (x, y, z) reaches face 'N', which
it does not bound") and does not yet change the body's validity or a
`regen`: the corpus, the fuzzer and the action matrix record what it flags
on real parts first. It becomes a validity failure, and tolerance growth
past a factor a `regen` warning, only with thresholds set from that
record, as a separate change.

`check_geometry {deep: true}` adds `BOPAlgo_ArgumentAnalyzer`'s
self-interference test on a copy, non-destructive, under a budget, and
answers `deep: {checked, reasons}`; over budget it says `checked: false`
and why. The corpus, the fuzzer's end checks and the bake-off ask for it;
regeneration does not.

### 9. Conditions on a minimised case

`query_geometry` answers, for an edge, `dihedral_deg` and `tolerance_mm`,
and for a face `min_curvature_radius_mm` (the measures
[fillet-size-that-fits.md](fillet-size-that-fits.md) adds to core).
`fuzz.py one --minimise` asks them of the failing step's picks and writes
them into the case header as `conditions`, beside the step's own sizes, so
a known-bugs entry starts from "radius 5 on an edge whose face curves at
4.8" instead of a seed.

### 10. Performance cases

Rows in `tools/bench_workloads.py`, each at n and 2n so `compare` shows the
growth, each asserting its closed-form volume and body count:
`boolean/pegs` (separate pegs joined to a plate in one feature),
`pattern/fins` (a circular Pattern of an add), `pattern/pins` (a
rectangular Pattern of a body), `boolean/ring` (overlapping cylinder
segments joined round a ring), `boolean/grooves` (grooves cut in a bore
that carries an unrelated fillet) and `blend/outline` (a Chamfer of every
edge of a polyline outline). A row that more than triples when n doubles
gets a known-bugs entry under Performance.

## Stages

Each merges on its own with the suites green and its failures rowed.

- **A. The corpus runner** on the ladder's tiers: vendoring and licence
  files, the translator's primitives, sketches, extrudes, booleans, blends
  and shells, the checks, the ratchet, the generated page.
- **B. The areas**: `booleans`, `fillets`, `shell`, then `sweeps`,
  `solids`, `threads`, `tessellation`, `crashes`; revolve, loft, sweep,
  thread, pattern, split and hole in the translator; recipes for the
  solidcore-only cases.
- **C. Import**: the `import` area, the check on imported bodies, the
  bake-off.
- **D. Metamorphic checks**: `moved`, `order`, `self`, `mirror` and
  `split`; then `step` and `scan`.
- **E. The tolerance watch, the deep check, the conditions.**
- **F. The performance rows.**

## Files

- New `tools/kernel_cases.py` (`vendor`, `run`, `bless`, `replay`,
  `self-test`), `tests/kernel_cases/` (`solidcraft/`, `solidcore/`,
  `SOURCE`, `LICENSES.txt`, `status.json`), `tests/kernel_cases_smoke.py`,
  `tests/CMakeLists.txt`: `kernel_cases_ladder` in the headless suite,
  the whole corpus sharded under the ctest configuration `kernel_cases`,
  a row of its own in CLAUDE.md's table like the big generators: run
  before a release and after a change to the kernel layer, not by
  `tools/merge-branch.sh`. The ladder is sized to add seconds, not
  minutes, to the headless suite.
- `tests/reference_parts.py` (its `resolve`, `build` and `measure` shared,
  not copied), `THIRD_PARTY.md`.
- `tools/fuzz_design.py`, `tools/fuzz.py`, `tests/fuzz_seeds_smoke.py`.
- `src/cad/CADKernel.{h,cpp}` and the `Import` feature (the check and its
  warning); new `tools/step-bakeoff.py`.
- `src/core/Measure.{h,cpp}`, `src/remote/RemoteInspection.cpp`
  (`check_geometry`, `query_geometry` and their `MethodDoc` rows; the
  regenerated `rpc.py` and `rpc-surface.json`), `src/remote/RemoteHelpers.h`
  (the `regen` warning).
- `tools/bench_workloads.py`.
- Docs when each stage lands: a "Kernel cases" section in
  docs/modeling/README.md beside "Random designs" (which gains the new
  classes), the generated `kernel-cases.md` beside it,
  docs/interchange/README.md (the import check, the bake-off, Tests),
  docs/inspect/README.md#geometry-checks, docs/performance/README.md (the
  workloads), docs/known-bugs.md, and the test table in CLAUDE.md.

## Verified by

- `kernel_cases.py self-test`: a planted recipe per table row translates to
  the calls written beside it; an unknown command, an unknown parameter and
  an ambiguous pick each come out `untranslated` naming it; planted answers
  (a pass, a volume off in the eighth digit, a refusal that changed the
  part, a `known` case that passes, an edited vendored file) are each
  judged.
- The ladder's primitives tier passes whole, with `bbox` and
  `center_of_mass` where the cases give them: a plane frame the translator
  gets wrong moves both.
- A negative control per check: the run with one expected volume changed
  in a scratch copy fails on that case alone.
- The page test: the generated `kernel-cases.md` equals what the run
  renders.
- Import: each quirk file meets its `expected.json` or carries a row; a
  STEP with one face removed by hand comes in with the warning naming the
  body and its free edges, through `import_step` and through the GUI's
  import.
- `fuzz_design.py`'s self-test judges one planted failure per new class;
  `fuzz.py run --metamorphic --count 200` ends with every finding rowed.
- `check_geometry` on a body built with a vertex tolerance set past a
  neighbouring edge reports it `loose`; a clean box does not. Two boxes
  crossing inside one shell fail `deep` and pass the plain check.
- `tools/bench.py run` on the new rows, twice, within its own noise.
- The headless suite, the action matrix (its baseline unmoved by the new
  warning) and `tools/ci-local.sh` per stage.
