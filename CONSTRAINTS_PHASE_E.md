# Phase E — Snapping / auto-constraint inference

Detailed plan for Phase E of [CONSTRAINTS_PLAN.md](CONSTRAINTS_PLAN.md). The
Onshape-feel milestone: while drawing, snap the cursor to nearby geometry and
add the implied constraint automatically, with a ghost glyph showing what will
be applied. Generalizes the `PointOnLine`-on-commit taste added in Phase D.

## Current state (verified)

- The draw FSM (`SketchInteraction.cpp` `HandleSketchViewport`) sets
  `edit.hoverPoint = uv` each frame; the first click stores `edit.pendingPoint`,
  the second commits via `CommitLine` / `CommitEntity` / `CommitRectangle`.
- `BuildSketchOverlay` draws the rubber-band from `pendingPoint` to
  `hoverPoint` — so snapping `hoverPoint` makes the preview follow for free.
- `CommitLine` already does one inference by hand: if the drawn endpoint `b`
  lands on a line body, it appends a `PointOnLine`. Phase E replaces this with a
  general inference pass.
- `SketchSolver`'s `SolveResult` carries `dof`, `conflicting`, and `redundant`
  — enough for add-then-diagnose (a DOF-delta is the simplest signal).
- Screen/pixel math is `ui::SketchUVToScreen` (needs camera + viewport).

## Design: a pure, testable inference pass

`src/core/SketchInference.{h,cpp}` — UI-agnostic so it unit-tests without a
camera. Pixel tolerances come in as a `pixelsPerUV` scale the UI computes from
the current zoom (`length(SketchUVToScreen(uv + {1,0}) - SketchUVToScreen(uv))`).

```
namespace sketch {
enum class GlyphKind { Coincident, PointOnLine, Horizontal, Vertical,
                       Parallel, Perpendicular };

struct Snap {
  glm::vec2 uv;                          // the point, snapped (or unchanged)
  std::optional<PointRef> coincident;    // snap the point coincident to this
  std::optional<uint32_t> pointOnLine;   // else bind onto this line id
  // segment-level (needs the segment's other end; only for line drawing):
  bool horizontal = false, vertical = false;
  std::optional<uint32_t> parallelTo, perpendicularTo;
  std::vector<GlyphKind> glyphs;         // what to draw at the cursor
};

// Infer snaps for a point being placed at `uv`. `other` is the segment's fixed
// end (the pending start) when drawing a line, else nullopt (no segment-level
// inference). Excludes `skipId` (the entity being drawn) from candidates.
Snap InferPoint(const std::vector<Entity>& entities, glm::vec2 uv,
                std::optional<glm::vec2> other, uint32_t skipId,
                float pixelsPerUV);
}
```

Detection, in priority order (higher wins the *point* snap; segment-level H/V/
parallel are independent and combine):

1. **Coincident** — `uv` within ~10 px of an existing endpoint / circle centre
   → snap `uv` to it, set `coincident`. (Highest priority — a vertex hit beats
   an edge hit.)
2. **PointOnLine** — else `uv` within ~8 px of a line body → snap `uv` to the
   nearest point on that segment, set `pointOnLine`.
3. **Horizontal / Vertical** — with `other` present, if the segment
   `other→uv` is within **2°** of an axis (use a slope ratio, not an absolute
   angle): snap `uv` to exact alignment (`uv.y = other.y` / `uv.x = other.x`)
   and set the flag. (Skip if a tangent-class snap won — matches FreeCAD's
   "tangent suppresses alignment".)
4. **Parallel / Perpendicular** — else if the segment is within 2° of an
   existing line's direction (nearest such line wins), set `parallelTo` /
   `perpendicularTo` (no geometry snap — the solver settles it).

Coincident/PointOnLine snapping happens **before** H/V so the endpoint lands on
the target first; H/V then only nudges along the free axis.

## Applying on commit (add-then-diagnose)

The commit path translates a `Snap` (which references "the new point") into real
constraints once the entity id exists, then keeps only the ones that bite:

- Place geometry at the snapped `uv` (solve starts already satisfied).
- For each inferred constraint, in priority order: append it, `Solver::Solve()`,
  and **keep it only if `dof` decreased and the result isn't conflicting**;
  otherwise pop it back off. This silently drops redundant inferences (e.g. a
  second H on an already-horizontal line) without needing to map solver tags.
