# Recognising holes and planes in a faceted STL

The mechanical half of the mesh work. It builds `src/core/mesh/`'s `Segment`
and `Fit`, plus the part of `TriMesh` they need, in the shape the organic
plan (docs/plans/organic-meshes-and-scans.md) sets out. The scan round then
extends it rather than replacing it.

## The problem

People download mechanical parts as STL: brackets, adapters, enclosures from
Printables and Thingiverse. Imported today, that file is one planar face per
triangle (`ImportFeature::ReadModel` → `CADKernel::LoadSTL` → `SewAt`):

- A hole is a prism of 26–223 facets. It has no radius to measure, no axis
  to mate to (`CADShape::FaceAxis` needs `GeomAbs_Cylinder`), and no circle
  to project into a sketch (`ProjectElement` samples it into a B-spline).
- A flat is a fan of triangles, so Offset Face, sketch-on-face and
  `grow_faces` need a `Refine` first.
- Everything after the import is slow, because the body has thousands of
  faces.

Measured in a headless instance on a 120×80×6 plate with 24 through holes
(Ø5–7.5), exported by our own `export_stl` and imported back with
`import_step`:

| file | triangles | import | `mass_properties` | a box cut at one corner |
|---|---|---|---|---|
| STEP of the same part | 30 faces | 0.03 s | 0.02 s | 0.03 s |
| STL at 0.05 mm (26-gon holes) | 2,652 | 0.35 s | — | — |
| STL at 0.01 mm (50-gon) | 5,468 | 0.66 s | 0.42 s | 1.72 s |
| STL at 0.002 mm (112-gon) | 12,060 | 2.32 s | — | — |
| STL at 0.0005 mm (223-gon) | 23,948 | 8.18 s | 1.92 s | 9.14 s |

A mechanical mesh costs about three times what `MeshImportCost`'s law
predicts: 2.46 s at 24k triangles. Its long fan triangles make the quilt work
harder than the torus in the law's table did. The STEP row is the floor a
rebuilt B-Rep should approach, since it is the same 30 faces.

A second defect sits beside this one. A mate's connector names an import's
face by its position (`<id>/b<k>/faceN`), and `ConnectorLocalFrame`
(src/core/AssemblySolve.cpp) resolves it by name alone. When the file
changes, or recognition changes the face set, the faces renumber and the
mate **silently lands on another face**. Sketch projections and direct edits
already carry a print of what they picked
(src/core/sketch/ExternalPrint.{h,cpp}, v195; `directedit::FaceFingerprint`);
a mate carries none.

## The shape of the fix

Recognition happens **inside the import**, as the Import feature's
`representation`, not as a separate feature:

- the face set it produces is what every later feature names;
- it replaces the sewing instead of adding a step after it, and the sewing is
  the slow part;
- it runs in the import job, which is already off the frame, with progress
  and cancel.

```
STL bytes ─► mesh::ReadStl ─► TriMesh ─► mesh::Segment ─► Regions (each fitted)
                                                     │
                       cad::RebuildFromRegions ◄─────┘  (OCC, src/cad/)
                                │
                  solid of analytic faces, built on shared edges
                  ── or, failing a check, today's sewn solid, with the reason
```

### `src/core/mesh/` — pure data, no OpenCASCADE

**`TriMesh`** (`TriMesh.{h,cpp}`) is as the organic plan specifies it:
`glm::dvec3` positions, `uint32` indices, weld, edge adjacency, consistent
orientation, components, boundary loops, `Closed()`, volume by the divergence
theorem, and a content hash. On top of that:

- **The weld is exact first.** CAD exporters write shared vertices
  bit-identically, so vertices are joined on exact equality. The diagonal
  tolerance is only for vertices that equality does not join.
- **`quantum`** is the precision the file was written at, set by the reader:
  - for binary STL, the float32 spacing at the file's largest coordinate
    (2⁻²³·max|x|);
  - for ASCII STL, the place value of the last digit printed at that
    magnitude.

  A vertex of an exported CAD mesh lies on its true surface to within this
  value, and every tolerance below is a small multiple of it.

**`ReadStl`** (`MeshReaders.{h,cpp}`) reads binary and ASCII STL straight
into a `TriMesh`, building no B-Rep, and joins `tests/test_reader_fuzz.cpp`
(which has no STL family today). OBJ keeps its XCAF read, for names and
colours; an adapter in `src/cad/` turns each OBJ triangulation into a
`TriMesh`, so OBJ is recognised too. The organic plan's own OBJ reader can
replace the adapter.

