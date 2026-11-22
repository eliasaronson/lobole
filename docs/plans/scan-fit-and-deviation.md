# A scan: how far the model is from it

A primitive fitted to a scan -- a plane, cylinder, cone, sphere or torus --
rests on the surface that was clicked, says how good it is, takes hints and
says what it found when one surface does not explain the region
([fitting a primitive to a mesh](../modeling/README.md#fitting-a-primitive-to-a-mesh)).
A scan is laid onto a model, or onto the scan before it, by
`align_import {mode: best_fit}`
([laying a scan onto a body](../interchange/README.md#laying-a-scan-onto-a-body)).
A model built on a scan should also be checkable against that scan:
coloured by how far it stands off.

## The problem

- **Nothing says how far the model is from the scan.** `compare_geometry`
  is B-Rep against B-Rep by volume. The scan route (datums, a section into a
  sketch, the model) ends with no check of the result, and neither a Cradle
  nor a recognised mesh can be judged against the mesh it came from.

## The shape of the fix

### How far a body is from the scan

`compare_scan {scan, body, tolerance_mm?}` (inspection; changes nothing).
`scan` is a mesh body, `body` any body.

- **Signed distance** from each scan vertex to the body, through
  `mesh::Bvh`. The sign is from the angle-weighted pseudonormal of the
  closest feature (face, edge or vertex), which is exact on a closed
  surface; a face normal speckles at sharp edges.
- **Facing.** A scan point whose nearest surface faces away from the scan's
  normal there is measured to the nearest surface that faces its way. On a
  thin wall the nearest surface past half the thickness is the far side,
  and the reading would have the wrong sign. `mesh::FacingSurface`
  (src/core/mesh/Facing.h, the best fit's) answers the nearest surface
  within an angle of a normal; `mesh::SampleSurface`
  (src/core/mesh/Register.h) gives a scan point's blended vertex normal.
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

- **E.** Deviation: the RPC, then the picture and the tool.

## Not in this plan

Repairing, reducing, smoothing or wrapping a mesh, meshing a point cloud:
MeshLab and Blender do these, and the refusals already say so. Minimum-zone
form, GD&T on a scan and inspection reports: metrology, not modeling.
Freeform surface fitting. Relations and round sizes offered for a section's
sketch: [section-sketch-relations.md](section-sketch-relations.md).

## Files

- new `src/core/mesh/Deviation.{h,cpp}`
- `src/remote/RemoteInspection.cpp` (`compare_scan`,
  `set_scan_deviation`)
- a new `src/ui/ScanCompareTool.{h,cpp}`, the field renderer
- `tests/mesh_organic.{h,cpp}` (fixtures: `ScannedSolid` gives a part and
  its scan standing on it), new `tests/test_mesh_deviation.cpp`
- docs/modeling/README.md, docs/inspect/README.md

## Verified by

Every fixture is tilted off the axes, so a fit that only works axis-aligned
cannot pass, and every number is checked against the generator's.

- **Deviation.** A body with one face raised 0.20 mm and another sunk
  0.15 mm against the scan: exactly those two hotspots, with those signs and
  depths within the noise, on those faces, and every other face inside the
  tolerance. A 1 mm wall reads its own side, not through to the far one. A
  scan of half the part gives `no_scan` near a half. A scan against the
  body it was sampled from reads `within` near 1.
- **Cost** (CPU time, `[slow]`): the deviation of a 500k-triangle scan has
  a stated budget, set from the first measurement.
- **Through the pipeline:** `tests/scan_align_smoke.py` imports a scan,
  lays it on a body, fits a datum and reads the same numbers after save and
  reopen; it gains the compare; `refused_calls_smoke` covers each new
  refusal changing nothing.
