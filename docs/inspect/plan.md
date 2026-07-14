# Inspection — measure, mass properties, sections — plan

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
V1 leaves the cut hollow; capped/hatched cut faces (real section geometry
via BRepAlgoAPI_Section) are v2.

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
