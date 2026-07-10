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

### M2 — Revolve — toponaming-free
`RevolveFeature`: a sketch profile revolved about an axis (a sketch line, a datum
axis, or later an edge) through an angle. OCC `BRepPrimAPI_MakeRevol`; modes
new-body / add / cut like Extrude. Axis from a sketch line or datum axis is
toponaming-free. Highest value after datum planes.

### M3 — Richer extrude
Symmetric / two-sided, blind distance both ways, **thin/shell** (wall
thickness), **draft angle**. Blind is easy; **up-to-face / up-to-body** needs a
face/body ref → the up-to-face variant waits for M6.

### M4 — Body booleans as features — toponaming-free
A `BooleanFeature` (union / subtract / intersect) between two bodies by id. The
kernel ops already exist (`CADShape::Union/Difference/Intersection`); this is
wiring them as a history feature + a two-body pick UI. Whole-body refs, so
toponaming-free.

### M5 — 3D mirror / pattern (of features/bodies) — toponaming-free
Mirror a body/feature across a **datum plane**; linear/circular **pattern** of a
body/feature. References bodies + datum planes only. This is the model-level
analog of the sketch `DerivedOp` mirror/pattern — reuse that id-stable-replay
discipline.

### M6 — Topological naming (the foundation)
Persistent identification of faces/edges across regeneration. Unblocks every
edge/face-referencing feature below. Promoted to its own workstream with a
design study, candidate design (element maps seeded by our stable sketch-entity
ids), and milestones T1–T5: **[toponaming.md](toponaming.md)**. Start the
*design* (T1) early even while M1–M5 ship, because it shapes the reference
model.

### M7 — 3D fillet / chamfer (needs M6)
`BRepFilletAPI_MakeFillet` / `MakeChamfer` on selected edges. A v1 that
references edges by current index works but breaks on upstream edits; the robust
version wants M6. Requires **edge picking** (extend the `FaceSelector` to edges).

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
