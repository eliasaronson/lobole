# Phase B — Stable entity IDs + entity deletion (implementation plan)

Detailed plan for Phase B of [plan.md](plan.md). This
is the foundation the topology-changing work (Phase C split/trim, and safe
delete) stands on: constraints must survive entities being inserted and removed.

## Why now / what's actually here

- Constraints reference geometry by **raw vector index** (`PointRef.entity` is
  an index into `SketchFeature::entities`). Any insert/erase before an index
  shifts every later reference — a latent corruption bug the moment topology
  changes. This is FreeCAD's GeoId problem; SolveSpace avoids it with stable
  handles (see the reference notes in plan.md).
- **There is no sketch-entity deletion today at all** — only whole-feature
  deletion (`Document::RemoveFeature`). So Phase B *adds* a delete path with
  correct constraint cleanup; it isn't lifting a code guard.
- The remote/LLM interface references entities across calls, so stable IDs also
  keep controller-held references valid across concurrent edits.

## Design decision: ID stored in each entity struct

Chosen over a wrapper struct (`{id, Entity}`) or a parallel `vector<id>`:

- **Add `uint32_t id = 0;` to each geometry struct** (`Point`, `Line`,
  `Circle`, `Rectangle` in `SketchFeature.h`). The many iterate-all sites
  (rendering, `BuildLoops`, `TessellateEntity`, picking loops) keep working on
  the `Entity` variant with **zero churn** — no `.shape` indirection.
- A `std::visit` accessor `sketch::EntityId(const Entity&)` reads the id
  regardless of alternative; `sketch::SetEntityId(Entity&, uint32_t)` sets it.
- **IDs are 1-based; 0 = "none"** (matches the existing `activeSketchId == 0`
  sentinel). A default-constructed `PointRef{}` (entity 0) resolves to nothing.
- Resolution is a **linear scan** `SketchFeature::IndexOfEntity(uint32_t id)`
  returning `entities` index or `SIZE_MAX`. Sketches hold <100 entities;
  scanning avoids a rebuild-on-mutation cache and the desync bugs a parallel
  map invites. The solver keeps building its own id-keyed maps once per Load
  (see below), so hot paths don't scan repeatedly.

`SketchFeature` gains `uint32_t nextEntityId = 1;` (serialized) and
`uint32_t AddEntity(Entity)` which stamps `nextEntityId++` and appends. All
three production add-paths route through it:
`SketchInteraction.cpp::CommitEntity`, remote `add_feature`
(`EntityFromJson` loop), and any future tool.

## Changes

### 1. `src/core/SketchFeature.h`
- `uint32_t id = 0;` field on `Point`/`Line`/`Circle`/`Rectangle`.
- Free helpers `uint32_t EntityId(const Entity&)` /
  `void SetEntityId(Entity&, uint32_t)` (both `std::visit`).
- `SketchFeature`: `uint32_t nextEntityId = 1;`,
  `uint32_t AddEntity(sketch::Entity e)`, `size_t IndexOfEntity(uint32_t id)
  const` (linear scan, returns `SIZE_MAX` if absent), and
  `bool DeleteEntity(uint32_t id)` (see step 5).
- Update the `PointRef` doc comment: `entity` is now a stable **id**, not an
  index.

### 2. `src/core/SketchSolver.cpp` — `Solver::Load` / `WriteBack`
Currently the `pointIndex`/`lineOf`/`circleOf` maps are keyed by the load-loop
index `i`, which today equals the constraint's `entity` index. Re-key them by
the entity's **id**:
- In the build loop, use `EntityId(sketch.entities[i])` as the map key instead
  of `i` (`lineOf[id]`, `circleOf[id]`, `pointIndex[Key(id, which)]`).
- Constraint resolution (`con.a.entity`, `con.b.entity`) is unchanged — it now
  naturally carries ids that match the re-keyed maps.
- `WriteBack` iterates `entities[i]` and looks up `Key(i, which)`; change to
  `Key(EntityId(entities[i]), which)`. Circle radius lookup `circleOf.at(i)` →
  `circleOf.at(EntityId(entities[i]))`.
- The Tangent side computation reads `im.entityShapes[con.a.entity]` /
  `[con.b.entity]` **as indices** — change these two to
  `entityShapes[IndexOfEntity(...)]` style, or capture the resolved shapes when
  building the maps. (Simplest: a small `im.shapeOfId` map alongside.)

### 3. `src/core/Serialization.cpp`
- `EntityToJson` adds `{"id", EntityId(e)}` to every entity object.
- `EntityFromJson` reads `id` (default 0) and sets it on the constructed
  entity.
