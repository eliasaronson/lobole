# Phase M — Interaction & keyboard (detailed plan)

Interaction primitives: Select as the default mode, an Escape cascade, a
**context-sensitive Onshape keyboard scheme** (sketch keys while editing, 3D/view
keys otherwise), and box (rubber-band) multi-select. Mostly wiring on top of the
existing tool/selection/camera state; the only "core" piece is a pure `BoxSelect`
so the hit-test is unit-testable. The keymap spans both the sketch
(`SketchInteraction`) and the model view (`ViewportInput` / camera), so this
phase reaches slightly past the sketcher — deliberately, to match Onshape.

Anchors in the current code (verified):
- Default tool: `AppState.h:58` `SketchTool tool = SketchTool::Line;`.
- New sketch resets state: `SketchInteraction.cpp:290`
  `state.sketchEdit = SketchEditState{};` (so the struct default *is* the new
  sketch's tool).
- Escape handler: `SketchInteraction.cpp:365-372` (today: cancel pending → else
  leave edit).
- Select branch click: `SketchInteraction.cpp:438-455` (empty click clears at
  :448 — the box-select hook; pick-hit begins a drag at :446).
- Frame entry / `overViewport` gate: `SketchInteraction.cpp:344,374`.
- Routers to reuse: `edit.tool = …` (toolbar `toolButton`), `ApplySketchConstraint`
  (`SketchInteraction.cpp`), `SketchUVToScreen` (the UV→screen projector),
  `sketch::TessellateEntity` (entity → UV polyline).

Each step compiles; run tests after steps 4 and 5.

## 1. Select is the default mode
- `AppState.h`: change the `SketchEditState::tool` initializer from
  `SketchTool::Line` to `SketchTool::Select`. `BeginSketchOnPlane` already
  resets `sketchEdit`, so a new sketch lands in Select automatically.
- Drawing tools already stay active for repeat placement (the Line branch
  re-arms `pendingPoint`); nothing to change there. Escape returns to Select
  (step 2), making Select the resting state — its toolbar button stays but is
  rarely needed.

## 2. Escape cascade
Replace `SketchInteraction.cpp:365-372` with a one-level-per-press back-out:
1. an in-progress draw (`hasPendingPoint` / `hasPendingPoint2`) **or** an active
   box drag (step 4) → cancel just that;
2. else a non-Select tool active → `edit.tool = SketchTool::Select`
   (also clear `edit.actionMsg`);
3. else selection non-empty → `edit.selected.clear()`;
4. else → `edit = SketchEditState{}` (leave sketch edit).
Predictable "undo my mode". Keep returning `true` (consumed).

## 3. Keyboard shortcuts — adopt Onshape's scheme (context-sensitive)
Onshape's map is **context-sensitive**: the same key does different things in a
sketch vs the model view — `n` = Coincident while sketching but "view normal to"
otherwise; `f` = zoom-to-fit globally while `Shift+f` = Fillet in a sketch. Match
that so the whole scheme transfers, not just the sketch keys. Dispatch by the
active context:
- **Sketch context** — a sketch is being edited (`sketchEdit.IsEditing()`).
- **Model context** — otherwise (3D view / part studio).

Guard everywhere: skip when `ImGui::GetIO().WantTextInput`; let Ctrl/Alt chords
(undo/redo) pass through; `Shift` is used by some bindings. Mechanism: a
`{context, chord, action}` table. Sketch keys go in a `HandleSketchKeys` called
from `HandleSketchViewport` (only while editing); model keys **extend the
existing camera-key block** in `ViewportInput.cpp:117-141` — which already does
`z`/`Shift+z` zoom + arrow orbit/pan/roll and *matches Onshape*, so keep it.

### Global (both contexts) — several already exist
| chord | action | status |
|---|---|---|
| `Ctrl+Z` / `Ctrl+Y` (also `Ctrl+Shift+Z`) | undo / redo | exists (MainMenuBar) |
| `Delete` | delete selection | exists (sketch Select branch) |
| `Space` | clear selection | new |
| `Esc` | cancel / back out | new cascade (§2) |
| `Enter` | accept pending preview / commit dimension | new |
| `f` | zoom to fit | new (route to the fit used by the toolbar/`fit_view`) |
| `z` / `Shift+z` | zoom out / in | exists (ViewportInput) |
| arrows (`Shift`=90°, `Ctrl`=pan, `Alt`=roll) | rotate / pan / roll | exists |

### Sketch context (editing a sketch)
- **Tools** (bare key → `edit.tool`, clearing pending/`actionMsg` like
  `toolButton`): `l` Line · `c` Circle · `g` Rectangle · `a` 3-pt Arc · `m` Trim
  · `o` Offset · `Shift+f` Fillet. (Onshape avoids collisions here: Trim is `m`,
  not `t`=Tangent; Rectangle is `g`, not `r`.)
- **Constraints** (only when `!edit.selected.empty()` → `ApplySketchConstraint`):
  `h` Horizontal · `v` Vertical · `n` Coincident · `b` Parallel · `t` Tangent ·
  `e` Equal · `d` Dimension.
- **Also:** `q` toggle construction (`ToggleConstruction`) · `Shift+s` suppress
  inferencing (toggle `autoConstrain`).
- No Onshape single-key for Point / Mirror / Pattern → toolbar (add `Shift+…`
  later if wanted). Our extra constraint kinds (perpendicular, concentric,
  symmetric, midpoint, fixed, collinear, angle, radius, diameter, point-on-line)
  have no Onshape key → toolbar only.

### Model context (not editing a sketch)
| chord | Onshape | our action |
|---|---|---|
| `n` | View normal to | orient the camera normal to the selected face (`AlignToFace`) |
| `Shift+1…7` | Front/Back/Left/Right/Top/Bottom/Iso | standard camera views (new helper) |
| `Shift+p` | Hide/show planes | toggle the default-plane visibility |
| `Shift+s` | Sketch | start a new sketch (on the selected plane/face, else arm plane-pick) |
| `Shift+e` | Extrude | extrude the selected sketch |

### Reserved (feature not present yet)
Onshape keys we leave unbound until the feature exists, so the letters aren't
reused: `x` Extend, `u` Use/project, `w` zoom-to-window, mate/assembly keys,
section & named views, drawings.

## 4. Box (rubber-band) select
### 4a. Pure hit-test — `src/core/SketchSelection.{h,cpp}` (new, like SketchInference)
```cpp
// Entity ids whose geometry falls in the screen-space rect [min,max].
// `crossing` false = window (fully enclosed); true = crossing (touching).
// `project` maps an entity UV point to screen pixels (injected so this is
// camera-free and unit-testable).
std::vector<uint32_t> BoxSelect(const std::vector<sketch::Entity>& entities,
                                glm::vec2 rectMin, glm::vec2 rectMax,
                                bool crossing,
                                const std::function<glm::vec2(glm::vec2)>& project);
```
- Sample each entity via `TessellateEntity` (Line = 2 pts, Circle/Arc = polyline;
  Point = its one point), project each to screen.
- **Window:** keep the entity iff *all* its projected points lie inside the rect.
- **Crossing:** keep iff *any* projected point is inside **or** any tessellated
  segment intersects a rect edge (segment-vs-rect, so a line spanning the box
  with both endpoints outside still counts). Construction geometry is eligible
  too (selection isn't a profile).
- Skip zero-point entities appropriately; dedupe ids.

### 4b. State + interaction (`AppState.h`, `SketchInteraction.cpp` Select branch)
- `SketchEditState`: `bool boxActive = false; glm::vec2 boxStart{0}, boxEnd{0};`
  (screen px).
- In the Select branch, split mouse-down by what's under it:
  - **on an entity** (`pick.hit`) → begin a drag (existing path).
  - **on empty** → `boxActive = true; boxStart = boxEnd = mouse;` (do *not*
    clear yet).
  - while `boxActive` and mouse held → `boxEnd = mouse`.
  - on release: if `|boxEnd-boxStart|` < ~4 px → treat as a plain click (clear
    selection unless Ctrl). Else build the rect, `crossing = boxEnd.x <
    boxStart.x` (right→left), call `BoxSelect` with the `SketchUVToScreen`
    projector; if not Ctrl clear first, then append the hits as whole-entity
    refs `{id, 0}`. `boxActive = false`.
- Escape cancels an active box (step 2, level 1).

### 4c. Overlay
Draw the box while dragging via `ImGui::GetForegroundDrawList()` (like
`DrawSnapGlyphs`): a filled + outlined rect; **window** (L→R) drawn solid/blue,
**crossing** (R→L) dashed/green — the CAD convention cue. Add a
`DrawSelectionBox(state)` called from the UI pass next to `DrawSnapGlyphs`.

## 5. Tests
- **`tests/test_sketch_selection.cpp`** (new, pure — register in
  `tests/CMakeLists.txt`): identity projector (UV=screen); a window rect that
  encloses two of three entities selects exactly those two; a crossing rect that
  merely touches a line (endpoints outside, segment through the box) selects it
  while the same rect in window mode does not; a point just outside is excluded.
- **GUI (`src/GuiTests.cpp`)**: `keys/tool_switch` (`l` sets the Line tool;
  `WantTextInput` guard: focus a field → `l` does nothing);
  `keys/constraint_on_selection` (select two lines, press `h` → both horizontal
  via the solver); `keys/context_routes` (`n` while editing a sketch adds
  Coincident to a two-point selection; `n` with no sketch active orients the
  camera instead — proves the context split); `select/box_selects_enclosed`
  (drag a box over two entities → both in `edit.selected`);
  `select/escape_cascade` (pending → tool → selection → leave, one press each).

## Risk & sequencing
- Low–medium. Fiddly bits: the pure crossing test (segment-vs-rect), the
  `WantTextInput`/modifier guard, and getting **context routing** right so a key
  like `n` hits Coincident while sketching but view-normal-to otherwise (the
  model-context handler must check `!sketchEdit.IsEditing()`, and the existing
  `z`/arrows block must not fight the sketch keys — they don't overlap).
- Scope note: the model-context keys (`f` fit, `n` normal-to, `Shift+1…7` views,
  `Shift+p` planes, `Shift+s/e`) touch `ViewportInput` + Camera + plane
  visibility, a bit beyond the sketcher — so ~2–2.5 days, not 1.5.
- Order: 1 → 2 (tiny) → 4 (box, the substance) → 3 (keys: sketch context first,
  then model context) → 5. Steps 1–2 and 4 are independent of the keymap; ship
  incrementally. Commit `feat: sketch select-default, escape cascade, box-select`
  then `feat: onshape-style keyboard shortcuts` (or one commit if small).

## Bookkeeping
- Update CONSTRAINTS_PLAN.md Phase M status on ship. Phase M was the roadmap's
  planned finale before N (UI redesign) was added; N naturally follows since it
  builds on these interaction primitives (Select-default, the tool-state model,
  the shared prompt area that supersedes `actionMsg`).
