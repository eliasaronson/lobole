# One printability check

## The problem

Four print-in-place generators and the coupon each check printability in
their own Python, and each checks a different subset:

| check | planetary | hook | jar | counter | coupon |
|---|---|---|---|---|---|
| lowest point on the bed | — | binary-STL scan, \|z\| ≤ 1e-3 | — | — | — |
| separate shells | `export_stl.shells == bodies` | `== 1` | `== 1` | `== 2·digits` | `== 1 + discs` |
| chords ≤ a fifth of the gap a side | yes | — | — | yes | — |
| overhang past 45° | — | — | STL facet scan, +3° slack, 1 mm² allowed | — | — |
| clearance vs the gap a side | `measure` on chosen pairs, −0.01 mm | — | — | `measure` on chosen pairs, −0.01 mm | — |

The jar's overhang scan reads facets, so a designed 45° flank reads 47-50°
and needs 3° of slack and an area allowance to pass; anything that slack
hides is invisible. Nothing in the app answers "will this print", so a part
built by hand or by a new generator gets none of it.

## The shape of the fix

### Core: `src/core/PrintCheck.{h,cpp}` (namespace `printcheck`)

`Check(const std::vector<const Body*>& bodies, const Options&) -> Report`,
no document, no I/O, so a unit test builds bodies and asks. `Options` holds
the print direction (unit, default +Z), the overhang limit, the area
allowance, the gap a side required and the export's chord deviation; the
caller derives them from the part's print process. Each item of the report
is `{measured, limit, status: pass | fail | skipped, note}` plus its
detail:

- **bed** — the lowest point of the bodies along the direction, exact on the
  B-Rep (each body rotated so the direction is +Z, then
  `BRepBndLib::AddOptimal` without triangulation). Pass within 1 µm of 0. Also
  the area on the bed (downward planar faces at that height).
- **shells** — how many separate pieces the print comes off the bed as,
  against one per body: the bodies' solids, joined wherever two bodies come
  within 1 µm of each other (the clearance item's distances, so no boolean
  is run). Two bodies that touch or overlap are one piece and fail; a body
  in two solids fails the other way. With watertightness
  (`Measure::AnalyzeWatertight`).
- **overhang** — downward faces steeper than the limit, the angle measured
  from vertical (0 a wall, 90 a ceiling). **Exact normals**: each face is
  tessellated on a copy, and the surface normal is evaluated on the B-Rep at
  every node's and every triangle centroid's UV, never taken from a facet.
  The worst angle is then refined by a hill-climb on the face's UV domain
  from its worst sample, so a 45° cone reads 45.000 and a plane is exact.
  The area past the limit is the tessellation's triangles classified by the
  exact normal at their centroid: its error is bounded by the triangles that
  straddle the limit, and the report says so. Triangles within 1 µm of the
  bed are on the bed. Horizontal ceilings are not here, they are bridges.
  Reports area past, worst angle, the body, face and point where it is.
- **bridges** — downward planar faces within 1° of horizontal, off the bed.
  The span of each is twice the largest distance from a point of the face to
  its boundary (the inscribed diameter, sampled on its tessellation): the
  width a bridge crosses between supported edges. The worst span against the
  process's bridge limit.
- **clearance** — the minimum distance between every pair of bodies
  (`BRepExtrema`, the same number `measure` gives on two bodies), pruned by
  oriented boxes further apart than the required gap; an overlapping pair
  (one inside the other, classified exactly) reads 0. Against the gap a side
  (`printfit::SmallestGapSideMm` of the process, or a stated one) less a
  tenth of itself: a gap modelled within 10% of the profile's prints the
  same, and arc-approximated involutes come out a few µm short. Lists the
  closest pairs.
- **chords** — the chord deviation `export_stl` meshes this print at
  (`printfit::StlDeflectionMm`, or a stated one) against a fifth of the gap
  a side, the rule the planetary and the counter carried.

An unprinted part skips clearance and chords with a note saying why.

The process limits are data next to the allowance tables, each with a
source, in `PrintAllowance.{h,cpp}`: `printfit::MaxOverhangDeg` (45° for
FDM) and `printfit::MaxBridgeMm` (FDM). No format change: they are the
process's, not the profile's.

### RPC: `check_print` in `RemoteInspection.cpp`

A new method, not an extension of `export_stl`: the check is a read that
must not write a file, and `export_stl` is NotInBatch because it does.
`{part?, bodies?, direction?, max_overhang_deg?, overhang_allowance_mm2?,
max_bridge_mm?, min_gap_mm?, stl_deflection_mm?}` → `{ok, process,
direction, bodies[], bed{…}, shells{…}, overhang{…}, bridges{…},
clearance{…}, chords{…}, receipt}`. Every parameter is read and checked
before anything runs (a zero or non-finite direction, a negative or
out-of-range limit, an unknown body are refused naming what is accepted).
A typed MethodDoc row with the returns column; units in every name.

### GUI: the Print process section

A *Check print* button in the Print process section
(`ui::DrawPrintProcessSection`) runs the same core call on the part's
bodies and lists the six items, measured against limit, failures amber.
The overhang and bridge rows have a *Show* button that selects the worst
face, so the viewport points at it. The report keeps the document revision
it was taken at and says when the part has changed since. No overhang
shading: that is renderer work this does not need.

### The generators

`generators/_lib.py` gains `check_print(d, need=…)` that calls the RPC and
raises on the items the caller requires, naming the measured value and the
limit. planetary, hook, jar, counter and print_coupon delete their own
`stl_bounds`, `stl_overhangs`, `clearances`/`gate`, shell and deflection
tests, call it, and put its report in their receipts. Each gates on what it
gated on before (and the hook and jar on overhang as well, now exact); a
generator's stdout markers stay. Geometry does not change. The counter's
half-gap negative control runs through the same check.

## Files

- `src/core/PrintCheck.{h,cpp}` (new), `src/core/PrintAllowance.{h,cpp}`,
  `CMakeLists.txt`
- `src/remote/RemoteInspection.cpp`, `tools/designer_rpc.py` (regenerated)
- `src/ui/TolerancePanel.cpp`, `src/ui/ToleranceActions.h`,
  `src/ui/AppState.h`, `src/app/guitests/GuiTestsModeling.cpp`
- `tests/test_print_check.cpp` (new), `tests/CMakeLists.txt`,
  `tests/print_check_smoke.py` (new)
- `generators/_lib.py`, `generators/{planetary,hook,jar,counter}/build.py`,
  `generators/print_coupon.py`, their READMEs
- `docs/tolerances/README.md`, `docs/inspect/README.md`,
  `docs/scripting/rpc.md`

## Verification

- Unit (`[print][check]`): a box on the bed passes and one lifted fails; a
  cone frustum at 45° reads 45.000 and passes, one at 60° fails with its
  area; a plate under a bridge reports its span; two boxes 0.5 apart pass a
  0.25 gap and fail when 0.1 apart; two touching boxes count one shell and
  fail; a deflection coarser than a fifth of the gap fails.
- `print_check_smoke.py` over RPC: the same negative controls (a 60° overhang
  fails, a fused print-in-place pair fails), refusals of a bad direction and
  an unknown body, and a receipt.
- GUI test `tolerance/print_check_from_the_panel`.
- Headless suite, the GUI suite, and `tools/ctest -C generators -L
  generators -j2` with every generator's markers.
