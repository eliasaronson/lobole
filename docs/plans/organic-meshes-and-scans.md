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
- There is no PLY or glTF import, no way to place, rotate or scale an
  import, and no section into a sketch. Mesh simplification and repair are
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

**`MeshBvh`**
- The AABB tree `collide::Mesh` already has (`src/core/MeshCollide.*`), lifted
  out and generalised.
- Queries: ray, closest point, and the fast generalised winding number
  (Barill et al. 2018).
- The winding number is what decides inside from outside for an OPEN mesh,
  where ray parity (`collide::Mesh::Contains`) is wrong. `collide::Mesh`
  becomes a user of it.

**`Slice`**
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
- STL (binary and ASCII), OBJ, PLY and GLB all produce a `TriMesh` without
  building any B-Rep.
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
  from mechanical.** Seed triples of noisy facets fit small spheres and
  cylinders within four quanta: the noisy sphere leaves only 1.9% (10k) to
  54% (2M) of its area as facets, the torus 0%, the figure 90–94%. What
  does tell them apart is the triangles a region holds: the tube and the
  plate 300 to 500,000 (4 and 30 regions at any size), the clean torus 70
  to 380, the noisy sphere 1.4 to 2.7, the figure 1.5 to 1.7. Stage 1's
  fallback to a mesh body reads that (a mechanical part's regions hold
  hundreds), not the fitted area.
- **The `auto` limit** is solved from the figure's law and
  `interchange::kPatientImportSeconds` (60 s, the wait a person accepts
  without being asked, as the facet import's limit is): segmentation alone
  reaches it at n = 10⁴ · (60 / 2.0)^(1/2.2), about **47,000 triangles**,
  written down nowhere as a number. Below it, `auto` segments and, by the
  region size, rebuilds (mechanical) or keeps a mesh body without the
  rebuild (organic); at or above it, a mesh body without recognition.
  Recognition does not scale to the sizes that matter for scans
  (100k–2M). A mechanical STL past 47,000 triangles therefore arrives as a mesh
  body under `auto` although it would recognise in a second or two (the
  2M tube in 3.7 s); `recognize` still forces it.

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
under `parallel::For` (manifold is serial here), which stage 2 measures
before it sets the budget it refuses past. Its `BuildCostS` is this law
on the cells and triangles it would make.

### Stage 1 — a mesh body (L)

An imported mesh stays a mesh and works everywhere a person looks at it,
measures it or exports it.

**The model**
- `Body` (`src/core/Feature.h`) gains `std::shared_ptr<const mesh::TriMesh>
  mesh`. A mesh body has an empty `CADShape`, and `Body::IsMesh()` says so.
- A mesh body has one element, `<id>/b<k>/mesh`. It has no faces or edges in
  the `ElementMap`.
- A pick on it carries the hit point (and, inside the session only, the
  triangle). A reference stored in the document is a point and the body,
  never a triangle index, following the precedent of the fingerprinted
  import projections: geometry, not index.

**Import**
- `Import`'s `representation` (today `facets | recognize`,
  `mesh::Representation`) gains `mesh` and `auto`; anything else is refused,
  naming them. `facets`, `recognize` and `mesh` force the choice.
- `auto` is the default. It uses the stage-0 recognition cost law, with a
  limit derived from that measured cost (as `MeshImportCost` derives the
  patient-import limit), never a literal:
  - below the limit, it segments, and when its regions hold many triangles
    (a mechanical part's hold hundreds, an organic one's two or three:
    stage 0) it rebuilds a solid (`recognize`); otherwise it keeps a mesh
    body without the rebuild;
  - at or above the limit, it goes straight to a mesh body without running
    recognition.
- The reply says which representation it chose and why ("mesh: 346 112
  triangles, past the recognition limit of 46 900"; "mesh: 1.6 triangles a
  region"), as the reply does for `up_axis: auto`.
- The default is the existing `mesh_representation` preference
  (docs/ui/settings.md), which gains the same two values. A per-import
  `representation` overrides it.
- Recognition does not scale to a scan's size (stage 0: superlinear on
  organic meshes, past 300 s at 100k), so the limit is the one solved from
  its law, about 47,000 triangles. `topo_to_body` follows the same rule.
- An open mesh is kept, reported as open with its boundary loops and the
  length of the longest. It is no longer skipped.
