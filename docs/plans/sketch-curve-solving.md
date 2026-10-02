# Plan: solve ellipses, elliptical arcs and splines

## Problem

Three sketch curve types are only partly solved.

- **`Ellipse` and `EllipseArc`.** Only the centre is a solver point
  (`SketchSolver.cpp`, `Solver::Load`). The radii, the rotation and an
  elliptical arc's end parameters are rigid. So a user cannot dimension an
  ellipse's radii, make a line tangent to one, put a point on one, or join
  an elliptical arc's end to anything: those ends can be picked, but they
  are not solver points, so constraints on them are skipped.
- **`BSpline`.** Each pole is a free point with no rules between the poles.
  A user cannot put a point on the curve or make it tangent at an end, and
  a closed spline (first pole repeated as the last) opens when its first
  pole is dragged.
- **Skipped means silent.** A constraint the solver cannot represent — a
  radius on an ellipse, a tangent between a circle and a spline, a refs pair
  that names no curve — is accepted by `add_constraint` and the GUI and then
  added to nothing. The sketch looks constrained and is not.

## Fix

**Ellipses.** Load each ellipse as PlaneGCS's own `GCS::Ellipse`: centre,
`focus1` (at `c = √(a² − b²)` from the centre along `rotation`) and
`radmin` (b). Those five numbers are the ellipse's five degrees of freedom,
so nothing has to hold it together. Write back `rMajor = √(|F−C|² + b²)` and
`rotation = atan2(F − C)`; when the focus offset and b come back unchanged,
keep the stored `rMajor`/`rotation` bit for bit, so repeated solves do not
walk the shape by an ulp each regen.

