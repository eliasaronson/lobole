# The model in double

## Problem

Sketch planes and sketch geometry are double end to end
(docs/sketching/README.md#plane). The rest of the model is not: a
primitive's position is a `glm::vec3`, an occurrence's placement is a
`glm::vec3` + `glm::quat`, a mate's DOF values are `float`, and the
assembly solve composes `glm::mat4`. A value typed as 24.7 becomes
24.70000076 the moment it lands in one of them, and a face built from it sits
0.8 um off the face it was meant to meet. Two symptoms already cost code:

- `CoplanarStartSlack` (src/core/BodyMode.cpp) moves a cut or add tool a
  hair behind its plane whenever a face lies within float rounding of it,
  because a float-placed box's top is not where the plane typed at its height
  is (docs/modeling/README.md, "A tool starts a hair behind its plane").
- docs/inspect/README.md#positions-are-float32 has to tell every caller that
  a filter on a position needs a tolerance sized to its magnitude.

Float is right for bulk DERIVED data — meshes, vertex buffers, CAM height
maps, FEM and topology fields, GPU kernels (fp64 is 1/64 rate on the GPU,
half the SIMD width and twice the memory on the CPU) — and wrong for the
model, where nothing is bulk and every value is a fact the geometry is built
from.

## The rule

The MODEL is double: every position, direction, plane, placement, mate value
and parameter a feature, an occurrence or a solver reads, and every query
result that feeds a geometric decision (a connector frame, a bounding box a
split is centred on, a point classified against a solid). DERIVED bulk data
stays float. Conversion runs ONE way, model -> derived, at the boundary where
a renderer, a mesh or a GPU buffer is filled; nothing derived flows back.
Camera, view, section and exploded-view offsets are display state and may
stay float. Appearance (colour, roughness, metallic) is not geometry.

## Shape of the fix

Model types, to double (`glm::dvec3`, `glm::dquat`, `glm::dmat4`, `double`):

- **Primitives** (`src/core/Feature.h`): `BoxFeature::position`,
  `SphereFeature::center`, `CylinderFeature::center/direction`, and the
  constructors; `ImportFeature::Shift()`.
- **Features** (`src/core/features/`): `CurveFeature::points`,
  `axisOrigin/axisDir`, `Bend::corner`; `DerivedFeature::position/
  orientation/Placement()`; `FrameFeature` axes and frame;
  `CenteredSplitOffset`'s inputs.
- **Assemblies**: `Occurrence` placement and attachment, `InnerPose`,
  `InnerMateDof`, `Mate` DOFs, `Configuration` mate overrides; the solve
  (`AssemblySolve.h`, `AssemblyLoopSolve.h`: connector frames, worlds,
  `FlatInstance`, `PathHit`, `MateAdjustFor`, `DeriveMateDofs`), motion study
  worlds, `FrameMath`, `AxisStack`, worm-gear frames, imported assembly
  placements (`ImportAssembly.h`, `CADKernel::ImportedModel` transforms).
- **Kernel queries that feed decisions** (`src/cad/CADKernel.h`):
  `Transformed`, `CreateBox`, `GetBoundingBox`, `FaceAxis`, `FacePlane` (the
  float form goes), the snap and corner points, `EdgeSnapPoint`,
  `ContainsPoint(s)`, `MassProps::centerOfMass`.
- **Measure** (`src/core/Measure.h`): the placement transforms it takes and
  the points and boxes it returns; `CollisionCache`.
- **CAM**: `FaceNormalOf` (it orients a setup); the setup paths that narrowed
  `BodyToSetup` to `mat4` for `Transformed`.
- **Readers**: every `get<float>` / `value(..., 0.0f)` in serialization and
  the RPC layer that fills one of the above reads a double.

Derived, staying float, each named in the conventions test's allow-list:
render matrices (`SketchPlane::GetPlaneMatrix`), mesh vertices, CAM height
maps and stock meshes, FEM/topology output, the curve's display polyline, the
cosmetic-thread overlay, the edge-pick probe polyline, GLB animation export,
appearance colours, lighting constants, exploded-view offsets.

The renderers keep their float matrices: `RendererSync` and the overlays
narrow `glm::mat4(world)` where an instance transform is filled.

**The slack.** With nothing model-side float, a face is either on the plane
to double precision or measurably off it. `CoplanarStartSlack` is measured
after the conversion: the tests that needed it are rewritten to place their
boxes exactly, and if the exact-placement cases fuse and cut cleanly without
it, it is removed (with its call sites in `ExtrudeFeature.cpp` and
`HoleFeature.cpp`) and the modelling doc's paragraph goes with it.

**The guard.** A conventions test (`tests/test_conventions.cpp`) scans the
model headers for `float`, `glm::vec2/3/4`, `glm::mat3/4` and `glm::quat`
and fails on any line not on an explicit allow-list, each entry carrying its
one-line reason.

**Saved form.** JSON numbers are already doubles and the writers emit what
they are handed, so a file reads the same; only the values carry more digits
(a 24.7 now saves as 24.7, not 24.700000762939453). `kVersion` moves only if
a field's form changes.

## Files

`src/core/{Feature,Occurrence,Configuration,AssemblySolve,AssemblyLoopSolve,
MotionStudy,FrameMath,Measure,CollisionCache,WormGear}.h` and their .cpp;
`src/core/features/{Curve,Derived,Frame,Split}Feature.*`;
`src/core/parts/AxisStack.*`; `src/core/io/{ImportAssembly,
SerializeAssembly,SerializeFeatures,SerializeConfigurations}.*`,
`SerializationInternal.h`; `src/cad/CADKernel.*`;
`src/core/BodyMode.*`, `ExtrudeFeature.cpp`, `HoleFeature.cpp`;
`src/core/cam/{CamToolpath.h,CamSetup.cpp}`; `src/remote/Remote*.cpp`
readers; the `ui/` and `render/` call sites that fill a float buffer from a
placement; `tests/test_conventions.cpp`; docs/conventions.md,
docs/modeling/README.md, docs/inspect/README.md, docs/assemblies.

## Verification

- New tests: primitives at awkward coordinates (0.1*k, 1e3 + 0.1, negative)
  fuse and cut exactly — one solid, the expected volume, no sliver; a pocket
  from a plane typed at a box's height leaves no skin; an occurrence
  placement round-trips through save/load bit for bit; a mate solve repeated
  N times does not move a placement; regenerating twice changes nothing.
- The conventions test, and a deliberate float member to see it fail.
- Headless suite, `tools/gui-tests-xvfb.sh`, `tests/render_pixel_smoke.py`,
  `ctest -L pygui`, `benchmarks/runners/reference.py` and
  `negative_controls.py`.
