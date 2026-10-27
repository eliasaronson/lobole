# Interchange: import and export

Getting geometry into and out of Loböle. Document storage and the
project format are in [../versioning/README.md](../versioning/README.md).

| format | import | export |
|---|---|---|
| STEP (`.step`, `.stp`) | names, colours, product structure; a part or a whole assembly; stood up by its [header](#up-axis) | named, coloured products placed per occurrence, nested sub-assemblies, millimetres, `Loböle` as the originating system |
| IGES (`.igs`, `.iges`) | as STEP, and trimmed surfaces [sewn into solids](#iges) | — |
| BREP (`.brep`) | exact | exact; one shape (a part's bodies fused, or one body) |
| STL (`.stl`) | sewn triangles: a solid when closed, a shell when open | binary, re-meshed at a chord tolerance; one shape |
| OBJ (`.obj`) | [each object a named, coloured body](#obj), sewn as an STL | visual meshes (below) |
| glTF (`.glb`, `.gltf`), OBJ, PLY | — | visual meshes with PBR materials, part or assembly, instanced |
| DXF | into a sketch ([sketching](../sketching/README.md#importing-dxf-and-svg)) | a sheet-metal flat pattern (see [../sheetmetal/README.md](../sheetmetal/README.md)) |
| SVG | into a sketch ([sketching](../sketching/README.md#importing-dxf-and-svg)) | drawing sheets (see [../drawings/README.md](../drawings/README.md)) |
| PDF | — | drawing sheets |

Native SolidWorks files are not read: their feature tree is undocumented and
their geometry is Parasolid. From SolidWorks, save as STEP AP242 and import
that.

All OpenCASCADE data exchange (the STEP, IGES and OBJ readers, the STEP
writer, the XCAF document) runs under one process-wide lock,
`DataExchangeMutex()` in `src/cad/XcafDocument.h`.

## Importing into a part

An import into a part is an `Import` feature (`ImportFeature` in
`src/core/Feature.h`, executed in `src/core/Feature.cpp`): the file path is
the recipe, and each regeneration reads it.

- **STEP** is read through XCAF (`CADKernel::LoadSTEPStructured`): unique
  products with their names and colours, and the instance tree flattened to
  placed leaves. Each placed leaf becomes a body named after its product (not
  the writer's instance name, `Widget` rather than `Widget:1`), coloured by
  the product's STEP colour unless a material is assigned. Colour is display
  only; it is not a simulation material. The label walk is one
  `ModelFromXcaf` that IGES and OBJ share, and the reader also returns the
  file's header (`ImportedModel::origin`) for the [up axis](#up-axis).
- **IGES** and **OBJ**: see [IGES](#iges) and [OBJ](#obj).
- **STL** becomes one planar face per triangle, sewn: a closed mesh arrives
  as a solid, an open one as a shell. OpenCASCADE's reader hands back the
  triangles as a compound of loose faces, so `CADKernel::LoadSTL` sews them
  as an OBJ's are (`SewAt`); unsewn, the volume still read right while a
  cut removed the wrong amount and Delete Face refused every hole. Every
  triangle is a face, so organic meshes are heavy: what that costs, and the
  limit past which an import asks first, are under
  [what a mesh costs](#what-a-mesh-costs).
- **BREP** is read as is.
- **What is skipped is named.** A placed shape with no volume (a lone
  surface) cannot be a body; the feature's warning names each one ("skipped
  1 of 3: \"Lid\" (a surface, not a solid)"), and when nothing is left the
  error does. It used to vanish without a word.
- **The extension decides the reader**, case-blind; any other is refused,
  naming `.step/.stp, .igs/.iges, .brep, .stl, .obj`.
- **Placement.** By default the combined bounding box is centred on the
  origin, keeping the shapes' relative placement. `keep_origin` keeps the
  file's own coordinates, which is what an import whose coordinates mean
  something needs (a neighbour part, a fixture, a re-import of our own
  export). `add_feature` reports the shift it applied as `recentred_by`.
- **Units.** STEP and IGES declare their own unit (OpenCASCADE converts to
  mm). STL, OBJ and BREP carry none, so the feature's `units` (`mm` default,
  `cm`, `m`, `in`) scales the shape once as it is read; it is refused on a
  STEP or IGES and an unknown string is refused. An STL, OBJ or BREP that
  comes out under 1 mm across gets a warning naming the fix.
- **At most 500 shapes** are imported from one file, with a log line when
  capped; the cap guards against pathological files only.
- **Face names** carry the body ordinal (`<id>/b<N>/…`), because every body
  of one import shares its producing feature.
- **The chain hash keys on the file's bytes**, not its path
  (`ImportFeature::ContentHash`, cached by path + size + mtime), so moving a
  staged file into the project on save does not invalidate anything built on
  it.
- **Editing an import.** An import has no history to edit, so it is changed
  by the direct edits after it: Delete, Offset, Move and Replace Face
  ([modeling](../modeling/README.md#direct-edit)) — a hole moved or filled,
  a cosmetic round removed, a wall made thicker — and by ordinary features
  on top (a cut, a pad, a Hole). Its face names are indices into the file,
  so each direct edit keeps the fingerprint of the face it was given, and a
  changed file read again that puts another face under the name fails the
  edit naming both, rather than editing the wrong face. A sketch's
  projection of an import's face or edge keeps the same kind of print: a
  renumbered element is followed to its new name, and one the file no
  longer holds (a hole the new file moved) fails the sketch naming every
  such reference
  ([projecting an imported part](../sketching/README.md#projecting-an-imported-part)).
  Our own `export_step` of an edited import numbers its faces afresh.
  What a downloaded part takes is in [editing a downloaded part](#editing-a-downloaded-part).
- **One parse per import.** When the caller already parsed the file (the GUI
  worker does, to decide part vs assembly), the model is handed to the
  feature (`SeedGeometry`) and consumed by its first regeneration. It is not
  serialized and not sticky: later regenerations re-read the path, so editing
  the source file still updates the part.

## Editing a downloaded part

What the imported-edits round (a Voron grill from STL, the SO-101 wrist
bracket and an STS3215 servo from STEP) found the import takes, through the
same RPCs an MCP client has:

- **A mesh (STL) takes direct edits after a Refine.** Each triangle is a
  face, so the face a hole sits in is a fan of triangles with no inner loop,
  and `grow_faces` refuses ("no loop of edges in a host face bounds them").
  `Refine` merges the coplanar ones (the grill: 5642 faces to 2937), after
  which `grow_faces` takes a faceted hole whole (229 faces each) and Delete
  Face fills it. It is slow: four filled holes, 916 faces, built in about
  25 s, twice in one `add_feature` (the copy it is checked on, then the
  part), and again on an open that has no baked geometry. A faceted hole has
  no radius: `query_geometry`'s `radius`, a measured diameter and a Hole
  centred on a projected edge all need a true circle, so its size and centre
  are the caller's circle fit to the wall faces' centroids. New holes cut
  into a mesh are true cylinders.
- **A STEP takes direct edits where its faces are analytic.** An Autodesk
  export blends its flats into B-spline transitions; every Delete, Offset or
  Replace Face that would extend one is refused naming it, and on the SO-101
  bracket a tab, the end face and an outer wall each led to one more
  freeform face however many were added (an Offset of a wall's whole tangent
  chain reached 61 faces). A Delete Face of a fillet whose corner blends are
  analytic (the end's x-edge fillet and its two sphere corners) works. A cut
  through the region is what removes it: the bracket's 4.2 mm past the servo
  horn went with one Extrude cut on a datum plane.
- **Placing it.** An import is centred on its box or kept where the file put
  it (`keep_origin`), turned by `up_axis` and `turn_deg`; there is no
  translate. A print plate is a second part holding a `Derived` of it with a
  `placement` (and `reference: false`, or it is a ghost `check_print` will
  not print).
- **Measuring it.** `measure` with `distance_kind: centres` gives a
  cylinder face's centre, which is how a hole pattern or a horn axis is
  located: the face's own row gives its axis direction and radius, not
  where the axis is. A multi-body import's faces (`<id>/b<k>/…`) all
  measure; only the first body's did before.

## Up axis

Our world is Z-up ([conventions](../conventions.md#world-axes)); SolidWorks,
among others, writes Y up, so its parts arrived lying on their backs. An
`Import` carries `up_axis`, which axis OF THE FILE is up, and `turn_deg`, a
turn about world Z after it (`src/core/io/ImportUpAxis.{h,cpp}`):

| `up_axis` | turn applied |
|---|---|
| `auto` (default) | decided from the file's header, below |
| `none` | the file as it is |
| `+y` | +90° about X (SolidWorks' front, +Z, lands on our front, −Y) |
| `-y` | −90° about X |
| `+x` | −90° about Y |
| `-x` | +90° about Y |
| `-z` | 180° about X |

Any other string is refused, naming the seven (`UpAxisFromString`).

**`auto` says how it knows.** `interchange::DecideUpAxis` reads the header
the reader returned: STEP's `FILE_NAME` originating_system and
preprocessor_version and its `FILE_DESCRIPTION`, IGES's global-section
native system ID and preprocessor version. SolidWorks named in any of them
(case-blind) is `+y`; Loböle is `none`; any other writer is `none`, *because
it is not one known to write Y up*. A row of that table is added only with a
real file's header in its test (`tests/fixtures/import/make_fixtures.py`
copies them). STL, BREP and OBJ have no header: `auto` is `none`, and says
so (for an OBJ, that one is usually Y up). Our own STEP export writes
`Loböle <version>` as its originating system, so a round trip says why it
stayed `none`.

**Applied as a placement.** `Execute` turns every placed shape about the
FILE origin before the centring, so `keep_origin` keeps the file's
coordinates, turned. The turn changes no topology: face names are the same,
and a sketch on an imported face follows an axis change. What was decided is
derived state beside the shift (`ImportFeature::UpApplied()`).

**The reply says what was applied.** `import_step` and `add_feature` of an
Import return `up_axis: {requested, applied, turn_deg, because, system}`,
e.g. `because: "FILE_NAME originating_system 'SolidWorks 2024': SolidWorks
writes Y up"`.

**In the GUI** the Import's feature editor shows the axis, the turn and the
reason under them; a change is one undoable modify, like the units. An
import that `auto` turned says so in a viewport pill (`AppState::importNote`).

**Saved files.** `up_axis` and `turn_deg` are written when they are not the
defaults (v190, [format](../versioning/format.md#pending-changes)). An
Import saved before them loads as `none` (`MigratedFeatureJson`), so no
saved part moves.

## IGES

`CADKernel::LoadIGESStructured` reads through `IGESCAFControl_Reader` (name,
colour and layer modes) into the same `ModelFromXcaf` walk as STEP, so
names, colours and instances come through alike. IGES colours each surface
rather than the part; a product whose coloured faces all agree takes that
colour.

**Surfaces become bodies, or are counted.** IGES is often trimmed surfaces
(entity 144). Each product with faces and no solid is sewn at the
resolution the file's global section declares (field 19, converted to mm);
when that leaves it open it is sewn again at 1/10000 of the product's box
diagonal, since real files' gaps are wider than the resolution they claim. A
closed shell becomes a solid, as in `LoadSTL`; an open one stays a shell.
The feature's warning says when the tolerance was widened or a shell stayed
open, with how many free edges; a product that sewed shut at the file's own
resolution says nothing. OpenCASCADE's translator names an unnamed entity by
its shape type ("COMPOUND"); that is not taken as a name.

Units come from the global section, so `units` is refused on IGES. The up
axis reads the global section as above. `.igs`/`.iges` go through
`import_step`, the Import feature, File > Import and the in-app browser, and
a GUI import runs on the background `ImportJob` like STEP (a file of several
placed instances becomes an assembly).

**Linking.** OCCT 7.8+ has the IGES reader in `TKDEIGES`; 7.6/7.7 (stock
Ubuntu's `libocct-data-exchange-dev`) in `TKIGES` and `TKXDEIGES`. The
`CMakeLists.txt` branch that picks `TKDESTEP` picks the matching pair.

## OBJ

`CADKernel::LoadOBJStructured` reads through `RWObj_CafReader` into XCAF:
each `o` object is a product named as the file names it, coloured by its
`usemtl` material (a visual material on its faces). Each triangulation
becomes one planar face per triangle on shared vertices, sewn and promoted
to a solid when closed, as in `LoadSTL`. The faces are built by hand rather
than with `BRepBuilderAPI_MakeShapeOnMesh`, which is OCC 7.7+ and broke the
Ubuntu 24.04 (7.6) build; a triangle too thin to span a plane is skipped; an open one is a shell and the
warning says so. `units` applies, as for STL. No axis or unit conversion is
asked of the reader: the import's `units` and `up_axis` do both, and `auto`
is `none`.

glTF and 3MF are not read: OpenCASCADE's glTF reader needs RapidJSON, which
the Windows cross build turns off, and it reads no 3MF.

## What a mesh costs

A mesh is the expensive import: every triangle becomes a face, and the faces
are sewn (`BRepBuilderAPI_Sewing`), whose joining of faces into shells
(`BRepTools_Quilt::Shells`, inside `CreateSewedShape`) is most of a large
import and grows faster than the count: a profile of a 40k-triangle import
put two thirds of its samples there (and three quarters in the sewing as a
whole). So before anything is built,
`interchange::MeshImportCost` (`src/core/io/MeshImportCost.{h,cpp}`) counts
the triangles -- a binary STL's header (believed only when the file's size
agrees with it), an ASCII STL's `facet`s, an OBJ's `f` records with an
n-gon as n − 2 -- and a cost law says what the import may take.

Measured on this machine (22 cores, load ≈ 2), whole `import_step` calls:

| mesh | triangles | seconds |
|---|---|---|
| closed torus | 10,000 / 19,740 / 40,000 / 79,524 / 160,000 | 0.8 / 1.6 / 4.2 / 11.4 / 30.2 |
| scan pieces (the Stanford armadillo's first n triangles: open, many shells) | 10,000 / 20,000 / 40,000 / 70,000 / 100,000 | 1.1 / 2.5 / 7.9 / 24.2 / 61.2 |
| the same 20,000 as OBJ | 20,000 | 4.4 |
| the whole armadillo (a closed scan) | 345,944 | not done after 16 min of CPU, when it was stopped; the law says 15 |

The law is `t = 0.36 s · (n / 10⁴)^2.2`, fitted in log-log to the scan
pieces at 40k, 70k and 100k, where the limit falls: the slowest meshes
measured, so it says what an import *may* take. A mesh whose triangles
are in grid order (the torus) takes a third to a fifth of it; a real scan,
closed as it is, took longer than the law says. The limit is solved from the law and
`interchange::kPatientImportSeconds` (60 s, the wait a person accepts
without being asked) -- about 102,000 triangles, written down nowhere as a
number. Past it:

- `import_step` refuses, naming the count, what it may take, the limit and
  `allow_large_mesh: true`, which overrides it;
- File › Import asks (above);
- `--file` refuses in the log, naming `--allow-large-mesh`, which overrides;
- `add_feature`, and a `modify_feature` that changes the path, of an
  `Import` past it is refused naming `import_step`: built there it would sew
  on the frame with nothing to cancel it. An Import already past the limit
  keeps its other edits.

Under the limit the job's label and status line still say the count and what
it may take ("Importing scan (40,000 triangles, about 8 s)"). The worker logs
what each mesh read took against the law (wall and CPU seconds), which is
how the table above was read.

**Progress and cancel.** `LoadSTL` and `LoadOBJStructured` take the
`ImportProgress` the STEP reader takes, with phases `ReadingFile`,
`Building` and `Sewing`. On OCC 7.7 and later the STL read is
`RWStl::ReadFile` + `BRepBuilderAPI_MakeShapeOnMesh` with ranges -- exactly
what `StlAPI_Reader` does there, so the faces and their order are the
same; 7.6 keeps `StlAPI_Reader`, which takes none. The OBJ face loop checks
the cancel every 4096 triangles, and the sewing gets a range. What
OpenCASCADE reports covers the file, the faces and the sewing's own stages,
the first third of the bar; the quilt reports nothing, so the rest of the
bar is the time spent against what the law expects, held under the end.
The quilt cannot be interrupted either: a cancel there is seen when it ends,
so the RPC answers the cancel at once, the modal goes at once, and the
worker's result is dropped when it lands (`background_jobs` lists it until
then, and a new import is refused naming why).

## Importing an assembly

A STEP or IGES with several placed instances becomes an assembly
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
- **Up axis.** The plan takes the resolved turn R (`plan.up`): each part's
  Import of its `.brep` carries the axis explicitly (a `.brep` has no
  header), keeping the `.brep`'s origin when turned, so a part tab stands
  alone; and each placement P becomes R·P·R⁻¹, composed with the centring,
  so occurrences stay where the file put them, turned. To change it,
  re-import with another `up_axis`.
- It is not undoable; like Open, it creates documents.

**In the GUI**, File > Import (Ctrl+I, `file.import`) accepts STEP, IGES,
STL, OBJ, BREP, DXF and SVG. DXF and SVG go into a sketch
([sketching](../sketching/README.md#importing-dxf-and-svg)); everything else
runs as the `import` background job (`src/ui/ImportJob.{h,cpp}`,
[long operations](../ui/long-operations.md#import)) under one progress
modal, as `import_step` and `--file` do. The worker reads the file
(`ImportFeature::ReadModel`, what the feature's own regeneration reads):
a STEP's parse (no progress available from OpenCASCADE) and transfer (a
real percentage), a mesh's file, faces and sewing; when a STEP has more than
one placed instance it extracts the `.brep` files, and the main thread then
creates the tabs and occurrences a slice per frame. A STEP with one placed
instance, and any mesh or BREP, becomes an `Import` feature in the part the
import was started from, seeded with what the worker read (and the volume it
measured), so the feature does not read it again. The worker touches only
plain data; every document mutation stays on the main thread. The job can be
cancelled until materializing starts, and only one import runs at a time. A
mesh past the [triangle limit](#what-a-mesh-costs) asks first: its count and
what it may take, *Import anyway* or *Cancel*. The progress callback is
sampled, not read per tick: OpenCASCADE's transfer reports hundreds of
thousands of times on a large file.

**Over RPC**, `import_step {path, as_assembly?, keep_origin?, units?,
up_axis?, turn_deg?, allow_large_mesh?}`: without `as_assembly` it adds an
`Import` feature to the active part and returns `{bodies, regen, digest,
up_axis, recentred_by?, warning?, mesh?}`, `mesh` being `{triangles,
expected_s, limit}` for an STL or OBJ; with it, it returns
`{assembly_index, occurrences, parts, ok, up_axis}` plus `failed` naming
every part whose feature did not build. Every parameter is checked before
anything is read: an unknown extension (a DXF or SVG is pointed at
`import_sketch`), `units` on STEP or IGES (and so with `as_assembly`), an
unknown `up_axis`, a mesh past the limit without `allow_large_mesh`. The
read is the `import` job, waited on off the frame
([long operations](../ui/long-operations.md#import)); a cancel answers the
call with an error saying so, and a read that fails adds nothing.

The part tab's feature editor exposes the import's file units, up axis and
turn; `keep_origin` is reachable over RPC and in the feature JSON only.

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
  leaf. Each (part, configuration, material) is written once, as a product
  named `part (configuration) material` -- plain `part` for a placement in
  the part's active configuration and its own material -- and placed per
  occurrence under the occurrence's name, each placement in its own
  configuration (v135); each
  (sub-assembly document, configuration) is one nested STEP assembly, since
  one sub-assembly in two configurations has two contents. Hidden and
  suppressed occurrences are left out.
- **Colour** is what the viewport shows: one resolver,
  `ui::ResolveBodyLook` (`src/ui/BodyAppearance.{h,cpp}`), shared with
  `RendererSync`: appearance override > material (a placement's `material`
  before its bodies' own) > imported display colour > palette. The palette
  is seeded the way the showing tab seeds it, so every
  occurrence of an unpainted part is one colour. Colours are written linear,
  as the shaders light them. A face with an appearance of its own (v164) is
  written as a colour on that face's sub-shape label, over the product's
  ([a face of its own](../rendering/materials.md#a-face-of-its-own)); the
  importer reads product colours only.
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

`export_stl {path, body?, stl_deflection_mm?}` and `export_brep {path, body?}`
write the active part. Without `body` the non-reference bodies are fused
into one shape, those whose boxes meet in one boolean and the rest gathered
as the compound a fuse would leave (`CADShape::UnionOf`,
[tolerances](../tolerances/README.md#exporting-a-printed-part)); STL and
BREP hold one shape each. STL is binary and
re-meshed at `stl_deflection_mm` (chord tolerance, the name `check_print`
takes for the same deviation; `deflection` is refused, naming it), never the viewport's
tessellation; the reply reports the deflection used, where it came from, and
whether the source B-Rep was watertight. Left out, it is 0.1 mm on an
unprinted part and, on a printed one, the print profile's chord deviation,
else the material's own (TPU's 0.02), else a tenth of its smallest gap; a coarser one asked for on a printed part is
written and warned about
([exporting a printed part](../tolerances/README.md#exporting-a-printed-part)).
File › Export goes through the same helper (`ui::ExportPartStl`) and says
what it wrote in a viewport pill. BREP is OpenCASCADE's exact native form. STL has no unit and is
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
  same colour resolver as STEP. A face with an appearance of its own carries
  its own material on its sub-shape, so the writer emits it as its own
  primitive.
- glTF is written in metres and Y-up, as the format defines; OBJ and PLY
  stay in millimetres, Z-up.

## The Export dialog

File > Export (`file.export`; `ui::ExportDialog` in `src/ui/FileDialogs.cpp`)
offers STEP first. A part tab offers STEP, STL, BREP, glTF, OBJ and PLY; an
assembly tab STEP and the visual formats; a drawing tab SVG and PDF. After
the dialog everything is `ui::ExportActiveDocument(state, path)`, which the
tests drive directly because a native dialog cannot be driven -- except a
drawing tab's, which goes through `ui::ExportActiveDrawingWhenProjected` and
writes once its views have landed
([drawings](../drawings/README.md#projection-off-the-frame)). STL and BREP
on an assembly tab, and unknown extensions, are refused by name.

## RPCs

| RPC | module | does |
|---|---|---|
| `import_step {path, as_assembly?, keep_origin?, units?, up_axis?, turn_deg?, allow_large_mesh?}` | RemoteProject | STEP / IGES / STL / OBJ / BREP into the part, or a STEP or IGES as parts + an assembly, as the `import` job; replies `up_axis`, and `mesh` for a mesh |
| `import_sketch {path, sketch_id? \| plane?, units?, layers?, construction_layers?, keep_origin?}` | RemoteModeling | a DXF or SVG into a sketch ([sketching](../sketching/README.md#importing-dxf-and-svg)) |
| `export_step {path, part?, body?}` | RemoteProject | structured STEP of a part or the assembly |
| `export_stl {path, body?, stl_deflection_mm?}`, `export_brep {path, body?}` | RemoteProject | one shape of the active part |
| `export_visual {path, deflection?}` | RemoteProject | glTF / OBJ / PLY of the part or assembly |
| `export_members {dir}` | RemoteTube | one STEP per tube member |
| `export_dxf` | RemoteSheetMetal | a flat pattern as R12 DXF |
| `export_print_job {path, plates, slicer_config?, name?}` | RemotePrint | a print job, one 3MF of plates for Bambu Studio ([print jobs](../tolerances/README.md#print-jobs)) |
| `add_feature` with `type: "Import"` | RemoteModeling | the feature directly; reports `recentred_by` and `up_axis` |

## Tests

| where | covers |
|---|---|
| `tests/test_step_export.cpp` | an assembly exports as its tree (each part once per configuration and material, placed by reference, sub-assemblies nested, hidden occurrences left out); a part is one named product of its bodies; a one-solid compound keeps its name and colour; a face painted apart is a colour on that face in the STEP and its own primitive and material in glTF; visual export colours every occurrence as the screen does; an assembly tab writes STEP and refuses STL by name |
| `tests/test_cadkernel.cpp` `[kernel]` | structured STEP import (names, colours, instances); STL units; the assembly import plan (centred parts, placed occurrences); no XCAF document left open; each `auto` decision and its reason; each axis puts the 30 mm along Z and SolidWorks' front on −Y; a sketch on an imported face follows an axis change; R·P·R⁻¹; an older Import reloads unmoved; IGES names and colours as STEP's, surfaces sewn, a skipped one named; OBJ objects, colours and units; our STEP names Loböle |
| `tests/fixtures/import/` | the fixtures and `make_fixtures.py` / `make_iges.cpp`, which wrote them: our box under real SolidWorks 2024 and Autodesk headers, IGES as a solid, as surfaces and with a SolidWorks system ID, ezdxf DXFs, SVGs, an OBJ |
| `tests/golden/v190-import-up-axis.json` | `up_axis` and `turn_deg` round-trip; v97 and v103 reload as `none` |
| `tests/test_assembly_flatten.cpp` | `Flatten`'s levels compose to exactly its world placement |
| `tests/headless_smoke.py` | STEP / STL / BREP round trips, `keep_origin`, `as_assembly`; every import fixture through `import_step` / `import_sketch` with its reply read |
| `tests/refused_calls_smoke.py` | an unknown `up_axis`, `units` on IGES, a drawing given to `import_step`, each changing nothing |
| GUI `import/solidworks_part_stands_up_and_lies_down` | the pill, the editor's axis combo, undo |
| `tests/test_mesh_import.cpp` `[interchange][import]` | counting a binary, an ASCII and a lying STL and an OBJ's polygons; the limit is where the law reaches the patient wait, and the refusal names the count, the time and the override; a mesh read reports its phases and stops on a cancel (STL and OBJ); a seeded import is the import of its file (bodies, names, volume, units) |
| `tests/import_job_smoke.py` | `import_step` of a mesh as a job: listed with its triangles, its fraction moving, other calls answered; a cancel from another connection answers the call and leaves the part; a second import refused; past the limit refused naming the count, the time and `allow_large_mesh` (and `add_feature` naming `import_step`), with it started; `--file` cancellable, landing, and past the limit refused naming `--allow-large-mesh` |
| GUI `import/a_mesh_import_runs_in_background`, `import/a_large_mesh_asks_first` | the modal says the triangles, lands, and its Cancel goes at once adding nothing; past the limit the ask's Cancel imports nothing and Import anyway starts the job |
| `tests/visual_export_smoke.py` | parses the binary glTF: meshes, materials, instancing |
| `tests/part_configuration_smoke.py` | one part placed in two configurations and two materials exports as two products |

`export_step` is an MCP tool, so a change to it is measured by the
benchmark's `reference.py`, `negative_controls.py` and `villain.py` too
(`benchmarks/README.md`).

## Not done

- IGES export.
- glTF and 3MF import (see [OBJ](#obj)).
- A SolidWorks IGES header from a real file: the IGES row of the `auto`
  table rests on the STEP files' and a modelled global section.
- 3MF as an export of a part or an assembly (OpenCASCADE has no writer).
  A print job is a 3MF of Loböle's own writing
  ([print jobs](../tolerances/README.md#print-jobs)).
- DXF of a drawing.
- Mesh bodies: an STL arrives as sewn B-Rep faces, not as a lightweight
  reference mesh.
- A faster sewing of a mesh. The faces `BRepBuilderAPI_MakeShapeOnMesh`
  makes already share their edges, so a shell could be built from them
  directly, in linear time, instead of through the quilt. Unless it kept
  the faces in the sewing's order, it would rename every face of every
  mesh import already saved.
- Feature recognition on a mesh: a faceted hole is hundreds of planes, with
  no radius to filter, measure or project
  ([editing a downloaded part](#editing-a-downloaded-part)).
- Translating an imported body (a placement on the Import itself).
- STEP PMI / AP242 GD&T.
- Fusion 360 (no server-side feature API) and native SolidWorks.