**`Fit`** (`Fit.{h,cpp}`) fits plane, cylinder, cone and sphere, each to
`FitResult {kind, origin, axis, radius, half_angle, rms, max}`.

- The axis comes from the Gauss map:
  - a cylinder's facet normals lie on a great circle and a cone's on a small
    circle, so the axis is the normal of the plane fitted to the normals'
    tips;
  - a sphere's normals pass through one point, which is a linear
    least-squares solve.
- The radius and centre then come from an algebraic circle fit of the
  vertices projected along the axis, refined by Gauss–Newton. The residuals
  are those of the vertices.
- **No Eigen.** `HAVE_EIGEN` is optional in `designer_core`, so the fits use
  their own 3×3 Jacobi eigen-solve and small normal equations.
- **No RANSAC.** A CAD export has no outliers. The scan round adds RANSAC
  behind the same signature.

**`Segment`** (`Segment.{h,cpp}`):
`SegmentAll(mesh, options) → Segmentation {regionOf[], regions[],
polygonal[]}`. Every triangle ends in exactly one region.

1. **Flat patches.** A flood fill joins triangles that are coplanar to within
   `k·quantum`, measured as a vertex's distance to the patch plane, not as a
   normal angle. Each patch is a candidate plane face.
2. **Curved regions.** Patches meeting across smooth edges are grown greedily
   under a hypothesis:
   - the Gauss-map fit holds;
   - every vertex is on the fitted surface to within `k·quantum`.

   A patch that breaks the fit is not taken. That keeps the plane a fillet is
   tangent to out of the fillet's cylinder: its normal is on the fillet's
   great circle, but its vertices are far from it.
3. **Polygonal, not round.** A hex nut trap's corners lie on a circle too, so
   the vertex fit cannot tell it from a hole. The tessellation can:
   - a curved region is taken only when its facets turn **evenly**, by at
     most 360°/`min_sides` each;
   - `min_sides` is 12 by default, and an Import may set 5 to 1024 (anything
     else is refused, naming the range), so an OpenSCAD pentagon hole can be
     asked for;
   - a ring that fits but turns more stays as its flat faces, and is listed
     in the reply with its side count and circumradius, because a hex trap's
     flats are real faces a nut sits on.
4. **What fits nothing** stays as its flat patches: a torus corner of two
   fillets, or a freeform blend. These are counted as facets left.

The same module gives the organic plan its seeded form: `GrowFrom(mesh, seed,
kind)` is step 2 from one triangle, for `fit_primitive`. Growth is by the
fitted surface's residual, not by normal deviation from a seed: a cylinder's
normals turn by design. The organic plan's `Segment` paragraph says this.

`MeshBvh` is not built here, because the fits read adjacency, not closest
points. It stays the scan round's.

### `src/cad/MeshRebuild.{h,cpp}` — the B-Rep from the regions

`RebuildFromRegions(mesh, segmentation, progress) → {shape, report}` is the
round's only OCC code.

- **Faces.** Each region is one face on a `Geom_Plane`,
  `Geom_CylindricalSurface`, `Geom_ConicalSurface` or `Geom_SphericalSurface`.
- **Edges.** The boundary between two regions is cut into chains at corner
  vertices, where three or more regions meet. Each chain becomes ONE shared
  `TopoDS_Edge`. Its curve comes from the chain's own points, which lie on
  both surfaces:
  - a line when they are collinear;
  - a circle or arc when they are coplanar and concyclic;
  - otherwise an interpolated B-spline (a hole meeting a boss).

  A closed chain with no corner, such as a hole's rim, is a full circle with
  one seam vertex.
- **Assembly.** Faces are built on the shared edges and assembled into
  shells directly. `ShapeFix_Face` adds p-curves and seams. There is no quilt.
- **Checks.** The result must pass all three, or that body takes today's
  path (`MakeShapeOnMesh` + `SewAt`) and the reply names the check that
  failed. A face that will not build demotes its region to its flat patches
  first.
  - `BRepCheck_Analyzer`;
  - a closed solid wherever the mesh was `Closed()`;
  - a volume within the bound the fits imply: Σ over curved regions of area
    × sagitta, plus the `quantum` term.
- **Face order is fixed.** Faces are ordered by the lowest file triangle each
  holds, so the same file with the same options always gives the same names.

### The `Import` feature and the setting

- **`representation: 'facets' | 'recognize'`** applies to STL and OBJ.
  - Given for STEP, IGES or BREP it is refused, as `units` is.
  - Any other value is refused, naming the two.
  - `min_sides` is taken only with `recognize`.
- **The organic round adds `mesh` and `auto` to the same field and the same
  setting**, so neither is renamed.
