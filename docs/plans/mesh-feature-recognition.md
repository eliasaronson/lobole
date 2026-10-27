# Recognising holes and planes in a faceted STL

The mechanical half of the mesh work. It builds the `Segment` and `Fit` parts
of `src/core/mesh/`, plus the part of `TriMesh` they need, in the shape the
organic-meshes plan sets out. The scan round extends this; it does not
replace it.

## The problem

People download mechanical parts as STL: brackets, adapters and enclosures
from Printables and Thingiverse. Today such a file imports as one planar face
per triangle (`ImportFeature::ReadModel` → `CADKernel::LoadSTL` → `SewAt`):

- a hole is a prism of 26–223 facets, so it has no radius to measure, no
  axis to mate to (`CADShape::FaceAxis` needs `GeomAbs_Cylinder`) and no
  circle to project into a sketch (`ProjectElement` samples it into a
  B-spline);
- a flat is a fan of triangles, so Offset Face, sketch-on-face and
  `grow_faces` need a `Refine` first;
- everything after the import is slow, because the body has thousands of
  faces.

These costs were measured in a headless instance. The part is a 120×80×6
plate with 24 through holes (Ø5–7.5 mm), exported by our own `export_stl`
and imported back with `import_step`.

| file | triangles | import | `mass_properties` | a box cut at one corner |
|---|---|---|---|---|
| STEP of the same part | 30 faces | 0.03 s | 0.02 s | 0.03 s |
| STL at 0.05 mm (26-gon holes) | 2,652 | 0.35 s | — | — |
| STL at 0.01 mm (50-gon) | 5,468 | 0.66 s | 0.42 s | 1.72 s |
| STL at 0.002 mm (112-gon) | 12,060 | 2.32 s | — | — |
| STL at 0.0005 mm (223-gon) | 23,948 | 8.18 s | 1.92 s | 9.14 s |

A mechanical mesh costs three times what `MeshImportCost`'s law predicts
(2.46 s predicted at 24k triangles). Its long fan triangles make the quilt
harder than the torus in the law's table did. The STEP row is the floor a
rebuilt B-Rep should approach: the same 30 faces.

## The shape of the fix

Recognition happens **inside the import**, as an option on the `Import`
feature, not as a separate feature:

- the face set it produces is what every later feature names, so it belongs
  where the faces are made;
- it replaces the sewing rather than adding a step after it, and the sewing
  is the slow part;
- it runs inside the import job, which is already off the frame, with
  progress and cancel.

```
STL bytes ─► mesh::ReadStl ─► TriMesh ─► mesh::Segment ─► Regions (+ Fit each)
                                                     │
                       cad::RebuildFromRegions ◄─────┘  (OCC, src/cad/)
                                │
                     solid with analytic faces and leftover triangles
                     ── or, if it fails a check, today's facet path, with the reason
```

### `src/core/mesh/` — pure data, no OpenCASCADE

**`TriMesh`** (`TriMesh.{h,cpp}`), as the organic plan specifies it:
- `glm::dvec3` positions and `uint32` indices;
- weld, edge adjacency, consistent orientation, components, boundary loops,
  `Closed()`, and volume by the divergence theorem;
- a content hash.

What this round adds to the organic sketch:
- **The weld is exact first.** CAD exporters write shared vertices
  bit-identical, so they are joined on exact float equality. The diagonal
  tolerance applies only to vertices left unmatched.
- **`Quantum()`** is the float32 spacing at the file's largest coordinate
  (2⁻²³·max|x|), taken before the import recentres. A vertex of an exported
  CAD mesh lies on its true surface to within that spacing, so every
  tolerance below is a small multiple of it.

**`ReadStl`** (`MeshReaders.{h,cpp}`) reads binary and ASCII STL straight
into a `TriMesh`, with no B-Rep. It joins `tests/test_reader_fuzz.cpp`,
which has no STL family today. OBJ keeps its XCAF read for names and
colours. An adapter in `src/cad/` turns each OBJ triangulation into a
`TriMesh`, so OBJ is recognised too. The organic plan's own OBJ reader can
replace that adapter later.

**`Fit`** (`Fit.{h,cpp}`) fits plane, cylinder, cone and sphere:
- Each fit returns `FitResult {kind, frame, radius, half_angle, rms, max}`,
  the shape the organic plan asks for.
- The axis comes from the Gauss map, so no iterative search is needed:
  - a cylinder's facet normals lie on a great circle, and a cone's on a
    small circle, so the axis is the normal of the plane fitted to the
    normals' tips;
  - a sphere's normals pass through one point, which is a linear
    least-squares solve.
