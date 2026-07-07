# Phase C — Selectable entities: picking, points, hover, visible lines

Detailed plan for Phase C of [CONSTRAINTS_PLAN.md](CONSTRAINTS_PLAN.md). Pulled
ahead of the topology work because **constraints are unusable today**: only
entity endpoints are pickable and lines render 1 px un-anti-aliased, so there's
nothing to reliably click. Reference UX: Onshape/SolidWorks draw a dot at every
vertex and highlight whatever is under the cursor.

Four parts — (1) body picking, (2) point/vertex dots, (3) hover highlight, (4)
anti-aliased thick lines — three of which share one new renderer path.

## Current state (verified)

- `PickSketchPoint` (`SketchInteraction.cpp`) tests only endpoints, 12 px.
- Selection feedback is a fixed 0.05-world-unit cross drawn per selected point
  in `AppActions.cpp` (`AppendSketchLines` also draws points as a 0.05 cross) —
  both scale with zoom instead of staying a constant screen size.
- `SketchRenderer` has two pipelines: `m_linePipeline` (LINE_LIST, `lineWidth
  = 1.0`, `VK_SAMPLE_COUNT_1_BIT`) and `m_quadPipeline` (TRIANGLE_LIST for plane
  quads), sharing one `{view, proj}` UBO. Shaders `sketch.vert`/`sketch.frag`;
  the vertex is `Vertex3D {position, normal, color}` and **`normal` is unused**
  by the sketch shader.
- `SketchEditState` has `selected` and drag fields but no hover state.

## Part 1 — Body picking

- New `SketchPick PickSketchEntity(state, sketch, mousePos, tolPx)` (or extend
  `PickSketchPoint`): first run the existing endpoint test; if nothing is within
  the endpoint tolerance, test the cursor against each entity's **body** —
  point-to-segment distance for lines, `abs(dist(center) - radius)` for circle
  circumference — and return the nearest within `tolPx` as a whole-entity ref
  `{id, 0}`. Endpoints always win ties so coincident/distance/drag are
  unaffected.
- Wire into the Select-tool click (`HandleSketchViewport`) — it already calls
  `PickSketchPoint`; switch to the entity-aware pick. A body hit selects the
  whole entity (enables Horizontal/Parallel/… in the toolbar); it does **not**
  begin an endpoint drag (dragging a body is Phase F).

## Part 2 — Hover highlight

- Add `sketch::PointRef hoveredRef; bool hasHover;` to `SketchEditState`.
- Each frame in the Select tool (even with no mouse button down), run the pick
  at the current cursor and store the result in `hoveredRef`/`hasHover`. Cheap —
  the sketch has few entities.
- The renderer draws the hovered entity/vertex brighter and slightly larger (see
  Part 4). Clears when the cursor leaves the viewport or an entity.

## Part 3 — Point / vertex visualization

- Emit a **dot** at every line endpoint, circle center, and standalone point —
  the endpoint pick targets, drawn at constant screen size (Onshape-style).
- Replace the 0.05-world cross in `AppendSketchLines` (points) and the selection
  marker in `AppActions.cpp` with dots fed to the new primitive path (Part 4).
- Styling by role: normal = small dim dot; hovered = enlarged/brighter; selected
  = filled bright dot. Circle centers and standalone points always get a dot;
  line endpoints get one too (shared endpoints overlap into one visually).

## Part 4 — Anti-aliased thick lines + the shared primitive pipeline

**Decision (from CONSTRAINTS_PLAN.md): quad expansion in the vertex shader, not
`wideLines`.** One new pipeline serves thick lines, point dots, and
hover/selection emphasis.

- **UBO** gains `vec2 viewportSize` (pixels), so the VS can convert a pixel
  offset to NDC.
- **New vertex format** for this pipeline (a dedicated struct, rather than
  overloading `Vertex3D`, though `normal` is free if we want to avoid a new
  binding): per-vertex `posThis` (world), `posOther` (world; == posThis for a
  point), `corner` (which of the 4 quad corners / offset sign), `halfPx` (half
  width or dot radius in pixels), `color`.
- **Vertex shader**: project `posThis` and `posOther` to clip → NDC → screen;
  for a line, offset `posThis` along the screen-space perpendicular of
  (posOther−posThis) by `corner.side * halfPx`; for a point, offset along
  `corner` (the 4 corners of a `halfPx` square). Pass a local UV to the
  fragment for SDF.
- **Fragment shader**: SDF alpha — distance-to-centerline for lines (feathered
  over ~1 px), distance-to-center for dots (round). Gives AA without MSAA. Alpha
  blends over the existing translucent pipeline (blend is already enabled).
- **Emphasis**: hover/selection just vary `color` and `halfPx` per vertex when
  building the buffer — no extra pipeline.
- Keep the old LINE_LIST pipeline for the rubber-band overlay (or migrate it
  too; the overlay is thin and transient, so it can stay 1 px initially).
- `SketchRenderer` reuses its `DynamicBuffer` upload + descriptor infra; add a
  `SetPrimitives(std::vector<PrimVertex>)` alongside `SetStaticLines`, and a
  draw call for the new pipeline. Build the primitive buffer in `AppActions.cpp`
  from the active sketch each frame (it already rebuilds `AppendSketchLines`).

## Build path / ordering

1. Picking (`PickSketchEntity` + Select-tool wiring) + `hoveredRef` state —
   testable via GUI tests before any rendering change.
2. Renderer: UBO viewport size, new pipeline + shaders, `SetPrimitives`.
3. Move lines → expanded quads; add point dots; add hover/selection styling.
4. Tests.

Compile shaders as part of the existing SPIR-V build step (see how
`sketch.vert` → `sketch_vert.spv` is produced in CMake).

## Tests

- GUI (`GuiTests.cpp`, ImGui test engine):
  - `pick/select_line_body`: draw a line, click its midpoint (not an endpoint) →
    the line is selected and Horizontal applies and solves.
  - `pick/prefers_endpoint`: two lines sharing a near-vertex; click the vertex →
    an endpoint ref (`which` set), not a body ref.
  - `pick/hover_tracks_cursor`: move the mouse over a line → `hoveredRef`
    resolves to it; move to empty space → `hasHover` false.
- The rendering itself isn't unit-tested; verify via `/run` screenshot (lines
  visibly thicker/smoother, dots at vertices, hover brightens).

## Verification

- `cmake --build build && ctest` green (new GUI pick tests included).
- `/run` the app: draw a few lines and a circle; confirm vertex dots, hover
  highlight, clickable line bodies, and that selecting a line body then pressing
  the Horizontal button levels it. Compare crispness against the pre-change 1 px
  lines.

Commit as `feat: selectable sketch entities with point dots and hover`.
