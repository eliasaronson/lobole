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
`Modified`/`Generated`/`IsDeleted` history before designing it).

So: **do the toponaming-free milestones first**, and treat toponaming as the
foundation that gates the edge/face-referencing features.

## Milestones (sequenced)

### M1 — Datum planes (+ axes / points) — toponaming-free, do first
A `DatumPlane` feature so sketches aren't stuck on the three default planes.
Variants: **offset** from a plane, **angled** about an edge/axis, **through**
points/lines. Each produces a `SketchPlane` a sketch references by feature id
(survives edits because the datum regenerates). The sketch origin/axes work is
the mini-precedent; this is the model-level analog. *Offset/angled/through-points
are toponaming-free; "plane on a face" needs a face ref — defer that variant to
after M6.* Also expose datum **axes** and **points** (needed by revolve/pattern).

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
Persistent identification of faces/edges across regeneration. Design study first
(FreeCAD RealThunder element maps; OCC modification history). Unblocks every
edge/face-referencing feature below. Start the *design* early even while M1–M5
ship, because it shapes the reference model.

### M7 — 3D fillet / chamfer (needs M6)
`BRepFilletAPI_MakeFillet` / `MakeChamfer` on selected edges. A v1 that
references edges by current index works but breaks on upstream edits; the robust
version wants M6. Requires **edge picking** (extend the `FaceSelector` to edges).

### M8+ — advanced
Sweep, loft (`BRepOffsetAPI_ThruSections`), shell (face-select → M6), hole/thread
wizards, rib, draft on selected faces. All either need M6 or build on M1–M5.

## Cross-cutting

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
