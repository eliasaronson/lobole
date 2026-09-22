# Interchange — export (and import widening) — plan

> **Status 2026-09-22: v1 complete, and STEP export is structured.** X1
> (STEP/STL/BREP) and X2 (glTF/OBJ/PLY, parts + assemblies) are shipped and
> reachable from the File menu, RPC, and MCP. X4 rewrote STEP export for
> parts AND assemblies: named products, one per part, placed per occurrence,
> nested sub-assemblies, on-screen colours, explicit millimetres
> ([step-export-plan.md](step-export-plan.md)). X3 (IGES / mesh import / DXF)
> and 3MF are **explicitly demand-driven** — added when a real need appears,
> not before. Nothing here blocks other workstreams.

Getting geometry *out* of Designer. STEP import shipped early (modeling), but
no export is reachable from the UI or RPC — even though the kernel already has
`CADKernel::SaveSTEP` / `SaveSTL` (`src/CADKernel.h:116,119`) with **zero
callers**. Export is table stakes for a real CAD tool: STL/3MF is how a part
gets 3D-printed (the most likely first real-world use), STEP is how models
move to other CAD, and glTF is how they reach renderers and web viewers. Also
disproportionately LLM-useful: an assistant that can export closes the
"model → use it somewhere" loop.

## Current state

- Kernel: `SaveSTEP(filename, shapes)` (multi-shape, `STEPControl_AsIs`) and
  `SaveSTL(filename, shape)` exist and are dead code. `LoadSTL` also exists,
  unused (and would import a triangle shell, not a solid — see X3).
- `SaveSTL` uses `StlAPI_Writer` defaults: **ASCII** output, and it writes
  whatever triangulation the shape already carries (the render mesh) — no
  explicit quality control.
- Nothing in the File menu, no RPC, no tests.
- OCC 7.9.3 (nix) — all writers below are available.

## Milestones

### X1 — STEP + STL export, exposed end-to-end (~0.5–1 day)

The 90% milestone; everything sits on existing kernel code.

- **File → Export…** with format chosen by extension; export the whole
  document (STEP: one shape per body) or the selected body/bodies.
  (2026-09-22: superseded by X4 — a part is now one named product, with a
  named, coloured body product per body; see below.)
- **STL**: binary by default (`StlAPI_Writer::ASCIIMode() = false`); re-mesh
  explicitly via `BRepMesh_IncrementalMesh` with a user-visible **deflection /
  quality** parameter instead of reusing whatever the viewport happened to
  tessellate. One solid per file (STL has no part structure); exporting many
  bodies fuses or writes one file per body — decide in-milestone, Onshape
  fuses.
- **BREP** (`BRepTools::Write`) nearly free at the same time — the native OCC
  debug format; invaluable for kernel bug reports upstream.
- **RPCs** `export_step(path, body_ids?)` / `export_stl(path, body_id?,
  deflection?)` + MCP tools (scripting parity — the LLM exports the same way).
- **Units note**: document units are mm; STEP carries units explicitly, STL is
  unitless-by-convention (mm for printing) — document it in the dialog + RPC.
- Tests: export → re-import → volume/CoG match to tolerance (STEP round-trips
  via the existing import path; STL re-imports as a mesh so assert triangle
  count/AABB instead). Headless mode makes these plain ctest cases in CI.

### X2 — glTF export (+ OBJ/PLY) — **SHIPPED (July 2026)**

The visualization interchange: Blender, web viewers, game engines. Shipped as
planned: `src/MeshExport.{h,cpp}` builds one shape→XCAF bridge
(`TDocStd_Document` + `XCAFDoc_ShapeTool`/`ColorTool` labels + a
`XCAFDoc_VisMaterial` per node) that feeds `RWGltf_CafWriter` /
`RWObj_CafWriter` / `RWPly_CafWriter`, format chosen by extension
(`.glb` binary default / `.gltf` / `.obj` / `.ply`).

- **Materials carry through as real PBR**: `CADMaterials` baseColor/metallic/
  roughness → `XCAFDoc_VisMaterialPBR` (glTF is natively metallic-roughness),
  so an export looks like the viewport; an unassigned body gets its palette
  color. Verified: a steel body exports `metallicFactor > 0.5`.
