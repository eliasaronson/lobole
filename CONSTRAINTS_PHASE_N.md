# Phase N — Sketch UI redesign, generative-op editing, alignment inference

The last sketch milestone. A–M shipped the modelling + interaction layer; N is
UX polish plus the two editing/inference gaps that make the shipped features
feel unfinished: multi-step tools give no guidance, generative ops expose only
`count`, and drawing has no point-to-point alignment snapping.

**Status:** N5 (`08428cd`), N1 (`f0bb57a`), N2 (`0377737`), N3 (`606cb24`),
N4-pattern-editing (`9f02de1`) shipped. Remaining: the **follow-ups** below
(datum geometry, op inspector, edit-fillet-radius).

## Current state (grounded in code)

- **Sketch UI is already docked.** The old floating `Begin("Sketch Tools")`
  window is gone; there is a top **icon ribbon** that context-switches on
  sketch-edit (`Toolbar.cpp` `DrawSketchModeToolbar`) plus a docked **"Sketch"**
  panel tabbed next to Properties (`SketchToolbar.cpp` `Begin("Sketch")`,
  `DockLayout.cpp:74`). So the original N's "dock, don't float" half is **done**;
  what remains is the per-tool options/staging and the generative editing.
  (Note: `SketchToolbar.cpp:49` still has a dead `Begin("Sketch Tools")` path —
  delete it as part of N1.)
- **Generative ops.** Mirror + fillet are **constraint-linked** (Symmetric /
  Tangent, no stored record). Patterns use `sketch::DerivedOp` (`kind`,
  `sourceIds`, `vec` = linear step / circular centre, `scalar` = circular step
  angle, `count`, `instances[]` mapping `sourceId,k → genId`) replayed by
  `RegenerateDerived()`. Offset is **bake-only** (OCC `OffsetChain`, no op
  record). **The DerivedOp already stores the full transform — only `count` is
  surfaced in the UI.**
- **Multi-step tools** rely on a prior selection + a one-line
  `SketchEditState::actionMsg`; no staged prompts, no pre-commit preview for
  mirror/pattern/offset.
- **Inference (Phase E).** `SketchInference::InferPoint` returns a `Snap`
  (coincident vertex, point-on-line, and — only when a segment's other end is
  known — H/V/parallel/perpendicular for the *segment being drawn*). There is
  **no** inference that aligns a new point to *another* point's X/Y with a guide
  line.

## Corrected assumptions (verified)

- Editing pattern params (spacing/direction/angle/centre/count) is **UI-only** —
  `DerivedOp` already holds all of them; `RegenerateDerived()` replays. No core
  change beyond an `EditPatternParams` setter mirroring the existing count path.
- Two-point Horizontal/Vertical **is supported by the solver**: PlaneGCS has
  `addConstraintHorizontal(Point&, Point&)` and `…Vertical(Point&, Point&)`
  (`deps/planegcs/GCS.h:407,409`). Today `SketchSolver.cpp:211-217` only wires
  the **line** form. Adding the point-pair branch is small.
- Fillet radius is **not** a stored op param — a fillet is the arc + a Tangent
  constraint, so "edit fillet radius" means editing the arc's radius (add/edit a
  Radius dimension on the arc), not a DerivedOp field.
- Offset becoming editable needs real work: a new `DerivedKind::Offset` + a
  `RegenerateDerived` case backed by `OffsetChain`. Ship it as an opt-in, since
  bake-only offset already works.

## Workstreams

### N1 — Tool state machine + staged prompts (foundation)
Replace the ad-hoc per-tool pending state + `actionMsg` with an explicit,
per-tool **staged input** model.
- `SketchEditState`: a `toolStage` index and a small per-tool descriptor
  (ordered steps, each with a prompt string and what input it consumes). Generic
  advance/cancel so a click that doesn't match the current step reports *why*.