The **axis ends** become solver points: `which` 3/4 = the major-axis ends
(+û, −û), 5/6 = the minor-axis ends (+v̂, −v̂), tied to the ellipse by
`addConstraintInternalAlignmentPoint2Ellipse` with tag 0 (definitional, never
reported as a user's conflict). Each adds two unknowns and two equations, so
the DOF stays 5. They make radii and orientation ordinary point
constraints:

- a radius is `distance` from the centre (0) to an axis end (3 or 5);
- Horizontal/Vertical on an ellipse levels its major axis (3–4);
- an axis end can be made coincident, dimensioned, fixed, or dragged.

`radius`/`diameter` on an ellipse is refused, naming the distance form: an
ellipse has two radii, and picking one for the caller is a substitute.

A circle-shaped ellipse (`rMajor == rMinor`) has no focus direction, so it
stays rigid (centre free, the other points riding it). `r_minor > r_major`
is refused on load (OCC refuses to build it anyway). `EllipseFromClicks`
canonicalizes a third click outside the second, which is how the tool made
one.

**Elliptical arcs.** Load them as `GCS::ArcOfEllipse`, with real start/end
points (`which` 1/2) and their end parameters as unknowns, tied by
`addConstraintArcOfEllipseRules` (tag 0). PlaneGCS's arc parameter is the
same `t` the model stores (measured from the major axis), so the stored
params write back directly; the sweep is renormalized CCW like `Arc`'s.

**Splines.** Load each as a `GCS::BSpline` whose poles are the existing
solver points, with the clamped-uniform knots (`BSplineKnots`), unit
weights, and both knots and weights fixed. A clamped spline's ends ARE poles
0 and n−1, so they need no separate start/end points. A closed spline (first
pole equal to the last) gets a definitional coincident between them.

**Constraints on the new curves.**

- point-on-curve (`point_on_line`): `addConstraintPointOnEllipse` for an
  ellipse or elliptical arc; `addConstraintPointOnBSpline` for a spline,
  with a per-constraint curve parameter as an extra unknown, seeded from the
  nearest point.
- `tangent`, generalized: two curves joined at an end by a Coincident (any
  of line, arc, elliptical arc, spline end) are tangent there as an angle
  (0 or π, whichever they already make) — the rule the line–arc fillet
  already uses, now for every pair; a spline's end tangent is its end leg
  (pole 0→1). Without a shared end: line–circle/arc (signed distance, as
  now), line–ellipse/elliptical arc (`addConstraintTangent(Line&,
  Ellipse&)`), circle/arc–circle/arc (`addConstraintTangent` for those).
  Anything else is refused.
- `equal` between two ellipses/elliptical arcs: `addConstraintEqualRadii`.
- `concentric` accepts ellipses and elliptical arcs.

**Refusals, from one place.** `Solver::Load` records, for every constraint
it could not represent, the reason and what that kind accepts
(`Solver::Unsupported()`). The dispatch is the validator, so the two cannot
drift. `add_constraint(s)` refuses with that text and leaves the sketch
alone; the GUI shows it and adds nothing; a sketch loaded with one carries a
regeneration warning naming it.

**GUI.** The axis ends are pickable and drawn as dots (`EntityPoints`,
`EntityDots`, inference's vertex walk, `EntityPointUV`), so dragging one
resizes/rotates the ellipse directly. Dragging an ellipse, elliptical arc or
spline by its body translates it rigidly (all its points move by the cursor
delta). The Dimension tool on an ellipse body places its major radius
(centre → major end); the constraint commands accept ellipses and splines
where the solver does, and show the solver's refusal otherwise.

**Format.** Constraint refs to an ellipse's `which` 3–6, and the
`r_minor > r_major` refusal, are new; `kVersion` bumps with a golden that
holds both kinds of constraint.

## Files

- `src/core/sketch/SketchSolver.{h,cpp}`: load, rules, write-back, the
  fixed-parameter handling (linked copies, external geometry), the
  under-defined map, `Unsupported()`.
- `src/core/features/SketchFeature.{h,cpp}`: the axis-end `which` values and
  `EntityPointUV`; `EllipseFromClicks`; the header comments; the regen
  warning in `Execute`.
- `src/core/io/SerializeFeatures.cpp`: the ellipse radius refusal;
  `kVersion` in `SerializationInternal.h`.
- `src/core/sketch/SketchInference.cpp`: axis ends as snap vertices.
- `src/ui/SketchOps.cpp`, `SketchDimensions.cpp`, `SketchPick.cpp`,
  `SketchOverlay.cpp`, `SketchTools.cpp`: validators, the Dimension tool on
  an ellipse, points/dots, body drags.
- `src/remote/RemoteModeling.cpp`: `add_constraint(s)` refusals; the
  `describe` rows.
- `docs/sketching/README.md`, `docs/versioning/format.md`.

## Verification

`tests/test_sketch_curves.cpp` `[solver][ellipse]`/`[spline]`, every case
checked against the geometry, not the formula:

- a distance centre→major end drives `rMajor`; the minor radius, rotation
  and DOF behave; an extruded ellipse's volume is `π·a·b·h` at the new a;
- Horizontal on a rotated ellipse levels it;
- a line tangent to an ellipse touches it (min distance from the ellipse
  samples to the line ~0) and stays on its side;
- a point on an ellipse / a spline lands on the curve (`EllipsePointUV`
  param search / `BSplineEvalUV` nearest sample);
- an elliptical arc's end joined to a line end moves both, the arc stays on
  its ellipse; a half-ellipse closed by its chord revolves to the right
  volume after a dimension change;
- a line tangent to a spline end, an arc tangent to an elliptical arc end:
  the tangent directions agree at the joint;
- DOF: ellipse 5, elliptical arc 7, spline 2n, a closed spline 2n−2;
- repeated load/solve/write-back is bit-identical (no drift), unconstrained
  curves do not move, and goldens `v40-spline`, `v46-ellipse-arc` load
  unchanged;
- refusals: radius on an ellipse, tangent circle–spline, point-on-curve on a
  text label — each names what is accepted, via the RPC too.

GUI: a `sketch/*` case drags an ellipse's major end and checks rMajor, and
dimensions an ellipse with the Dimension tool. The chess and watch
generators (splines) run as the generators label.
