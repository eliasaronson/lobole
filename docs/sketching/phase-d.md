# Phase D — Rectangle decomposition + split + line trim

Detailed plan for Phase D of [plan.md](plan.md). This is
the "cuttable segments" milestone the user asked for: rectangles become four
real, individually-editable lines, and any line can be trimmed at its crossings.

Builds directly on Phase B's stable ids + `DeleteEntity` constraint cleanup and
Phase C's body picking. Circle trim produces arcs, so it waits for Phase G;
**this phase is lines-only**, which covers the rectangle workflow.

## Current state (verified)

- `sketch::Rectangle` is a rigid variant alternative, invisible to the solver
  (`SketchSolver.cpp` skips it). It's handled in ~10 active sites: the variant
  (`SketchFeature.h:28,33`), `BuildLoops`/`TessellateEntity`
  (`SketchProfile.cpp:65,215`), serialization (`Serialization.cpp:46,66`), the
  draw FSM + preview + `EntityPoints` (`SketchInteraction.cpp:279,318,352`), the
  dot builder (`AppActions.cpp:292`), the `Rect` toolbar button
  (`SketchToolbar.cpp:57`, `SketchTool::Rectangle` in `AppState.h:34`), and a
  test (`GuiTests.cpp:330`). (The legacy `src/Sketch.{h,cpp}` `SketchRectangle`
  is the dead old system — ignore it.)
- `BuildLoops` (`SketchProfile.cpp:51`) chains loose line segments into closed
  loops by endpoint proximity, so **four lines forming a rectangle already
  extrude** once their corners coincide — decomposition needs no profile change
  beyond dropping the `Rectangle` branch.
- `DeleteEntity` (Phase B) already does transitive-coincidence repair + orphan
  sweep — reuse it to remove a trimmed piece.
- No line–line intersection helper exists yet; trim needs one.

## Part 1 — Rectangle as four lines

A shared helper both the tool and the loader use:

```
// Appends 4 lines + 4 corner coincidents + 2 H + 2 V, returns the 4 line ids.
std::array<uint32_t,4> DecomposeRectangle(SketchFeature&, glm::vec2 c1, glm::vec2 c2);
```

Corners A(x1,y1) B(x2,y1) C(x2,y2) D(x1,y2); lines AB, BC, CD, DA (each via
`AddEntity`). Constraints: coincident joining each line's end to the next
line's start (4), Horizontal on AB & CD, Vertical on BC & DA. Emit them all,
then solve — the shape is fully determined by the two drag corners + H/V.

- **Tool** (`SketchInteraction.cpp` rectangle case): replace
  `CommitEntity(..., Rectangle{a,uv})` with one `ModifyFeatureCommand` wrapping
  `DecomposeRectangle` (baseline before, snapshot after) so it's a single undo
  step. The preview (`BuildSketchOverlay`) stays a rubber-band rectangle outline
  (4 preview lines) until committed.
- **Remove the `Rectangle` variant** and its branches everywhere above. The dot
  builder / `EntityPoints` / `Tessellate` / `BuildLoops` just lose their rect
  cases.

## Part 2 — Serialization: drop the rigid rect (v4 → v5)

Bump `kVersion` to 5. On load, a legacy `"rect"` entity is expanded via
`DecomposeRectangle` instead of constructing a `Rectangle`:

- In `FeatureFromJson`'s sketch branch, after building entities, replace any
  entity that came from a `"rect"` JSON with the four decomposed lines +
  constraints (run before the id-migration pass, or fold into it).
- Any constraint that referenced a legacy rect entity is dropped — rects were
  solver-invisible, so such constraints were no-ops. Note this in a comment.
- `EntityFromJson` no longer needs a `"rect"` case for constructing entities,
  but `Load` must still recognize the string to expand it. Old (v4) files thus
  round-trip into v5 with real lines.
- Update `test_sketch_feature.cpp` / any serialization test that used rects.

## Part 3 — `SplitEntity` (the core op)

```
// Splits the line `id` at point P (assumed on the segment). Piece 1 keeps `id`
// (its `b` becomes P); piece 2 is a new line P→b. Returns piece 2's id (or 0 if
// `id` isn't a splittable line).
uint32_t SketchFeature::SplitEntity(uint32_t id, glm::vec2 P);
```

Constraint transfer (FreeCAD `deriveConstraintsForPieces`, lines-only subset):