- **Old documents read as `facets`.** A document written before `vNEXT`
  reads as `facets`, which is how its faces were named.
  - It is a format change: `kVersionPending`, the
    `vNEXT-import-representation.json` golden with its `.expect.json`, and a
    pending row in format.md.
- **New imports follow the user's setting.**
  - The setting is `mesh_representation`: a Preferences row (an Import page),
    a string over `recognize | facets`, tier Next time, with default
    `recognize`. It is documented in docs/ui/settings.md.
  - Any import that does not name a representation takes it: `add_feature`,
    `import_step`, File › Import, `--file` and the in-app browser.
  - The reply says which representation was applied and that it came from the
    setting.
- **The baked geometry tells the two apart.** `representation` and
  `min_sides` are in the feature's input hash, so `GeometryCache` (keyed by
  `TipChainHash`) keeps them apart, and a recognised import reopens with no
  re-read.
- **A referenced import changes nothing silently.** Changing either field on
  an Import whose faces something in its part references is refused, naming
  the referrers. Mates in assemblies are covered by their prints (below).
- **The reply says what was recognised.** It is in `add_feature`,
  `import_step` and the feature's `get_state` row:
  ```
  recognition: {representation, min_sides, planes, cylinders: [{radius_mm,
     axis, centre_mm, sides, sagitta_mm}], cones, spheres,
     polygonal: [{sides, circumradius_mm, centre_mm}], facets_left,
     fell_back?: "<the check that failed>"}
  ```
  The GUI's import note shows one line of it ("12 holes, 9 planes; a 6-sided
  pocket left flat").

**The mesh limit.** `MeshImportCost` prices sewing every triangle, and a
recognised import sews none.
- Pricing: after the import job segments, it prices what the rebuild will
  build from the measured segment and build costs, not the quilt law.
- The refusal before the read: for a `recognize` import it moves to the job,
  which refuses only when the mesh falls back to facets past the limit.

### Mates that follow an imported face

A connector's face, and its snap edges, gain a print when the element is an
imported file's own (`sketch::IsFileOwn`). It is the same `ElementPrint`
(kind, size, centroid) that sketch projections carry.

