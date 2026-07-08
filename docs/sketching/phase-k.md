# Phase K — Generative operations (detailed plan)

Mirror, linear/circular pattern, fillet, offset — the ops that *produce geometry
from other geometry*. Placed after arcs (Phase J) because fillet and offset
produce arcs, which now exist.

## Key finding from the reference repos (revises the approach)

I read how FreeCAD Sketcher and SolveSpace implement these. The decisive fact:

- **FreeCAD does all four as one-shot *bakes* into the flat geometry list.**
  "Parametric" behaviour (move the source → the copy follows) is achieved
  **purely through constraints** it leaves behind — `Symmetric` for mirror,
  `Tangent` for fillet, `Parallel`/`Radius`/`Coincident` for offset, a
  construction-line + `Coincident`/`Equal`/`Distance` grid for arrays.
  Regeneration is just the constraint **re-solve**. There is *no* in-sketch
  producer→consumer dependency graph and *no* derived-geometry list.
  (`SketchObjectOperations.cpp`: `addSymmetric` :1284, `addCopy` :2052,
  `fillet` :238; offset via OCC `BRepOffsetAPI_MakeOffset`.)
- **SolveSpace is the opposite**: a true generative DAG — a `Group` stores its
  source group + a copy count as solver params and *replays* the copy every
  regen (`group.cpp` `Generate`/`CopyEntity`), with a **remap table**
  (`Group::Remap`, `group.cpp:863`) giving copy *k* of entity *e* a stable
  handle so references survive. This is the model for *editable count* patterns.

**Implication.** The derived-record + `RegenerateDerived` machinery the plan
originally specified is only actually needed where a *parameter must be edited
without re-running the op* — in practice, **pattern count** (and maybe offset
distance). Mirror and fillet get "parametric" essentially for free via the
constraints we already have (`Symmetric` exists and is tested; `Tangent(line,
arc)` exists). So this phase splits cleanly:

- **K1 — Mirror + Fillet**: constraint-linked, *no new persistence or regen
  pass*. Cheap, reuses the solver. Ships first.
- **K2 — Pattern**: introduces the derived-record + id-stable `RegenerateDerived`
  (the SolveSpace remap discipline) so count is editable. This is where the
  "hard part" the plan flagged actually lives and pays off.
- **K3 — Offset**: OCC-backed (`BRepOffsetAPI_MakeOffset`), bake first.

Every op still offers **bake vs linked** per invocation: bake = copy only;
linked = copy + the linking constraints (K1/K3) or + a derived record (K2).

## Shared groundwork (all parts)

- Reflect/transform helpers in `sketch::` (SketchFeature.h): `ReflectPoint(p,
  a, b)`, and arc/line/circle transformers that preserve the **CCW-always**
  invariant (a reflection reverses orientation → swap+reflect an arc's
  start/end angles).
- **Emit template** = `AddRectangle` (`SketchFeature.cpp:58-76`): `AddEntity`
  each generated entity (stamps a stable id), then `constraints.push_back` the
  linking constraints referencing the fresh ids — atomic, id-stable.
- **Commit template** = `CommitRectangle` (`SketchInteraction.cpp:146-158`):
  snapshot `FeatureToJson` → mutate → `Solver::Load/Solve/WriteBack` →
  `Document::Regenerate` → one `ModifyFeatureCommand`. Undo/redo is free.
- Selection to operate on = `edit.selected` (`AppState.h:63`); the selection-
  driven button idiom is `conButton` in `SketchToolbar.cpp:137`.

---

# K1 — Mirror + Fillet (constraint-linked) — ~1.5 days

### 1. `sketch::` reflect helpers (SketchFeature.h)
`glm::vec2 ReflectPoint(glm::vec2 p, glm::vec2 a, glm::vec2 b)` (reflect across
the infinite line a→b). `Entity ReflectEntity(const Entity&, a, b)` via
`std::visit`: Point→p'; Line→a',b'; Circle→center',r; Arc→center', and
`startAngle'/endAngle'` recomputed from the two reflected endpoints so the
result stays CCW (reflection flips winding).

### 2. `SketchFeature::MirrorEntities(sourceIds, axis, bool link) → generatedIds`
`axis` = a line entity id, or two UV points (a sketch axis). For each source
entity: `AddEntity(ReflectEntity(...))`, capturing new ids into a source→copy
`geoIdMap` (needed to translate source-internal constraints, and to add links).
If `link`:
- **Line**: `Symmetric` on start and end about the axis line.
- **Circle**: `Equal` (radius) + `Symmetric` on center.
- **Arc**: `Equal` + `Symmetric` on both endpoints.
- **Point on the axis** (within tol): `Coincident` instead of `Symmetric`
  (FreeCAD does this to avoid a solver singularity — `addSymmetric` :1550).
