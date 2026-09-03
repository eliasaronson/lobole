# 3D modeling — plan

> **Status: complete** (M1–M8 + rib + cosmetic thread, format v36; see
> [thread-rib.md](thread-rib.md), [curves-3d-plan.md](curves-3d-plan.md)).
> A modelled helical thread is the one deliberate remainder. The prose below
> is the original plan, kept for its sequencing argument and the toponaming
> fork it turns on — read it as history, and [roadmap.md](../roadmap.md) for
> what is true now.

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

### M3 — Richer extrude — **shipped (July 2026)** — [design: m3-extrude.md](m3-extrude.md)
**Multi-profile sketches** (the headline — a four-bolt-hole cut in ONE
sketch, found via the MCP live run): loops classified by containment depth
into outers + holes (islands re-emerge), one prism/revolve per group,
compound-assembled with per-group cap names ("cap0@g<entityId>"); overlap
stays a loud error. **End conditions** (`ExtrudeEnd`, format v18): symmetric,
two-sided (expression-driven `distance2`), and **up-to-face** on a planar
`ElementRef` (resolved like fillet edges via the shared `ResolveFacePlane`;
follows upstream edits — the first extrude-side toponaming consumer).
**Thin/wall extrude** (`BuildThinPrismNamed`): every loop offset via OCC
`MakeOffset` into a hollow wall (out/in/both), composing with any end
condition. All variants build ONE prism of the computed total translated
back along the normal, so element names stay exactly a blind extrude's.
Editor: End combo + target-face combo + Thin block. Tests in
`test_extrude.cpp` ([profile]/[end]/[thin]). *Draft angle deferred* (needs
face-level surgery that fights the element maps).

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
in-app (a 6-up circular ring and a 4-up linear row). *Per-instance references
deferred.*

**2026-08-26 (R14): a CUT source patterns its cutter.** A cut leaves no body
to replicate, so the pattern used to refuse "source body not found" and
repeated holes had to go into one multi-loop sketch — which throws the count
and spacing away as editable parameters. A cut Extrude now caches the prism
it swept (`Feature::CutToolShape` + `CutToolMap`) and the pattern subtracts a
transformed copy per instance, each carrying the source's element names with
an instance suffix (`4/g7#2`). In the GUI the pick is the CUT'S TREE ROW with
the Source slot armed — a body pick resolves to its `createdBy`, which is the
plate, never the cut.

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
dangling-ref error + "Pick edges…" repair (toolbar creation with preview —
the "menu creation" this line used to claim died with the Create menu).
Format v17. Tests:
`test_fillet.cpp` + a `fillet_body_edge_via_click` GUI case.

### M8 — advanced solids
**Shell, loft, and sweep shipped (July 2026)** — the solid-creation quartet
(extrude/revolve/loft/sweep) is complete. ShellFeature hollows a body
(BRepOffsetAPI_MakeThickSolid, open-face required); LoftFeature skins through
2+ profile sketches (ThruSections); SweepFeature sweeps a profile along an
open path sketch (MakePipeShell, RoundCorner so sharp corners sweep fully).
Shell is a toponaming consumer (faces by ElementRef); all give their results
fresh primitive-style names. **Hole wizard shipped** (HoleFeature: simple/counterbore/countersink
holes at a sketch's points, cut into the bodies below — a bolt pattern is
one feature). *Shipped 2026-07-19:* rib + cosmetic thread (v36, thread-rib.md;
modeled helical thread deferred as its opt-in flag). Draft on selected faces
shipped earlier (M8 draft). Workstream complete.

All of the above shipped as *features* — reachable over the script API and
editable in the UI, but for a long time not **creatable** from the GUI: hole,
shell, draft (and split/refine) had no toolbar button until UI **U9**
(2026-08-13, [../ui/u9-gui-parity.md](../ui/u9-gui-parity.md)). Draft's
`bodyId` was in fact unwritable from the UI at all. Worth remembering when
this plan says "shipped": check that a human can reach it.

## Cross-cutting

**Derived geometry (A2)** — another part's bodies copied into this one as a
snapshot, drawn on like any body, updated on request:
[derived-plan.md](derived-plan.md) (planned 2026-09-03).

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
