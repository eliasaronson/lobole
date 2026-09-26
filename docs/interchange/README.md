# Interchange: import and export

Getting geometry into and out of Designer. Document storage and the
project format are in [../versioning/README.md](../versioning/README.md).

| format | import | export |
|---|---|---|
| STEP (`.step`, `.stp`) | names, colours, product structure; a part or a whole assembly | named, coloured products placed per occurrence, nested sub-assemblies, millimetres |
| BREP (`.brep`) | exact | exact; one shape (a part's bodies fused, or one body) |
| STL (`.stl`) | sewn triangles: a solid when closed, a shell when open | binary, re-meshed at a chord tolerance; one shape |
| glTF (`.glb`, `.gltf`), OBJ, PLY | — | visual meshes with PBR materials, part or assembly, instanced |
| DXF | — | a sheet-metal flat pattern (see [../sheetmetal/README.md](../sheetmetal/README.md)) |
| SVG, PDF | — | drawing sheets (see [../drawings/README.md](../drawings/README.md)) |

Native SolidWorks files are not read: their feature tree is undocumented and
their geometry is Parasolid. From SolidWorks, save as STEP AP242 and import
that.

All OpenCASCADE data exchange (the STEP readers and writers, the XCAF
document) runs under one process-wide lock, `DataExchangeMutex()` in
`src/cad/XcafDocument.h`.

## Importing into a part

An import into a part is an `Import` feature (`ImportFeature` in
`src/core/Feature.h`, executed in `src/core/Feature.cpp`): the file path is
the recipe, and each regeneration reads it.

- **STEP** is read through XCAF (`CADKernel::LoadSTEPStructured`): unique
  products with their names and colours, and the instance tree flattened to
  placed leaves. Each placed leaf becomes a body named after its product (not
  the writer's instance name, `Widget` rather than `Widget:1`), coloured by
  the product's STEP colour unless a material is assigned. Colour is display
  only; it is not a simulation material.
- **STL** becomes one planar face per triangle, sewn: a closed mesh arrives
  as a solid, an open one as a shell. Every triangle is a face, so organic
  meshes are heavy.
- **BREP** is read as is.
- **Placement.** By default the combined bounding box is centred on the
  origin, keeping the shapes' relative placement. `keep_origin` keeps the
  file's own coordinates, which is what an import whose coordinates mean
  something needs (a neighbour part, a fixture, a re-import of our own
  export). `add_feature` reports the shift it applied as `recentred_by`.
- **Units.** STEP declares its own unit. STL and BREP carry none, so the
  feature's `units` (`mm` default, `cm`, `m`, `in`) scales the shape once as
  it is read; it is refused on a STEP and an unknown string is refused. An
  STL or BREP that comes out under 1 mm across gets a warning naming the
  fix.
- **At most 500 shapes** are imported from one file, with a log line when
  capped; the cap guards against pathological files only.
- **Face names** carry the body ordinal (`<id>/b<N>/…`), because every body
  of one import shares its producing feature.
- **The chain hash keys on the file's bytes**, not its path
  (`ImportFeature::ContentHash`, cached by path + size + mtime), so moving a
  staged file into the project on save does not invalidate anything built on
  it.
- **One parse per import.** When the caller already parsed the file (the GUI
  worker does, to decide part vs assembly), the model is handed to the
  feature (`SeedGeometry`) and consumed by its first regeneration. It is not
  serialized and not sticky: later regenerations re-read the path, so editing
  the source file still updates the part.

## Importing an assembly

A STEP with several placed instances becomes an assembly
(`src/core/io/ImportAssembly.{h,cpp}`, `ui::ImportStepAsAssembly` in
`src/ui/AssemblyActions.cpp`):

- `interchange::BuildAssemblyImportPlan` takes an already-parsed model and
  writes one `.brep` per unique product, centred at its own origin. Each
  occurrence's placement composes the instance transform with that centring,
  so the parts land where the file put them. A non-rigid instance transform
  imports as its closest rigid placement.
- Materializing creates one part tab per unique product (an `Import` of its
  `.brep`, carrying the product's colour on the feature) and one assembly
  that places them. A hundred identical screws are one part and a hundred
  occurrences. Names come from the file, made unique among the parts.
- The `.brep` files go to `<project>/imported/<name>/`, or, before the
  project has been saved, to `<user state dir>/import-staging/<name>/`. The
  first save into a directory moves staged files into the project and
  repoints the features, so a saved project is self-contained.
- It is not undoable; like Open, it creates documents.

**In the GUI**, File > Import (Ctrl+I, `file.import`) accepts STEP, STL and
BREP. STL and BREP import straight into the active part. A STEP runs as a
background job (`src/ui/ImportJob.{h,cpp}`) with a progress modal: the
worker reads the file (no progress available from OpenCASCADE), transfers
it (a real percentage), and, when it has more than one placed instance,
extracts the `.brep` files; the main thread then creates the tabs and
occurrences a slice per frame. The worker touches only plain data; every
document mutation stays on the main thread. The job can be cancelled until
materializing starts, and only one import runs at a time. A STEP with one
placed instance becomes an `Import` feature in the active part. The
progress callback is sampled, not read per tick: OpenCASCADE's transfer
reports hundreds of thousands of times on a large file.

**Over RPC**, `import_step {path, as_assembly?, keep_origin?, units?}`:
without `as_assembly` it adds an `Import` feature to the active part and
returns `{bodies, regen, digest}`; with it, it returns `{assembly_index,
occurrences, parts, ok}` plus `failed` naming every part whose feature did
not build. `units` is refused with `as_assembly`.

The part tab's feature editor exposes the import's file units; `keep_origin`
is reachable over RPC and in the feature JSON only.

## Exporting STEP

`src/ui/StepExport.{h,cpp}` builds a `CADKernel::StepExportModel` — parts,
assemblies and a root — and `CADKernel::SaveSTEPModel` writes it through
XCAF. The RPC and the Export dialog both go through `ui::ExportStep`, so a
file carries the same structure whichever way it was asked for.

- **A part tab** exports one product named after the tab. One body is the
  product itself; several make it an assembly of named, coloured body
  products, because STEP colours attach to products. Derived reference
  bodies are never written. `body` narrows the export to one body.
- **An assembly tab** is built from `assembly::Flatten`, the one tree walk,
  whose `FlatInstance::levels` record the sub-assembly placements above each
  leaf. Each part is written once and placed per occurrence under the
  occurrence's name; each (sub-assembly document, configuration) is one
  nested STEP assembly, since one sub-assembly in two configurations has two
  contents. Hidden and suppressed occurrences are left out. Each part exports
  in its document's active configuration.
- **Colour** is what the viewport shows: one resolver,
  `ui::ResolveBodyLook` (`src/ui/BodyAppearance.{h,cpp}`), shared with
  `RendererSync`: appearance override > material > imported display colour >
  palette. The palette is seeded the way the showing tab seeds it, so every
  occurrence of an unpainted part is one colour. Colours are written linear,
  as the shaders light them.
- **Units** are written as millimetres, explicitly.
- **A body's location is baked into its geometry.** XCAF turns a located
  shape into an assembly of one auto-named component, which would push the
  name and colour one level away from the solid.
- A transform `gp_Trsf` cannot represent is refused, naming it.

`export_step {path, part?, body?}` works from any tab (`part` names another
part) and replies `bodies` (part) or `shapes` (assembly) — placed body
shapes — plus `products` and `instances`. The tube shop's `export_members`
writes one STEP per member, each one product named after the member.

`CADKernel::SaveSTEP` (a flat list of anonymous shapes) remains for callers
that write raw shapes, and `SaveSTEPStructured` writes a flat `ImportedModel`
through `SaveSTEPModel` for the import tests' fixtures.

**Reading our own files back.** `CADKernel::LoadSTEP` (used by
`compare_geometry {golden_path}`) returns one shape per placed leaf, so a
structured multi-body file still yields one shape per body.

## Exporting STL and BREP

`export_stl {path, body?, deflection?}` and `export_brep {path, body?}`
write the active part. Without `body` the non-reference bodies are fused
into one shape; STL and BREP hold one shape each. STL is binary and
re-meshed at `deflection` (chord tolerance in mm, default 0.1), never the
viewport's tessellation; the reply reports whether the source B-Rep was
watertight. BREP is OpenCASCADE's exact native form. STL has no unit and is
millimetres by convention.

## Exporting visual meshes

`export_visual {path, deflection?}` and the Export dialog write glTF (`.glb`
binary, `.gltf`), OBJ or PLY by extension (`src/cad/MeshExport.{h,cpp}`,
OpenCASCADE's `RWGltf_CafWriter` / `RWObj_CafWriter` / `RWPly_CafWriter`).

- `ui::BuildVisualExportItems` walks a part's bodies, or the active
  assembly's visible occurrences at their SOLVED placements (never exploded).
  Occurrences of one part share the underlying shape, so the writers emit
  instanced geometry. Reference bodies are left out.
- Materials are written as PBR (base colour, metallic, roughness) from the
  same colour resolver as STEP.
- glTF is written in metres and Y-up, as the format defines; OBJ and PLY
  stay in millimetres, Z-up.

## The Export dialog

File > Export (`file.export`; `ui::ExportDialog` in `src/ui/FileDialogs.cpp`)
offers STEP first. A part tab offers STEP, STL, BREP, glTF, OBJ and PLY; an
assembly tab STEP and the visual formats; a drawing tab SVG and PDF. After
the dialog everything is `ui::ExportActiveDocument(state, path)`, which the
tests drive directly because a native dialog cannot be driven. STL and BREP
on an assembly tab, and unknown extensions, are refused by name.

## RPCs

| RPC | module | does |
|---|---|---|
| `import_step {path, as_assembly?, keep_origin?, units?}` | RemoteProject | STEP / STL / BREP into the part, or a STEP as parts + an assembly |
| `export_step {path, part?, body?}` | RemoteProject | structured STEP of a part or the assembly |
| `export_stl {path, body?, deflection?}`, `export_brep {path, body?}` | RemoteProject | one shape of the active part |
| `export_visual {path, deflection?}` | RemoteProject | glTF / OBJ / PLY of the part or assembly |
| `export_members {dir}` | RemoteTube | one STEP per tube member |
| `export_dxf` | RemoteSheetMetal | a flat pattern as R12 DXF |
| `add_feature` with `type: "Import"` | RemoteModeling | the feature directly; reports `recentred_by` |

## Tests

| where | covers |
|---|---|
| `tests/test_step_export.cpp` | an assembly exports as its tree (each part once, placed by reference, sub-assemblies nested, hidden occurrences left out); a part is one named product of its bodies; a one-solid compound keeps its name and colour; visual export colours every occurrence as the screen does; an assembly tab writes STEP and refuses STL by name |
| `tests/test_cadkernel.cpp` `[kernel]` | structured STEP import (names, colours, instances); STL units; the assembly import plan (centred parts, placed occurrences); no XCAF document left open |
| `tests/test_assembly_flatten.cpp` | `Flatten`'s levels compose to exactly its world placement |
| `tests/headless_smoke.py` | STEP / STL / BREP round trips, `keep_origin`, `as_assembly` |
| `tests/visual_export_smoke.py` | parses the binary glTF: meshes, materials, instancing |

`export_step` is an MCP tool, so a change to it is measured by the
benchmark's `reference.py`, `negative_controls.py` and `villain.py` too
(`benchmarks/README.md`).

## Not done

- IGES import or export.
- 3MF (OpenCASCADE has no writer).
- DXF of a drawing, and DXF import.
- Mesh bodies: an STL arrives as sewn B-Rep faces, not as a lightweight
  reference mesh.
- STEP PMI / AP242 GD&T, and per-occurrence part configurations in export.
- Fusion 360 (no server-side feature API) and native SolidWorks.
- `import_step` on a part does not report the centring shift the way
  `add_feature` does.