- A shared **prompt bar** ("Step 2 of 2 — click the mirror axis") rendered once
  (top of the viewport or the Sketch panel), superseding `actionMsg`.
- Delete the dead `Begin("Sketch Tools")` block in `SketchToolbar.cpp`.
- Tests: GUI — a tool with steps shows the right prompt and rejects wrong input
  with feedback.

### N2 — Guided Mirror
- Explicit two steps: (1) select geometry → (2) pick axis; selection highlighted
  distinctly from the axis; **live preview** of the reflected result; confirm /
  cancel. Today it silently reuses a prior selection.
- **Axis sources beyond a drawn line**: a construction line, or the plane
  **X/Y axes** (add pseudo-pick targets for the two axes). `MirrorEntities`
  already reflects across an arbitrary axis line — only the axis *picker* grows.
- Tests: GUI staged flow (select → axis → preview → commit); mirror across a
  plane axis reflects correctly.

### N3 — Guided Pattern / Offset with ghost preview
- Inline param form + **live ghost preview** before commit for both.
- Pattern form exposes the full transform (linear spacing + direction; circular
  centre + step angle; count; **link / unlink**).
- Offset: distance form + ghost. Optionally introduce `DerivedKind::Offset` so
  the distance stays editable (backed by `OffsetChain` in `RegenerateDerived`);
  otherwise keep bake-only and just add the preview.
- Tests: GUI pattern/offset preview reflects param changes; committing matches
  the preview.

### N4 — Generative-op editing + selected-op inspector
- `EditPatternParams(opIndex, vec, scalar, count, link)` alongside the existing
  count setter; `RegenerateDerived()` replays id-stably.
- **Edit-fillet-radius** affordance: surface the fillet arc's radius as an
  editable field (writes/edits its Radius dimension) instead of requiring the
  user to add one by hand.
- **Selected-op inspector**: clicking a generated entity (or the op in a list)
  opens its params for editing — `DerivedOp.instances` already maps
  `genId → op`, so build the reverse lookup and route the click to the inspector.
- Tests: `EditPatternParams` unit-tested like `SetPatternCount` (id-stable
  regen); GUI param-edit regenerates the array.

### N5 — Point-alignment inference  ("snap to H/V of other points") — user-requested
The Onshape-style dashed guide when a new point lines up with an existing one.
- **Inference** (`SketchInference::InferPoint`): after the coincident /
  point-on-line checks, scan existing vertices; if `|cursor.x − v.x|` is within
  the pixel tolerance → **vertical alignment** (snap x to `v.x`); if
  `|cursor.y − v.y|` within tol → **horizontal alignment** (snap y). Carry the
  target `PointRef` + axis in `Snap` (new fields `alignShareX` / `alignShareY`).
  Precedence: coincident > point-on-line > **alignment** > segment H/V (it's a
  positional snap, resolved before the segment-angle snap; both can apply).
- **Guide render**: a dashed guide line from the aligned vertex through the
  cursor (reuse the dashed-line primitive; distinct colour from constraint
  glyphs).
- **Constraint on commit** (when `autoConstrain` is on): add a Vertical
  (shared X) / Horizontal (shared Y) constraint between the new point and the
  target vertex — so the alignment persists, matching Phase E's behaviour.
- **Solver**: extend `SketchSolver.cpp` Horizontal/Vertical loading to accept a
  **two-point** form — when `con.b` resolves to a point, call the PlaneGCS
  `addConstraint{Horizontal,Vertical}(p1, p2, tag)` overload; keep the existing
  line form when only `con.a` is a line. (`ConstraintKind` unchanged; the two
  refs disambiguate.)
- Tests: unit — `InferPoint` returns alignment to a vertex sharing X or Y, and
  snaps the coordinate; solver — two-point Horizontal/Vertical solves (two free
  points end up sharing Y / X). GUI — placing a point under an existing one shows
  the vertical guide and (autoConstrain on) adds a Vertical constraint.