- Readers go through `mesh::` (STL and OBJ here; PLY and GLB in stage 4).
- Where the mesh is past what can be worked with here, the refusal names
  what to do externally and the target: decimate in MeshLab or Blender to
  under N triangles (the cost limit), or close the named holes in the
  scanning app or Blender.
- The file stays the recipe, as for every import: staged into `imported/`
  and re-read on regeneration, with `ContentHash` unchanged.
- `vNEXT` adds a `vNEXT-import-mesh.json` golden.

**Baked geometry**
- `src/core/io/GeometryCache.cpp` stores a mesh body as `N.mesh`, a small
  binary of a header, double positions and `uint32` indices. This bumps
  `kGeometryFormat`.
- An open project therefore does not re-parse a 2M-triangle file.

**Rendering and picking**
- `TessellationFor` (`src/ui/TessellationCache.cpp`) returns the body's own
  triangles with smooth normals split at a crease angle, and no BRepMesh.
  The part render cache keys on the mesh hash.
- `FaceSelector` uses `MeshBvh` for meshes above the size where a brute
  force pick takes a quarter frame (stage 0: about 265,000 triangles, from
  15.7 ns a triangle), instead of brute force (docs/rendering/README.md#picking already names
  this). The whole mesh highlights as one range.
- The ray tracer's re-mesh calls (`RendererSync.cpp` render-still paths)
  take the triangles.

**Section**
- The raster clip already works on any triangles.
- Caps of a closed mesh body come from `mesh::Slice`, not from
  `Measure::SectionCaps`' OCC boolean.
- An open mesh gets no cap, and `list_sections` says so for that body.
- The `section` RPC (area, regions, loops, enclosed) reads the same slice,
  so a scan's cross-section area and outline are numbers.

**Measure**
- `measure` of a mesh body gives the bounding size and area, and volume
  when it is closed. When it is open, the volume is refused, naming the
  boundary loops.
- The new `{feature_id, point_mm: [x, y, z]}` names a point on a body. It is
  snapped to the nearest surface point, and the reply gives the snapped
  point and how far it moved, so point-to-point is the existing `{a, b}`
  distance.
- Between a mesh body and anything else the distance comes from `MeshBvh`.
  The answer is exact to the mesh (its triangles are the geometry), so
  `tolerance_mm` is the B-Rep side's deflection only.
- The GUI Measure tool records the hit point on a mesh and measures point to
  point.
- `mass_properties` uses the divergence theorem, for closed meshes only.
- `list_elements` on a mesh body lists its one element with its statistics
  (triangles, components, loops, edge-length median), instead of tripping
  `max_faces`.

**Interference and clashes**
- `collide::Mesh` is built from the body's triangles, with deflection 0.

**`check_print`**
- Each item stage 0 found reads the B-Rep switches to triangles.
- Overhang is by triangle normal.
- "One piece per solid" is the closed-component count. An open mesh body is
  refused for printing, naming its holes and saying to close them in an
  external tool (MeshLab, Blender or the scanning app), or to Cradle it.

