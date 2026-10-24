# BRepOffsetAPI_DraftAngle::Build faults on a degenerated edge and throws on a parallel extremum

**OCCT version:** 7.9.3 (V7_9_3, a016080b). **Component:** ModelingAlgorithms
/ TKOffset, `Draft_Modification_1.cxx` (`Draft_Modification::Perform`).
**Patch:** [`../occt-draft-null-curve-and-parallel.patch`](../occt-draft-null-curve-and-parallel.patch).

Two defects in one function, both reached by an ordinary draft of planar side
faces; each is a crash or an exception from inside `Build` where a failed
draft (`IsDone() == false`, `Status()` set) was meant.

## 1. SIGSEGV at address 0: an edge with no 3D curve

`tests/fixtures/occ/draft-null-curve.brep` (a block with a sphere subtracted
from it; valid), drafting faces 1 2 3 4 (`TopExp_Explorer` order) by 2° about
the plane y = -15 with the pull +Y:

    occ_probe draft draft-null-curve.brep 2 0 1 0 0 -15.0000001 0 1 2 3 4

dies with SIGSEGV in `Geom_Geometry::Transformed`, called from
`Draft_Modification::Perform` in the loop "Calculate new edges":

    C = BRep_Tool::Curve(theEdge, L, f, l);
    C = Handle(Geom_Curve)::DownCast(C->Transformed(L.Transformation()));

`BRep_Tool::Curve` returns a null handle for an edge without a 3D curve -- a
degenerated edge, here the sphere's pole. The fix returns
`Draft_EdgeRecomputation` with that edge as the bad shape.

## 2. Standard_OutOfRange: an extremum that found the curve parallel

`tests/fixtures/occ/draft-parallel-extrema.brep` (two blocks unioned; valid),
faces 0 3 4 5, 2° about x = -20 with the pull +X:

    occ_probe draft draft-parallel-extrema.brep 2 1 0 0 -20.0000001 0 0 0 3 4 5

throws `Standard_OutOfRange: NCollection_Sequence::Value` from `Build`. In
"Calculate new vertices", when `IntCurveSurface_HInter` finds no point the
vertex is placed from `Extrema_ExtCS(AC, AS)`: the loop reads
`extr.Points(i, ...)` for `i <= extr.NbExt()`. When the curve is parallel to
the surface, `Extrema_ExtCS` reports the distance (`NbExt() == 1`) and no
points, so `Points(1)` reads past an empty sequence. The fix checks
`IsParallel()` and returns `Draft_VertexRecomputation`: a parallel pair does
not place the vertex.

Both reproduce in plain OCC ([`occ_probe.cpp`](occ_probe.cpp)); with the
patch each draft returns `IsDone() == false`.
