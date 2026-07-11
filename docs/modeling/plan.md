# 3D modeling — plan

The sketcher is feature-complete (see [sketching/plan.md](../sketching/plan.md));
3D modeling is the next frontier. Today the solid side is thin — this plan
sequences it, with one fork (**topological naming**) deciding the order.

## Current state (grounded in code)

- **Feature model** (`src/core/`): `Document` is an ordered feature list that
  regenerates the B-Rep deterministically; `RegenContext` resolves
  feature→feature references by stable id. Features: **Box, Sphere, Cylinder,
  Extrude** (new-body / add / cut), **Import** (STEP), **Sketch**.
- **Kernel** (`src/CADKernel.*`): OCC B-rep with primitives and **booleans
  already implemented** — `CADShape::Union` / `Intersection` / `Difference`
  (`BRepAlgoAPI_Fuse` / `Common` / `Cut`) — just not exposed as features.
  `ExtrudeFeature` already fuses/cuts against existing bodies.
- The full Onshape loop works: sketch → extrude → sketch-on-a-face → cut.
- **Sketch datum geometry** (origin + X/Y axes, shipped) is the sketch-level
  precedent for the model-level reference geometry below.

## The fork: topological naming

A parametric feature refers to either **whole bodies / datums** or **specific
faces/edges**. The first kind is safe. The second needs *persistent naming* so
"the edge I filleted" stays identified after an upstream edit reorders faces —
the classic hard problem (getting it wrong is the #1 way parametric kernels die;
study FreeCAD's RealThunder element-maps and OCC's `BRepBuilderAPI`
`Modified`/`Generated`/`IsDeleted` history before designing it). The design
study, candidate element-map design, and its own milestones now live in
**[toponaming.md](toponaming.md)**.

So: **do the toponaming-free milestones first**, and treat toponaming as the
foundation that gates the edge/face-referencing features.

## Milestones (sequenced)

