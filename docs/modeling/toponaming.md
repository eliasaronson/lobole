# Topological naming — design study & plan

Persistent identification of faces/edges across regeneration: how "the face I
sketched on" or "the edge I filleted" stays identified after an upstream edit
adds, removes, or reorders topology. Getting this wrong is the #1 way parametric
kernels die — FreeCAD lived with the bug for a decade before the RealThunder
element-map work. It gates every face/edge-referencing feature
([modeling plan](plan.md) M6): fillet/chamfer, up-to-face extrude, plane-on-face,
shell, projected/external sketch geometry, and (later) assembly mates on faces.

**Strategy: design early, adopt incrementally.** The study costs reading + a
memo; the prototype targets one feature (Extrude); adoption then spreads
feature-by-feature. Do not block the toponaming-free modeling milestones on it.

## What we already have (the precedents)

- **Stable feature ids** — `Document` features are referenced by id via
  `RegenContext`, never by index.
- **Stable sketch-entity ids** (sketching Phase B) — monotonic, never reused;
  the id-stable replay discipline from `RegenerateDerived` is the in-sketch
  analogue of this whole problem, solved.
- **Face mapping** — `FaceSelector` maps triangles → B-Rep faces with surface
  normals, so "a face" is already a first-class pickable thing; it just has no
  *persistent* name.
- The anti-precedent: sketches **bake** their plane at creation
  (`SketchPlane`), which is exactly the behavior toponaming replaces.

## Reading list (before designing)

- **FreeCAD RealThunder element maps** — the mapped-element-name design:
  every subshape gets a string name composed from the generating feature +
  OCC history ops; names propagate through the feature chain. Study the
  design notes and `ElementMap`/`TopoShape` code in the checkout under
  `~/Documents/CAD/FreeCAD`.
- **OCC modification history** — `BRepBuilderAPI_MakeShape::Modified /
  Generated / IsDeleted` (and the same API on `BRepAlgoAPI_*` booleans,
  `BRepFilletAPI_*`): for each input subshape, which output subshapes it
  became. This is the raw material every naming scheme is built from.
- **SolveSpace** — entity naming via stable request handles (already studied
  for the sketcher; the same philosophy at the 3D level).

## Candidate design (to validate in the memo)

**Element maps, seeded by our stable ids.** Each feature, during `Regenerate()`,
labels every face/edge of its output shape with a name derived from *why that
element exists*:

- **Extrude**: side face ← the generating sketch entity's stable id
  (`extrude42/side/entity17`); start/end caps ← `cap/start`, `cap/end`; side
  edges ← the adjacent entity pair. Sketch-entity ids are stable, so these
  names survive any sketch edit that keeps the entity.
- **Primitives**: fixed names (`box/top`, `cylinder/side`, …).
- **Booleans / fillets / later features**: propagate input names through OCC's
  `Modified`/`Generated`/`IsDeleted` history — an element that survives keeps
  its name; a new element gets a name composed from its generators; splits get
  a disambiguating suffix.

A **persistent reference** is then `ElementRef { featureId, elementName }`,
serialized. Resolution happens each regeneration: walk the named map of the
referenced feature's output → concrete `TopoDS` subshape.

**Resolution failure is a feature, not a crash**: if a name no longer resolves
(the generating entity was deleted), the referencing feature enters the
per-feature **error state** (modeling plan cross-cutting: regeneration-failure
UX) with a "reattach" repair action. An optional geometric-similarity fallback
(nearest face by centroid + normal + area) can *suggest* a reattachment but
must never silently guess.

## Milestones

### T1 — Design memo (no code) — **done (July 2026)**
See **[toponaming-design.md](toponaming-design.md)**: the naming grammar
(human-readable, no hashing), per-OCC-op propagation rules with reliability
verdicts (extrude/revolve safe; boolean/fillet need care), the `ElementRef`
API + resolution + serialization, failure semantics, and the three scenario
walkthroughs. It also pins five **prerequisites** T2 depends on: thread the
sketch-entity id onto profile edges, keep OCC makers alive (booleans currently
discard theirs), a per-feature error state, an `ElementMap` on `Body`, and
normalizing op history onto `BRepTools_History`.