- **New coincident** joining piece1.b (=P) and piece2.a (=P).
- **Endpoint refs**: `(id, 0)` stays (piece1.a unchanged). `(id, 1)` → the new
  id's `which=1` (moved to piece2.b).
- **Whole-line H / V / Parallel / Perpendicular**: piece1 keeps the original
  (same id); **copy** the constraint onto piece2's id so both stay
  axis-aligned/parallel. (This is what keeps a trimmed rectangle side straight.)
- **Equal**: kept on piece1 only (first piece), per FreeCAD.
- **Distance**: a whole-edge length (both refs are the line's own endpoints)
  re-anchors to span `piece1.a ↔ piece2.b`; a point-referenced distance follows
  the piece whose parameter range contains the referenced endpoint.
- **Angle / PointOnLine**: follow the piece containing the referenced point (by
  parameter along the original line).
- No collinear is added between pieces — matches FreeCAD/SolveSpace; H/V
  transfer already keeps rectangle sides straight, which is the case that
  matters.

Piece1 keeping `id` means most refs to it stay valid for free; only `(id,1)`
and the whole-line copies need work.

## Part 4 — Trim tool (lines only)

- New `SketchTool::Trim` + a **Trim** toolbar button (scissors icon). Uses
  Phase C body picking to identify the clicked line and the click's parameter.
- `SketchFeature`-level helper computes the **cut points** on a line: for every
  other line, the segment–segment intersection point that lies within both
  segments (new `SegmentIntersect(a1,b1,a2,b2)` util); plus the line's own
  endpoints. Sort by parameter `t` along the line.
- The clicked parameter selects the interval `[t_i, t_{i+1}]` to remove:
  - `SplitEntity` at `t_{i+1}` then at `t_i` (order matters so ids stay valid),
    yielding the piece between them, then `DeleteEntity` that piece (Phase B
    cleans up its constraints).
  - If no interior cut brackets the click (a free line) → `DeleteEntity` the
    whole line.
- One `ModifyFeatureCommand` per trim (baseline on click). Re-solve + regen.
- Circles are skipped with a log line ("circle trim needs arcs — Phase G") so
  the limitation is explicit, not silent.

## Part 5 — Draw-onto-a-line (minimal)

When the Line tool's committed endpoint lands within a pixel tolerance of an
existing line body (reuse `PickSketchEntity`), also append a `PointOnLine`
constraint (Phase A) binding the new endpoint to that line — no eager split.
This is a small taste of Phase E inference; fuller snapping (H/V, coincident
glyphs) stays in Phase E. Behind the existing commit path so it's one undo step.

## Tests

- `test_sketch_solver.cpp` / a new `test_sketch_topology.cpp`:
  - `DecomposeRectangle` → 4 lines + 8 constraints; solves fully-constrained;
    `BuildLoops` returns one closed loop; extrudes to the right volume.
  - `SplitEntity` on a dimensioned line: the length re-anchors across both
    pieces; an H constraint ends up on both pieces; `(id,1)` refs moved.
  - Trim the middle of a line crossed by two others → three pieces minus the
    middle = two lines; crossing constraints intact.
  - `SegmentIntersect` unit cases (crossing, parallel, t-outside-segment).
- `test_sketch_feature.cpp`: a v4 file with a `"rect"` loads as 4 lines + H/V in
  v5; round-trips.
- GUI (`GuiTests.cpp`): `rect/decomposes` (Rect tool → 4 line entities, drag
  stays rectangular); `trim/removes_segment` (draw a cross, trim one arm).
- `remote_smoke.py`: a rectangle via the wire is 4 lines; a `trim` call (if
  exposed) removes a segment.

## Build order (each step green)

1 (DecomposeRectangle + tool, keep variant temporarily) → 2 (remove variant +
serialization/migration) → 3 (SplitEntity + tests) → 4 (Trim tool) → 5
(draw-onto-line) → tests. Steps 1–2 are the riskiest (wide variant removal);
3–4 are additive.

## Verification

- `cmake --build build && ctest` green; `remote_smoke.py` passes.
- `/run` + screenshot: draw a rectangle, confirm 4 selectable sides with corner
  dots; drag a corner and it stays rectangular; draw two crossing lines and trim
  a segment away; open a pre-Phase-D file containing a rectangle and confirm it
  loads as editable lines.

Commit as `feat: rectangles as lines, entity split and line trim`.
