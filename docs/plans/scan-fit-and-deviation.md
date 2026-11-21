# A scan: what a fit found, and how far the model is from it

A primitive fitted to a scan rests on the surface that was clicked, says how
good it is and takes hints
([fitting a primitive to a mesh](../modeling/README.md#fitting-a-primitive-to-a-mesh)).
It should also say what it found when it will not fit, and fit a torus. A
model built on a scan should be checkable against that scan: laid onto it,
and coloured by how far it stands off.

## The problem

- **A refusal does not say what is there.** A click on a stepped bore, a
  lip, or two faces meeting at a few degrees grows one region over both and
  reports a fit with a large `rms_mm`, or refuses with a distance. Nothing
  says "two cylinders, R4.68 and R5.18".
- **No torus.** `fit_primitive` cannot fit a round or a ring, and mesh
  recognition leaves the corner where two fillets meet as facets
  (docs/interchange/README.md#not-done).
- **A scan cannot be laid onto a model, or onto the scan before it.**
  `align_import` has floor, three points, principal axes and scale
  (src/core/ImportAlign.cpp). A re-scan has to be placed by hand "in the
  same place" for its datums to follow, and a seed left off the surface
  fails the datum.
- **Nothing says how far the model is from the scan.** `compare_geometry`
  is B-Rep against B-Rep by volume. The scan route (datums, a section into a
  sketch, the model) ends with no check of the result, and neither a Cradle
  nor a recognised mesh can be judged against the mesh it came from.

## The shape of the fix

### 1. A fit that says what it found

- **Two surfaces.** When the fit's `rms_mm` is over the tolerance, the
  region is split in two by distance to two models of the asked kind (from
  several starts) and each refitted. The split is kept only if both hold a
  real share of the region, they stand further apart than their scatter, and
  one smooth surface (a quadric for planes; a radius varying evenly along
  the axis for cylinders) does not explain the region as well. The call is
  then refused naming both: "two cylinders here: R4.68 mm and R5.18 mm,
  stepping at 12.4 mm along the axis; click one, or give `radius_mm`". A
  `from_fit` datum fails with the same sentence.
- **A plane on a crown.** A plane whose region's normals spread past 10°, or
  whose region covers under 15% of its own bounding rectangle, is a strip
  along the top of a round. It is refused naming the cylinder that fits.

### 2. A torus

`kind: "torus"` in `mesh::Fit` and `fit_primitive`: every normal of a torus
passes through its spine circle at the tube's radius. For a candidate tube
radius (searched on a log scale, both signs: a convex tube and a concave
round) the points are moved inward along their normals by it; a plane and a
circle through the moved points give the axis, the centre and the major
radius. The candidates are scored as the other kinds' hypotheses are, and
the best refined by least squares on the seven parameters. The reply:
`centre_mm`, `axis`, `major_radius_mm`, `minor_radius_mm`, and `concave`.
`auto` tries it last and takes it only when no simpler kind covers 95% as
much. A `DatumAxis` fits a torus (its axis through its centre).

Recognition (`mesh::SegmentAll`, `cad::RebuildFromRegions`) gains the torus
as a region kind, rebuilt on a `Geom_ToroidalSurface`, with the same three
checks and demotion as the other kinds. The fillet-corner line leaves
"Not done".

### 3. Laying a scan onto a body

`align_import {mode: "best_fit", onto, …}` moves an Import's placement so its
mesh lies on `onto`: a body of the part (B-Rep, by its tessellation, or
another mesh body).

- **Rigid only.** Scale is never fitted: a scale error is something to see.
- **Starts.** The placement as it stands, and the 24 attitudes that lay the
  mesh's principal axes on the target's. A partial scan of a near-cube ranks
  its axes differently from the whole, so all 24 are tried.
- **Each start** runs point-to-plane ICP on a thinned sample (each step
  solved about the correspondences' centroid, the pose kept in doubles),
  with pairs past three times the median distance left out of the step.
- **The winner** is the pose with the lowest *capped mean distance over
  every sample*, not over the pairs a step kept: scoring the survivors lets
  a pose slide off the part and look better.
- `pairs: [[from_mm, to_mm] × 3]` instead of the 24 starts: three picked
  pairs give the start (refused when collinear).
- `region: {feature_id, point_mm, radius_mm}` limits the fit to a patch.
- The reply: `rotate_deg`, `translate_mm` as applied, `moved_mm` (the
  largest displacement of a mesh corner), `mean_mm` and `p95_mm` after, and
  `within_mm` with the share of samples inside it. It is one undo step. It
  runs as a heavy job with progress and can be cancelled between passes.

A datum's seed is a world point, so a re-scan laid onto the old scan's body
by this keeps its datums.

### 4. How far a body is from the scan

`compare_scan {scan, body, tolerance_mm?}` (inspection; changes nothing).
`scan` is a mesh body, `body` any body.

- **Signed distance** from each scan vertex to the body, through
  `mesh::Bvh`. The sign is from the angle-weighted pseudonormal of the
  closest feature (face, edge or vertex), which is exact on a closed
  surface; a face normal speckles at sharp edges.
- **Facing.** A scan point whose nearest surface faces away from the scan's
  normal there is measured to the nearest surface that faces its way. On a
  thin wall the nearest surface past half the thickness is the far side,
  and the reading would have the wrong sign.
- **The other direction.** Points on the body are measured to the scan where
  scan surface faces the same way and lies under them; elsewhere they read
  "no scan". This finds what the model has and the object does not, and
  gives the share of the body the scan never saw.
- **The tolerance** is `tolerance_mm`, or three times the scan's noise
  (`mesh::ScanTolerance`), never less than the body's tessellation chord,
  which is named in the reply as the floor under the map.
- **Hotspots.** Vertices past the tolerance are grouped by distance (a few
  point spacings), not by shared triangles. Each has `at_mm`, `area_mm2`,
  `depth_mm` (signed peak), and the body's face it lies on. A patch under
  4 mm² or under twice the tolerance deep is `texture`, the rest `feature`.
- **The reply:** `within` (share of scan area inside the tolerance),
  `mean_mm`, `rms_mm`, `p95_mm`, `min_mm`, `max_mm`, `no_scan` (share of the
  body), `hotspots[]` worst first, and a `receipt`.
- **The picture.** A scalar field on the scan's vertices, drawn by the
  renderer that draws FEM fields (`src/core/mesh/Field.{h,cpp}`), with its
  legend, on a diverging map centred on zero; `set_scan_deviation {scan,
  body}` shows it and `clear` removes it. The Inspect toolbar gains
  **Compare to scan**; its tool window lists the hotspots, and a row frames
  the camera on its place.

The recognised mesh's check and the Cradle's are restated on this distance
where they compare a body with its mesh.

## Stages

Each lands on its own with the suites green.

- **B.** Two surfaces and the crown (§1).
- **C.** The torus (§2), in `fit_primitive` and the datums, then in
  recognition.
- **D.** Best fit (§3).
- **E.** Deviation (§4): the RPC, then the picture and the tool.

## Not in this plan

Repairing, reducing, smoothing or wrapping a mesh, meshing a point cloud:
MeshLab and Blender do these, and the refusals already say so. Minimum-zone
form, GD&T on a scan and inspection reports: metrology, not modeling.
Freeform surface fitting. Relations and round sizes offered for a section's
sketch: [section-sketch-relations.md](section-sketch-relations.md).

## Files

- `src/core/mesh/Grow.cpp`, `Fit.{h,cpp}`, `Segment.{h,cpp}`; new
  `src/core/mesh/Split.{h,cpp}` (two surfaces), `Register.{h,cpp}` (ICP),
  `Deviation.{h,cpp}`
- `src/core/MeshFit.{h,cpp}`, the datum features' `fit`, `FitReport`
- `src/core/ImportAlign.{h,cpp}`, `src/core/mesh/Align.{h,cpp}`
- `src/cad/MeshRebuild.cpp` (the torus)
- `src/remote/RemoteInspection.cpp` (`fit_primitive`, `compare_scan`,
  `set_scan_deviation`), `src/remote/RemoteModeling.cpp` (`align_import`)
- `src/ui/FitTool.cpp`, a new `src/ui/ScanCompareTool.{h,cpp}`, the field
  renderer
- `tests/mesh_organic.{h,cpp}` (fixtures), `tests/test_mesh_fit_scan.cpp`,
  new `tests/test_mesh_register.cpp`, `tests/test_mesh_deviation.cpp`
- docs/modeling/README.md, docs/interchange/README.md,
  docs/inspect/README.md

## Verified by

Every fixture is tilted off the axes, so a fit that only works axis-aligned
cannot pass, and every number is checked against the generator's.

- **Two surfaces.** A stepped bore (R4.68 / R5.18) and a 0.3 mm step
  between two planes are refused naming both sizes; a single tapered bore
  and a gently crowned plate are not split.
- **The torus.** A scanned ring and a concave round between two faces:
  both radii within the noise, `concave` right. A faceted CAD part with
  fillets meeting at a corner is recognised with a toroidal face and passes
  recognition's volume check.
- **Best fit.** A scan displaced by random rigid motions (seeded) from its
  body is brought back within the noise, for a bracket, a near-cube with one
  feature, and a scan of half the part; a thin plate is not flipped onto its
  back. Scale is never changed. A datum fitted before a re-scan refits after
  it without its seed moving.
- **Deviation.** A body with one face raised 0.20 mm and another sunk
  0.15 mm against the scan: exactly those two hotspots, with those signs and
  depths within the noise, on those faces, and every other face inside the
  tolerance. A 1 mm wall reads its own side, not through to the far one. A
  scan of half the part gives `no_scan` near a half. A scan against the
  body it was sampled from reads `within` near 1.
- **Cost** (CPU time, `[slow]`): best fit of a 500k-triangle scan and its
  deviation each have a stated budget, set from the first measurement; the
  two-surface split keeps the region's cost check
  (`tests/test_mesh_fit_scan.cpp`) within twice the old growth.
- **Through the pipeline:** a smoke that imports a scan, lays it on a body,
  fits its datums, compares, and reads the same numbers after save and
  reopen; `refused_calls_smoke` covers each new refusal changing nothing.