## Sequencing
N1 is the foundation for N2/N3's staging. **N5 is independent** (inference +
solver, no UI-framework dependency) — good first slice and directly the
user-visible snapping gap. Then N1 → N2 → N3 → N4. Each workstream is its own
commit; N5 can ship before or in parallel with the UI work.

## Risk
Medium, and mostly UI/UX wiring. The only core/solver changes are the two-point
H/V branch (N5, tiny) and the optional `DerivedKind::Offset` (N3). Everything
else exposes params the model already stores or restructures ImGui panels.

---

# N follow-ups (planned)

Deferred from the N workstreams above. **NF1 is the big one** and the highest
value: it is what every CAD sketcher leans on — mirror across an axis, and
snap / align to the origin — and all of it falls out of one feature.

## NF1 — Datum geometry: origin point + X/Y axes

Today a sketch has no origin or axes you can pick, snap to, or mirror across —
so "mirror across the Y axis" and "line this vertex up with the origin" (both
extremely common) are impossible. Add the **datum set**: the origin point at
(0,0) and the two axis lines (X = the +U direction through the origin, Y = +V),
always present, never user-created or deletable. This single feature unlocks
mirror-across-axis, snap-to-origin, align-H/V-to-origin, point-on-axis, and
symmetric-about-axis — the whole family.

### Model — datum references (not entities)
The datums are **not** stored in `SketchFeature::entities`; they are constant
references the solver, picking, inference, and mirror special-case. Reserve a
high id block so `nextEntityId` (which grows from 1) can never collide:

```
constexpr uint32_t kDatumBase = 0xFFFFFF00;
kOrigin = kDatumBase;      // point (0,0)
kAxisX  = kDatumBase + 1;  // line (0,0)->(1,0):  which 0 = origin, which 1 = +U
kAxisY  = kDatumBase + 2;  // line (0,0)->(0,1):  which 0 = origin, which 1 = +V
```
A small helper (`sketch::IsDatum(id)`, `sketch::DatumPointUV(ref)`,
`sketch::DatumAxisEndpoints(id) -> (a,b)`) is the single source of truth for
their geometry. **Integration caveat to thread everywhere:**
`SketchFeature::IndexOfEntity(datumId)` returns `SIZE_MAX` (they aren't in the
vector), so every site that resolves a ref by index — `MirrorEntities`,
`RefUV`/`EntityPointUV` callers, drag, delete — must check `IsDatum` first and
route to the datum helpers. Grep the `IndexOfEntity(... .entity)` call sites; a
missed one is a crash on a datum ref.

### Solver (`SketchSolver.cpp` `Load`)
Register the datum points once, up front, and pin them:
- `addPoint(kOrigin, 0, {0,0})`, `addPoint(kAxisX, 0, {0,0})`,
  `addPoint(kAxisX, 1, {1,0})`, and the same for `kAxisY`; insert every one of
  their params into `fixedParams` so `DeclareUnknowns` holds them out (they
  never move — same mechanism as the `Fixed` constraint at
  `SketchSolver.cpp:~300`).
- Build `GCS::Line`s for `kAxisX`/`kAxisY` (p1 = origin point, p2 = dir point)
  and populate `lineOf[kAxisX]`/`lineOf[kAxisY]`.
Then `Resolve` already returns the datum points (it's just a `pointIndex`
lookup, `SketchSolver.cpp:67`), and the existing constraint cases work as-is on
datum refs: Coincident-to-origin, PointOnLine-onto-an-axis, two-point
Horizontal/Vertical-to-origin (from N5), and — crucially — **Symmetric about a
datum axis**, since its case already does `im.lines[im.lineOf[con.c.entity]]`
(`SketchSolver.cpp:314`) and `lineOf` now contains the axis.

