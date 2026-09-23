# Modelled (helical) thread

## Problem

`Thread` is cosmetic only. Its `modeled` flag is in the schema, and setting it
errors the feature ("modeled threads are not implemented yet") while the
cosmetic overlay stays drawn (`src/core/features/ThreadFeature.cpp`). A part
that needs real thread geometry — a printed screw, a render close-up, a check
against a mating thread's flanks — has no way to get it.

## Fix

When `modeled` is set:

1. Build the helix the standard OCC way: a `Geom2d_Line` on a
   `Geom_CylindricalSurface` of the face's radius, turned into an edge, over the
   threaded span (`length`, or the whole face). The `Curve3D` helix builder in
   `src/core/features/CurveFeature.cpp` already does this and can be shared.
2. Sweep the ISO 60° V profile for the designation's pitch along it with
   `BRepOffsetAPI_MakePipeShell` (`SweepProfileAlongSpine`,
   `src/core/features/SweepFeature.h`), chaining per turn if one sweep over many
   turns fails.
3. Cut the groove from the body (external) or the hole wall (internal) through
   the named boolean path; name the result with `topo::NamePrimitiveElements`
   (per-flank naming is out of scope).
4. Ends are square — no runout. Any OCC failure is caught into the feature
   error with the cosmetic overlay still drawn; a failing modelled thread must
   never take the document down.

A helical sweep-cut is the slowest and most failure-prone boolean OCC does,
which is why cosmetic stays the default.

## Files

`src/core/features/ThreadFeature.{h,cpp}`, possibly a helix helper shared with
`CurveFeature.cpp`; `src/ui/FeatureEditor.cpp` (the Modeled checkbox);
`docs/modeling/holes-and-threads.md`.

## Verification

`tests/test_thread.cpp`: one M8×1.25 through a short boss — volume strictly
below the blank and within a loose band of blank minus the groove estimate (no
exact helical-volume assertion), identical volume after regenerating twice; an
internal thread in a tapped hole; a failure case that errors without removing
the body. A screenshot of the result reviewed by eye.