**Export**
- `export_stl` and `export_print_job` (`PrintJob3mf`, through
  `ui/PrintJobActions.cpp`'s `MeshObject`) write the triangles as they are.
  The reply says that mesh bodies were not re-meshed, and so that
  `stl_deflection_mm` did not apply to them.
- `export_visual` hands mesh bodies to `MeshExport` unchanged (stage 0:
  glTF, OBJ and PLY write a triangulation-only face whole).
- `export_brep` writes the triangulation (it round-trips). `export_step`
  writes a mesh body as a tessellated shell in AP242 with
  `write.step.tessellated` on, set for that write only; on OpenCASCADE
  older than 7.7, where the writer crashed on such a face, it is refused
  naming the body and the formats that carry it.

**Everything else refuses**
- Every feature, tool and RPC that needs a B-Rep refuses a mesh body by
  name, through one helper (`RequireBRepBody(body, what)`): fillet, shell,
  sketch on a face, project_geometry, a mate, a drawing view, FEM, CAM.
  None of them substitutes something else or yields an empty result.
- The way this is checked is the action matrix and the random-design fuzz
  run with a mesh body present. That is how a site the helper missed is
  found.

### Stage 2 — the cradle (L)

This is the operation the round exists for. It makes a block (B-Rep or mesh)
into a holder shaped to a scan, with a clearance, which can be lifted in and
out.

**The decision**

The pocket is built in a **distance field**, and only the final subtraction
is a **mesh boolean (manifold)**. The evidence:

- **Clearance and insertion are both trivial operations on a field, and
  hard on a mesh.**
  - Clearance is an iso-level shift.
  - Making the pocket insertable along a direction (removing the undercuts
    that would trap the object) is a running minimum down each grid column:
    `f_swept(p) = min_{t≥0} f(p + t·dir)`.
  - On a mesh, an offset is a Minkowski sum. manifold's own source says that
    on non-convex input it "scales with the product of face counts", which
    rules it out against a 1M-triangle scan.
- **A field takes an open or dirty scan.**
  - The sign comes from the generalised winding number, which tolerates
    holes, even the missing bottom of a table-top scan.
  - A mesh boolean refuses non-manifold input: manifold returns
    `NotManifold`, and its `Merge()` promises no repair.
- **manifold's boolean is the step that must be exact and fast.**
  - It keeps the block's sharp edges and holes, and its result is watertight
    by construction.
  - A third-party benchmark (MeshLib) measured a 2M-triangle union at about
    0.27 s.
  - It also supplies the iso-surface extraction (`Manifold::LevelSet`),
    which outputs a manifold mesh.
- **The dependency is permissive and fits every build.**
  - Apache-2.0, built static. Only LGPL code has to be a shared library.
  - Its one dependency, Clipper2, is already vendored.
  - Stage 0 has proven it in all three builds.
  - The field code is ours, on `MeshBvh`, so there is no TBB, Blosc or Boost
    and no GPL dependency.

**The feature**

`Cradle {target, tool, clearance_mm?, direction?, sweep?, resolution_mm?}`

- `target` is the blank, a body.
- `tool` is the scan: a mesh body, or a B-Rep tessellated at a deflection
  derived from its own size, as `collide::Mesh` does.
- `clearance_mm` defaults from the part's print process (the print
  allowance, docs/tolerances/README.md). With no process set it is refused,
  naming `set_print_process` and `clearance_mm`, because a literal default
  would be wrong for any real printer.
- `direction` is the direction the object moves into the cradle. It defaults
  to world −Z, the world's own down, so it carries no unit.
- `sweep: false` keeps the undercuts, for a snap-in grip.
- `resolution_mm` defaults to half the clearance. The grid covers only
  target ∩ (tool + clearance), and is capped by a cell budget set in stage 0.
  If the cap would force a cell coarser than the clearance, the feature is
  refused, naming the budget. The reply reports the resolution used and the
  surface deviation it implies.

**The pipeline**, all in `src/core/mesh/`:

1. Closest-point distance on the grid by `MeshBvh`, computed within a band.
2. The sign from the winding number.
3. Shift by the clearance, then sweep along `direction`.
4. `LevelSet` with trilinear sampling.
5. Subtract the pocket from the target by manifold.
6. The result is a mesh body.

**Mesh booleans in general**

`Boolean` with a mesh operand runs through manifold. This is what puts a
screw hole or a slot (a B-Rep tool body) into a cradle after it is cut:
- A mesh operand must be `Closed()`. An open one is refused, naming its
  loops, and saying to close them in an external tool or to use Cradle.
- A B-Rep operand is tessellated at a derived deflection, which the reply
  states.

**After the cradle**
- Once a body is a mesh, a later B-Rep feature on it is refused, naming the
  feature that made it one.
- The way to finish such a part is to do the B-Rep work first (fillets,
  chamfers, bosses) and the cradle and mesh-tool booleans last.
  docs/modeling says so.

