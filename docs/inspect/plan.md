# Inspection — measure, mass properties, sections — plan

The "is it right?" toolset: measuring geometry, mass/volume properties, and
seeing inside a part. Small, high-value, and disproportionately useful to the
LLM interface (a model that can *ask* "how far apart are these faces / what
does it weigh" closes the text-to-cad verification loop — see
[scripting/plan.md](../scripting/plan.md)).

## I1 — Measure tool

Select two things → get the answer: point–point / point–face / face–face
distance (OCC `BRepExtrema_DistShapeShape`), edge length, angle between
faces/edges, circle/arc radius from an edge. UI: a Measure mode using the
unified selection (ui U1) with a readout overlay + a docked results panel.
**Needs edge picking** (modeling cross-cutting) for the edge cases; the
face/point subset can ship before it. RPC: `measure(ref_a, ref_b)`.

## I2 — Mass properties (nearly free — do early)

`BRepGProp::VolumeProperties/SurfaceProperties` → volume, surface area,
center of mass, inertia matrix. `CADMaterials` gains a **density** per
material → mass. Panel showing the selected body's (or whole document's)
properties; a CoG marker in the viewport. RPC: `mass_properties(body_id)`.
Tests: analytic solids (box, cylinder, extruded rect with hole) match to
tolerance. This is a day of work sitting on top of what OCC already computes.

## I3 — Section view (clipping plane)

**Geometry half shipped (July 2026):** `Measure::CrossSectionArea` +
`section` RPC/MCP — cut all bodies with a plane (base_plane+offset or
origin+normal), return the summed cross-section area (BRepAlgoAPI_Common
against a big planar face). Headless-testable; the LLM can check material at
a cut.

**Visual half (deferred — needs a reviewable session):** the raster
clipping plane — a clip plane in the mesh + outline push constants, `discard`
beyond it in `mesh.frag`/`outline.frag`, an AppState toggle + RPC + drag UI.
It is a shader-ABI change across both renderers, verifiable only by eye, so
it was deliberately left for a session where the render can be reviewed
rather than landed autonomously. V1 leaves the cut hollow; capped/hatched
cut faces are v2.

## I4 — Later

- **Bounding box / quick dimensions** readout for a selection.
- **Interference check** between bodies (OCC `Common` non-empty ⇒ clash) —
  becomes important with assemblies; trivial to expose then.
- **Measurement-driven variables** (bind a variable to a measurement) — wants
  variables + this; deferred.
- **Draft/thickness analysis, curvature maps** — out of scope for now.

## Not here: 2D drawings

Dimensioned drafting output (the classic "drawing" document) is a separate,
large workstream — consciously **not planned** until after assemblies (see
roadmap "Explicitly not planned"). The measure tool + section views cover the
day-to-day inspection need until then.

## Recommended order

I2 first (cheap, LLM-useful, no prerequisites) → I1 (face/point subset, then
edges once edge picking lands) → I3 → I4 with assemblies.