- **Resolving.** When the connector is resolved:
  - the element under its name is checked against the print;
  - a mismatch is searched for with `sketch::FindPrinted`;
  - one match is followed, and the connector's name is updated;
  - none refuses the mate, naming it and what it was ("a cylinder face of
    52.1 mm² at (…)"), and asks for a re-pick;
  - several identical matches also refuse it and ask for a re-pick.
- **When the check runs.** The check costs an OCC surface analysis, so it
  runs when the reference is resolved: once per part-geometry change, cached
  on (part geometry version, connector). It never runs inside the solver's
  iterations.
- **Writing the print.** A print is written whenever a connector is made or
  re-picked on an imported face. A connector from a document before `vNEXT`
  has none and keeps today's behaviour until it is re-picked: there is no
  print to check against, and making one from the current file would bless
  the face it may already have slipped to.
- **Format.** `vNEXT` again, with a `vNEXT-mate-import-print.json` assembly
  golden.

## Tolerances

Every bound comes from the mesh:
- coplanarity and on-surface use `k·quantum`, where `k` is a unitless
  multiplier fixed against the fixtures;
- the volume check uses the fits' own sagittas;
- the weld uses exact equality, then the diagonal;
- the edge and vertex tolerances handed to OCC are the largest on-surface
  residual found, and never below `quantum`.

A file noisier than its own precision recognises less and says so in
`facets_left`. Loosening to noise is the scan round's RANSAC.

## Stages

Each lands on its own commit.

**Stage 0 — fixtures.**
- Generated parts: `tests/mesh_fixtures.{h,cpp}`, the file the organic plan
  names, writes known CAD parts through `CADKernel::SaveSTL` with free
  deflection and angular deflection. Every radius and axis is known exactly.
  - the plate above;
  - a bracket with a counterbore, a countersink, a boss and its hole, and a
    hex nut trap;
  - an L-bracket with straight fillets and torus corners;
  - an oblique hole;
  - each at 24 and 128 sides and at chord-limited deflections;
  - polygonal controls: hexagon, octagon and decagon pockets.
- Files from another exporter: two or three small STLs authored in
  OpenSCAD, committed under `tests/fixtures/mesh/` beside their `.scad`
  sources, with the radii and axes in the source.

**Stage 1 — `TriMesh`, `ReadStl`, `Fit`.** Unit tests cover:
- the weld;
- components and loops;
- the volume against the CAD volume;
- each fit's parameters on fixture regions;
- the residual of a polygon.

The reader fuzz gains STL.

**Stage 2 — `Segment`.** On every fixture, the region counts and every
radius, axis and side count must come out as built:
- 24 and 128 sides are found;
- the hexagon, octagon and decagon are listed polygonal and kept flat, and
  `min_sides: 5` takes the OpenSCAD pentagon;
- a fillet does not swallow the planes it is tangent to;
- torus corners are left as facets.

**Stage 3 — the rebuild and `representation`.**
- What lands: `MeshRebuild`, the Import fields, serialization, `vNEXT`, the
  reply, the fallback and the refusal of a referenced change.
- Tests through the real pipeline:
  - `measure` gives a hole's diameter;
  - `FaceAxis` gives its axis;
  - a projected rim is a sketch circle, and a Hole placed on it is
    concentric;
  - Offset Face moves a recognised flat, and changes a recognised bore's
    radius;
  - the volume matches the CAD part within the reported bound;
  - a forced failure of each check falls back and names it;
  - the same file twice names the same faces.

**Stage 4 — the setting and the GUI.**
- The `mesh_representation` row, the Import panel's representation and
  `min_sides`, and the import note.
- A GUI test, and the `defaults/*` entry.

**Stage 5 — mates that follow an imported face.**
- What lands: prints on connectors, following, refusals, the cache and
  `vNEXT`.
- Tests:
  - a mate to an STL face re-exported with its faces renumbered follows it;
  - a face that is gone refuses by name;
  - two identical faces refuse as ambiguous;
  - a pre-`vNEXT` connector behaves as before.
- A cost check: the solve and a drag of a large imported assembly (the
  SO-101 STEPs), timed before and after, CPU time.

**Stage 6 — cost and the limit.**
- The bench workloads (`tools/bench.py`): the plate at each resolution, in
  both representations.
- The interchange doc's cost table gains the recognised column, and the
  limit moves as described.
- A cost check on the recognised import of the plate, shown to fail against a
  planted spin.

**Stage 7 — dogfood on real downloads.**
- The files: complex mechanical STLs from Printables, Thingiverse and
  similar, kept out of the repo in untracked `scratch/mesh-downloads/`.
- What it reports, per file: what was recognised, the import time in both
  representations, and what failed. Each failure is fixed or logged.
- On one bracket, through the RPC surface only:
  1. measure a hole's diameter;
  2. mate an ISO bolt into it and check that it is coaxial;
  3. Offset Face a flat by 1 mm and check the volume change;
  4. project a rim and place a Hole on it.

## Files

- New:
  - `src/core/mesh/`: `TriMesh`, `MeshReaders` (STL), `Fit`, `Segment`;
  - `src/cad/MeshRebuild.{h,cpp}`.
- The OBJ adapter in `src/cad/CADKernel.cpp`.
- `src/core/Feature.{h,cpp}`: the Import's `representation`, `min_sides`, the
  reply and the refusal.
- `src/core/io/MeshImportCost.{h,cpp}` and `src/ui/ImportJob.{h,cpp}`.
- `src/core/io/Settings.{h,cpp}` and `src/ui/SettingsWindow.cpp`.
- Mates: `src/core/AssemblySolve.cpp`, the connector model and its
  serialization (`SerializeAssembly.cpp`), and `src/ui/MateActions.cpp`.
- Serialization and goldens: `src/core/io/SerializeFeatures.cpp`,
  `SerializationInternal.h`, and `tests/golden/vNEXT-*.json`.
- RPC: `src/remote/Remote{Project,Modeling,Assembly}.cpp` with their
  `MethodDoc` rows, plus the regenerated `rpc.py` and `rpc-surface.json`.
- The Import feature's panel in `src/ui/`.
- `CMakeLists.txt`.
- Tests:
  - `tests/mesh_fixtures.{h,cpp}`, `tests/test_mesh_*.cpp`,
    `tests/fixtures/mesh/` and `tests/test_reader_fuzz.cpp`;
  - a mate-print test, the GUI test, and the `tools/bench.py` workloads.
- Docs:
  - docs/interchange/README.md;
  - docs/ui/settings.md;
  - docs/assembly's connector section;
  - docs/versioning/format.md;
  - docs/architecture.md (the `src/core/mesh/` line);
  - the organic plan's `Segment` and `representation` paragraphs.

## Licences

Nothing new is linked. The fits and the segmentation are ours, and the
rebuild uses OCC classes already linked (`GeomAPI`, `ShapeFix`, `BRepCheck`).
OpenSCAD only authored fixture files; it is not built or linked.
