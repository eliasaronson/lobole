# Sketch fillet on splines and elliptical arcs, and a Split tool

A curve should split in two where it is clicked, and the two pieces should
stay one curve until one is changed. The sketch fillet, which rounds the
corner of two lines or arcs (docs/sketching/README.md#fillet), should round
a corner with a spline or an elliptical arc in it too.

## The problem

- **Splines and elliptical arcs refused.** `FilletCorner` refuses a spline or
  an elliptical arc end by name ("a spline end is not filleted yet"), so a
  traced outline with spline sides cannot be rounded. `sketch::CornerAt`
  already lists their ends, so the sector a click means is right; the
  construction is what is missing.
- **No Split.** `SplitEntity` takes lines only, adds no collinear, and has no
  tool or RPC. It moves constraints by ref alone: a midpoint or an equal
  length on the line then holds only the first piece, and a tangent naming
  `{line, 1}` moves to piece 2 whether it meant the end or the line.
- **Trimming an arc's middle loses its far end.** `TrimCurveAt` rebuilds the
  survivors with no transfer: refs to the arc's end stay on the first
  survivor, whose end has moved, and the second joins nothing.

## The shape of the fix

### 1. The fillet of a spline or an elliptical arc

`sketch::FilletOfCurves` (src/core/sketch/SketchFillet.cpp) gains the
elliptical arc and the spline. The centre is still where the two curves'
offsets by the radius toward the inside of the corner meet nearest it; for
these curves that is Newton on the two curve parameters, seeded by the
fillet of the two end tangents. Each tangent point must lie on its curve,
with no stretch between it and the corner bending tighter than the radius
(the `tooLarge` refusal, so `LargestFillet` bisects it as it does for lines
and arcs).

An elliptical arc is trimmed at its parameter, and the corner point is held
on its ellipse. A spline is cut as in §2 and gets no such lock, since
nothing of it runs past the tangent point. `FilletCorner` then stops
refusing them; its reply's `replaced` names each cut spline and its pieces,
and `dropped` each constraint the cut could not keep.

### 2. A spline cut exactly

The sketch stores a spline as clamped-uniform poles, so a piece that starts
mid-span is not one storable spline. Knot insertion at the cut u in span
[k, k+1] changes no point of the curve. On each side it gives a Bézier for
the partial span, plus a clamped-uniform spline for the whole spans past the
next knot, once that knot is raised to full multiplicity. Both are exact
`BSpline`s, joined Coincident and Curvature (G2), or Tangent at degree 1. A
side inside one span is one Bézier, and the largest piece keeps the id. The
insertion moves into `src/core/sketch/SketchSpline.{h,cpp}`, and the DXF
reader's `SplineToSketch` uses it.

A pole the insertion leaves in place keeps its constraints, re-indexed onto
its piece. A constraint on a pole it replaces is dropped. The reply's
`dropped` and the pill name each one ("distance (constraint 6) on pole 4 of
spline 9"). The pieces carry more poles than they replace, and the pill's
DOF says by how much. No format change is needed: the results are entities
and constraint kinds the file already holds.

### 3. Split

`SplitEntity(id, at)` cuts a curve into pieces that stay one curve:

- a line: piece 2 runs at→b, joined Coincident plus Parallel to piece 1,
  or the H/V the line carried, so the pieces are collinear;
- an arc: two arcs joined Coincident and Concentric (the radius follows); a
  circle takes two cuts, joined the same way at both;
- an elliptical arc, or an ellipse in two cuts: Concentric, Equal and
  Coincident;
- a spline: cut as in §2, with G2 at the cut; a closed spline takes one cut,
  since its seam is the other.

A constraint on an end goes with the piece that holds the end, and so does a
Tangent or Curvature joined there. One on the whole curve (its direction, its
circle, symmetry about it, a point on a line or arc) stays on piece 1, and
the joints hold piece 2 to it. A point on a spline goes to the piece it lies
on. A whole-edge distance spans both pieces, as today, and a midpoint of the
line becomes a symmetric of its outer ends. An equal length on the line or
an `arc_length` on the arc has no two-piece form and is refused by name. At a
crossing, the joint is put on the crossing curve, so the DOF does not change.
At another curve's end, the joint becomes coincident with that end, replacing
the point-on-curve that held it. Anywhere else the joint slides (one DOF).
Trim keeps its own transfer, so its survivors are not left parallel.
`TrimCurveAt` cuts an arc through this split, so the far end keeps its
constraints.

### 4. The Split tool

`SketchTool::Split`, command `sketch.split` "Split a curve" (Modify),
`###tb_sk_split` after Trim, default key `K`. Its icon is `ICON_DS_SPLIT`,
part Split's glyph, so the two go into one same-idea group in
`test_conventions.cpp`. Hover snaps to a crossing with any curve (sampled,
then refined) within the coincident snap's 10 px, then to another curve's
end on it, else to the nearest point. It previews a cut mark and the two
pieces in two shades. Each click cuts, with one solve and one undo step, and
a tap on empty space or Enter finishes, as with Trim. A closed curve takes two
clicks (Step 1/2, 2/2). Refusals name why, prefixed "Not split:" as the
Fillet tool's are "Not filleted:": a generated entity (`GeneratedBy`), a
point or a label, a cut at an end, or the constraints above.

### 5. RPCs

- `fillet` adds `replaced` (each cut spline and its pieces) and `dropped` to
  its reply once it takes splines.
- `split {sketch_id, entity_id, at, crossing?}`. `at` is one [u, v] in
  sketch mm, or two for a closed curve. With `crossing`, the cut is that
  curve's crossing nearest `at` and the joint goes on it. Otherwise `at`
  moves to the nearest point of the curve, and is refused past a hundredth of
  the curve's length. It returns `entity_ids` in order along the curve, the
  first keeping the id, plus `dropped`, `dof`, `regen` and `digest`.

## Stages

Each lands with the suites green.

- **B.** Split of lines, arcs and circles: §3–4, `split`, the Trim fix.
- **C.** Splines and elliptical arcs: §2, then §1 in Fillet and in Split.

## Not in this plan

A fillet between curves that share no end (crossing lines, two circles),
sketch chamfers, and Trim at crossings with ellipses and splines.

## Files

- `src/core/features/SketchFeature.{h,cpp}` (`FilletCorner`, `SplitEntity`,
  `TrimCurveAt`); `src/core/sketch/SketchFillet.{h,cpp}` (the Newton
  construction) and new `SketchSpline.{h,cpp}`;
  `src/core/io/SketchImport.cpp`; `CMakeLists.txt`
- `src/ui/SketchInteraction.cpp`, `SketchToolOverlay.cpp`,
  `SketchToolbar.cpp`, `SketchOverlay.cpp`, `AppState.h`, `Toolbar.cpp`,
  `Commands.cpp`, `Icons.cpp`; `src/core/io/Keymap.cpp`
- `src/remote/RemoteModeling.cpp`, `api/rpc-surface.json`
- `tests/test_sketch_feature.cpp`, `test_sketch_solver.cpp`,
  `test_sketch_curves.cpp`, `test_keymap.cpp`, `test_conventions.cpp`;
  `src/app/guitests/GuiTestsModeling.cpp` (`fillet/*`), `GuiTestsSketch.cpp`
  (`split/*`), `GuiTestsShell.cpp`; `tests/remote_smoke.py`,
  `refused_calls_smoke.py`
- docs/sketching/README.md, docs/ui/tool-sessions.md,
  docs/ui/keybindings.md

## Verified by

- **Curve pairs.** Line–spline, spline–spline and line–elliptical arc
  corners: each tangent point on its curve within 1e-9 mm, the arc tangent
  to both, the sketch solving clean at the DOF predicted, extruding. A
  radius too large is refused with the largest that fits, and that radius
  succeeds.
- **The owner's corner, all of it.** A spline, a vertical line and two
  collinear horizontal lines at one point: a click in either sector the
  spline bounds fillets the spline and the line beside it (stage A refuses
  those, naming the spline).
- **Exact spline cuts.** The pieces of a cut spline lie on the original at
  every sample (1e-12 relative); constraints on untouched poles survive and
  each replaced one is named in `dropped`.
- **Split.** A line, an arc, a circle, an ellipse and a spline, each split at
  a crossing and at a free point: the geometry unchanged after the solve, DOF
  unchanged at a crossing and one higher elsewhere, and making either piece
  construction leaves the other where it was. A midpoint carried, an equal
  length refused. A trimmed arc's far end keeps its coincident.
- **The bar.** `toolbar/every_tool_is_reachable` covers the sketch bar too
  (the row fits, every `###tb_sk_*` exists); `no_duplicate_glyph` holds; the
  keymap has no conflict.
- **Through the RPCs.** `split` in `remote_smoke.py`, each refusal changing
  nothing in `refused_calls_smoke`, and `rpc_units_gate`.