- Whole undo record as today (one `ModifyFeatureCommand`).

Line drawing infers on **both** endpoints: the start's `Snap` is captured on the
first click (`edit.pendingSnap`) and applied to `(newId, 0)` at commit; the
end's `Snap` is applied to `(newId, 1)`; segment-level H/V/parallel come from the
final `other→uv` segment. `CommitLine`'s hand-rolled PointOnLine is removed in
favour of this.

## Per-frame preview + snapping

In `HandleSketchViewport`, after computing `uv` and when a draw tool is active
and `edit.autoConstrain` and **not** Ctrl-held:

- `Snap s = InferPoint(...)` with `other = pendingPoint` when `hasPendingPoint`.
- Set the working `uv = s.uv` (so the rubber-band preview and the click both use
  the snapped point) and store `s` in `edit.currentSnap` for glyph rendering.
- On first click, also stash `s` in `edit.pendingSnap`.
- Ctrl held → skip inference for that frame/click (raw placement).

## Glyph rendering (2D overlay)

Ghost glyphs are 2D markers at the cursor, not 3D line geometry — render them
with `ImGui::GetForegroundDrawList()` in the UI pass (a new
`ui::DrawSnapGlyphs(state)` called from `UI.cpp`), positioned at
`SketchUVToScreen(edit.currentSnap.uv)`:

- `Coincident` → a small filled circle ●; `PointOnLine` → a short tick on the
  line; `Horizontal`/`Vertical` → "H"/"V"; `Perpendicular` → "⟂";
  `Parallel` → "∥". Offset a few px from the cursor; stack multiple.
- Also draw a snap indicator (a hollow ring) at the snapped point so the user
  sees the cursor "grabbed".

## State + toggle

`SketchEditState` gains: `bool autoConstrain = true;`, `Snap currentSnap;`
`bool hasCurrentSnap;`, `Snap pendingSnap;` (for the line start). A checkbox in
`SketchToolbar` toggles `autoConstrain`. Cleared each frame like the hover/trim
state.

## Tests

- `test_sketch_inference.cpp` (new, pure — no GUI/PlaneGCS needed for the
  geometry logic):
  - cursor near an endpoint → `coincident` set, `uv` snapped to it.
  - cursor near a line body → `pointOnLine` set, `uv` on the segment.
  - near-horizontal segment (within 2°) → `horizontal`, `uv.y == other.y`;
    likewise vertical; a 30° segment → neither.
  - near-parallel to an existing line → `parallelTo` that line.
  - Ctrl-equivalent (autoConstrain off path) is a UI concern, not here.
- `test_sketch_solver.cpp`: add-then-diagnose — applying a redundant H to an
  already-horizontal line leaves `dof` unchanged and the constraint is dropped.
- GUI (`GuiTests.cpp`, deterministic — set `hoverPoint`/`pendingPoint` and call
  the commit path directly, as the pick/trim tests do):
  - `infer/horizontal_snaps`: draw a near-horizontal line → an H constraint is
    added and the endpoints' y equalize after solve.
  - `infer/endpoint_coincident`: end a line near another's endpoint → a
    Coincident is added and the points merge.
  - `infer/ctrl_suppresses`: with autoConstrain off (or the Ctrl path), no
    constraints are inferred.

## Build order (each step green)

1. `SketchInference` + unit tests (pure geometry; no UI). 2. Wire the commit
paths to apply the snap with add-then-diagnose (replace CommitLine's ad-hoc
PointOnLine); solver tests. 3. Per-frame preview snapping (hoverPoint) + state +
autoConstrain toggle. 4. Glyph overlay. 5. GUI tests + `/run` visual check.

## Verification

- `cmake --build build && ctest` green; `remote_smoke.py` passes.
- `/run`: draw a nearly-horizontal line and watch it snap flat with an "H"
  glyph; end a line on a vertex and see the ● coincident glyph + merge; hold
  Ctrl to place freely; toggle auto-constrain off and confirm no inference.
  Draw a rectangle-ish shape freehand and confirm it comes out constrained.

Risk: medium — tolerance tuning and avoiding over-constraining. The tolerances
above (10 px point, 8 px edge, 2° angular) are the proven values from the
FreeCAD/SolveSpace research; add-then-diagnose is the safety net against
over-constraint.

Commit as `feat: sketch auto-constraint inference and snapping`.
