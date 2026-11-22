# Kernel case corpus: what is left

The corpus itself is built (docs/modeling/README.md#kernel-cases): the
runner, the translator, the checks, the ratchet, every area's cases
including the imports and solidcore's later ones, and the check on every
imported body (docs/interchange/README.md#an-imported-body-is-checked).
What it does not yet do is ask the random designs the same questions, read
a result's tolerances, or time the operations that scale.

## The problem

The random designs (`tools/fuzz_design.py`) check three boolean identities,
a replay, undo and a reopen; none of them moves a design, mirrors it,
reverses a blend's edges or scans a size. Nothing reads a result's
tolerances, so a fuzzy retry or a sewing that leaves a vertex whose
tolerance ball swallows a neighbouring edge goes unseen. And a union of
many separate bodies, a pattern of many copies and a blend of many edges
are not timed.

## The shape of the fix

### 1. Metamorphic checks in the random designs

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

### 2. A tolerance watch and a deep check

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

### 3. Conditions on a minimised case

`query_geometry` answers, for an edge, `dihedral_deg` and `tolerance_mm`,
and for a face `min_curvature_radius_mm` (the measures
[the size that fits](../modeling/README.md#the-size-that-fits) added to
core).
`fuzz.py one --minimise` asks them of the failing step's picks and writes
them into the case header as `conditions`, beside the step's own sizes, so
a known-bugs entry starts from "radius 5 on an edge whose face curves at
4.8" instead of a seed.

### 4. Performance cases

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

- **C (the rest).** A pinned manifest for `tools/step-bakeoff.py`
  (docs/interchange/README.md#the-step-bake-off): which public STEP
  corpora, their SHA-256 and reference volumes.
- **D. Metamorphic checks**: `moved`, `order`, `self`, `mirror` and
  `split`; then `step` and `scan`.
- **E. The tolerance watch, the deep check, the conditions.**
- **F. The performance rows.**

## Files

- `tools/fuzz_design.py`, `tools/fuzz.py`, `tests/fuzz_seeds_smoke.py`.
- `src/core/Measure.{h,cpp}`, `src/remote/RemoteInspection.cpp`
  (`check_geometry`, `query_geometry` and their `MethodDoc` rows; the
  regenerated `rpc.py` and `rpc-surface.json`), `src/remote/RemoteHelpers.h`
  (the `regen` warning).
- `tools/bench_workloads.py`.
- Docs when each stage lands: "Random designs" in docs/modeling/README.md
  (the new classes), docs/inspect/README.md#geometry-checks,
  docs/performance/README.md (the workloads), docs/known-bugs.md.

## Verified by

- `fuzz_design.py`'s self-test judges one planted failure per new class;
  `fuzz.py run --metamorphic --count 200` ends with every finding rowed.
- `check_geometry` on a body built with a vertex tolerance set past a
  neighbouring edge reports it `loose`; a clean box does not. Two boxes
  crossing inside one shell fail `deep` and pass the plain check.
- `tools/bench.py run` on the new rows, twice, within its own noise.
- The headless suite and the action matrix (its baseline unmoved by the
  new warning) per stage.