### T2 — Prototype on Extrude — **done (July 2026)**
Extrude NewBody faces are named from sketch-entity ids: `BuildPrismNamed`
(`src/core/SketchProfile.cpp`) keeps the `MakePrism` maker, names each side
face `<extrudeId>/g<entityId>` via `Generated(edge)` + geometric edge→entity
matching (reusing `EdgeToSketchEntity`), and the caps `cap0`/`cap1` via
`FirstShape`/`LastShape`. The name↔faceIndex map (`ElementMap`, new header)
rides on the `Body` (transient); face indices align with `GenerateMesh`/picking
because the prism's TShape is shared unchanged into the body (NewBody only —
Add/Cut run a boolean that renumbers, so `ApplyBodyMode` clears the map there).
`LoopEdge` gained an `entityId`. A **View → Show Element Names** overlay labels
the hovered/selected face. Mutation tests (`tests/test_toponaming.cpp`) prove
the contract: names survive a geometry edit and an added hole; a deleted
entity's name stops resolving. Landed T1 prerequisites 1, 2, 4.

### T3 — `ElementRef` plumbing — **done (July 2026)**
`ElementRef {featureId, name}` (`src/core/ElementMap.h`); resolution finds the
body the feature produced (`Body::createdBy`) and looks the name up in its
`ElementMap`. `CADShape::FacePlane(faceIndex)` extracts a planar face's frame
at the B-Rep level (centroid + normal + deterministic u-axis). Per-feature
**error state**: `Feature::m_error` (transient, mirrors `Parameter::error`);
`Document::Regenerate` clears it before each `Execute` and stamps a generic
message on silent failure; features `SetError` specifics. Landed T1
prerequisite 3.

### T4 — Adopt: sketch-on-face — **done (July 2026)**
`SketchFeature` gained a `faceRef` (`ElementRef`) resolved in `Execute` before
the datum block: the sketch re-derives its plane from the attached face every
regeneration, so **editing the base body moves the dependent sketch and
everything built on it** (verified: raising a base box from 3→6 carried the
top feature from z=3 to z=6). "Sketch on Selected Face" stores a `faceRef` when
the face is named (extrude-NewBody faces), else bakes (no regression). Format
v16. UI: errored features show red + a tooltip in the tree, and a face-attached
sketch has a **Reattach face…** context action. Base/datum-plane sketches keep
their stable feature-id refs unchanged.

### T5 — Adopt: booleans propagation + fillet edges — **done (July 2026)**
Booleans keep their `BRepAlgoAPI_*` maker alive, normalize its reports onto
`BRepTools_History` (T1 prerequisite 5, the last one), and `PropagateNames`
(`src/core/ElementHistory.{h,cpp}`) rewrites the operands' names onto the
result: survivors verbatim, `Modified` → `;M<tag>` (`.i` on 1→N splits),
`Generated` → `;G<tag>` (a pierced face *generates* its section edges —
verified against OCC 7.9.3), leftovers `<tag>/seam<i>`. The **U/L fallback
passes** then name everything history missed; U is what gives edges stable
names. `ElementMap` gained an edge space (`edgeNames`, keyed by the deduped
`TopExp::MapShapes(TopAbs_EDGE)` order that `GenerateEdgePolylines` and edge
picking share). `BooleanFeature` and extrude/revolve Add/Cut all attach
propagated maps — no path clears a map anymore — and `BuildRevolveNamed`
brought revolve up to extrude's naming (sides, caps, poles).

On top of that, **M7 fillet + chamfer** (`src/core/FilletFeature`,
`topo::EdgeBlendNamed`): edges held as `ElementRef`s, resolved each regen,
blend faces named `<spineEdgeName>;G<tag>`, dangling refs error with a
re-pick repair action. Edge picking = `FaceSelector::PickEdge`
(ray-to-polyline, occlusion-aware) + depth-tested highlight through the
sketch-primitive pipeline. Format v17. Mutation tests:
`tests/test_toponaming.cpp` (T5 section), `tests/test_fillet.cpp`.

From here every new feature uses `ElementRef` from birth. *Still out of
scope: pattern/mirror (`FuseInstancesReplacingSource`) does not propagate
maps — pattern outputs stay unnamed until per-instance references are
designed (FreeCAD's `;:C` child maps, deliberately skipped).*

## Test strategy

Mutation tests, not unit tests: build a model, capture refs, apply an upstream
edit (dimension change / entity insert / entity delete / feature reorder),
regenerate, assert each ref still resolves to the geometrically-correct element
(compare centroid/normal/area) or reports failure exactly when it should.
These run headless — pure `designer_core`.

## Risks

- **Name explosion in boolean chains** — RealThunder names grow long through
  deep histories; mitigate with hashing long names (keep the readable form for
  debugging).
- **OCC history gaps** — some OCC ops report incomplete `Generated` sets;
  the memo must list which ops we rely on and prototype-verify each.
- **Over-engineering** — we need names good enough for *our* features, not a
  general theory. T2's extrude-only scope is the guard.