**Threading and the job**
- The field and the boolean are pure data, and run under `parallel::For`.
- Cradle is a heavy feature (docs/ui/long-operations.md#heavy-features):
  it declares `MayBuildOffFrame` and estimates `BuildCostS` from its cells
  and triangles (the stage-0 cost law), and builds as a `feature` job with
  that mechanism's progress, cancel rule and result store, keyed by its
  chain hash (target, tool, clearance, direction, sweep, resolution are in
  it), so reopening reuses it. Its Execute reads only its bodies, its
  referenced features and the symbols, as that mechanism requires. Added
  here: a refusal past a cost budget derived from the stage-0 measurement,
  and progress reported through `heavy::ReportFraction`.
- A refusal past the budget names what to do: a coarser `resolution_mm` (the
  value that fits the budget), a smaller target, or decimating the scan
  externally to under the triangle count that fits.

`vNEXT` adds a golden for Cradle and one for a mesh Boolean.

### Stage 3 — placing a scan (M)

A scan arrives tilted, off-centre and, from photogrammetry, at an arbitrary
scale.

- **Import gains `placement`** (`translate_mm`, `rotate_deg` about world
  axes) and **`scale`**. This also closes the roadmap's "translating an
  imported body" item for B-Rep imports.
- **`align_import {feature_id, mode, …}`** writes that placement and echoes
  its inputs. The modes:
  - `floor`: fit a plane to the region grown from a picked point, and lay it
    on z = 0 facing down.
  - `three_points`: the plane through three `point_mm` values.
  - `principal`: principal axes to the world axes.
  - `scale`: two `point_mm` values and the true `distance_mm` between them
    give the factor.

  An unknown mode is refused, naming the four.
- The GUI Align tool offers the same four modes. Each one previews in the
  viewport and lands as one undo step.
- `vNEXT` adds a golden.

### Stage 4 — PLY and GLB (S)

These are the formats phone scanning apps export besides OBJ and STL.

- **PLY** uses a vendored miniply (MIT), placed in `src/thirdparty` as stb is.
  OpenCASCADE has a PLY writer but no reader. Supported content is ASCII and
  binary of either endianness, vertex positions, face lists and vertex
  colour (kept as the body's display colour when it is uniform; otherwise
  dropped, with a warning).
- **GLB and glTF** use a vendored cgltf (MIT, one header).
  `RWGltf_CafReader` exists in OCC, but the Windows OCC build is configured
  with `USE_RAPIDJSON=OFF`, so it would read nowhere there.
- The node tree is flattened to placed meshes, and metres are converted to
  mm per the format.
- The extension list in the import refusal grows to include both.
- Both readers join the reader fuzz. `tests/test_cadkernel.cpp` gains a
  fixture written by our own `export_visual` and read back.

### Stage 5 — a section into a sketch (M)

This is the B-Rep route to a holder. For a stand, an outline-shaped pocket or
a profile-following rim, a 2.5-D part from a sketch of the scan's outline is
often the better part than a cradle, because it stays fully parametric.

- **A sketch gains a projected section**: `project_section {sketch, bodies?,
  tolerance_mm?, fit?}`, where `fit` is `polyline | arcs | spline`.
  - The sketch's plane cuts the bodies (`mesh::Slice`, or the existing
    `Measure` section loops for B-Rep).
  - The cut is simplified to `tolerance_mm` (default derived from the body's
    size) and drawn as locked construction geometry that follows the source,
    as `project_geometry` does.
  - It is refitted on every regeneration from the geometry, never kept as an
    index.
- The reply gives the maximum deviation of what was drawn from the true
  cut.
- The clearance is the sketch's existing offset tool.
- The Section tool gains "To sketch" for the active cut.
- `vNEXT` adds a golden.

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
- `src/core/mesh/` (new): `TriMesh`, `MeshBvh`, `Slice`, `Segment`, `Fit`,
  `Field` (grid, winding number, sweep), `MeshReaders` (STL, OBJ, PLY,
  GLB) and `MeshBoolean` (the one manifold caller).
- `src/core/MeshCollide.*` becomes a user of `MeshBvh`.
- `src/core/Feature.{h,cpp}`: `Body::mesh`, Import's `representation`,
  `placement` and `scale`.
- New feature: `src/core/features/CradleFeature.*`, and its heavy-feature
  job beside `src/ui/ImportJob.*`.
- The settings table row for the default `representation`
  (`docs/ui/settings.md`).
- Changed features: `BooleanFeature.cpp`, `DatumPlaneFeature.*`,
  `DatumAxisFeature.*`.
- `src/core/Measure.cpp`, `src/core/MassProps.cpp`, `src/core/PrintCheck.cpp`
  and `src/core/sketch/` (the projected section).

**Serialization**
- `src/core/io/SerializeFeatures.cpp`, `src/core/io/GeometryCache.cpp`
  (`kGeometryFormat`) and `src/core/io/SerializationInternal.h`
  (`kVersionPending`).
- `tests/golden/vNEXT-*.json`.

**Rendering, picking and export**
- `src/cad/CADKernel.cpp` (readers move out), `src/cad/FaceSelector.cpp`
  (BVH) and `src/cad/MeshExport.cpp`.