- A cylinder's or cone's radius and centre then come from an algebraic
  circle fit of the vertices projected along the axis. Gauss–Newton refines
  it, and the residuals are reported.
- Cones are included because countersinks and chamfers are cones, and the
  Gauss map makes them almost free.
- **No Eigen.** `HAVE_EIGEN` is optional in `designer_core`
  (CMakeLists.txt:799), and recognition must exist in every build. The fits
  need only a 3×3 symmetric eigen-solve (Jacobi) and normal equations up to
  4×4, written here.
- **No RANSAC this round.** A CAD export has no outliers: its vertices are
  on the surface. RANSAC is for scans, and the scan round adds it behind the
  same `Fit` signature.

**`Segment`** (`Segment.{h,cpp}`):

`SegmentAll(mesh) → Segmentation {regions[], left[]}`. Each region holds its
triangles, its `FitResult`, its boundary chains, and for a cylinder or cone
its side count and sagitta.

1. **Flat patches.** A flood fill joins triangles across edges whose two
   triangles are coplanar to `k·Quantum()` (vertex distance to the patch
   plane, not a normal angle). A patch is a plane region when its fit holds
   over the whole patch.
2. **Curved regions.** Patches that meet across smooth edges are grown
   greedily under a hypothesis: the Gauss-map fit must hold, and every vertex
   must be on the fitted surface to `k·Quantum()`. A patch that breaks the
   fit is not taken. This keeps the plane a fillet is tangent to out of the
   fillet's cylinder: its normal is on the fillet's great circle, but its
   vertices are far from the cylinder.
3. **Polygonal, not round.** A deliberate hex nut trap's vertices lie on a
   circle too, so the vertex residual cannot tell it from a hole. What does
   is the tessellation:
   - A curved region is taken only if its facets turn by a **uniform**
     angle. An exporter's circle has equal chords; a designed polygon
     usually does too, so this alone is not enough.
   - It must also turn by **at most 30° a facet** (12 or more sides a full
     turn), with at least three facets.
   - Everything else is reported as polygonal and kept as flats. Its side
     count and circumradius go in the reply, because a hex trap's flats are
     real faces a nut sits on.

   The bound is a count of sides, not a length, so the derived-defaults rule
   does not apply. It is still a judgement, and the first question below.
4. **The rest.** A torus corner of two fillets, or a freeform blend, stays
   as its triangles, listed in `left`.

The same module gives the organic plan's seeded form:
`GrowFrom(mesh, seed, kind)` is step 2 started from one triangle, which
`fit_primitive` (organic stage 7) calls. **This differs from the organic
sketch**, which grows "by normal deviation from a seed". A cylinder's normals
turn by design, so normal deviation cannot grow one. Growth is by the fitted
surface's residual, and the organic plan should say so.

`MeshBvh` is not needed here and is not built here. The fits read adjacency,
not closest points. It stays the scan round's.

### `src/cad/MeshRebuild.{h,cpp}` — the B-Rep from the regions

`RebuildFromRegions(mesh, segmentation, tolerance, progress) → {shape,
report}` is the only OCC code in the round.

- **Surfaces.** Each region becomes one face on a `Geom_Plane`,
  `Geom_CylindricalSurface`, `Geom_ConicalSurface` or `Geom_SphericalSurface`.
- **Shared edges.** A boundary chain between two analytic regions is one
  edge on their exact intersection:
  - a line for plane ∩ plane;
  - a circle, or an ellipse for an oblique plane, for plane ∩ cylinder,
    plane ∩ cone and plane ∩ sphere (`IntAna_QuadQuadGeo`);
  - otherwise `GeomAPI_IntSS`, taking the branch nearest the chain.

  The chain's corner vertices are the mesh vertices where three regions meet,
  projected onto that curve. A hole's rim is a closed circle with one seam
  vertex.
- **Edges to leftover triangles** are the chain's polyline, one edge a
  segment, so the leftover triangles share them exactly.
- **No sewing.** Every face is built on shared `TopoDS_Edge`s with
  `BRep_Builder`. P-curves come from `ShapeFix_Edge`, and the shell is
  assembled directly. The quilt never runs, which is the linear-time shell
  the interchange doc's "Not done" asks for.
- **Checks.** The result must pass all of these:
  - `BRepCheck_Analyzer`;
  - closed when the mesh was `Closed()`;
  - a volume within the bound the fits imply: Σ over curved regions of area
    × sagitta, plus the mesh's own `Quantum()` term.

  If any check fails, that body takes today's facet path (`MakeShapeOnMesh`
  + `SewAt`), and the reply says which check failed. Nothing is substituted
  silently.
