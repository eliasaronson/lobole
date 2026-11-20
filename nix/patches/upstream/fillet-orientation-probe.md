# ChFi3d_Builder::StoreData probes for the blend's orientation for minutes

**OCCT version:** 7.9.3 (V7_9_3, a016080b). **Component:** ModelingAlgorithms
/ TKFillet, `ChFi3d_Builder_6.cxx`. **Patch:**
[`../occt-fillet-orientation-probe.patch`](../occt-fillet-orientation-probe.patch).

## Symptom

A 0.5 mm fillet of both edges of the top outline of a pad on a closed cubic
B-spline (61 poles round a 30 x 12 ellipse, split into two edges) whose
pole near (0, 12) was thrown to x = 83 229 by a damaged file:
`BRepFilletAPI_MakeFillet::Build` does not return. One call of
`ChFi3d_Builder::StoreData` takes all the time (minutes; stopped by hand),
in `Geom_BSplineSurface::D1` and `Geom2d_Curve::Value`.

## Cause

To orient the blend face against its support, `StoreData` evaluates both
normals at `VFirst + (VLast - VFirst) / aDenom`, and while either is
degenerate (magnitude under `tolget3d`) increments `aDenom` and tries again,
stopping only when the step `(VLast - VFirst) / aDenom` is under `tolget2d`,
the approximation's 2D tolerance. Where the normals stay degenerate that is
`(VLast - VFirst) / tolget2d` probes, each two surface derivatives.

## Fix

Give up after a thousand probes (`return Standard_False`, the same answer
the tolerance test gives): the stripe fails and `IsDone()` is false. Every
blend that found a normal within the first thousand probes is unchanged.
