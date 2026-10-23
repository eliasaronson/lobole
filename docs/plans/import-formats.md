# Import formats: up axis, IGES, DXF and SVG into a sketch, OBJ

## The problem

- **A STEP from SolidWorks arrives on its side.** SolidWorks (the team's CAD)
  writes up as +Y; our world is Z-up (docs/conventions.md#world-axes).
  `ImportFeature` stores a path, `keep_origin` and `units`, and its only
  placement is the centring shift, so nothing can stand the part up.
- **IGES is not read**, though vendors and shops still send it, often as
  trimmed surfaces rather than solids.
- **A laser plate or a logo cannot come in as a sketch.** We write DXF and
  SVG but read neither, so a 2D outline is redrawn by hand.
- **Mesh import is STL only**; an OBJ has to be converted elsewhere first.

## The shape of the fix

### Stage 1: up axis on import (STEP first; every later format follows it)

1. **The model.** `ImportFeature` gains `upAxis`, one of `auto`, `none`,
   `+x`, `-x`, `+y`, `-y`, `-z` (which axis of the file is up), and
   `turnDeg`, a turn about world Z applied after it. `none` leaves the file
   as it is. Each axis has one fixed turn, chosen so the writer's front
   becomes our front (−Y):

   | file up | turn |
   |---|---|
   | `+y` | +90° about X (SolidWorks' front, +Z, lands on −Y) |
   | `-y` | −90° about X |
   | `+x` | −90° about Y |
   | `-x` | +90° about Y |
   | `-z` | 180° about X |

   Serialized as `up_axis` / `turn_deg` (`vNEXT`); an unknown string is
   refused naming the seven (`UpAxisFromString`, `[[nodiscard]]`). The
   default is `auto`; a `kVersionNEXT` migration loads older Imports as
   `none`, so no saved part moves.
2. **Detection says how it knows.** `LoadSTEPStructured` also returns the
   header (`FILE_NAME` originating_system and preprocessor_version,
   `FILE_DESCRIPTION`) on `ImportedModel::origin`.
   `interchange::DecideUpAxis(requested, origin)` resolves `auto` from a
   table: SolidWorks named in any of the three (case-blind) is `+y`, Loböle
   is `none`, and any other writer is `none` *because it is not one known to
   write Y up*. A row is added only with a real file's header in its test.
   STL and BREP have no header: `auto` is `none`, and says so.
3. **Applied as a placement, recorded like the shift.** `Execute` turns the
   placed shapes about the file origin before centring (`keep_origin` keeps
   the file's coordinates, turned). The decision is derived state beside
   `m_shift`. The turn changes no topology, so a sketch on an imported face
   follows an axis change.
4. **The result says what was applied.** `import_step` and `add_feature`
   (Import) reply `up_axis: {requested, applied, turn_deg, because,
   system}`, e.g. `because: "FILE_NAME originating_system 'SolidWorks
   2024': SolidWorks writes Y up"`. `import_step` gains `up_axis?` and
   `turn_deg?`, and now reports `recentred_by` for a part import too.
5. **Editable afterwards.** The Import's feature editor shows the axis, the
   turn and the reason; a change is one undoable modify, like `units`. A
   GUI import that `auto` turned says so in a viewport pill.
6. **Assembly import.** `BuildAssemblyImportPlan` takes the resolved turn
   R: each part's Import of its `.brep` carries the axis explicitly (a
   `.brep` has no header), so a part tab stands alone, and each placement P
   becomes R·P·R⁻¹, composed as the centring is. The reply carries the same
   `up_axis`; to change it, re-import (assembly import is not undoable).
7. **Our own files name us.** `SaveSTEPModel` writes `Loböle <version>` as
   the originating system, so a round trip says why it stayed `none`.

### Stage 2: IGES

1. `CADKernel::LoadIGESStructured` reads through `IGESCAFControl_Reader`
   (colour, name, layer modes) under `DataExchangeMutex()`. The label walk
   in `LoadSTEPStructured` becomes one `ModelFromXcaf(doc)` both use, so
   names, colours and instances come through as for STEP.
2. **Surfaces become bodies, or are counted.** IGES is often trimmed
   surfaces (entity 144; both OCCT samples in `~/Documents/CAD/OCCT/data/iges`
   are). Each product's faces are sewn at the resolution the file's global
   section declares (field 19); a closed shell becomes a solid as in
   `LoadSTL`, an open one stays a shell. `Execute`'s `Volume() <= 1e-6`
   filter drops a surface without a word today: each skipped shape is now
   named in a warning.
3. Units come from the global section (OCC converts to mm, as STEP does), so
   `units` is refused on IGES. Up axis: the global section's native system
   ID and preprocessor version feed the same `DecideUpAxis`.
4. `.igs`/`.iges` join `import_step`, the Import feature, File > Import and
   the in-app browser, and run through the background `ImportJob` like STEP.
5. **Linking.** CMake's `TKDESTEP` branch adds `TKDEIGES` (OCCT 7.8+; nix
   7.9.3 ships it) or `TKIGES TKXDEIGES` (the 7.6 layout of stock Ubuntu's
   `libocct-data-exchange-dev`). The Windows cross OCC builds DataExchange
   by default, and `cross-app.nix` copies every DLL.

### Stage 3: DXF into a sketch

**Baked entities, not a linked file**: a DXF becomes ordinary, editable
sketch entities, so there is no format change. A shared layer
(`src/core/io/SketchImport.{h,cpp}`, reused by stage 4) holds the parsed
curves in mm, what was not imported, and the unit's source; it applies a
2D affine (a mirror reverses arcs into the CCW-always model) and snaps
endpoints within `kChainTol` onto one point.

The reader is ours (`src/core/io/ImportDxf.{h,cpp}`, no dependency): ASCII
DXF is code/value line pairs, as `ExportDxf.cpp` writes. R12 to R2018, CRLF
or LF, numbers by `std::from_chars` (never the locale); binary is refused.

| DXF entity | sketch |
|---|---|
| LINE | Line |
| CIRCLE | Circle |
| ARC | Arc (degrees CCW → radians) |
| LWPOLYLINE, 2D POLYLINE/VERTEX | Lines, and an Arc per non-zero bulge (sweep 4·atan(bulge), negative = clockwise, stored reversed); flag 70 bit 1 closes it |
| ELLIPSE | Ellipse when the parameters span a full turn, else EllipseArc; rMinor = ratio·\|major axis\| |
| SPLINE, non-rational | BSpline: as is when the knots are clamped-uniform, else split into its Bézier pieces by knot insertion (each exact, as text glyphs are), which also keeps every piece under the 255-pole cap |
| POINT | Point |
| INSERT | its BLOCK expanded with position, rotation and uniform scale (a negative scale mirrors), nested to a fixed depth |

**Refused, naming the kind and handle, importing nothing** (a plate
missing one curve cuts wrong): a rational or fit-point-only SPLINE, a
polyline with width, an INSERT scaled unevenly over a curve, an extrusion
other than ±Z (−Z, the mirrored OCS, is mapped), mixed Z elevations, 3D
polylines and meshes, and any type in neither list. **Not imported, listed
by kind and count** (annotation, no outline): TEXT, MTEXT, DIMENSION,
LEADER, MLEADER, HATCH, SOLID, IMAGE, VIEWPORT.

**Units.** `$INSUNITS` 1 (in), 4 (mm), 5 (cm), 6 (m) are read; any other
code is refused naming the four. A `units` contradicting a declared unit is
refused, as on STEP. With none declared, `units` applies, mm by default as
for STL, and the reply says it came from the default. **Layers:**
`layers?` filters (an unknown name refused, listing the file's), and
`construction_layers?` brings those in as construction (our BEND_*).

**The RPC.** `import_sketch {path, sketch_id? | plane?, units?, layers?,
construction_layers?, keep_origin?}` in `RemoteModeling.cpp`, with a
`MethodDoc` row (NeedsPart | NotInBatch, as `import_step`). Without
`sketch_id` it makes a Sketch on `plane` (Top when absent); it centres on
the sketch origin unless `keep_origin`. The file is parsed and checked
before anything is created; one undo step. Reply: `{sketch_id, entity_ids,
imported, not_imported, units{unit, source}, recentred_by, open_ends,
regen, digest}`; `open_ends` (endpoints joining nothing) is why an outline
will not extrude. **GUI:** File > Import takes `.dxf`, into the open sketch,
else a new one on the preselected plane or face, else Top; a pill reports.

### Stage 4: SVG into a sketch

`src/core/io/ImportSvg.{h,cpp}` parses with OpenCASCADE's `LDOMParser`
(TKCDF, already linked through TKLCAF; now listed), so nothing is vendored.
It walks `svg`, `g`, `use`/`symbol`, `path`, `line`, `polyline`, `polygon`,
`rect` (rounded corners as elliptical arcs), `circle` and `ellipse`,
composing every `transform` with a final Y flip (SVG is Y-down, as
`DrawingExport.cpp` writes). An affine image of a conic is a conic, so it
stays exact: a skewed circle is an Ellipse, its arc an EllipseArc.

| path command | sketch |
|---|---|
| M m, L l, H h, V v, Z z | Lines (Z closes to the subpath start) |
| C c, S s | cubic BSpline, 4 poles (S reflects the previous control) |
| Q q, T t | quadratic BSpline, 3 poles |
| A a | Arc when the transformed radii are equal and unskewed, else EllipseArc. The endpoint-to-centre conversion and the radius scale-up follow SVG 1.1 F.6.5–F.6.6, which defines them, and a zero radius is a Line |

**Not imported, listed:** `text`, `image` (the reply names
`add_reference_image`), hidden elements. **Refused, naming the element
id:** `clip-path` and `mask` (the cut would not be the picture) and any
element in neither list. A stroke imports as its centreline; a stroked
shape with a width is reported, since a logo of thick strokes needs
"stroke to path" in its own program.

**Units.** A root `width`/`height` in mm, cm, in, pt or pc with a `viewBox`
fixes the scale, and `units` is refused. Without one, user units are CSS px
(1/96 in, as the spec defines) unless `units` (mm, cm, m, in, px, pt) says
otherwise; the reply states the scale and its source. Same RPC, reply and
GUI entry as DXF, by extension.

### Stage 5: OBJ (next to STL)

`RWObj_CafReader` (TKDEOBJ, already linked for export) reads into XCAF, so
object names and `usemtl` colours become products;
`BRepBuilderAPI_MakeShapeOnMesh` makes each triangulation planar faces,
sewn and promoted to a solid as in `LoadSTL`. `units` applies (`TakesUnits`
gains `.obj`); `auto` is `none`, and the reply notes OBJ is usually Y up.
glTF and 3MF are not in this plan: OpenCASCADE's glTF reader needs
RapidJSON, which the Windows cross build turns off, and it reads no 3MF.

## Files

- `src/core/Feature.{h,cpp}` (`ImportFeature`), new
  `src/core/io/ImportUpAxis.{h,cpp}`, `SketchImport.{h,cpp}`,
  `ImportDxf.{h,cpp}`, `ImportSvg.{h,cpp}`; `src/core/io/ImportAssembly.cpp`.
- `src/cad/CADKernel.{h,cpp}` (header, `ModelFromXcaf`, IGES and OBJ
  readers, sewing, the header we write); `CMakeLists.txt` (IGES, TKCDF).
- `src/core/io/SerializeFeatures.cpp` (fields, the Import `kFeatureTypes`
  row, the migration), `SerializationInternal.h` (`kVersionPending`).
- `src/remote/RemoteProject.cpp` (`import_step`), `RemoteModeling.cpp`
  (`import_sketch`), their rows; regenerated `python/lobole/rpc.py` and
  `api/rpc-surface.json`.
- `src/ui/ImportActions.cpp`, `ImportJob.cpp`, `AssemblyActions.cpp`,
  `FileDialogs.cpp`, `FeatureEditor.cpp`.
- `docs/interchange/README.md`, `docs/sketching/README.md`,
  `docs/versioning/format.md` (Pending changes).

## Verified by

- **Fixtures**, small and committed in `tests/fixtures/import/` with the
  script that made them: our box (30 mm tall along Y) under a real
  SolidWorks 2024 `FILE_NAME` header, and under an Autodesk one; an IGES
  box as a solid, as trimmed surfaces, and with a SolidWorks system ID;
  DXFs from ezdxf, an independent writer (bulges, a non-uniform SPLINE,
  ELLIPSE, nested INSERT, `$INSUNITS` 1, CRLF), one refusal file per refused
  row, and our own flat pattern; Inkscape- and Illustrator-shaped SVGs
  (mm + viewBox; DOCTYPE, pt); an OBJ of two objects and two materials.
- **Unit tests** (`test_cadkernel.cpp` `[kernel]`, new
  `test_sketch_import.cpp`): each `auto` decision and reason; each axis
  puts the 30 mm along Z and the front on −Y; a sketch on an imported face
  follows an axis change; R·P·R⁻¹ keeps occurrences where the file put
  them, turned; IGES names and colours match STEP's; surfaces sew to a
  solid, and a skipped one is warned. Each mapping row's entity and
  geometry (a bulge arc's radius; a split spline equal to the original at
  sampled parameters within 1e-9); our flat-pattern DXF extrudes to the
  blank's area; every refusal names its handle or id.
- **Golden** `vNEXT-import-up-axis.json` (+ `.expect.json`): `+y` with a
  turn; an older golden reloads as `none`, unmoved.
- **RPC:** the rows pass `rpc_units_gate.py`; `refused_calls_smoke.py`
  adds an unknown `up_axis`, `units` on IGES, a unit contradicting
  `$INSUNITS`, a rational spline and an unknown layer, each changing
  nothing; `headless_smoke.py` imports every fixture and reads the replies;
  `regen_honesty_smoke.py` covers `import_sketch`.
- **GUI:** import the SolidWorks fixture, see the pill, change the axis in
  the feature editor, undo; a DXF into an open sketch, extruded.
- **Real files, by hand:** the SolidWorks parts in
  `~/Documents/CAD/kicad-source-mirror` (the Würth 434133025816 and Omron
  B3U-3000PM-B switches, Y their height) stand on their footprints; the
  OCCT `bearing.iges` and `hammer.iges` import as solids.
- **Builds:** stock-Ubuntu CI (OCCT 7.6 names) and the Windows cross build
  link, and `packaging/windows/smoke.sh` imports the IGES and DXF fixtures
  under Wine from the `Loböle` folder. Headless, GUI and pygui suites.
