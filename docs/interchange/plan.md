# Interchange — export (and import widening) — plan

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

### X2 — glTF export (+ OBJ/PLY if trivial) (~1 day)

The visualization interchange: Blender, web viewers, game engines.
`RWGltf_CafWriter` (and `RWObj_CafWriter` / `RWPly_CafWriter`) work off an
**XCAF document** rather than a bare shape — the milestone is mostly building
the small shape→XCAF bridge (`TDocStd_Document` + color/name attributes),
which then serves all three writers. Map `CADMaterials` colors so exported
parts keep their appearance. Binary `.glb` default.

### X3 — Import widening (later, demand-driven)

- **IGES** import/export (`IGESControl_Reader/Writer`) — legacy interchange,
  cheap, add when someone needs it.
- **STL/OBJ import as mesh bodies** — `LoadSTL` exists but yields a triangle
  shell, not a B-Rep solid; useful only as a reference/visualization body.
  Needs a "mesh body" concept (non-parametric, render + measure only). Defer
  until a real need appears.
- **DXF** (2D sketch interchange) — not in OCC; out of scope for now, same
  status as 2D drawings.

## Explicitly deferred

- **3MF** — the modern print format, but OCC has no writer; needs a `lib3mf`
  dependency. STL covers printing v1; revisit if slicer-metadata (units,
  colors) demand appears.
- **Export of assemblies** — X2's XCAF bridge is where assembly structure
  (occurrences, transforms) would slot in naturally; design X2 so that adding
  it later is additive (see [assemblies/plan.md](../assemblies/plan.md)).

## Recommended order

X1 as a near-term one-day win (no prerequisites; pairs naturally with
inspect I2's "is it right?" story) → X2 when a demo/render need appears →
X3 on demand.
