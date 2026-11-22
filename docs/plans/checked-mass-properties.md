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
  44.609 where it should add 44.6277. The rule runs across u in one piece
  too, so a surface's own knot lines break it the same way: a closed
  24-span spline extruded 12 mm reads 15826.450 for 15841.851, and a
  slanted cylinder whose cut ellipse is a rational B-spline 3.3 mm³ light
  in 6283. A wrapped cut's ruled walls read 45 ppm loose, and
  tests/test_refold.cpp measures around them with its own rule.
- **The one caller that asks for an error trusts the wrong one.**
  `fem::Mesh` asks the adaptive rule for 1e-5 and adds that rule's own error
  estimate to its slack. It puts a revolved Bezier profile 0.64 mm³ over
  while estimating its error at 1e-16, and a horn torus 0.02 mm³ over; on
  the menger sponge's 400-face union it runs past the case's 120 s. (On the
  whole M10 thread, `06-threads/004`, it agrees with the integrator to
  3e-8; applied only to the faces of an elementary surface with a
  non-conic edge it read the thread 7 % heavy while reporting that it had
  converged.)
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

`cad::Integrate(shape)` (src/cad/Integrate.h) returns a body's volume,
centre, inertia about the centre and area, each with an uncertainty in its
own unit, and each face's figures (fixed, second, used) on request
(docs/inspect/README.md#the-face-by-face-integrator). The rule it follows,
which stages B and C rely on:

- Faces are integrated about the centre of the shape's box with the fixed
  rule, and a face is **suspect** when a p-curve is not a line or a conic
  or its surface has a knot line inside the face. A face that is not
  suspect keeps the fixed figure bit for bit, with no uncertainty.
- A suspect face's second estimate is the same Green's-theorem integral cut
  at the knots, halved until two estimates agree to **1e-13** of the body's
  scale, at most **six** times. The fixed figure stands when within
  **1e-12** of the scale of it; otherwise the second estimate is used.
- The scale is the sum of the faces' volume magnitudes (times the box's
  half diagonal for a moment, its square for the inertia), and the sum of
  their areas. A scale summed from the moments themselves reads rounding as
  disagreement on a whole sphere, whose moments vanish about the box's
  centre.

The thresholds were set in tests/test_integrate.cpp on this machine: on
faces where the fixed rule is exact (box, cylinder, cone, sphere, torus,
half and horn torus, every face given a second estimate) the two sit at
most 6e-14 of the scale apart; the smallest gap on a face where the fixed
rule was wrong was 4.5e-12; a halving after the knot cuts moves a figure by
2e-15 of the scale or less except on a rational p-curve and some thread
faces, none needing more than three halvings. Measured against closed
forms (CPU seconds; today = `ComputeMassProps`'s three passes):

| body | true mm³ | today | integrator ± | today s | integrator s |
|---|---|---|---|---|---|
| tube + one bar | 4238.653886133 | -1.9e-2 | -3.8e-8 ± 7e-11 | 0.004 | 0.005 |
| tube + nine bars | 4595.675434861 | +0.56 | -7.5e-7 ± 9e-11 | 0.011 | 0.021 |
| slanted cylinder, ellipse as B-spline | 6283.185307180 | -3.29 | -1.7e-8 ± 3e-11 | 0.001 | 0.002 |
| extruded 24-span spline | 15841.850645099 | -15.4 | -2e-11 ± 5e-11 | 0.002 | 0.004 |
| M10 thread to a shoulder | 3639.157562673 | +3.7e-4 | +2.4e-4 ± 6e-9 | 0.28 | 0.37 |
| revolved Bezier profile | 34131.160186501 | +1e-10 | +1e-10 ± 0 | 0.0007 | 0.0006 |
| horn torus, 270° | 28932.176098892 | +1e-10 | +2e-10 ± 0 | 0.0010 | 0.0009 |
| level-3 Menger sponge, 8280 faces | 1000000 | +5e-8 | -6e-8 ± 0 | 1.15 | 0.82 |

The uncertainty covers the rule, not the model: the bars' intersection
curves are B-spline approximations (the adaptive rule at 1e-10 agrees with
the integrator on the nine bars to 1.2e-8, both 7.5e-7 under the closed
form), the thread's groove is SMLib's sweep (the adaptive rule at 1e-5
agrees to 3e-8), and the sponge is rounding over 8280 faces. Stage B's
`[slow]` budgets start from the CPU column; the thread, 57 of 75 faces
suspect, costs 1.3 times today.

### 2. Every measure goes through it

- `CADShape::Volume` and `ComputeMassProps` call it. `Volume` today is one
  fixed pass; `Integrate` is a volume pass and an area pass, so a `Volume`
  that only needs the volume takes the volume figures without the area's
  second estimates. The volume cache keeps
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

- **B.** `Volume`, `ComputeMassProps` and the cache (§2), then the direct
  callers and the convention test.
- **C.** The uncertainty in the replies and the grader (§3).

## Files

- src/cad/Integrate.{h,cpp}: the cache's key and whatever `Volume` needs
  to skip the area
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
- tests/test_integrate.cpp; tests/test_cadkernel.cpp,
  tests/test_refold.cpp (drops its own rule), tests/test_conventions.cpp,
  tests/test_fem_mesh_report.cpp
- docs/inspect/README.md#mass-properties; docs/modeling/README.md (the
  volume cache paragraph, and
  docs/modeling/README.md#known-rows-that-are-not-kernel-defects);
  docs/known-bugs.md (the tube entry goes, the blend entry is re-measured);
  docs/simulation/README.md (the mesh check's slack)

## Verified by

Every fixture is placed off the origin and turned off the axes.

- **The cases that were wrong**, through `CADShape::Volume` and
  `ComputeMassProps` now: the bodies of tests/test_integrate.cpp (one bar
  and nine through the tube's wall, the slanted cylinder with its ellipse
  as a B-spline, the extruded spline) to their closed forms to 1e-9.
  test_refold's plug holds through `CADShape::Volume`.
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
