# X4 — STEP export with names, colours and structure

## The problem

A STEP file is how a part leaves this app for a shop or another CAD system,
and ours carries only the geometry. `CADKernel::SaveSTEP` is a bare
`STEPControl_Writer` loop: every body becomes an anonymous root shape, with
no name, no colour and no product structure. An assembly is worse. The
`export_step` handler moves each occurrence's shape to its solved placement
and writes the lot as a flat pile of solids. Opened in another CAD system, a
twelve-bolt assembly arrives as twelve unrelated copies of the bolt's
geometry, all called nothing, and the receiving BOM is empty.

The writer that fixes this already exists. `SaveSTEPStructured` writes named,
coloured products and named, placed instances through XCAF, and the import
tests use it to author their fixtures. No export path calls it.

Three smaller faults sit on the same path:

- **The GUI cannot export an assembly to STEP.** On an assembly tab the Export
  dialog offers only glb/gltf/obj/ply, and a typed `.step` is refused with
  "needs a part tab" (`ui/FileDialogs.cpp`).
- **Exported colour is not the colour on screen.** `FillItemAppearance` uses
  material, then a palette, and ignores `body.appearance` and
  `displayColor`. The renderer resolves appearance > material > displayColor
  > palette (`RendererSync.cpp`). So glTF has always dropped user appearance
  overrides and colours imported from STEP. The palette index also advances
  per item, so two occurrences of one unpainted part export in different
  colours.
- **Units are implicit.** No writer names millimetres; both rely on OCC's
  default.

## The shape of the fix

### 1. An export model that is a tree

A new `CADKernel::StepExportModel` replaces the flat `ImportedModel` as the
writer's input:

- **Products:** a leaf part. Each has one or more bodies, and each body has a
  shape, a name and a colour. A single-body part is a single product. A
  multi-body part is a product that is itself an assembly of body products,
  so every body keeps its own name and colour. Colour in STEP attaches to a
  product, and there is no other way to keep both.
- **Assemblies:** a name plus components, each component being {child product
  or assembly, local rigid transform, instance name}.
- **A root.**

`SaveSTEPModel` writes this recursively (products first, then assemblies
bottom-up via `AddComponent`), sets millimetres explicitly on the document
and the writer, and keeps the old writer's contract: it holds
`DataExchangeMutex`, uses the scoped `XcafDocument`, and fails, naming the
cause, on a transform `gp_Trsf` cannot represent. `SaveSTEPStructured`
(flat `ImportedModel`) becomes a thin adapter onto it, so the import tests
that author fixtures with it do not move.

**A body's location is baked into its geometry.** XCAF turns a LOCATED shape
into an assembly of one placed component, auto-named after its type. A body
at a position arrives as a product called `SOLID` inside the one we named,
and the name and colour stay on the outer product. The location is where the
body sits in its part, which is what the product must hold. Colours go in
linear (`Quantity_TOC_RGB`), as the shaders light them, and OCC writes sRGB
into the file.

### 2. Building it from a document — `src/ui/StepExport.{h,cpp}`

One builder, with one entry point per tab kind, called by the RPC and the
GUI. The tube shop writes one member per file, so it hands the writer a
one-product model directly.

- **Part tab:** the root is the part. The product is named after its
  `PartSlot`, with one body product per non-reference body (the `body` index
  still narrows it to one). Reference (Derived) bodies are never written, as
  now.
- **Assembly tab:** built from `assembly::Flatten`, the one tree walk
  (`core/AssemblySolve.h` says why consumers must not grow their own). Each
  `FlatInstance` now also carries the levels above it (each sub-assembly
  placement's occurrence id, slot id, configuration id, local transform and
  name), plus its own occurrence id, local placement and name. That is
  enough to rebuild the tree, and it inherits `Flatten`'s visibility,
  suppression, and cycle and depth rules instead of copying them.
  - A part occurrence becomes a component referencing that part's product,
    deduplicated on `partId`. A part always regenerates in its document's
    active configuration (per-occurrence part configs are deferred), so the
    id is a sufficient key.
  - A sub-assembly occurrence becomes a component referencing a nested
    assembly, deduplicated on (slot id, configuration id), because one
    sub-assembly placed in two configurations has two contents.
  - Hidden and suppressed occurrences are left out, as now.
  - The instance name is the occurrence name. Occurrence names are not
    unique, and STEP does not require them to be, so they are written as the
    user wrote them.
- **Colour** comes from ONE resolver, extracted from `RendererSync.cpp`'s
  order and shared with it: appearance > material > displayColor > palette.
  The palette is seeded the way the tab showing the part seeds it: from 0
  on a part tab, from the part's id on an assembly tab. So every occurrence
  of a part carries the same colour. `FillItemAppearance` switches to the
  same resolver, which fixes glTF export in the same change. The part-tab
  glTF export also stops writing Derived reference bodies. The resolver
  returns the value the shaders light, which is linear (`mesh.frag` applies
  gamma at the end).