### Mirror across an axis (`MirrorEntities` + N2 tool)
`MirrorEntities(sources, axisId, link)` currently does
`IndexOfEntity(axisLineId)` and reads the line's `a`/`b`. Generalise: if
`IsDatum(axisId)`, take `(a,b)` from `DatumAxisEndpoints(axisId)` instead of the
entities vector; the reflection (`ReflectEntity`) and the linked `Symmetric`
constraint (which references `axisId`) then work unchanged — the solver resolves
the datum axis line. In the N2 guided-mirror axis step, add the X/Y axes (and the
origin, for point mirroring later) as pick targets so step 2 accepts them.

### Inference / snapping (`SketchInference::InferPoint`)
Feed the datums into the candidate scans (the N5 `ForEachVertex` set already
generalises this):
- **Coincident**: the origin is a vertex candidate → snap-to-origin.
- **Point-on-line**: the two axes are line candidates → drop a point onto an
  axis (add a `PointOnLine` to `kAxisX`/`kAxisY` on commit).
- **Axis alignment (N5)**: include the origin so a new point aligns H/V with it,
  yielding a two-point Vertical/Horizontal to `kOrigin`.
Guides/glyphs render the same way; the constraints reference the datum ids.

### Picking + rendering
- **Render** the two axes as thin full-width lines through the origin (distinct,
  muted colour; X warmer, Y cooler is the usual convention) plus an origin dot.
  A plane grid already draws — the axes layer over it and, unlike the grid, are
  *pickable*.
- **Pick**: `PickSketchEntity` / `PickSketchPoint` add the origin (point) and the
  two axes (bodies) to the candidate set, returning datum refs. Hover highlight
  reuses the existing path. Keep datum hits lower priority than real geometry so
  they don't shadow user entities.

### Serialization
Nothing to store — datums are implicit. Constraints that *reference* datum ids
serialise normally (the ids round-trip); on load they resolve because the solver
re-registers the datums every `Load`. Add a guard so a stray constraint to a
datum id from a hand-edited file doesn't crash (it just resolves or is skipped).

### Tests
- Solver: Coincident(point, kOrigin) pins the point to (0,0); PointOnLine(point,
  kAxisX) drops it to y=0; Symmetric about kAxisY mirrors a point's X.
- Mirror: `MirrorEntities({line}, kAxisY, link)` reflects across x=0 and adds a
  Symmetric to `kAxisY` that solves.
- Inference: `InferPoint` near (0,0) returns coincident-to-origin; near the X
  axis returns point-on-line to `kAxisX`; sharing the origin's X returns
  `alignShareX == kOrigin`.
- GUI: guided mirror across the Y axis reflects the selection; drawing a point
  near the origin snaps to it.

### Risk
Medium-high — it's a cross-cutting reference type. The mechanics are small
(fixed points + two lines in the solver; helpers for geometry), but the
**`IndexOfEntity` audit** is the real work: every ref-by-index site must tolerate
a datum id. Do the solver + inference + mirror first (pure, testable), then
picking/rendering.

## NF2 — Selected-op inspector
Clicking a generated/derived entity surfaces its op's params for editing. Build
the reverse map `genId -> (opIndex)` from `DerivedOp::instances` (already stores
`genId`); on selecting such an entity, show the same inline form N4 added to the
Patterns list, scoped to that op, with a "delete pattern" affordance. Small once
N4's `EditPatternParams` exists — it's routing a selection to the editor.

## NF3 — Edit-fillet-radius
A fillet is an arc + a Tangent (constraint-linked, not a `DerivedOp`), so its
radius is the arc's `radius`, driven by a Radius dimension when one exists.
Affordance: when the selection is a fillet arc (or on hover), show a radius field
that (a) edits an existing Radius dimension on the arc, or (b) adds one if
absent, then re-solves. Reuses the dimension-edit path (`ApplyDimensionEdit`).
Smaller than NF1; independent.

## Sequencing
NF1 first (highest value, and NF2 wants a selected derived entity to inspect
anyway). NF1's solver/inference/mirror slices land before its picking/render
slice. NF2 and NF3 are small and independent.
