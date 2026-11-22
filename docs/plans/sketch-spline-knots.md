# Dimensions across a spline cut

A spline cut in two is two splines on its own knots joined Coincident +
Smooth (docs/sketching/README.md#a-spline-cut-exactly, #smooth). What is
left is that a dimension survives the cut wherever the thing it measured
still exists, and that the user sees what a cut drops before making it.

## The problem

- **A dropped dimension is named only after the click.** The cut replaces
  the `degree − 1` poles of its own span, and a constraint, dimension or
  expression on one of them is dropped and named in the prompt pill and the
  RPC's `dropped`. Nothing says so while the Split tool hovers, so the user
  cannot choose a cut a little further along instead.
- **The Dimension tool on a spline's body** draws its ghost from the last
  pole and then fails with "[id, 255] is no point of a spline".

## The shape of the fix

The rule: **a dimension stays on the thing it measured if that thing still
exists.** On an end, on a pole away from the cut and on a point placed on
the spline, the transfer already does this (`SplitSplineAt`); the joint is a
point like any other end, so Distance, X/Y and Coincident take it.

- **The Split tool's hover lists what the cut at the cursor would drop**,
  from the same transfer `SplitSplineAt` runs (factor the part of it that
  decides what is dropped into a dry run returning `CutReport::dropped`),
  drawn with the piece preview.
- **The Dimension tool on a spline's body refuses in the tool**, before a
  ghost is drawn: "a spline has no size of its own: dimension its ends, a
  pole, or a point placed on it". A spline's length and its radius at a
  point are not in this plan.

## Files

- `src/core/features/SketchFeature.{h,cpp}` (the dry run)
- `src/ui/SketchToolOverlay.cpp`, `SketchInteraction.cpp` (the hover's list)
- `src/ui/SketchDimensions.cpp` (the refusal)
- docs/sketching/README.md (Split, the Dimension tool)

## Verified by

- **Dimensions.** A distance on an end, on a far pole and on a
  point-on-spline each survive a cut with their value; one on a pole of
  the cut's span is dropped and named, and the hover named it first; a
  distance to the new joint drives where the joint lies along the curve.
- **The Dimension tool** on a spline body refuses with its sentence and
  adds nothing (GUI test).
