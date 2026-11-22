# Sketch fillet and split on splines and elliptical arcs

The sketch fillet, which rounds the corner of two lines or arcs
(docs/sketching/README.md#fillet), should round a corner with a spline or an
elliptical arc in it too, and Split (docs/sketching/README.md#split), which
cuts a line, an arc or a circle into pieces that stay one curve, should cut
a spline, an ellipse and an elliptical arc.

## The problem

- **Splines and elliptical arcs refused.** `FilletCorner` refuses a spline or
  an elliptical arc end by name ("a spline end is not filleted yet"), so a
  traced outline with spline sides cannot be rounded. `sketch::CornerAt`
  already lists their ends, so the sector a click means is right; the
  construction is what is missing.
- **Split stops at circles.** `SplitEntity` refuses a spline, an ellipse and
  an elliptical arc by name ("a spline is not split yet"), so a traced
  outline cannot be cut where another curve crosses it.

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

### 3. Split of the other curves

`SplitEntity` takes the rest, with the transfer it has for lines and arcs:

- an elliptical arc, or an ellipse in two cuts: Concentric, Equal and
  Coincident;
- a spline: cut as in §2, with G2 at the cut; a closed spline takes one cut,
  since its seam is the other. A point on a spline goes to the piece it
  lies on.

`sketch::Crossings` already finds where these curves are crossed, and the
Split tool already snaps to their crossings; `SplitPieces` and
`NearestOnCurve` gain them, and `SplitRefusal` stops refusing them.

### 4. RPCs

- `fillet` adds `replaced` (each cut spline and its pieces) and `dropped` to
  its reply once it takes splines.
- `split` adds `dropped` to its reply once it takes splines.

## Not in this plan

A fillet between curves that share no end (crossing lines, two circles),
sketch chamfers, and Trim at crossings with ellipses and splines.

## Files

- `src/core/features/SketchFeature.{h,cpp}` (`FilletCorner`, `SplitEntity`);
  `src/core/sketch/SketchFillet.{h,cpp}` (the Newton construction),
  `SketchSplit.{h,cpp}` and new `SketchSpline.{h,cpp}`;
  `src/core/io/SketchImport.cpp`; `CMakeLists.txt`
- `src/ui/SketchInteraction.cpp`
- `src/remote/RemoteModeling.cpp`, `api/rpc-surface.json`
- `tests/test_sketch_feature.cpp`, `test_sketch_curves.cpp`;
  `src/app/guitests/GuiTestsModeling.cpp` (`fillet/*`), `GuiTestsSketch.cpp`
  (`split/*`); `tests/remote_smoke.py`, `refused_calls_smoke.py`
- docs/sketching/README.md

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
- **Split.** An ellipse, an elliptical arc and a spline, each split at a
  crossing and at a free point: the geometry unchanged after the solve, DOF
  unchanged at a crossing and higher elsewhere, and making either piece
  construction leaves the other where it was.
- **Through the RPCs.** `fillet` and `split` with a spline in
  `remote_smoke.py`, each refusal changing nothing in `refused_calls_smoke`.
