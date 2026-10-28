# An arc-join offset cannot be stopped inside the intersection of two offset faces

**OCCT version:** 7.9.3 (V7_9_3, a016080b). **Component:** ModelingAlgorithms
/ TKOffset (`BRepOffset_Inter3d.cxx`, `BRepOffset_Tool.cxx`) and TKBO
(`IntTools_EdgeFace.cxx`, `IntTools_BeanFaceIntersector.cxx`,
`BOPAlgo_PaveFiller_5.cxx`). **Patch:**
[`../occt-offset-inter3d-progress.patch`](../occt-offset-inter3d-progress.patch).

## Symptom

`BRepOffsetAPI_MakeThickSolid::MakeThickSolidByJoin` of
`tests/fixtures/occ/shell-worm-thin-wall.brep` (a worm: a cylinder blank with
a swept BSpline thread cut into it; valid), its face 1 removed, a wall of
0.02 and `GeomAbs_Arc`, given a progress indicator: from about 3 s in, the
indicator is not asked again for one to three minutes (load-dependent), so
an indicator that asks for a break in that window is not heard until the
step is over.

## Cause

The whole silent stretch is ONE edge/face pair. `BRepOffset_Inter3d::
ConnexIntByArc` intersects two neighbouring offset faces with
`BRepOffset_Tool::Inter3D`, which runs a `BOPAlgo_PaveFiller` of the two
without a range; one of its edge/face intersections
(`IntTools_EdgeFace::Perform` -> `IntTools_BeanFaceIntersector::
ComputeLocalized`) runs a global `Extrema_GenExtCS` search on every
localized range of the edge against the BSpline face, and on this face there
are very many. `BOPAlgo_EdgeFace` checks the indicator before each pair, but
neither the pave filler nor the pair is given it.

## Fix

The range is passed down: `ConnexIntByArc` gives each `Inter3D` a range of
its own scope, `Inter3D` hands it to `BOPAlgo_PaveFiller::Perform`,
`BOPAlgo_EdgeFace` to `IntTools_EdgeFace::Perform`, and that to
`IntTools_BeanFaceIntersector::Perform`, which asks it between the localized
ranges. Each new parameter is defaulted, so other callers are unchanged. On a
break the intersector, the pair, `Inter3D` and `ConnexIntByArc` return with
nothing, and `Intersection3D` reports `BRepOffset_UserBreak` as before.
Without a break nothing changes.

## A failed intersection read as if it had worked

**Symptom:** `MakeThickSolidByJoin` of
`tests/fixtures/occ/shell-inter3d-failed.brep`, its face 9 (a plane beside a
blend) removed, a wall of 0.002 and `GeomAbs_Arc`: SIGSEGV at address 0 in
`BOPDS_DS::ShapeInfo`, under `BRepOffset_Tool::Inter3D` from
`BRepOffset_Inter3d::ConnexIntByArc`.

**Cause:** `Inter3D` runs a `BOPAlgo_PaveFiller` of the two offset faces and
reads its section curves without asking whether it succeeded. One that
failed leaves pave blocks with no edge (`Edge()` is -1), and
`pDS->Shape(-1)` is read in `CheckIntersFF` and again below it. Stock 7.9.3
does the same; the progress range above plays no part in it.

**Fix:** after `Perform`, a pave filler with errors throws
`Standard_ConstructionError` ("intersecting two offset faces failed"), so
the offset fails as an ordinary exception instead of reading the data
structure.
