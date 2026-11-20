# IntTools_Context::FClass2d loses its block when the classifier's constructor throws

**OCCT version:** 7.9.3 (V7_9_3, a016080b). **Component:** ModelingAlgorithms
/ TKBO, `IntTools_Context.cxx`. **Patch:**
[`../occt-inttools-fclass2d-leak.patch`](../occt-inttools-fclass2d-leak.patch).

## Symptom

LeakSanitizer, after a pave filler ran on faces that fail to intersect
(Loböle's `tests/fixtures/occ/shell-inter3d-failed.brep`, a 0.002 mm thin
shell, face 9 open, arc join):

    Direct leak of 832 byte(s) in 4 object(s) allocated from:
        NCollection_IncAllocator::Allocate
        IntTools_Context::FClass2d(TopoDS_Face const&)

## Cause

    pFClass2d = (IntTools_FClass2d*)myAllocator->Allocate(sizeof(IntTools_FClass2d));
    new (pFClass2d) IntTools_FClass2d(aFF, aTolF);
    myFClass2dMap.Bind(aFF, pFClass2d);

When the constructor throws (`Standard_ConstructionError` for a face it
cannot classify) the block is neither bound to the map, which the context's
destructor frees, nor freed. The pave filler catches the exception, and no
pointer to the block is left. 208 B a face.

## Fix

Construct inside a `try`; on an exception free the block through the same
allocator and rethrow. The caller sees the same exception as before.