- **Face order is fixed.** Faces are ordered by the lowest file-triangle
  index each one holds, so the same file with the same option always names
  the same faces.

### The `Import` feature

- **`surfaces: 'facets' | 'recognize'`** (STL and OBJ only):
  - refused on STEP, IGES and BREP, as `units` is refused on STEP;
  - any other value is refused, naming the two.
- **A document written before `vNEXT` reads as `facets`**, because its faces
  were named that way. Every saved import keeps its names, so this is a
  format change: `kVersionPending`, the golden `vNEXT-import-surfaces.json`
  plus `.expect.json`, and a pending row in format.md.
- **A new import takes `recognize`** from `add_feature`, `import_step`, File
  › Import, `--file` and the in-app browser. This is question 2.
- `surfaces` is part of the feature's input hash, so the baked geometry
  (`GeometryCache`, keyed by `TipChainHash`) tells the two apart. A
  recognised import reopens with no re-read, exactly as today.
- **Changing `surfaces` on an Import whose faces something references is
  refused**, naming the referrers. Every face is renumbered by the change, and
  the referrers react differently:
  - a sketch projection re-finds by print only when the kind matches (a
    facet's "plane" never matches a "cylinder");
  - a direct edit refuses;
  - a mate resolves purely by name (`AssemblySolve.cpp`), so it would
    **silently land on another face**.

  The last one is an existing bug for any import whose file changes. It is
  question 3.
- **The reply reports what was recognised**, in `add_feature`/`import_step`
  and in the feature's row in `get_state`:

  ```
  surfaces: {mode, planes, cylinders: [{radius_mm, axis, sides, sagitta_mm}],
             cones, spheres, polygonal: [{sides, circumradius_mm}],
             left_triangles, fell_back?: "<check that failed>"}
  ```

  The GUI's import note pill shows one line of it ("12 holes, 9 planes, 6
  facets left").

**The mesh limit.** `MeshImportCost` prices the sewing of every triangle. A
recognised import sews only its leftover triangles, so after segmenting, the
job prices `left_triangles` with the same law. The refusal before the read
stays on the triangle count until the segment cost is measured (stage 4).
Then a `recognize` import past the limit is read and segmented on the
worker, and refused there only if what is left would still be past it.

## Tolerances

Every bound comes from the mesh:
- coplanarity and on-surface tests use `k·Quantum()`;
- the volume check uses the fits' own sagittas;
- the weld uses exact equality, then the diagonal;
- the edge and vertex tolerance handed to OCC is the largest on-surface
  residual found, never below `Quantum()`.

The constant `k` is unitless, about 8, and is fixed by the fixtures in
stage 1. A file noisier than float precision (a mesh re-saved through a
mesh editor that moved vertices) recognises less, says so in
`left_triangles`, and imports as today. Loosening to noise is the scan
round's RANSAC.

## Stages

Each stage lands on its own.

**Stage 0 — fixtures (S).** `tests/mesh_fixtures.{h,cpp}`, the file the
organic plan names, gains generators of known CAD parts written through
`CADKernel::SaveSTL`, where both the deflection and the angular deflection
are free:
- the plate above;
- a bracket with counterbores, a countersink, a boss and its hole, and a
  hex nut trap;
- an L-bracket with straight fillets and torus corners;
- an oblique hole.

Each is written at 24 and 128 sides and at chord-limited deflections, so
every radius and axis is known exactly. There are also polygonal controls
(hexagon, octagon and decagon pockets) and two or three real-style files from
other exporters (question 4). The bench workloads in `tools/bench.py` import
the plate at each resolution in both modes.

**Stage 1 — `TriMesh`, `ReadStl` and `Fit` (M).** Unit tests check:
- the weld of exact and near-duplicate vertices;
- components and loops;
- the volume of a closed fixture against its CAD volume;
- each fit's parameters on fixture regions against the generated ones;
- the residual on a polygon.

ReadStl joins the reader fuzz.

**Stage 2 — `Segment` (M).** On every fixture, the region counts and every
radius, axis and side count must match:
- 24 and 128 sides are both found;
- hexagon, octagon and decagon are reported polygonal and kept flat;
- a fillet is not merged with the planes it is tangent to;
- torus corners are left.

Run on the plate, segmentation is linear in time (a `[slow]` cost check
against a bound set from the bench).

**Stage 3 — the rebuild and `surfaces` (L).** `MeshRebuild`, the Import
option, serialization, `vNEXT`, the reply, the fallback, and the refusal of a
referenced change. Tests check the outcome through the real pipeline:
- `measure` gives the hole's diameter, to the fixture's radius ±
  `Quantum()`;
- `FaceAxis` gives its axis, and a cylindrical mate of a bolt to it solves
  coaxial;
- a projected rim is a sketch circle, and a Hole placed on it is concentric;
- Offset Face of a recognised flat moves it by the distance, and of a
  recognised bore changes its radius;
- the volume matches the CAD part to within the reported bound;
- a fixture forced through each failed check falls back and says which;
- the same file imported twice names the same faces;
- a sketch projection of a recognised face re-finds it when the file is
  re-exported at another resolution, since the kind, area and centroid stay
  within print tolerance. If they do not, it says so by name.

**Stage 4 — cost and the limit (S).** The bench numbers go into the
interchange doc's cost table for both modes, and the limit moves to
`left_triangles` as described above. Cost checks (CPU time) cover the
plate's recognised import and the segmentation, each shown to fail against a
planted spin.

**Stage 5 — GUI and dogfood (S).**
- The Import feature's panel gains the `surfaces` choice; changing it is
  refused, with its reason in the panel, when the import is referenced. A
  GUI test covers it, plus the `defaults/*` entry.
- The dogfood drives only the RPC surface on a downloaded-style bracket:
  1. import it;
  2. measure a hole's diameter;
  3. mate an ISO bolt into it in an assembly and check that it is coaxial;
  4. Offset Face a flat by 1 mm and check the volume change;
  5. project a hole rim and place a Hole on it.

  It reports each number, and the import time in both modes.

## Files

- `src/core/mesh/` (new): `TriMesh`, `MeshReaders` (STL), `Fit`, `Segment`.
- `src/cad/MeshRebuild.{h,cpp}` (new), and the `TriMesh` adapter for OBJ
  triangulations in `src/cad/CADKernel.cpp`.
- `src/core/Feature.{h,cpp}`: `ImportFeature::surfaces`, `ReadModel`, the
  reply and the referenced-change refusal.
- `src/core/io/MeshImportCost.{h,cpp}`: pricing what is left.
- `src/ui/ImportJob.{h,cpp}`: the segment and rebuild phases, and the
  worker-side limit.
- `src/core/io/SerializeFeatures.cpp`, `src/core/io/SerializationInternal.h`,
  `tests/golden/vNEXT-import-surfaces.json` and its `.expect.json`.
- `src/remote/RemoteProject.cpp` and `RemoteModeling.cpp` (the `MethodDoc`
  rows and the `surfaces` reply), and the regenerated `python/lobole/rpc.py`
  and `rpc-surface.json`.
- The Import feature's panel in `src/ui/`, and its GUI test.
- `CMakeLists.txt`: the new sources.
- Tests: `tests/mesh_fixtures.{h,cpp}`, `tests/test_mesh_fit.cpp`,
  `tests/test_mesh_segment.cpp`, `tests/test_mesh_rebuild.cpp`,
  `tests/test_reader_fuzz.cpp`, and `tools/bench.py` workloads.
- Docs, when it lands:
  - `docs/interchange/README.md`: the STL bullet, *Editing a downloaded
    part*, *What a mesh costs*, and *Not done*, which loses "feature
    recognition" and "a faster sewing";
  - `docs/versioning/format.md`;
  - a `docs/architecture.md` line for `src/core/mesh/`;
  - and the organic plan's `Segment` paragraph (growth by residual).

## Licences

Nothing new is linked. The fits and the segmentation are ours, and the
rebuild uses OCC classes already linked (`IntAna`, `GeomAPI`, `ShapeFix`,
`BRepCheck`).

## Questions for the user

1. **Where a polygon stops being a polygon.** At most 30° a facet (12 or
   more sides a full turn) recognises every CAD export but not OpenSCAD's
   defaults, which make a small hole a pentagon. Is a pentagon left as
   facets right, or should there be a per-import override (`min_sides`)?
2. **The default for a new STL/OBJ import.** The proposal is `recognize`
   (it is better and faster, and falls back by name), with saved documents
   reading as `facets`.
3. **Mates that misbind when an import's faces renumber.** This happens
   today on any file change. Should a mate to an import's face carry the
   same print sketches do, in this round, or be planned separately?
4. **Real-style fixtures.** Should we commit two or three small STLs
   authored by us in other exporters (OpenSCAD, FreeCAD), with their
   sources, and keep a real Printables download for the dogfood only
   (licences vary per model)?
