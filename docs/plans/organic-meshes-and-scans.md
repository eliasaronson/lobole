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

### Stage 0 — measure, fixtures, and the dependency in all three builds (S)

This stage settles what the later stages must not guess. It starts after the
mechanical round has landed, so recognition exists to be measured.

1. **Fixtures with known answers, generated in-test.** Stanford's models are
   research-only, so none of the probe's files are committed. The fixtures
   are built by `tests/mesh_fixtures.{h,cpp}`:
   - a noisy sphere, a torus and a metaball "figurine" at 10k, 300k and 2M
     triangles;
   - the same blob with its bottom cut away, as a phone scan of an object on
     a table arrives;
   - a dirty one with duplicate vertices, flipped triangles, slivers and a
     second component.

   The volume, area and radius of each are known analytically or by
   construction.
2. **Bench workloads** in `tools/bench.py` (docs/performance/README.md):
   import, render sync, a hover pick, mass properties and a section on each
   size. These are the budgets the later cost checks are set from, against
   the slowest core (docs/ui/testing.md#tests-that-time-things).
3. **manifold, vendored.** manifold is Apache-2.0. nixpkgs has 3.5.4 and the
   mingw derivation evaluates, but no Ubuntu release packages it (neither
   noble nor resolute). It goes in as a submodule under `deps/manifold`:
   - built static with `MANIFOLD_PAR=OFF`, against the Clipper2 that is
     already vendored;
   - pinned in `.gitmodules` and as a flake input
     (`tests/flake_submodules_gate.py`), with a `THIRD_PARTY.md` entry and its
     licence under `LICENSES/`.

   This is proven by building and linking it in the nix build, the stock
   Ubuntu containers (`tools/ci-local.sh`) and the mingw cross build
   (`packaging/windows/cross-app.nix`). The test is one boolean of two
   fixture meshes, run on each.
4. **Questions answered in this stage**, each written into this plan before
   stage 1 starts:
   - **Rendering a 2M-triangle mesh.** What do the outline adjacency
     (`render/OutlineEdges.cpp`), the settled silhouettes and the part
     render cache's 256 MB budget cost? Do mesh bodies draw feature edges at
     all, or only silhouettes?
   - **The OCC writers.** Do the glTF, OBJ and PLY writers (and STEP's
     tessellated mode, `write.step.tessellated`) accept a face carrying only
     a `Poly_Triangulation`? If they do, `MeshExport` takes mesh bodies
     unchanged. If not, a mesh body gets its own writer path.
   - **`check_print`.** Which of its nine items read the B-Rep in a way that
     needs a per-triangle version (`src/core/PrintCheck.cpp`)?
   - **What recognition costs.** The mechanical round's recognised import
     (`surfaces: recognize`) is timed on the fixture meshes, 10k to 2M
     triangles: wall time and memory of the segmentation and rebuild, and the
     share of the area it fits, for the mechanical fixtures and for the
     organic ones. The answer is a cost law in triangles, like
     `MeshImportCost`'s, and it sets the limit that stage 1's `auto` uses.
   - **The cost budget of a heavy feature job.** What a Cradle costs per
     cell and per triangle on the fixtures, so the budget past which the job
     refuses is derived from it (stage 2).

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
  - below the limit, it recognises (`recognize`) and builds a solid. If most of the area does not fit a primitive (an organic shape),
    it falls back to a mesh body;
  - at or above the limit, it goes straight to a mesh body without running
    recognition.
- The reply says which representation it chose and why ("mesh: 346 112
  triangles, past the recognition limit of 120 000"; "mesh: 14% of the area
  fits a primitive"), as the reply does for `up_axis: auto`.
- The default is the existing `mesh_representation` preference
  (docs/ui/settings.md), which gains the same two values. A per-import
  `representation` overrides it.
- If the stage-0 measurement shows recognition does not scale to the sizes
  that matter, `auto` instead takes `mesh` above a limit derived from the
  measured B-Rep import cost (about 10 s) and `recognize` below it.
  `topo_to_body` follows the same rule.
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
- `FaceSelector` uses `MeshBvh` for meshes above a size set in stage 0,
  instead of brute force (docs/rendering/README.md#picking already names
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
- `export_visual` uses whatever stage 0 settled.
- `export_step` and `export_brep` either write the tessellation (if stage 0
  shows OCC does) or are refused, naming the body and the formats that
  carry it.

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
