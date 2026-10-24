# BRepFilletAPI_MakeFillet::Add never returns: ReorderFaces walks round a vertex for ever

**OCCT version:** 7.9.3 (V7_9_3, a016080b). **Component:** ModelingAlgorithms
/ TKFillet, `ChFi3d_Builder_1.cxx`. **Patch:**
[`../occt-fillet-reorder-faces-loop.patch`](../occt-fillet-reorder-faces-loop.patch).

## Symptom

`BRepFilletAPI_MakeFillet::Add(0.125, E)` on one edge of the attached solid
does not return: one core busy, the stack always under
`ChFi3d_Builder::PerformElement` → `ReorderFaces` →
`TopExp::MapShapesAndAncestors`. `BRepFilletAPI_MakeChamfer::Add` hangs the
same way. No progress indicator reaches it (it is in `Add`, before `Build`).

## Reproduction

`tests/fixtures/occ/fillet-reorder-faces-loop.brep` (a valid solid: a block
with bores, `BRepCheck_Analyzer` valid), edge 50 in `TopExp::MapShapes(S,
TopAbs_EDGE)` order (an edge of a bore, next to its seam):

    occ_probe fillet fillet-reorder-faces-loop.brep 0.125 50

([`occ_probe.cpp`](occ_probe.cpp) is plain OCC.) It prints `Add edge 50...`
and never returns.

## Cause

`ReorderFaces` (static, `ChFi3d_Builder_1.cxx`) is called for each edge
`PerformElement` adds to the spine, to put the new edge's two faces in the
order of the previous one's. When neither of them is the previous edge's
first face it walks the faces round the common vertex, from edge to edge,
until it meets one of them:

    for (;;) { ... for each edge at the vertex on PrevFace ... CurFace = the
               other face of CurEdge; if CurFace is F1 or F2 return; ...
               PrevEdge = CurEdge; PrevFace = CurFace; }

with no test of having been round. At a seam the "other face" of the seam
edge is the same face, so the walk steps between the edges of one face and
never reaches `F1` or `F2`.

## Fix

Keep the edges the walk has left from; leaving from one twice means it went
round without meeting the faces, and the chain is refused with
`Standard_ConstructionError` ("the faces round a vertex of the tangent chain
do not lead to the next edge's faces"), which `Add` passes to the caller like
its other construction errors. Returning without reordering instead was
tried: `Build` then throws "ChFi3d_Builder:only 2 faces", so the edge cannot
be blended either way, and the error belongs where the walk fails.
