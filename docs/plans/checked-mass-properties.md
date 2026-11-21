# Volume and mass that are checked before they are reported

`mass_properties`, the kernel cases, `compare_geometry`, the BOM, a printed
part's weight, a study's mesh check and every guard that compares a volume
before and after read one integral per body. It should be right to the
digits it is reported to, or say how far off it can be.

## The problem

- **The fixed rule is wrong on some trimmed faces, and says nothing.**
  `CADShape::Volume` and `CADShape::ComputeMassProps` (src/cad/CADKernel.cpp)
  call `BRepGProp::VolumeProperties` with no precision, which is
  OpenCASCADE's fixed Gauss rule. Each boundary edge of a face gets one
  Gauss-Legendre rule over its whole parameter range: 2(d+1)(k-1) points
  for a B-spline pcurve of degree d with k knots, at least the surface's
  order (18 on an elementary surface), at most 61. The integral across the
  face uses the surface's order. That is exact on a face bounded by lines
  and conics. An intersection curve is a B-spline that is smooth only
  between its knots, and one rule across the knots is not exact. The tube
  and nine bars
  (docs/known-bugs.md#a-union-of-a-tube-and-nine-bars-through-its-wall-gains-volume)
  measures 4596.234 mm³ where the closed form is 4595.675, and one bar adds
  44.609 where it should add 44.6277. A wrapped cut's ruled walls read
  45 ppm loose, and tests/test_refold.cpp measures around them with its own
  rule.
- **The one caller that asks for an error trusts the wrong one.**
  `fem::Mesh` asks the adaptive rule for 1e-5 and adds that rule's own error
  estimate to its slack. On the faces of the M10 thread
  (`06-threads/004`) the adaptive rule reads 7 % heavy while reporting that
  it converged. It also puts a revolved Bezier profile 0.6 mm³ over and a
  horn torus 0.02 mm³ over, and on the menger sponge's 400-face union it
  runs past the case's 120 s.
- **Area uses the same rule.** `SurfaceProperties` integrates a face's
  edges the same way, and the cases grade `area_mm2`.
- **Volume is integrated in many places.** Besides the two above,
  `BRepGProp::VolumeProperties` is called directly in src/core/Measure.cpp
  (interference, overlap, set comparison, a body's centre),
  MeasureSeal.cpp, MeasureInsertion.cpp, PrintSplit.cpp, HelicalSweep.cpp,
  WormGear.cpp, src/cad/MeshRebuild.cpp and fem/AssemblyStudy.cpp.
- **Mass properties are integrated three times on every call.**
  `ComputeMassProps` runs the volume rule twice (about the origin, then
  about the centre of mass) and the area rule once, with no cache. The
  BOM, the Properties dock, an assembly's mass, a printed part's mass and
  the pendulum's length search all call it. `Volume` alone is cached, by
  TShape.

## The shape of the fix

### 1. One integrator, face by face

`cad::Integrate(shape)` returns a body's volume, its first and second
moments and its area, each with an uncertainty in its own unit. The moments
are taken about the centre of the shape's box and then shifted to the
centre of mass. Faces are summed in explorer order, so the same shape gives
the same bits on every run and with any number of threads.

- **The fixed rule first.** Each face is integrated alone by BRepGProp,
  about the same point. The faces' figures sum to the solid's.
- **A face is suspect** when one of its pcurves is not a line or a conic.
  A face that is not suspect keeps the fixed rule's figure, and a body with
  no suspect face is measured exactly as today.
- **A suspect face gets a second estimate.** This is the same
  Green's-theorem integral, computed here: each pcurve is split at its own
  knots and at the surface's knot lines it crosses, and Gauss-Legendre runs
  on each piece. Then every piece is halved, until two successive estimates
  agree to 1e-12 of the body's scale (the sum of the faces' magnitudes), at
  most six times. Convergence is seen, not estimated.
- **The two are compared.** If they agree to 1e-10 of the body's scale,
  the fixed rule's figure stands, so nothing moves where it was already
  right. Otherwise the subdivided figure is used, and its uncertainty is
  the last halving's change.
- The area of a suspect face is checked the same way.

### 2. Every measure goes through it

- `CADShape::Volume` and `ComputeMassProps` call it. The volume cache keeps
  today's key but holds the whole result in the TShape's own frame, and a
  location moves the centre and turns the inertia. Mass properties of an
  unchanged body become a lookup. `MassProps` gains `volumeUncertainty` and
  `areaUncertainty`.
- Every direct caller listed above calls `cad::Integrate` or `Volume`. The
  FEM mesh check (`fem::MeshVolumeSlack`) takes the integrator's
  uncertainty in place of the adaptive rule's estimate.
- A convention test: `BRepGProp::VolumeProperties` appears only in
  src/cad/Integrate.cpp. A face's area that is used only to choose or sort
  faces keeps calling `SurfaceProperties`.

### 3. An uncertain number says so

- A `mass_properties` row carries `volume_uncertainty_mm3` and
  `area_uncertainty_mm2` when they are not zero. When one is past 1e-9 of
  the body's figure (the kernel cases' floor), `warnings` names the body
  and the faces that did not converge. `compare_geometry` repeats that
  warning beside its volumes. `get_state`'s digest stays a bare
  `volume_mm3`.
