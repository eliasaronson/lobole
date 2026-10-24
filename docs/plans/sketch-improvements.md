# Sketch improvements

## The problem

Six gaps in the sketcher, each one a place where the user does work the
sketcher could do, or cannot express something at all:

1. **No curvature continuity.** A spline can meet a line or an arc tangent
   (G1) but not with matching curvature (G2), so the join shows as a kink in
   reflections and in a fillet that follows it.
2. **No arc-length dimension.** An arc can be dimensioned by radius or angle,
   not by the length along it, which is the number a slot, a bent part or a
   cable path is specified by.
3. **The first dimension distorts a sketch drawn by eye.** Giving the first
   length its real value moves that one entity, and the rest of the profile
   keeps its rough size, so the shape the user drew is lost.
4. **A variable takes a trip to another panel.** To drive a dimension by a
   new variable the user creates the variable first, then comes back and
   types its name.
5. **An open profile is found by hunting.** Extrude refuses a profile that
   does not close, and nothing shows where the gap is.
6. **Trim is one click per piece.** Cleaning up a sketch of crossing lines
   takes a click on every span.

## PlaneGCS: keep ours and add to it

`deps/planegcs/` matches FreeCAD's planegcs at commit 057d51f846
(2026-07-20) apart from our shims and the two determinism patches
(THIRD_PARTY.md). The four upstream commits since are refactors (an enum for
the solve status, renamed reference functions, log formatting) with no
behaviour change. Arc length is already in our copy
(`addConstraintArcLength`, circular arcs only), and neither our copy nor
upstream has anything for curvature. Updating buys nothing; the copy stays,
and THIRD_PARTY.md records the commit it came from.

New constraints are `GCS::Constraint` subclasses in `src/core/sketch/`,
added through the public `System::addConstraint`, so the vendored tree is
not modified further.

## The shape of the fix

### 1. G2 continuity

A new constraint kind `curvature` between two curves that share an end. It
loads as the existing tangent constraint at that end plus one curvature
equation:

- **spline–line:** the spline's curvature at the end is zero, which with
  tangency puts its third pole on the line: `addConstraintPointOnLine`, no
  new class.
- **spline–arc:** the spline's end curvature times the radius is ±1, the sign
  taken from the geometry at load as tangency already does.
- **spline–spline:** the two end curvatures are equal.
- The spline's end curvature comes from its last three poles and knots
  (signed κ = C′×C″/|C′|³). The class `ConstraintEndCurvature` gives the
  error and an analytic gradient.
- **Refused, naming why:** line–line, line–arc (G2 would need an infinite
  radius), arc–arc (it makes them one circle), a degree-1 spline, and two
  curves that do not share an end.

The tool sits beside Tangent in the constraint list, with its own glyph and
keymap entry.

### 2. Arc-length dimension

A dimension kind `arc_length` (mm) on a circular arc, loaded with
`addConstraintArcLength`. `MeasureDimension` reports r × sweep, so value
edits, driven dimensions and the label work unchanged. The Dimension tool
infers it when the label is placed along the arc, outside the radius label's
zone; the drawing is a dimension line parallel to the arc. Refused, naming
the right kind: a line (distance), a circle (diameter), an elliptical arc
and a spline (no arc-length form; they stay out of this plan).

### 3. The first dimension rescales

When a length dimension (distance, radius, diameter; never an angle) is
given a value and the sketch has no other driving dimension and no
constraint driven by an expression, every movable entity is scaled by new ÷ measured about an
anchor, then the dimension is applied, so the drawn shape is kept at its new
size.

- **The anchor**, derived: the one point pinned to the origin, a datum, a
  fixed point or external geometry if there is exactly one; otherwise, with
  none pinned, the centre of the non-construction geometry's bounding box.
  With two or more pinned points nothing is scaled; the edit runs as today
  and the status line says why.
- **What scales:** geometry and text height. Not reference images (they are
  calibrated separately) and not derived-op parameters: a sketch holding a
  pattern or offset with a millimetre parameter is not rescaled, and the
  status line says so.
- **One undo step** in both GUI paths: the inline editor's
  `ApplyDimensionEdit` and the dimension typed while drawing. The second
  needs the scale inside `ApplySketchConstraint`'s before/after window.
- **Only in the GUI.** `add_constraint` and `modify_feature` keep scripts'
  geometry literal; no RPC rescales.

`SketchFeature` gains `ScaleEntity` beside `TranslateEntity` and
`RotateEntity`; the rescale itself is a core function so it is unit-tested
without the GUI.

### 4. `name = value` in a dimension

In the inline editor and the field typed while drawing, `name = expr`
creates the project variable `name` with the value of `expr` and drives the
dimension by it, in one undo step (the pattern `LiteralActions.cpp` already
uses for extracting a literal). Project scope, matching `set_variable`'s
default.

