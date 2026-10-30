# Organic meshes and scans

Built after the mechanical-mesh round (recognising holes and planes in
faceted STLs, `Import.surfaces: facets | recognize`), which lands first. The
two share one foundation, `src/core/mesh/` (below): the mechanical round
builds it in the shape this plan sets out, and this plan only extends it.

## The problem

A person should be able to download a sculpted figurine (100k–500k
triangles) or scan an object with a phone (200k–2M triangles) and design a
printed part that fits it: a stand, a holder, a cradle, a grip. Today that
cannot be done, because every triangle becomes a B-Rep face:

- STL goes through `StlAPI_Reader` plus `SewAt`, and OBJ through
  `AddTriangleFaces` plus sewing (`src/cad/CADKernel.cpp`, `LoadSTL` and
  `LoadOBJStructured`). An import names one face per triangle
  (`<id>/b<k>/faceN`, `src/core/Feature.cpp`).
- Measured on a 100k-triangle armadillo, in an offscreen instance:

  | operation | cost |
  |---|---|
  | import, on the main thread | 560 s and 1.1 GB |
  | `mass_properties` (BRepGProp over 100k faces) | 13.7 s |
  | a section and a screenshot (BRepMesh of 100k faces, plus the cap boolean) | 41 s and 2.3 GB peak |
  | a block minus the scan, the cradle itself (stuck in `BRepBuilderAPI_Copy` inside `topo::BooleanNamed`) | did not finish in 600 s |
  | `list_elements` | refuses: 99976 faces |
  | a 346k-triangle import | did not finish in 330 s |

- An open scan, the normal case for a phone scan of an object standing on a
  table, is dropped outright: `ImportFeature` skips any shape whose volume
  is 0 as "a surface, not a solid".
- There is no section into a sketch. Mesh simplification and repair are
  not built here: they are done in external tools (MeshLab, Blender, the
  scanning app).

## The foundation: `src/core/mesh/`

Pure data and no OpenCASCADE, so all of it can run on worker threads under
`parallel::For` without the OCC sharing rules (`core/Parallel.h`). It has
five parts.