- Sketch feature to/from json: serialize `nextEntityId`.
- **Format bump v3 → v4.** On load of a v3 sketch (no per-entity ids,
  constraints hold indices): assign ids `1..N` to entities in order, build an
  `oldIndex → newId` table, remap every constraint's `a.entity`/`b.entity`
  through it, and set `nextEntityId = N + 1`. The loader already rejects
  versions *newer* than `kVersion` (`Serialization.cpp:260`); bump `kVersion`
  to 4.

### 4. `src/remote/RemoteServer.cpp`
- `add_feature`: route entity construction through `sketch->AddEntity(...)` so
  wire-created entities get ids; the response already returns the feature id —
  also return the assigned **entity ids** array so a controller can reference
  them in later `add_constraint` calls.
- `add_constraint` refs now carry ids (unchanged wire shape `[[id,which],...]`,
  just reinterpreted). Add a `delete_entity` method (`{sketch_id, entity_id}`)
  exposing step 5.

### 5. Entity deletion + constraint cleanup (`SketchFeature::DeleteEntity`)
Follows the FreeCAD/SolveSpace recipe (plan.md "deletion
machinery"):
1. **Transfer** — if the deleted entity shares a coincident point with a
   surviving entity, move point-referencing constraints onto the survivor
   before erasing (so intent isn't silently dropped). Reuse the coincidence
   graph from step 2.
2. **Transitive coincidence repair** — collect all points coincident (directly
   or transitively) with the deleted entity's points; after removing the
   dead constraints, re-link the survivors pairwise so A~B~C with B deleted
   leaves A~C.
3. **Orphan sweep** — drop any remaining constraint whose `a`/`b` references
   the deleted id. Count dropped **non-trivial** kinds (everything except
   Coincident/Horizontal/Vertical) and return the count so the UI can warn.
4. Erase the entity from `entities`. Re-solve + regenerate at the call site.

Wrap in one `ModifyFeatureCommand` (baseline before, snapshot after) so delete
is a single undo step — same pattern as `CommitEntity`.

### 6. UI wiring
- `src/ui/AppActions.cpp:295` selection-marker loop: `ref.entity >=
  entities.size()` / `entities[ref.entity]` → `IndexOfEntity(ref.entity)` with
  a `SIZE_MAX` skip.
- `src/ui/SketchInteraction.cpp`: `PickSketchPoint` must emit **ids** in the
  returned `PointRef` (it builds refs from the loop index `i` today — use
  `EntityId`). The `isLine`/`isCircle`/`isPoint` lambdas in
  `ApplySketchConstraint` resolve via `IndexOfEntity`. `EntityPoints` is
  id-agnostic (takes the entity by value) — no change.
- `src/ui/SketchToolbar.cpp`: the `isLine`/`isCircle`/`isPoint` lambdas resolve
  via `IndexOfEntity`.
- Delete key / context-menu "Delete" for a selected sketch entity → call the
  step-5 command; if it dropped non-trivial constraints, surface a short
  toast/log line ("Deleted N dependent constraints").

### 7. Tests
- `tests/test_sketch_feature.cpp`:
  - **v3→v4 migration**: hand-build a v3 json (entities without ids,
    constraints with index refs), load it, assert entities got ids `1..N` and
    the constraint refs were remapped to those ids; assert a re-save is v4.
  - **id stability across insert**: add entities A,B, constrain them, insert a
    new entity, assert the A–B constraint still resolves (ids unchanged).
  - **round-trip** already extended in Phase A — add `id`/`nextEntityId`
    assertions.
- `tests/test_sketch_solver.cpp`: convert the hand-built fixtures to capture
  ids from `AddEntity` and reference them in constraints (instead of literal
  indices). Add a case: delete a line that a neighbour is coincident to →
  neighbour keeps its other constraints, solve still succeeds. Add: A~B~C,
  delete B, assert A~C survives (points stay merged after solve).
- `tests/remote_smoke.py`: `delete_entity` removes an entity and its dependent
  constraint; a follow-up `add_constraint` referencing a surviving id still
  solves.

## Order of work (each step compiles)

1 (structs + helpers, `id==0` everywhere, nothing references ids yet) →
2 (solver re-key) → 3 (serialization + migration) → run solver/serialization
tests → 4 (remote) → 5 (DeleteEntity) → 6 (UI) → 7 (tests) → full `ctest`.

Because IDs are assigned sequentially from 1 and the solver re-keys internally,
**all existing append-only sketches behave identically** — id order matches
insertion order until the first delete/split, so the migration and existing
tests stay green.

## Verification

- `cmake --build build && ctest --test-dir build` green.
- `python tests/remote_smoke.py` passes.
- Manual: draw a few lines with coincident endpoints, delete one middle line,
  confirm neighbours stay put and the undo restores it; load a pre-Phase-B
  saved file (v3) and confirm constraints still hold after edits.

Commit as `feat: stable sketch entity ids and entity deletion`.
