# Inspection — measure, mass properties, sections — plan

> **Status 2026-07-14: v1 complete.** I1 measure (RPC + GUI tool, C3), I2 mass
> properties (RPC + panel + CoG marker, C3), I3 section (geometry + raster
> clip + filled caps, C4), I4 interference — all shipped. Deferred polish:
> hatched cut faces, angle/point measure, bbox readout,
> measurement-driven variables, curvature/thickness maps.
>
> **2026-08-19:** assembly section caps shipped, along with a fix for the
> cutter being aimed at the world origin (any model built away from 0,0,0 cut
> nothing). The section UI was replaced wholesale — one plane in a View menu
> became a tool with a list, drag arrows and up to four intersecting cuts
> (P1). **2026-08-21: P2 + P3 shipped** — datum-plane and picked-face
> sources resolved live, tilt angles that pivot about the offset point, and
> sections that PERSIST with the project (they were session state and
> vanished on save) plus `add/list/update/remove_section`. Details and the
> one deliberate deviation — an unresolvable source stops its cut instead of
> falling back — in [section-tool-design.md](section-tool-design.md).
>
> **2026-08-20:** contact checks run their boolean with a 1 µm fuzz, so parts
> designed to touch report contact instead of a sliver-sized "clash".

The "is it right?" toolset: measuring geometry, mass/volume properties, and
seeing inside a part. Small, high-value, and disproportionately useful to the
LLM interface (a model that can *ask* "how far apart are these faces / what
does it weigh" closes the text-to-cad verification loop — see
[scripting/plan.md](../scripting/plan.md)).

## I1 — Measure tool — **GUI shipped (C3, July 2026)**

Select two things → get the answer. **Shipped:** the `measure` RPC (I1
headless) plus the GUI Measure tool (C3) — a toolbar-armed viewport mode
(`state.measure`, `HandleMeasureInput`) that picks a face or edge (reusing
the fillet edge-pick + `FaceElementRef` machinery), reads **face area / edge
length / circular-edge radius** for one element and **min distance** (OCC
`BRepExtrema_DistShapeShape`) for two, into a Properties-panel readout. The
tool alternates describe → distance on successive clicks. Radius was added to
`Measure::Describe` (`BRepAdaptor_Curve` → `GeomAbs_Circle`).
**Deferred polish:** point picking, angle-between-faces/edges, and the
unified-selection (U1) integration + floating readout overlay.

## I2 — Mass properties — **shipped; panel + CoG marker added (C3)**

`BRepGProp::VolumeProperties/SurfaceProperties` → volume, surface area,
center of mass, inertia matrix; `CADMaterials` density → mass. The
`mass_properties` RPC shipped early; **C3 added the GUI**: a Mass properties
section in the Properties dock (volume / surface area / mass with the
null-taint rule / center of mass, cached by document version via
`Measure::MassOfBodies`) and an amber CoG cross drawn at the center of mass
in the no-depth overlay channel (`AppendCogMarker`, `state.showCog`).

## I3 — Section view (clipping plane)

**Geometry half shipped (July 2026):** `Measure::CrossSectionArea` +
`section` RPC/MCP — cut all bodies with a plane (base_plane+offset or
origin+normal), return the summed cross-section area (BRepAlgoAPI_Common
against a big planar face). Headless-testable; the LLM can check material at
a cut.

**Visual half shipped (July 2026):** the raster clipping plane — a vec4
clip plane through the mesh push constant (`discard` in `mesh.frag`) and the
wireframe UBO (`discard` in `wireframe.frag`), `AppState.sectionPlane` +
`set_section` RPC/MCP + a View > Section View menu (enable / plane / offset).

**Section caps shipped (C4, July 2026):** the cut no longer reads hollow.
`Measure::SectionCaps` intersects each body with the section plane
(`BRepAlgoAPI_Common` against a big planar face, sharing CrossSectionArea's
cutter) and returns the cap faces; `SyncDocumentRenderers` tessellates them
into **double-sided** opaque instances (both windings so the fill shows from
either orbit side), nudged onto the kept half so the global shader clip
keeps them, in the body's color. Caps rebuild when the plane changes and no
widget is being dragged (they settle on slider release; the shader clip
previews live). Part-tab only when written; **assembly caps shipped
2026-08-19**, and **hatching** stays deferred (cosmetic, wants the drawings
hatch style).

**The tool (P1–P3, August 2026):** the View menu entry is gone. Sections are
a tool with a list — up to four intersecting cuts, each with a source (base
plane, datum plane, or a picked planar face), an offset with a viewport drag
arrow, two tilt angles, flip and visibility — and they are saved with the
project. See [section-tool-design.md](section-tool-design.md).

## I4 — Later

- **Bounding box / quick dimensions** readout for a selection.
- **Interference check** between bodies (OCC `Common` non-empty ⇒ clash) —
  becomes important with assemblies; trivial to expose then.
- **Measurement-driven variables** (bind a variable to a measurement) — wants
  variables + this; deferred.
- **Draft/thickness analysis, curvature maps** — out of scope for now.

## Not here: 2D drawings

Dimensioned drafting output (the classic "drawing" document) is a separate,
large workstream — deferred until after assemblies, and now designed: see
[drawings/plan.md](../drawings/plan.md). The measure tool + section views
cover the day-to-day inspection need until it ships.

## Recommended order

I2 first (cheap, LLM-useful, no prerequisites) → I1 (face/point subset, then
edges once edge picking lands) → I3 → I4 with assemblies.
