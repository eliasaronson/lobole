# Trademarks

Onshape®, Onshape Full-Cloud CAD® and FeatureScript™ are trademarks of PTC
Inc.; SolidWorks of Dassault Systèmes; Fusion 360 of Autodesk, Inc.; FreeCAD is
a trademark of the FreeCAD project. Designer is **not affiliated with, endorsed
by, or sponsored by** any of them.

## How these names are used here

Where a name is not needed it is not used — this project is described on its
own terms, not by comparison. It is not offered as a replica of, or a drop-in
replacement for, any of them.

What remains is **referential**: naming someone else's product to refer to that
product. Three places, and nowhere else:

1. **Interoperation.** `tools/onshape_import.py` reads documents out of the
   Onshape API, so it names the format it reads and the operations it maps
   (`splitPart`, `fitSpline`, their face and edge queries). A tool that reads
   another system's files has to be able to say which system.
2. **Prior art in the design record.** The `docs/` design documents cite what
   other CAD systems do and why: a decision recorded as "we chose this because
   a named product does it, and here is where that stops working" can be
   checked later, and one recorded as "because CAD systems do" cannot. That
   citation belongs in the design record — the **source comments carry none of
   it** (2026-08-31: all 37 were rewritten to describe the convention rather
   than name a product), because a comment beside the code is read as part of
   the code, not as a bibliography.
3. **One licence attribution.** PlaneGCS is vendored from FreeCAD, so the About
   dialog and [`THIRD_PARTY.md`](THIRD_PARTY.md) name it. That attribution is
   an obligation, not a choice.

No third-party logos or branding are used, no compatibility or certification is
claimed, and no name appears in the application's own interface or its source —
no menu item, label, tool description, `help()` text or code comment names
another product. The single user-visible exception is the FreeCAD attribution
above.