- Perturb reflected arcs by ~`1e-6` rad so the solver can distinguish
  Symmetry from Equal (FreeCAD issue #13551, `SketchObjectOperations.cpp:1319`).
Bake mode (`link=false`) adds no constraints.
`Symmetric` uses `PointRef c` = the axis line (already in the model/solver).

### 3. `SketchFeature::FilletCorner(lineId1, lineId2, radius) → arcId`
Closed-form (FreeCAD `findFilletCenter`, `Part/Geometry.cpp:6342`):
- Find the shared corner `C` (the endpoint the two lines have in common) and
  each line's far-end direction `d1`, `d2` (unit).
- `theta = angle(d1, d2)`; `bis = normalize(d1 + d2)`; `center = C + bis * (r /
  sin(theta/2))`; tangent points `T1 = C + d1 * (r / tan(theta/2))`, `T2 = C +
  d2 * (r / tan(theta/2))`.
- Move each line's C-endpoint to its tangent point (mutate `a`/`b`; pick which
  endpoint by proximity to `C`).
- Insert the arc via `ArcCenterStartEnd`/angle math from `T1`→`T2` on the side
  away from `C` (minor arc).
- Constraints: remove the old `C` coincident joining the two lines; add
  `Coincident(line1 movedEnd, arc start)`, `Coincident(line2 movedEnd, arc
  end)`, `Tangent(line1, arc)`, `Tangent(line2, arc)`. (FreeCAD relies on the
  two tangents + moved endpoints; we add the coincidents too for robustness.)
- Guard: lines must actually share a corner and not be parallel; return 0 if
  the radius is too large for either segment (tangent point past the far end).

### 4. UI (AppState.h / SketchToolbar.cpp / SketchInteraction.cpp)
- `SketchTool::Mirror`, `SketchTool::Fillet` (+ a `bool linkGenerative` in
  `SketchEditState`, default true, shown as a "Linked" checkbox).
- **Mirror**: selection-driven — select the entities, then the mirror axis
  (click a line, or two points); a toolbar "Mirror" button (enabled when ≥1
  entity + an axis are selected) calls `MirrorEntities`. Commit via the
  `CommitRectangle` template.
- **Fillet**: click near a corner (two lines meeting) — reuse `PickSketchEntity`
  to find the two lines sharing the nearest endpoint; radius from a toolbar
  drag-float. Live preview via `BuildSketchOverlay` (the pending arc).

### 5. Remote + tests
- Remote: `mirror` and `fillet` semantic handlers modelled on `add_constraint`
  (`RemoteServer.cpp:301`) — resolve sketch by id, mutate, solve, one command;
  return generated ids.
- Tests (`test_sketch_feature.cpp` + `test_sketch_solver.cpp`): mirror bakes a
  reflected copy (no link → editing source leaves copy; **link → moving a
  source endpoint moves the mirror after re-solve**, asserted via the solver);
  a point on the axis mirrors to a coincident; fillet on a right-angle corner
  yields a tangent arc of the given radius, the profile still closes and
  extrudes (reuse `test_extrude.cpp` filleted-corner shape); fillet id-stability
  (a constraint on line1 survives the fillet).

**K1 done-when:** deterministic suite green; via the remote screenshot path,
mirror + fillet work and a filleted/mirrored profile extrudes. Commit
`feat: sketch mirror and fillet`.

---

# K2 — Pattern (bake + derived-record) — ~1.5 days

### 1. Derived-record model (SketchFeature.h + .cpp)
```cpp
enum class DerivedKind { LinearPattern, CircularPattern };
struct DerivedOp {
  DerivedKind kind;
  std::vector<uint32_t> sourceIds;
  // linear: dir (unit UV) + spacing; circular: center (UV) + totalAngle
  glm::vec2 vec{0,0}; double scalar = 0.0; int count = 1;
  std::vector<uint32_t> generatedIds; // count-1 instances × sourceIds.size()
};
```
Stored as `std::vector<DerivedOp> derived;` on `SketchFeature`. Bake mode stores
nothing (just emits copies).

### 2. `SketchFeature::RegenerateDerived()`
Replay each op in order (later ops may consume earlier output):
- Recompute each instance's geometry by transforming its source
  (translate by `k*spacing*dir`, or rotate `k*angle/(count-1)` about `center`).
- **Id-stable** (SolveSpace remap discipline, `group.cpp:863`): the k-th
  instance of source `s` keeps its stored `generatedId` — mutate the existing
  entity in place, don't recreate it, so constraints/refs survive. If `count`
  grew, `AddEntity` new instances and append ids; if it shrank, `DeleteEntity`
  the surplus (tail first).