- **Assemblies export too** (decision 1): `ui::BuildVisualExportItems` walks a
  part's bodies (identity) *or* the active assembly's occurrences at their
  **solved** placements (never exploded). Occurrences of one part share the
  underlying `TShape`, so the writers emit **instanced** geometry — verified
  (two pin occurrences → one mesh, three placed nodes).
- **Surface**: `export_visual(path, deflection?)` RPC (part or assembly tab) +
  MCP tool; **File → Export…** offers visual + STEP (assembly tabs, since
  X4) or visual + STEP/STL/BREP (part tabs). `visual_export_smoke` parses the binary
  glTF container directly (no external deps) and asserts meshes/materials/
  instancing.
- OCC 7.9.3's writers split into `TKDEGLTF`/`TKDEOBJ`/`TKDEPLY` (7.8+); the
  CMake block appends them beside `TKDESTEP`, and the XCAF stack
  (`TKXCAF`/`TKLCAF`/`TKCAF`/`TKVCAF`/`TKRWMesh`) is linked into
  `designer_core`.

### X3 — Import widening (later, demand-driven)

- **IGES** import/export (`IGESControl_Reader/Writer`) — legacy interchange,
  cheap, add when someone needs it.
- **STL/OBJ import as mesh bodies** — `LoadSTL` exists but yields a triangle
  shell, not a B-Rep solid; useful only as a reference/visualization body.
  Needs a "mesh body" concept (non-parametric, render + measure only). Defer
  until a real need appears.
- **DXF** (2D sketch interchange) — not in OCC. A writer now exists for
  sheet-metal FLAT PATTERNS (`src/core/io/ExportDxf.cpp`, `export_dxf`): R12
  ASCII, millimetres, LINE/ARC/CIRCLE on CUT / BEND_UP / BEND_DOWN plus an
  optional TEXT mark on MARK, arcs kept as arcs. R12 has no ELLIPSE (R14
  added it), so a cut that leaves one — a tube through a plate at an angle —
  tessellates to line segments. A general drawing DXF over `ProjectedCurve` is still open and would
  reuse that writer's entity half.

### X4 — STEP export with names, colours and structure — **SHIPPED (2026-09-22)**

[step-export-plan.md](step-export-plan.md) is the design. `export_step` and
File → Export go through one builder, `src/ui/StepExport.{h,cpp}`, onto
`CADKernel::SaveSTEPModel` (XCAF); the tube shop's `export_members` writes
each member's file through the same writer, as one product named after the
member.

- **Part tab:** one product named after the tab; a multi-body part is a
  product that is an assembly of named body products. Derived reference
  bodies are never written; `body` still narrows it to one.
- **Assembly tab:** built from `assembly::Flatten` (whose `FlatInstance` now
  carries the sub-assembly `levels` above each leaf). Each part is ONE
  product, placed once per occurrence under the occurrence's name; each
  (sub-assembly, configuration) is one nested STEP assembly. Hidden and
  suppressed occurrences are left out. The GUI offers STEP on an assembly
  tab; STL and BREP stay part-only.
- **Colour** is what the viewport shows (appearance > material >
  displayColor > palette), from one resolver `RendererSync` shares. glTF
  export uses it too, so appearance overrides and imported colours now
  survive a glTF export and every occurrence of a part is one colour; a
  part-tab glTF export no longer writes Derived reference bodies either.
- **Units** are written as millimetres.
- `export_step` accepts `part` from any tab; the reply adds `products` and
  `instances` beside `bodies` / `shapes`.
- `LoadSTEP` (`compare_geometry`) reads through XCAF and returns one shape
  per placed leaf, so a structured multi-body file still gives one shape
  per body.

No format change. Not carried: PMI / AP242 GD&T, and per-occurrence part
configurations (a part exports in its document's active configuration).

## Explicitly deferred

- **3MF** — the modern print format, but OCC has no writer; needs a `lib3mf`
  dependency. STL covers printing v1; revisit if slicer-metadata (units,
  colors) demand appears.

## Recommended order

X1 as a near-term one-day win (no prerequisites; pairs naturally with
inspect I2's "is it right?" story) → X2 when a demo/render need appears →
X3 on demand (X4 shipped out of order, 2026-09-22).
