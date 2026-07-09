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

### T1 — Design memo (no code)
Read the list above; write `toponaming-design.md`: the naming grammar, the
propagation rule per OCC op we use, the `ElementRef` API, failure semantics,
serialization. Review against three scenarios: reorder sketch entities, add a
hole through a filleted face, delete the referenced entity.

### T2 — Prototype on Extrude
Name extrude output elements from sketch-entity ids (no history propagation
needed — extrude generates directly from the profile). A debug overlay shows
element names on hover. Tests: edit the sketch (move, add entities) → the same
side face keeps its name; delete the entity → the name reports gone.

### T3 — `ElementRef` plumbing
The reference type, serialization (format bump + migration), resolution in
`RegenContext`, error surfacing into the feature-error UX.

### T4 — Adopt: sketch-on-face
Replace the baked `SketchPlane` for face-attached sketches with an
`ElementRef` — the first user-visible payoff: edit the base extrude's height
and the dependent sketch + its extrude follow. (Keep baked planes for datum/
default-plane sketches — those are already stable by feature id.)

### T5 — Adopt: booleans propagation + fillet edges
History propagation through `BRepAlgoAPI_*`; then M7 fillet consumes
`ElementRef` edges. From here every new feature uses `ElementRef` from birth.

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