### 3. Callers

- **`export_step`:** both branches go through the builder. The reply keeps
  `bodies` (part) and `shapes` (assembly), both of which the smokes read, and
  adds `products` and `instances`. Its `describe` row always promised
  "(or {part})", but `part` was not an accepted parameter. It is now, from
  any tab.
- **GUI Export:** an assembly tab offers STEP first, beside the visual
  formats. Everything after the dialog moves into
  `ui::ExportActiveDocument(state, path)`, because a native dialog cannot be
  driven by a test and the function after it can be.
- **Tube shop:** the per-member file names its product after the member
  (`RemoteTube.cpp`).
- **`SaveSTEP` stays** for the non-document callers (tests and baked-geometry
  sidecars) that write raw shapes.

### 4. Readers of our own files

A structured file is read differently from a pile of roots:

- **`LoadSTEP`** (used by `compare_geometry`) returns one `CADShape` per root.
  A structured file has ONE root, the top assembly, so a multi-body part
  would come back as one compound. It changes to read leaves, one shape per
  placed body, walking the product tree as `LoadSTEPStructured` does but
  composing exact `TopLoc_Location`s rather than float matrices.
- **The benchmark oracle** reads flat (`STEPControl_Reader`, `OneShape`) and
  counts solids with `TopExp_Explorer`. That visits located instances, so
  solid count, volume and per-solid COM should be unchanged. Should is not a
  claim: the three runners below decide it.
- **`import_step`** already reads XCAF, and now also gets names and colours
  back.

## Files

- `src/cad/CADKernel.{h,cpp}`: `StepExportModel`, the recursive writer, the mm
  unit, the flat adapter, `LoadSTEP` reading leaves.
- `src/core/AssemblySolve.{h,cpp}`: `FlatInstance` levels and its own
  placement.
- `src/ui/StepExport.{h,cpp}` (new): the builder.
- `src/ui/BodyAppearance.{h,cpp}` (new): the shared colour resolver, moved
  out of `RendererSync.cpp`'s two copies. `RendererSync` and
  `FillItemAppearance` (`AssemblyActions.cpp`) both call it.
- `src/remote/RemoteProject.cpp`: the `export_step` handler and its
  `MethodDoc` row.
- `src/remote/RemoteTube.cpp`: the member product name.
- `src/ui/FileDialogs.{h,cpp}`: STEP on assembly tabs, and
  `ExportActiveDocument`.
- `tools/designer_mcp.py`: the `export_step` description ("one shape per
  body" stops being true).
- Docs: `docs/interchange/plan.md` (X1 wording, and assemblies no longer
  "deferred"), `docs/manufacturing-gaps.md` ("STEP goes out naked"),
  `docs/scripting/mcp.md`, `benchmarks/README.md`.

No document format change: nothing is stored.

## Verification

- **Round-trip unit tests** (`tests/test_cadkernel.cpp`, `tests/test_step_export.cpp`):
  - A two-part assembly with one part placed three times, plus a nested
    sub-assembly placed twice (once turned) and a hidden occurrence,
    exports and is re-read with `STEPCAFControl_Reader`. The test walks the labels and asserts:
    - two leaf products, not five;
    - the sub-assembly written once and referenced twice;
    - every instance name and world transform;
    - each product's colour equal to the resolver's colour within 1/255;
    - the length unit is mm.
  - A multi-body part exports as one product with named, coloured body
    children.
  - A part with an appearance override, and an unpainted part placed twice,
    export in the on-screen colour, and in one colour for both occurrences.
    The same case runs through glTF.
  - A body that is a compound of one solid keeps its name and colour.
  - `Flatten`'s levels compose to exactly its `world`
    (`tests/test_assembly_flatten.cpp`).
- **`LoadSTEP`** on a structured multi-body export returns one shape per body.
- **The existing smokes that go through `export_step`**, unchanged:
  `headless_smoke` (body count, re-import origin), `toolmakers_clamp_smoke`
  (eight solids, conserved volume), `configuration_smoke`, `derived_smoke`,
  `batch_smoke`, `sdk_smoke`.
- **Export on an assembly tab** through `ExportActiveDocument`, headless:
  STEP writes, and STL and an unknown extension are refused by name.
- **Benchmark:** `benchmarks/runners/reference.py` must still score every
  check, and `negative_controls.py` and `villain.py` must still reject what
  they reject. `export_step` is an MCP tool, so there is no claim without
  them.
- **Full suites** before committing: headless, GUI, and `tools/ci-local.sh --nix`.