- The name must be a valid parameter name, else refused naming the rule.
- **An existing name is refused**, naming its current value and pointing at
  plain `name` to use it; it is never silently redefined.
- The dimension's own checks (the flip and refusal checks of
  `ApplyDimensionEdit`) run first; a refused edit creates no variable.
- An angle dimension's `a = 30` stores 30 in degrees, as angle expressions
  already are.
- No new RPC: a `batch` of `set_variable` and `add_constraint` with `expr`
  does it; `help('add_constraint')` says so.

### 5. Open profile ends

`sketch::OpenEnds(entities)` in `SketchProfile.cpp` counts how many chainable
non-construction edges meet at each endpoint, with the same tolerance as
`BuildLoops`. An end met once is open; an end met three or more times (a T or
an overlap) also stops a clean profile and is returned separately. Two open
ends just outside the chaining tolerance of each other are the usual
"almost closed" case; both are marked and joined by a line. It does
not reuse `BuildLoops`'s open chains, which are greedy and can report a
closed chain as open.

While a sketch is being edited, open ends draw as markers in the overlay,
recomputed only when the sketch changes. A refused Extrude says "N open
ends" and they are already on screen.

### 6. Trim by dragging

In Trim, pressing on empty space and dragging draws a path. Every
trimmable span the path crosses is previewed as it is crossed; on release
they are all trimmed on one copy of the sketch, with one solve and one undo
step. Escape cancels the path. A click with no drag still leaves the tool
as today. A pure `TrimAlongPath(sketch, uvPolyline)` in `SketchFeature`
reuses `TrimSpanAt`, `TrimLineAt` and `TrimCurveAt`, re-resolving each
crossing against the current entities after every split (a split keeps the
id on its first piece). Lines, circles and arcs only, as click-trim.

## Files

- `src/core/features/SketchFeature.{h,cpp}`: `ConstraintKind::Curvature`
  and `ArcLength`, `IsDimension`, `ScaleEntity`, the rescale,
  `TrimAlongPath`.
- `src/core/sketch/SketchSolver.cpp`: loading both kinds;
  `src/core/sketch/ConstraintEndCurvature.{h,cpp}` (new).
- `src/core/sketch/SketchOrientation.cpp`: `MeasureDimension` for arc length.
- `src/core/sketch/SketchProfile.{h,cpp}`: `OpenEnds`.
- `src/core/io/SerializeFeatures.cpp`: the two kind names; a format bump
  (`vNEXT`, `kVersionPending`, a golden pair, format.md's pending table).
- `src/remote/RemoteModeling.cpp`: `ConstraintKindTable` rows (units for
  `arc_length`), `help` text.
- `src/ui/SketchOps.cpp`, `src/core/io/Keymap.cpp`: the G2 command and key.
- `src/ui/SketchDimensions.cpp`, `src/ui/SketchDimensionDraw.cpp`: arc-length
  inference and drawing, the rescale, `name = value`.
- `src/ui/SketchOverlay.cpp`, `src/ui/Theme.h`: G2 glyph, open-end markers,
  the drag-trim preview.
- `src/ui/SketchInteraction.cpp`: the drag path in Trim.
- `src/ui/SketchToolbar.cpp`, `src/ui/CreateActions.cpp`: Extrude's refusal
  naming the open ends.
- `THIRD_PARTY.md`: the planegcs commit.
- `docs/sketching/README.md`, `docs/ui/numeric-input.md`.

## Verification

Each item gets tests written to fail first, and checks the outcome, not
the formula:

- **G2:** a spline joined to a line, an arc and another spline; the built
  wire's curvature on both sides of the joint, read through OCC
  (`BRepLProp`), agrees to a relative 1e-6. Every refused pair is refused
  with its reason (`sketch_constraint_refusal_smoke`). The solve is
  bit-identical across threads (`[parallel]`).
- **Arc length:** set to a value, the extruded edge's length (OCC
  `GCPnts_AbscissaPoint`) equals it; value edits step through it; refused
  kinds name the right one.
- **Rescale:** a rough profile given its first width keeps every ratio and
  angle, with the anchor fixed; a second dimension does not rescale; two
  pinned points do not rescale; one undo restores the drawn size; RPC
  `add_constraint` leaves the geometry literal.
- **`name = value`:** creates the variable and binds the dimension in one
  undo; an existing name and an invalid name are refused with the sketch and
  variables unchanged; a refused edit creates no variable.
- **Open ends:** unit cases for an open chain, a T, an overlap, a closed
  chain the greedy chainer misreports; a GUI test sees the markers and the
  Extrude refusal's count.
- **Drag trim:** a unit case trims three crossed spans in one call; a GUI
  test drags across them, sees the preview, and one undo restores all three.
- A golden pair for the format bump; `tools/ctest -L sketch`, `-L solver`,
  `-L spline`, the headless suite, the GUI suite (`sketch/*`, `dims/*`,
  `trim/*`, `region/*`), and the action matrix sample.
