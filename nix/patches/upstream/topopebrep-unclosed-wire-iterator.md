# CorrectUnclosedWire removes edges from a wire while iterating it (SIGSEGV in TopoDS_Iterator::Next)

**OCCT version:** 7.9.3 (V7_9_3, a016080b). **Component:** ModelingAlgorithms
/ TKBool, `TopOpeBRepBuild_Merge.cxx`. **Patch:**
[`../occt-topopebrep-unclosed-wire-iterator.patch`](../occt-topopebrep-unclosed-wire-iterator.patch).

## Symptom

A fillet of r 0.184391 on edge 13 of `tests/fixtures/occ/fillet-unclosed-wire.brep`
(a modelled thread: a revolved core with a swept thread fused on; valid):

    occ_probe fillet fillet-unclosed-wire.brep 0.184391 13

dies in `BRepFilletAPI_MakeFillet::Build` with SIGSEGV at a non-null address:

    TopoDS_Iterator::Next()
    TopOpeBRepBuild_Builder::MakeFaces(...)
    TopOpeBRepBuild_Builder::SplitFace1(...)
    ...
    TopOpeBRepBuild_Builder::MergeSolid(...)
    ChFi3d_Builder::Compute()

## Cause

`MakeFaces` repairs a wire `BRepCheck_Analyzer` calls invalid with the static
`CorrectUnclosedWire`, whose first loop is

    TopoDS_Iterator tdi(aWire, Standard_False, Standard_False);
    for (; tdi.More(); tdi.Next())
      if (tdi.Value().NbChildren() <= 1)
        BB.Remove(aWire, tdi.Value());

`BRep_Builder::Remove` erases the edge from the wire's list of children, the
list the iterator walks: the node it stands on is freed, and `Next()` reads
it.

## Fix

Collect the edges to remove first and remove them after the walk. With the
patch the build completes; the solid it returns is invalid
(`BRepCheck_Analyzer`), which the caller can test and refuse, where before it
took the process down.
