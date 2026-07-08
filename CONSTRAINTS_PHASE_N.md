# Phase N — Sketch UI redesign, generative-op editing, alignment inference

The last sketch milestone. A–M shipped the modelling + interaction layer; N is
UX polish plus the two editing/inference gaps that make the shipped features
feel unfinished: multi-step tools give no guidance, generative ops expose only
`count`, and drawing has no point-to-point alignment snapping.

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