- The kernel-case grader counts a figure whose uncertainty is wider than
  the case's tolerance as a `volume` failure, even when the figure itself
  is inside the tolerance, and `replay` prints the uncertainty.

## Stages

- **A.** The integrator (§1) beside today's rule, proved on the tube, the
  thread, the revolved profile, the horn tori and the sponge before any
  caller moves: that the faces' figures sum to the solid's, that the two
  estimates agree where the fixed rule was right, and what it costs. The
  agreement and convergence thresholds are set from these measurements.
- **B.** `Volume`, `ComputeMassProps` and the cache (§2), then the direct
  callers and the convention test.
- **C.** The uncertainty in the replies and the grader (§3).

## Files

- new src/cad/Integrate.{h,cpp}
- src/cad/CADKernel.{h,cpp}: `Volume`, `ComputeMassProps`, `MassProps`, the
  volume cache
- src/core/Measure.cpp, MeasureSeal.cpp, MeasureInsertion.cpp,
  PrintSplit.cpp, HelicalSweep.cpp, WormGear.cpp, src/cad/MeshRebuild.cpp,
  src/core/fem/FemMesher.cpp, src/core/fem/AssemblyStudy.cpp
- src/core/MassProps.{h,cpp} (`BodyMass` carries the uncertainty),
  src/remote/RemoteInspection.cpp (`mass_properties`, `compare_geometry`,
  their `describe` rows)
- tools/kernel_cases.py, tests/reference_parts.py (`measure`),
  tests/kernel_cases/selftest.json, tests/kernel_cases/status.json
- new tests/test_integrate.cpp; tests/test_cadkernel.cpp,
  tests/test_refold.cpp (drops its own rule), tests/test_conventions.cpp,
  tests/test_fem_mesh_report.cpp
- docs/inspect/README.md#mass-properties; docs/modeling/README.md (the
  volume cache paragraph, and
  docs/modeling/README.md#known-rows-that-are-not-kernel-defects);
  docs/known-bugs.md (the tube entry goes, the blend entry is re-measured);
  docs/simulation/README.md (the mesh check's slack)

## Verified by

Every fixture is placed off the origin and turned off the axes.

- **The mechanism.** A cylinder cut by an oblique plane has an ellipse as
  its edge. Built again with that edge's pcurve swapped for a B-spline
  approximation of it, the fixed rule moves and the integrator does not:
  volume, centre and inertia match the conic build to 1e-9.
- **The cases that were wrong.** One bar through the tube's wall adds
  44.6277 mm³ to 1e-9; nine bars give 4595.6754 with the centre on the axis
  to 1e-9 mm. test_refold's plug holds through `CADShape::Volume`.
- **Nothing moves where it was right.** On a body with no suspect face,
  each face's figure is bit for bit BRepGProp's. The analytic mass
  properties in test_cadkernel, the golden expectations and
  test_determinism are unchanged. `06-threads/004`,
  `04-spline-profiles/002`, the horn tori (`03-analytic-profiles/010`,
  `4007`) and the menger sponge (`5126`) still pass, the sponge inside its
  slow-list time.
- **The corpus.** `1107` and the five rows the adaptive rule fixed (1207
  twice, 926, `05-holes-and-taps/012`, 3528) pass, and their rows and
  entry go. Each case under the blend entry is re-run; a row that passes
  goes, and the rest keep the entry with the new figure.
- **An uncertain number is said.** With the halvings capped at zero by a
  test seam, a trimmed face reports a non-zero uncertainty, a
  `mass_properties` warning and a `compare_geometry` warning, and the
  grader's self-test fails a planted answer that sits inside its tolerance
  with an uncertainty wider than it.
- **FEM.** test_fem_mesh_report passes on the new slack, and a meshed
  thread body is not refused.
- **Cost** (`[slow]`): `mass_properties` on a body with no suspect face is
  no slower than today, since one volume pass replaces two. A sketch add on
  the SO-101 gripper's 197-body part keeps its digest a lookup. A tube
  trimmed by 40 intersection curves gets a budget, stated from its first
  measurement.