- `src/ui/TessellationCache.cpp`, `src/ui/RendererSync.cpp`,
  `src/ui/StlExport.cpp`, `src/ui/PrintJobActions.cpp` and
  `src/ui/ImportActions.cpp`.
- New tools: Align, Cradle and Fit, and "To sketch" in the
  Section tool, each in its own `ui/` file.

**RPC**
- `src/remote/Remote{Project,Modeling,Inspection}.cpp` and their
  `MethodDoc` rows. Every physical parameter carries its unit (`_mm`,
  `_deg`) for `tests/rpc_units_gate.py`.

**Dependencies**
- `deps/manifold` (submodule), plus
  `src/thirdparty/{miniply,cgltf}`.
- `CMakeLists.txt`, `.gitmodules`, `flake.nix`,
  `packaging/windows/cross-app.nix`, `THIRD_PARTY.md` and `LICENSES/`.

**Docs**
- `docs/interchange/README.md`, `docs/ui/long-operations.md`,
  `docs/ui/settings.md`, `docs/inspect/README.md`,
  `docs/modeling/README.md`, `docs/rendering/README.md`,
  `docs/architecture.md` (libraries), `docs/versioning/format.md` and
  `docs/roadmap.md`.

## Verified by

**Unit tests against the fixtures, checking the outcome**
- A sphere's volume, area and centroid against the analytic values, from
  `TriMesh` and through `mass_properties`.
- The open blob reports its loop, and its volume is refused.
- The dirty mesh welds to the expected components.
- A section of the sphere is a circle of the right radius.
- A point measure snaps onto the surface.
- **Cradle of a sphere of radius r, clearance c:**
  - the pocket wall is at r + c, to within the reported deviation;
  - with `sweep`, the pocket's width above the equator stays 2(r + c) all
    the way to the top;
  - without `sweep`, the pocket closes over the top;
  - the result is `Closed()`;
  - lowering the sphere along `direction` clashes with nothing
    (`collide::Mesh` against the cradle at each pose: the purpose, not the
    formula).
- The cradle of the open blob succeeds where a mesh Boolean refuses it by
  name.
- A fitted cylinder's radius and axis match the generated ones.
- `align_import floor` lands the cut face on z = 0.

**Refusals**
- A mesh past the limit is refused with the external-tool instruction and a
  numeric target (triangle count, or the holes to close).
- Each new enum or mode refused with its list.
- Each B-Rep-only feature refused on a mesh body by name.
- A Cradle with no clearance and no print process refused.
- A refused call changes nothing (`refused_calls_smoke`).

**Goldens and readers**
- Every `vNEXT` golden, and a save and reopen that keeps the baked mesh with
  no re-parse.
- The reader fuzz over STL, OBJ, PLY and GLB.

**Representation and the job**
- `auto` on a mechanical fixture below the limit gives a solid with
  recognised holes; on the organic blob it gives a mesh body; at or above the
  limit it gives a mesh body and recognition never runs. The reply names the
  choice and the reason in each case, and `facets`, `recognize` and `mesh`
  force it.
- A `mesh_representation` preference of `mesh` replaces `auto` as the
  default, and a
  per-import value overrides it.
- A Cradle job reports progress. Cancelling an edit leaves the document
  byte-identical with no undo step. Cancelling the build during open leaves
  the feature "cancelled" with its downstream unbuilt, visible in the tree
  and the reply, and a rebuild request builds it. A reopen after a build
  reuses the cached result with no recompute. A Cradle past the budget is
  refused naming the resolution or triangle count that fits.

**Cost checks** (CPU time, a bound per budget from stage 0, each shown to
fail against a planted spin)
- Import of the 2M fixture, and recognition on the largest fixture it runs on.
- Mass properties.
- A Cradle at the default resolution.
- A section cap.

**Responsiveness check**
- A hover pick on the 2M mesh stays under a frame, in a GUI test reading
  `MainThreadCpuMs()`.

**Smokes and suites**
- `headless_smoke.py` imports each format in each representation and
  exports STL, 3MF and glTF.
- The action matrix and `tools/fuzz.py` run with a mesh body in the part.
- A GUI test per tool, plus the `defaults/*` suite.
- `render_pixel_smoke.py` re-run, since `RendererSync.cpp` changes.
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