### M1 — Datum planes (+ axes / points) — toponaming-free, do first
**Shipped (July 2026, planes only).** `DatumPlaneFeature` (`src/core/`) with
all three toponaming-free variants — **offset** from a plane, **angled** about
an axis (global axis or a sketch line via the shared `AxisRef` in
`src/core/AxisRef.h`), **through** three points. Each recomputes a cached
`SketchPlane` every regeneration; missing/degenerate inputs fail the feature
like a missing extrude sketch. Sketches gain an optional `planeRef` (0 = baked,
today's behavior) resolved in `SketchFeature::Execute`, so editing an upstream
datum moves the sketch and its extrusion — the first lift of the baked-plane
limitation. Offset/angle are expression-driven (`VisitExprs`, degrees for
angle). Format v12. UI: Create → Datum Plane preview/accept, Properties editor
(kind/parent/axis combos + expression fields), amber viewport quads with name
labels, plane-picking + feature-tree double-click to sketch on a datum. RPCs
work through the generic `add_feature`/`modify_feature`. Tests: `test_datum.cpp`
+ a `remote_smoke.py` block (edit a variable → the body translates).

*Deferred:* "plane on a face" needs a face ref → after M6. **Datum axes &
points** move to **M2** (revolve is their real consumer, so `AxisRef` gets
designed against it); the angled-datum sketch-line axis already exercises the
`AxisRef` core.

### M2 — Revolve (+ datum axes) — toponaming-free
**Shipped (July 2026).** `RevolveFeature` revolves a sketch profile about an
`AxisRef` axis (global axis, sketch construction line, or a `DatumAxisFeature`)
through an angle — OCC `BRepPrimAPI_MakeRevol`, via a new
`sketch::BuildRevolve` sharing `BuildProfileFace` with extrude. Angle is
expression-driven (degrees); **symmetric** centers the sweep on the profile
plane (pre-rotate by −angle/2), `reversed` sweeps the other hand. Modes
new-body / add / cut reuse the extracted `ApplyBodyMode` (shared with extrude).
The axis must lie in the sketch plane and the profile must not cross it — both
enforced (out-of-plane → error; a crossing profile's invalid solid is
rejected). Also shipped the deferred **`DatumAxisFeature`** (two-points /
sketch-line / global), integrated through a new `AxisRef::Kind::DatumAxis`, so
axes are first-class tree features usable by revolve (and later 3D patterns).
Format v13. UI: "Revolve…" beside "Extrude…", a reusable axis-picker combo
(also upgraded the M1 angled-datum editor), Create → Datum Axis with a line +
label render. Tests: `test_revolve.cpp` (Pappus volumes, symmetric, failure
modes), `test_datum.cpp` datum-axis cases, a `remote_smoke.py` block.

*Deferred:* axis-by-B-Rep-edge → after toponaming (M6).

### M3 — Richer extrude
Symmetric / two-sided, blind distance both ways, **thin/shell** (wall
thickness), **draft angle**. Blind is easy; **up-to-face / up-to-body** needs a
face/body ref → the up-to-face variant waits for M6.

Also: **multi-profile sketches** (several disjoint loops → one face per outer
with its own holes, e.g. a four-bolt-hole cut in one sketch). Found via the
MCP live run (2026-07-10): `BuildProfileFace` assumes largest-loop-outer +
rest-are-holes; disjoint loops now fail the feature loudly (containment check)
instead of the previous silent no-op boolean, but supporting them properly
needs loop classification by containment + one prism per outer, fused.

### M4 — Body booleans as features — toponaming-free
**Shipped (July 2026).** `BooleanFeature` (union / subtract / intersect) over
two bodies referenced by their **producing feature's id** — the new
`Body::createdBy` stamp gives transient bodies a stable identity
(toponaming-free, whole-body refs). Both operands are consumed into one result
owned by the boolean, so booleans chain. The kernel ops
(`CADShape::Union/Difference/Intersection`) already existed; M4 wired body
identity + the feature + serialization (v14) + an editor with an op combo and
two operand combos (listing earlier body producers — no viewport body-picking
yet). Result inherits the target's material. Tests: `test_boolean.cpp`
(union/subtract/intersect volumes, consume-to-one, missing/degenerate operands,
boolean-of-boolean chain, round-trip) + a `remote_smoke.py` block. Verified
in-app: a corner-notch subtract of two boxes.

### M5 — 3D mirror / pattern (of bodies) — toponaming-free
**Shipped (July 2026).** One `PatternFeature` with kind **Mirror** (reflect
across a base/datum plane), **Linear** (array along an `AxisRef`), and
**Circular** (array about an `AxisRef`). The seed body is referenced by its
producing feature's id (M4 `Body::createdBy`); every instance (seed + copies)
is fused and split back into connected solids, so touching instances merge and
disjoint ones become separate bodies (Onshape behavior). Count is
**expression-driven** (evaluated against the parameter table, the variables
plan's flagged use), as are linear spacing and circular per-step angle. Reused
`ResolveParentPlane` (mirror plane), `AxisRef`/`ResolveAxis` (direction/axis),
the `BodyProducers` combo, and `ExprDim`; new kernel helpers `CADShape::Mirror`
/`RotateAbout`/`SplitSolids` and a shared `FuseInstancesReplacingSource`.
Format v15. Tests: `test_pattern.cpp` + a `remote_smoke.py` block; verified
in-app (a 6-up circular ring and a 4-up linear row). *Feature (vs body)
patterns and per-instance references deferred.*

**Toponaming is complete (T1–T5)** — M7 shipped on top of it; M3's
up-to-face extrude can now consume face `ElementRef`s directly.

### M6 — Topological naming (the foundation) — **shipped (July 2026)**
Persistent identification of faces/edges across regeneration. All milestones
T1–T5 done: **[toponaming.md](toponaming.md)** — element maps seeded by
stable sketch-entity ids, propagation through booleans via
`BRepTools_History`, U/L fallback passes, edge names, sketch-on-face and
fillet as consumers.

### M7 — 3D fillet / chamfer — **shipped (July 2026)**
`FilletFeature` (one class, fillet/chamfer op) referencing edges by
`ElementRef` from birth — the robust version directly, since T5 landed first
([toponaming.md](toponaming.md)). Edge picking shipped as
`FaceSelector::PickEdge` over `CADShape::GenerateEdgePolylines` (ray-to-
polyline with occlusion), highlighted through the sketch-primitive line
pipeline; toolbar/menu creation with preview, radius as an expression,
dangling-ref error + "Pick edges…" repair. Format v17. Tests:
`test_fillet.cpp` + a `fillet_body_edge_via_click` GUI case.

### M8+ — advanced
Sweep, loft (`BRepOffsetAPI_ThruSections`), shell (face-select → M6), hole/thread
wizards, rib, draft on selected faces. All either need M6 or build on M1–M5.

## Cross-cutting

- **Regeneration-failure UX** (land before the first failure-prone feature —
  fillets, up-to-face): today a feature that fails to regenerate has no story.
  With M7-era features, upstream edits *will* make downstream features fail
  (fillet radius exceeds the new face, referenced element gone). Needed:
  per-feature error state shown in the history panel (icon + message),
  regeneration that **continues past** a failed feature (downstream features
  see the pre-failure body or also error, deterministically), a **suppress**
  flag per feature, and a repair path (edit params / reattach references —
  ties into [toponaming.md](toponaming.md) resolution failure). Every
  parametric CAD has this; nobody plans it up front. Cheap now, painful later.
- **Expression-ready parameters**: every numeric param a new feature adds
  (revolve angle, datum offset, pattern count/spacing, extrude depth) should be
  an expression from birth, not a raw double — coordinate with
  [variables/plan.md](../variables/plan.md); its V1–V2 should land before or
  with M1–M2.
- **Edge picking**: M7 and face-select modes need it; `FaceSelector` currently
  maps triangles→B-Rep faces — add an edge analog.
- **Incremental regeneration**: `Document::Regenerate()` recomputes every feature
  on each edit (O(N) OCC ops). Fine now; as the feature count grows this becomes
  the scaling limiter — a per-feature output-shape cache keyed by a hash of its
  inputs is the cheap first win (see the roadmap's "Continuous" note).

## Recommended order

**M1 datum planes → M2 revolve** first (highest value, toponaming-free), then
**M4 booleans → M5 3D mirror/pattern → M3 richer extrude**. Begin the **M6
toponaming** design study in parallel; land it before **M7 fillets** and the
face-select feature variants. Each milestone is its own set of features + tests,
ending green.
