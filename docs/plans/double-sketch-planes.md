# Double-precision sketch planes and sketch geometry

## Problem

A sketch plane and everything drawn on it are single precision:
`SketchPlane` stores `glm::vec3` origin and axes, and every sketch entity
(`sketch::Point::p`, `Line::a/b`, the centres of circles, arcs and ellipses,
B-spline poles, the text anchor) is a `glm::vec2`. The model around them is
double — feature dimensions, expressions, PlaneGCS, OpenCASCADE — so a value
goes double → float → double on its way from the document to the kernel and
comes back a few ULPs off:

- A plane at z = 13.4 is 13.39999962. A pocket cut down from it into a part
  whose top is at 13.4 kept a 0.4 µm skin (a sealed counterbore that passed
  BRepCheck); an add from a plane that rounds up (24.7 → 24.70000076) left
  two solids. `CoplanarStartSlack` (`src/core/BodyMode.cpp`) now starts such
  tools a hair behind the plane — a patch over the symptom.
- A sketch rectangle meant to coincide with a body face is off by the same
  rounding in UV, so its side faces are microns from the body's.
- The solver solves in double and writes back through float, so an
  expression-driven sketch re-solves from a rounded seed every regeneration
  (the chess round fixed the arc endpoint seeds; every other stored coordinate
  still rounds).
- The paths that derive a plane are float too: `CADShape::FacePlane` (a sketch
  on a face, an up-to-face extrude), `DatumPlaneFeature` (offset adds a
  float-cast distance), `ResolveAxis` (a revolve about a sketch line), Hole's
  world centres, and the JSON readers (`Vec3FromJson` / `Vec2FromJson` read
  `get<float>()`).

## The fix

Double from the document through the solver to OpenCASCADE; float only where
a value is handed to a renderer or the GUI.

- `SketchPlane`: `glm::dvec3` origin, normal, U, V; `PlaneToWorld(dvec2) →
  dvec3`, `WorldToPlane(dvec3) → dvec2`. `GetPlaneMatrix()` stays a
  `glm::mat4` — it exists for the renderers.
- Sketch entities: every UV coordinate is `glm::dvec2`, and the header helpers
  (arc points, ellipse evaluation, B-spline de Boor, reflect / rotate /
  translate) compute in double with their `static_cast<float>` removed.
  `ReferenceImage::origin`, a constraint's `labelOffset` and the derived-op
  vectors follow, so no sketch field rounds.
- Solver, profile builder, text layout, inference and selection
  (`src/core/sketch/`) read and write double; the profile builds its OCC
  points from the double frame directly.
- Plane sources: a `FacePlane` overload taking `glm::dvec3&`;
  `DatumPlaneFeature` and `ResolveAxis` / `DatumAxisFeature` in double;
  `CADKernel::CreateCylinder` / `CreateCone` take `glm::dvec3` (float callers
  widen). Features that read a plane (Extrude, Revolve, Hole, Rib, Jog,
  NormalCut, Split, Pattern's mirror) keep its values in double.
- Serialization: `Vec2FromJson` / `Vec3FromJson` return double and the
  writers take double. A JSON number already is a double, so the schema does
  not change and there is no `kVersion` bump: a file saved before this keeps
  the float-rounded values it was written with (13.399999618530273), and a
  plane written now round-trips exactly. The RPC readers for sketch
  coordinates read `get<double>()`.
- The GUI and the renderers convert at their own boundary: a mouse pick in
  float UV widens into the sketch, an overlay narrows to draw.
- `CoplanarStartSlack`: measured once planes are exact. If the cut and add at
  13.4 / 24.7 and the counterbore open and fuse without it, it goes; if a
  coplanar boolean still needs it, it stays with its doc entry rewritten to
  say why.

## Files

`src/cad/SketchPlane.{h,cpp}`, `src/cad/CADKernel.{h,cpp}`,
`src/core/features/SketchFeature.{h,cpp}`, `src/core/sketch/*`,
`src/core/features/{Extrude,Revolve,Hole,Rib,Jog,NormalCut,Split,Pattern,
DatumPlane,DatumAxis}Feature.*`, `src/core/AxisRef.h`, `src/core/BodyMode.*`,
`src/core/io/SerializationInternal.h`, `src/core/io/SerializeFeatures.cpp`,
`src/remote/RemoteModeling.cpp`, the GUI sketch layer (`src/ui/Sketch*`,
`PreviewHandles`, `SceneOverlays`, `SelectionPipeline`, `UI.cpp`) and the GUI
tests for their boundary conversions; tests; `docs/sketching/README.md`,
`docs/modeling/README.md`, `docs/inspect/README.md#positions-are-float32`.

Headers change in one batch (`SketchPlane.h`, `SketchFeature.h`,
`CADKernel.h`, `AxisRef.h`, the datum headers): `CADKernel.h` sits under
`Feature.h`, so each edit there is a full rebuild.

## Verification

- The 13.4 repro — a cut from a plane at z = 13.4 opens a box whose top is at
  13.4 (no downward face left at the top, exact volume), an add from 24.7 is
  one solid, a counterbore keeps no skin — with `CoplanarStartSlack` removed
  if it goes.
- Planes at awkward heights: 0.1·k for k up to 100, 1e3 + 0.1, and negative
  heights; each cut opens, each add fuses.
- Regeneration stability: a dimension-driven sketch regenerated ten times
  keeps every coordinate bit-identical.
- Round trip: a sketch with a plane at 13.4 and entities at 0.1-step
  coordinates saves and loads with `==` equality on every value.
- Headless suite, GUI suite, `ctest -L pygui`, and `render_pixel_smoke`
  (the renderer boundary moved); `benchmarks/runners/reference.py` if RPC
  behaviour moved.