**`TriMesh`**
- Indexed triangles: positions are `glm::dvec3` (the model is double,
  docs/conventions.md#the-model-is-double, so no exemption is needed) and
  indices are `uint32`.
- Built by welding a triangle soup at a tolerance derived from the mesh's
  own bounding diagonal.
- Carries edge adjacency, consistent orientation, connected components,
  boundary loops, and a content hash that serves as its `GeometryId`.
- `Closed()` means no boundary loops and edge-manifold.
- Volume, area, centroid and inertia come from the divergence theorem, and
  are defined only when `Closed()`.

**`MeshBvh`** (built in stage 1: `src/core/mesh/MeshBvh.{h,cpp}`)
- An AABB tree like the one `collide::Mesh` has (`src/core/MeshCollide.*`).
- Queries: ray, closest point, and the fast generalised winding number
  (Barill et al. 2018).
- The winding number is what decides inside from outside for an OPEN mesh,
  where ray parity (`collide::Mesh::Contains`) is wrong. `collide::Mesh`
  is not yet a user of it.

**`Slice`** (built in stage 1: `src/core/mesh/Slice.{h,cpp}`)
- Plane ∩ mesh as closed or open polylines, chained exactly.
- Douglas–Peucker simplification to a stated tolerance.
- Triangulation of closed loops into section caps.

**`Segment` and `Fit`** (built by the mechanical round:
docs/interchange/README.md#recognising-a-faceted-mesh)
- Region growing by the fitted surface's residual, not by normal deviation
  from a seed: a cylinder's normals turn by design. `SegmentAll` segments a
  whole mesh; this plan adds the seeded form, `GrowFrom(mesh, seed, kind)`,
  for `fit_primitive`.
- Least-squares fits of plane, cylinder, sphere and cone, each with its RMS
  and maximum residual, at a tolerance of a few quanta of the file's own
  precision (`TriMesh::quantum`). This plan adds RANSAC behind the same `Fit`
  signature, and a tolerance a scan's noise sets, because a scan has
  outliers.
- The mechanical round uses them to rebuild a faceted part as a solid of
  true faces (`cad::RebuildFromRegions`). This plan uses them to align a scan
  and to put datums on it.

**Readers**
- STL (binary and ASCII), OBJ, PLY and glTF (all built) produce a
  `TriMesh` without building any B-Rep.
- Every reader joins `tests/test_reader_fuzz.cpp`
  (docs/versioning/README.md#reader-fuzzing).

## Stages

Each stage ships on its own and is useful without the next. They are in
order of value.

### Stage 0 — measured: fixtures, the dependency, and what things cost

Built. What it settled, so the later stages do not guess.

**The fixtures** are `tests/mesh_organic.{h,cpp}` (pure data, no
OpenCASCADE or Catch2; `mesh_fixtures.h` includes it), checked by
`tests/test_mesh_organic.cpp`, the 2M sizes `[slow]`:
- `NoisySphere` (radius 40, each vertex moved radially by at most 0.05 mm,
  a cube-sphere of 12 n² triangles), `Torus` (30 / 10, clean) and
  `Figurine` (eight metaball blobs, about 95 mm tall, contoured by marching
  tetrahedra), at any count to within 6%.
- `CutFigurine`: every triangle reaching below 3 mm cut away; open, one
  loop.
- `Dirty()`: a soup with near-duplicate points, 7 flipped triangles, 10
  needles from edge splits and a second component, with each count.
- Volume, area and radius are analytic (sphere, torus); the figure's volume
  is integrated from its field column by column, apart from its triangles.
  Each carries the bound its tessellation and noise allow.
- `build/mesh_fixture_writer NAME N OUT.stl` writes one with its answers as
  JSON, for the bench.

**manifold** 3.5.4 is the submodule `deps/manifold`, built from its sources
as a static library in `CMakeLists.txt` (not through its own CMake, which
fetches and installs), `MANIFOLD_PAR` off, its CrossSection on the vendored
Clipper2 with that target's `USINGZ`. Pinned in `.gitmodules` and
`flake.nix`, in `THIRD_PARTY.md` and `LICENSES/`. `mesh::Boolean`
(`src/core/mesh/MeshBoolean.{h,cpp}`) is its one caller and refuses an open
or non-manifold operand naming why. The proof, one sphere-minus-torus
boolean whose volumes must add up to 1e-9 and whose results must be closed:
- nix: `test_mesh_organic.cpp`;
- Ubuntu 24.04 and 26.04 containers (`tools/ci-local.sh --containers`):
  the same test, passing in both;
- mingw: `packaging/windows/manifold-probe.nix` builds
  `mesh_boolean_probe.exe` (static) from `cross-app.nix`, and under Wine it
  prints the same volumes as Linux, to the last digit printed.

**The bench** has `--set mesh` (docs/performance/README.md#mesh-workloads):
import, render sync, mass properties and a section at 10k, 300k and 2M.
Today, at 10k (the figure; the sphere for mass): import 3.25 s (recognised,
the default; the facet law says 0.38 s), the first capture 0.42 s (356 ms
of sync, 346 of it tessellation), `mass_properties` 0.93 s, a section
0.60 s (0.54 s of it the `section` RPC). The 300k and 2M rows are for stage
1: on today's code they run for hours (below). A hover pick has no RPC; it
is measured in-process.

How everything below was read: `tests/test_mesh_costs.cpp`, hidden cases
`[mesh-cost-recognition]`, `[mesh-cost-render]` and `[mesh-cost-cradle]`,
each measurement in a forked child capped at 300 s of CPU and 6 GB, on this
machine (Core Ultra 9 185H, one thread unless said). The load moved under
other work; the numbers quoted are the quietest run of each.

#### Rendering a 2M-triangle mesh

The figure at 2M triangles, as a mesh body would hand it over (shared
vertices, 1.0M of them) and as a faceted import does today (every face its
own vertices: 6.0M):

| | mesh body | faceted today |
|---|---|---|
| vertex + index buffers (24 B a vertex, 4 an index) | 46 MB, counted twice against the 256 MB tessellation budget (the outline asset uploads them again): 92 MB | 160 MB, 320 MB counted: past the budget, kept because it is on screen |
| `outline::BuildEdgeAdjacency` (once per asset, main thread) | 0.77 s, 270 MB peak | 0.46 s, 247 MB |
| static lines (boundary + feature edges past 60°) | 21,004 indices | 12.0M indices: every triangle edge, twice, every frame |
| smooth edges (silhouette candidates) | 3.0M | none |
| a settled silhouette pass (render thread, once per camera settle) | 13.9 ms (most of a 60 Hz frame), 52k indices | 6.5 ms, nothing found |
| `FaceSelector::AddMesh` / `PickFace` (brute force) | 0.20 s / 31 ms | 0.27 s / 27 ms |

At 300k a mesh body is 7 MB, 0.07 s of adjacency, a 1.4 ms silhouette pass
and a 3.8 ms pick. So a mesh body draws silhouettes and the few creases past
60° (on the figure, marching tetrahedra's folds), not a wireframe, and fits
the budget; what does not scale is picking: brute force costs 15.7 ns a
triangle, a quarter of a 60 Hz frame (4.2 ms) at about 265,000 triangles,
and the hover pick runs every frame. That size, derived from the measured
rate and the frame, is where `FaceSelector` switches to `MeshBvh`. A plain
median-split tree over the 2M figure builds in 1.8 s on one thread.

#### The OCC writers

`tests/test_mesh_writers.cpp` hands each writer a face carrying only a
`Poly_Triangulation` (no surface, no edges) of the 10k figure and reads the
file back:
- `BRepMesh_IncrementalMesh` at 1, 0.1 and 0.001 mm leaves it untouched, so
  `MeshExport::SaveVisual`'s in-place mesh call is harmless.
- glTF, OBJ and PLY (through `MeshExport::SaveVisual`), STL
  (`StlAPI_Writer`) and BREP (`BRepTools::Write`, read back) write every
  triangle. `MeshExport` takes mesh bodies unchanged.
- STEP: in the AP214 the app writes (no schema set) the face is dropped
  silently. With `write.step.schema` AP242DIS and `write.step.tessellated`
  On, it is written as a `TRIANGULATED_FACE` and reads back whole -- in a
  shell; a lone face is dropped even then. On OpenCASCADE 7.6.3 (Ubuntu
  24.04) the STEP writer CRASHED on it, and 7.6 has no PLY writer: there a
  mesh body's `export_step` must refuse by name, and the test runs these
  two only from 7.7.

#### `check_print`

Every item but one reads the B-Rep, and the whole check first meshes a copy
(`BRepMesh_IncrementalMesh`, `PrintCheck.cpp` around line 259):
- **needs a per-triangle version**: overhang (exact surface normals at UV
  nodes, `FaceArea`, a UV hill climb), bridges (edge discretisation, a UV
  grid classified by `BRepTopAdaptor_FClass2d`), stacking (rays refined on
  the exact surface, `IntCurvesFace_Intersector` near boundaries, UV
  sampling), clearance (BRepExtrema refinement and a `BRepAlgoAPI_Common`
  when meshes cross; on facets the triangle distance is already exact),
  bed fit's keep-out zones (`BRepExtrema` against a box), shells (solids by
  `TopExp`, watertight from edge-to-face ancestors) and bed (lowest point by
  `BRepBndLib::AddOptimal`, watertight as shells);
- **slender** reads triangles already, except its footprint, which picks
  downward planar faces by surface type;
- **chords** reads no geometry: for a mesh body the facets are the
  geometry, so it does not apply.

#### What recognition costs

`surfaces: recognize` (`cad::LoadStlRecognized`: read, `SegmentAll`,
`RebuildFromRegions`) on the fixtures:

| fixture | 10k | 30k | 100k | 300k | 1M | 2M |
|---|---|---|---|---|---|---|
| tube (bored cylinder, mechanical) | 0.09 s | | 0.72 s | 1.3 s | 2.0 s | 3.7 s, 568 MB |
| plate (24 holes, mechanical) | 0.07 s | 0.13 s | 0.14 s | BRepMesh ran out of 6 GB writing it | | |
| figure: segmentation | 1.4–2.0 s | 14.5–22.8 s | past 300 s | | | |
| figure: rebuild | 1.5 s | 5.2 s | | | | |
| sphere: segmentation | 0.07 s | 0.26 s | 1.0 s | 12.9 s | 19.3 s | 37.3 s |
| sphere: rebuild | 3.4 s, 111 MB | 10.4 s, 302 MB | 41.6 s, 961 MB | past 300 s | past 6 GB | past 6 GB |
| torus: segmentation | 0.06 s | 0.25 s | 1.6 s | 45.8 s | 189 s | past 300 s |
| torus: rebuild | 1.6 s | 3.7 s | 8.2 s | 28.1 s, 1.5 GB | 11.0 s | |

- **On a mechanical mesh it is linear and cheap**: segmentation about
  0.3 µs a triangle, and a rebuild that grows with the faces made (4 for
  the tube, 30 for the plate, whatever the count). Reading the file is most
  of it.
- **On an organic mesh segmentation is superlinear**, all of it in
  `Segmenter::FitSet` trying curve hypotheses on seed triples (up to
  `kMaxTriples`, 256, a seed). The slowest, the figure, is
  **t = 2.0 s · (n / 10⁴)^2.2**, fitted to its 10k and 30k points (the
  exponent from both runs, 2.1 and 2.2); it says 320 s at 100k, where the
  run passed its 300 s cap.
- **The rebuild costs about 1 ms and 25 KB a face made** (the sphere: 3,713
  faces in 3.4 s and 111 MB; 39,061 in 41.6 s and 961 MB). An organic mesh
  makes a face for nearly every other triangle: past about 500,000 faces it
  ran out of 6 GB in OpenCASCADE.
- **"The share of the area that fits a primitive" does not tell organic
  from mechanical**, and neither does the count of triangles a region
  holds, once coarse exports are measured: a coarse mechanical export holds
  2 to 22 a region, as an organic one holds 1 to 3. What separates them is
  the share of the surface in large regions; stage 1's `auto` reads that
  ([what auto chooses](../interchange/README.md#mesh-bodies)).
- **The `auto` limit** is solved from the figure's law and
  `interchange::kPatientImportSeconds`: segmentation alone reaches the
  patient wait at about 47,000 triangles (`RecognitionTriangleLimit`).
  Recognition does not scale to the sizes that matter for scans (100k–2M).

#### The cost budget of a Cradle

Each piece of the pipeline, by what it scales with:

| piece | measured | per unit |
|---|---|---|
| manifold's subtraction, a block minus the figure (step 5) | 0.37 s at 300k, 2.98 s and 720 MB at 2M; the result closed | 1.5 µs and 360 B a triangle |
| `Manifold::LevelSet` on a trilinear-sampled grid (step 4) | 0.17 s at 64³, 1.36 s at 128³, 10.1 s and 1.28 GB at 256³, 38.1 s and 2.1 GB at 400³ | 0.6 µs a cell (serial: `MANIFOLD_PAR` is off) |
| closest point on the scan, a point near the surface (step 1) | 0.6 µs at 10k, 1.6 at 300k, 4.1 at 2M | grows as log n |
| winding number, first order (Barill), near / far (step 2) | 3.9 / 0.8 µs at 10k, 11.8 / 0.8 µs at 2M | far points stay flat |
| the tree those queries walk | 0.24 s at 300k, 1.8 s at 2M | 0.9 µs a triangle |

So a Cradle costs about **1.4 µs a grid cell** (the sign and the level set,
everywhere in the box) plus **16 µs a cell in the band** (closest point and
an exact winding number, at 2M) plus 2.4 µs a scan triangle (its tree and
the subtraction of what the level set makes). Against the same 60 s budget
that is about **4 × 10⁷ cells**, a grid of 350³, and LevelSet's own memory
there is about 1.7 GB. A 100 mm scan at a 0.15 mm cell (half a 0.3 mm
clearance) is 3 × 10⁸ cells: past the budget seven times over, so either the
cell is coarsened to about 0.3 mm or the level set runs in parallel slabs
under `parallel::For` (manifold is serial here). Stage 2 measured the
slabs slower than one level set, and coarsens instead
([what a cradle costs](../modeling/README.md#what-a-cradle-costs)).

### Stage 1 — a mesh body

Built: an imported mesh stays a mesh body (`representation: mesh`, and
`auto` choosing), measured, sectioned, checked for printing, baked and
exported as its triangles, and refused by name by every B-Rep feature
([mesh bodies](../interchange/README.md#mesh-bodies),
[measuring one](../inspect/README.md#mesh-bodies),
[drawing one](../rendering/README.md#mesh-bodies)). `MeshBvh` and `Slice`
below are its. What the later stages still need of it:
- the action matrix run with a mesh body in the part (`tools/fuzz.py run
  --mesh-body` does it for random designs), which is how a B-Rep consumer
  `MeshBodyRefusal` misses is found;
- a mesh body in an assembly is drawn, picked and measured, but clashes,
  contact sweeps and motion studies there still read B-Rep shapes only;
- a frame with a 2M-triangle mesh body costs about 26 ms of main-thread
  time with or without the cursor on it, not yet profiled.

### Stage 2 — the cradle

Built: [Cradle](../modeling/README.md#cradle) and
[mesh booleans](../modeling/README.md#mesh-booleans), with the budget it
measured ([what a cradle costs](../modeling/README.md#what-a-cradle-costs)).
What the later stages still need of it: a `generators/scan_cradle/`
dogfood (below), and the printed holder tried on a real object, the only
test of the clearance that means anything.

### Stage 3 — placing a scan

Built: an Import's `placement` and `scale`, `align_import` (floor, three
points, principal axes, scale) and the GUI Align tool
([placing an import](../interchange/README.md#placing-an-import)). What the
later stages still need of it: the floor's fit is least squares over the
grown region, not yet RANSAC (stage 6's robust fit), so a floor whose
grown region takes in a stray lump fits through it.

### Stage 4 — PLY and GLB

Built: PLY and glTF read into a mesh body or any other representation
([PLY and glTF](../interchange/README.md#ply-and-gltf)), by vendored miniply
and cgltf.

### Stage 5 — a section into a sketch

Built: `project_section` and the Section tool's "To sketch", lines, lines
and arcs, or splines within a tolerance, refitted every regeneration, the
offset taking the outline as the clearance
([a section into a sketch](../sketching/README.md#a-section-into-a-sketch)).

### Stage 6 — fits and datums on a mesh (M; mostly the mechanical round's)

- `fit_primitive {feature_id, point_mm, kind?, tolerance_mm?}` grows a
  region from the point and fits it. `kind` is `plane | cylinder | sphere |
  cone | auto`. The reply gives the parameters and the RMS and maximum
  residual.
- `DatumPlane` and `DatumAxis` gain `from_fit`. They store the seed point
  and the kind, and refit on regeneration, so the datum follows a re-scan.
- A mate, a sketch plane and a measurement then reach a scan through its
  datums. That is how a scanned object takes part in an assembly.

## Files

**Core**
- `src/core/mesh/`: `Segment` gains `GrowFrom`; `Fit` gains RANSAC.
- `src/core/MeshCollide.*` becomes a user of `MeshBvh`.
- Changed features: `DatumPlaneFeature.*`, `DatumAxisFeature.*`.

**Serialization**
- `src/core/io/SerializeFeatures.cpp` and `src/core/io/SerializationInternal.h`
  (`kVersionPending`).
- `tests/golden/vNEXT-*.json`.

**Rendering, picking and export**
- A new tool, Fit, in its own `ui/` file.

**RPC**
- `src/remote/Remote{Project,Modeling,Inspection}.cpp` and their
  `MethodDoc` rows. Every physical parameter carries its unit (`_mm`,
  `_deg`) for `tests/rpc_units_gate.py`.

**Docs**
- `docs/interchange/README.md`, `docs/ui/long-operations.md`,
  `docs/inspect/README.md`, `docs/modeling/README.md`,
  `docs/architecture.md` (libraries), `docs/versioning/format.md` and
  `docs/roadmap.md`.

## Verified by

**Unit tests against the fixtures, checking the outcome**
- A fitted cylinder's radius and axis match the generated ones.

**Refusals**
- Each new enum or mode refused with its list.
- A refused call changes nothing (`refused_calls_smoke`).

**Goldens**
- Every `v205` golden.

**Smokes and suites**
- The action matrix run with a mesh body in the part, and with a Cradle's
  result in it.
- A GUI test per tool, plus the `defaults/*` suite.
- `tools/ci-local.sh` for the Ubuntu containers, and the mingw build for
  the vendored libraries.

**The dogfood**

A generator, `generators/scan_cradle/`, writes into `scratch/scan_cradle/`
and drives only the RPC surface:
1. Import a scan.
2. Align it (floor, then scale by a measured distance).
3. Sketch the stand's outline from a section.
4. Extrude the blank, and add B-Rep fillets and screw bosses.
5. Cradle it with the print process clearance.
6. Subtract the screw holes as a mesh Boolean.
7. Run `check_print`, then `export_print_job`.

The showcase object is a hand tool (a cordless drill or pliers), scanned
with a phone by the user, for a printed wall holder. Its grip and trigger
undercut tests the cradle's no-trap pull direction: the holder must let the
tool lift out along `direction` with the undercut present.

A CC0 scan from the Smithsonian Open Access 3D collection, fetched by the
generator with its URL and licence recorded on its page (each file's licence
checked), stands in where the phone scan is not available.

The printed holder is then tried on the real tool. That is the only test of
the clearance that means anything. The generator page records the fit
found.
