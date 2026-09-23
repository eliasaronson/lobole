# Plan: solve ellipses, elliptical arcs and splines

## Problem

Three sketch curve types are only partly solved.

- **`Ellipse` and `EllipseArc`.** Only the centre is a solver point
  (`SketchSolver.cpp`, `Solver::Load`). The radii, the rotation and an
  elliptical arc's end parameters are rigid. So a user cannot:
  - dimension an ellipse's radii,
  - make a line tangent to an ellipse,
  - put a point on an ellipse,
  - constrain an elliptical arc's end points. Those ends can be picked, but
    they are not solver points, so constraints on them are skipped.
- **`BSpline`.** Each pole is a free point with no rules between the poles.
  A user cannot put a point on the curve or make it tangent at a knot.

## Fix

**Ellipses.** Load each ellipse as PlaneGCS's own `GCS::Ellipse`, and add
the rules that hold it together:

- Represent the ellipse by its centre, `focus1` and `radmin`:
  - `focus1` lies at a distance `c = √(a² − b²)` from the centre, along
    `rotation`;
  - `radmin` is the minor radius, `b`.
- Emit the internal-alignment rules with the definitional tag 0, the same
  way arcs use `addConstraintArcRules`. Tag 0 keeps them out of conflict
  reporting. The rules live in `deps/planegcs/GCS.h`:
  - `addConstraintInternalAlignmentEllipseMajorDiameter`
  - `addConstraintInternalAlignmentEllipseMinorDiameter`
  - `addConstraintInternalAlignmentEllipseFocus1` / `Focus2`
- Convert back to `center / rMajor / rMinor / rotation` in `WriteBack`.

**Elliptical arcs.** Load them as `GCS::ArcOfEllipse`, with real start and
end points.

**Constraints on ellipses.** Let the constraint validators accept ellipses:

- Radius and Equal, mapped to major or minor;
- Tangent, via `addConstraintTangent(Line&, Ellipse&)`;
- point-on-curve, via `addConstraintPointOnEllipse`.

**Splines.** Load each spline as a `GCS::BSpline` whose poles are the
existing solver points. Add `addConstraintPointOnBSpline` for
point-on-curve, and tangency at the end knots.

## Files

- `src/core/sketch/SketchSolver.cpp`: load, rules, write-back, and the
  fixed-parameter handling for solver-fixed outputs (linked copies,
  external geometry).
- `src/core/features/SketchFeature.h`: the header comments on `Ellipse`,
  `EllipseArc` and `BSpline`.
- `src/ui/SketchOps.cpp` (`ApplySketchConstraint`) and
  `src/ui/SketchDimensions.cpp`: the constraint validators and the
  Dimension tool's kind inference for ellipses.
- `src/remote/RemoteModeling.cpp`: the kinds that `add_constraint` accepts
  on these curves.
- `docs/sketching/README.md`: the solver-treatment column of the entity
  table.

## Verification

Add these cases to `tests/test_sketch_solver.cpp`:

- A radius dimension on an ellipse's major axis drives `rMajor`. The minor
  radius and the rotation stay put.
- A line tangent to an ellipse solves, and the line stays on its original
  side.
- A point on an ellipse lands on the curve. Check it with
  `EllipsePointUV`.
- A coincident on an elliptical arc's end point moves the end point, and
  the arc stays on its ellipse.
- A point on a spline lands on the curve. Check it with `BSplineEvalUV`.
- An existing ellipse, dragged by its centre, still translates rigidly.

Keep `tests/golden/v40-spline` and `v46-ellipse-arc` loading
unchanged. Solving an unconstrained curve must not move it.