- Called from a shared commit helper **after** `WriteBack` and **before** the
  final solve (so generated geometry is present for the solve). Add
  `RegenerateDerived()` into the `CommitRectangle`-style path (and the drag /
  dimension-edit paths) — or, cleaner, factor those into one
  `SolveAndRegenerate(sketch)` helper and route all commits through it.

### 3. `SketchFeature::AddLinearPattern / AddCircularPattern(sourceIds, …, bool link)`
Bake: emit `count-1 × |sourceIds|` transformed copies, no record. Linked: emit
copies **and** push a `DerivedOp` recording sourceIds/params/generatedIds.

### 4. Serialization (Serialization.cpp) — bump `kVersion` 7 → 8
Add `derived_ops` to the SketchFeature branch (`FeatureToJson` :188, `FromJson`
:233). On load, read the records, then run `RegenerateDerived()` once to verify
the stored geometry matches (repair drift). Older files have no `derived_ops` →
empty list, no-op.

### 5. UI
`SketchTool::Pattern` + a small params panel (count, direction/spacing or
center/angle, linear-vs-circular, bake-vs-linked). For a **linked** pattern,
editing `count` in the Properties/params panel re-runs `RegenerateDerived` and
re-solves — the real payoff (change 3→5 instances live).

### 6. Remote + tests
- Remote `pattern` handler; `get_state` round-trips `derived_ops` for free once
  serialization lands.
- Tests: linear/circular counts; **editing a linked pattern's count regenerates
  id-stably** (surviving instances keep ids; a constraint on instance 1
  survives a count change); bake vs linked (editing the seed updates linked
  copies after regen, not baked ones); serialization round-trip of a derived op.

**K2 done-when:** suite green; a linked pattern's count edits live. Commit
`feat: sketch linear and circular pattern`.

---

# K3 — Offset (OCC-backed) — ~1 day

### 1. `SketchFeature::OffsetChain(chainIds, distance, joinArc, side) → generatedIds`
Delegate corner treatment to OCC (FreeCAD does the same —
`DrawSketchHandlerOffset.h`):
- Build a `TopoDS_Wire` from the selected connected chain on the sketch plane
  (reuse the `MakeLoopWire` machinery in `SketchProfile.cpp`).
- `BRepOffsetAPI_MakeOffset` with `GeomAbs_Arc` (arcs at convex corners) or
  `GeomAbs_Intersection` (extend/trim), offset by `±distance`.
- Walk the result edges, convert each back to a sketch `Line`/`Arc` via
  `plane.WorldToPlane` (inverse of `MakeLoopWire`), `AddEntity` them.
- Bake by default. Linked (optional): add `Parallel` (line↔offset line) +
  `Radius`/`Tangent` constraints à la FreeCAD `makeOffsetConstraint` — or defer
  linked-offset and ship bake-only first.

### 2. Reverse conversion helper
`std::optional<Entity> EdgeToSketchEntity(const TopoDS_Edge&, const
SketchPlane&)` — line edge → `Line`; circular edge → `Arc` (read `gp_Circ` +
first/last params). New; the mirror image of `MakeLoopWire`.

### 3. UI + remote + tests
- `SketchTool::Offset` + distance / join-mode / side controls.
- Remote `offset` handler.
- Tests: offset a rectangle outward → a larger rectangle (4 lines, +arcs at
  corners in Arc mode); offset inward; the offset profile extrudes; a convex
  corner in Arc mode yields an arc.

**K3 done-when:** suite green; an offset chain extrudes. Commit
`feat: sketch offset`.

---

## Risks & sequencing
- **K1 is low risk** — reflect math is trivial and it reuses `Symmetric` /
  `Tangent` (both shipped in earlier phases). Fillet's only fiddly bit is
  picking the correct arc side and the too-large-radius guard.
- **K2's risk is the id-stable regeneration** — the sketch-local toponaming-lite
  the plan flagged. Mitigated by copying SolveSpace's remap discipline (stable
  id per (source, k)) and by routing *all* commits through one
  `SolveAndRegenerate` helper so regen can't be skipped.
- **K3's risk is the OCC round-trip** (wire build → offset → edge extraction);
  the reverse `EdgeToSketchEntity` and open-vs-closed chains need care. Bake-
  only first.
- Independent commits; build K1 → K2 → K3. Est ~4 days total.

## Plan-file bookkeeping
- The original Phase K section (plan.md:392) assumed a derived-record
  for *all* ops. Update it to reflect the constraint-linked-primary approach
  (derived-record only for pattern), citing this file.
- Phase K unblocks nothing downstream; L (splines) and M (interaction) remain.
