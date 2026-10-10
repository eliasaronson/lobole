# Viewport handles and direct manipulation

Arrows and arcs you drag in the viewport. Four owners publish them, one hit
test says which one is under the cursor, and one drawer draws them all.

| owner | what it drags | grab payload | on release |
|---|---|---|---|
| preview handles (`src/ui/PreviewHandles.{h,cpp}`) | the pending feature's scalars: extrude distance (both of a two-sided one) and twist, fillet/chamfer radius, revolve angle | `AppState::handleDrag` | nothing: preview edits are not on the undo stack; Accept commits the feature as one step |
| section (same module) | the active section's offset — view state, no document | `handleDrag` | nothing |
| gizmo (`src/ui/GizmoInput.cpp`) | an occurrence's transform, a mate DOF, an exploded-view offset, a CAM job part's placement | `AppState::gizmoDrag` | one snapshot per drag, then solve |
| simulation force load (`StudyPanel.cpp`, `HandleLoadDragInput`) | the load's magnitude | `SimulationState::loadDrag` | one `ModifyStudyCommand` from a snapshot taken at grab |

## The shared layer

`ViewportHandle` (`src/ui/ViewportHandle.h`) is `{id, kind (Linear | Rotate),
origin, dir, length, color}` — an arrow's base and direction, or an arc's
centre and plane normal, with its length or radius. Every owner publishes its
handles into `AppState::viewportHandles` once per frame, in the pass that
already computes their geometry (`PublishGizmoHandles`,
`BuildPreviewHandleOverlay`, the simulation glyphs).

- **Hover.** `AppState::hoveredHandle` (0 = none) is recomputed every frame
  from the mouse ray while no drag is running, by `HitTestHandles` against
  that list: a linear handle at its closest ray-axis point clamped to the
  shaft, an arc at its radius in its own plane. It uses the same geometry and
  grab radius as the owners' click tests, so a highlight cannot promise a
  grab the click would miss.
- **Drawing.** `DrawViewportHandles` (`src/ui/ViewportHandleDraw.h`) walks
  `viewportHandles` once, after every owner has published, applies
  `HandleTint` against `hoveredHandle` and `ActiveHandleId(state)` (hovered
  brighter, dragged brighter still, each handle in its own colour so the
  gizmo's X/Y/Z stay distinguishable), and emits every arrow and ring as thick
  anti-aliased primitives (`kHandleHalfPx` = 2, a 4 px stroke) into the
  on-top primitive channel, so a handle stays visible and grabbable through
  the geometry it moves. What highlights is what is drawn, by construction:
  both read the same record. Owners draw only decorations (the section's
  plane outline, the preview arrow's base tick, the floating value readout).
  Not `lineWidth`: widths above 1 need the optional `wideLines` feature and
  are not anti-aliased.
- **Not shared: the drag payloads.** Each owner keeps its own grab
  bookkeeping and its own click test, because what a drag does differs
  (mutate and regenerate; snapshot and solve; move view state). The shared
  layer answers *which* handle, not *what happens to it*.

**Routing.** `RouteHandleInput` (`ViewportInput.cpp`) is called from both
the part path and the assembly path — the section arrow is the one handle an
assembly has — before the selection pipeline. A click that misses every
handle returns `false` and falls through to selection and armed tools, so a
live fillet preview can still collect edges. A drag in progress continues
off the viewport.

## Preview handles

One handle per draggable scalar of the pending preview, and only while the
parameter is a plain number: an expression-driven field (`distanceExpr`,
`distance2Expr`, `twistExpr`, `radiusExpr`, `angleExpr`) gets no handle,
since a drag would silently overwrite the formula. `ComputeHandleSpecs`
returns the list, and each handle's id is
`HandleId(HandleOwner::Feature, param)`, where `param` (`HandleParam`) says
which scalar it drags. `AppState::handleDrag.param` records it at the grab,
so two handles on one preview hover, highlight and drag apart. A click goes
through the same `HitTestHandles` as hover.

- **Extrude** (Blind, Symmetric, Two-sided): an arrow along the extrude
  direction from the profile's centre, based on the current end cap so it
  tracks the geometry (it follows `reversed`). A symmetric extrude's drag
  maps 2:1, its cap moving half the distance. A two-sided one has a second
  arrow, from the far cap pointing back, which drags `distance2`.
- **Extrude twist** (Blind and Symmetric, once the twist is not zero): a
  `HandleKind::Rotate` arc about the twist axis (the sketch normal through
  the sketch origin) at the end cap, wide enough to clear the profile. It
  snaps like any angle; a plain symmetric twist maps 2:1, as its cap turns
  half of it.
- **Fillet / chamfer**: at the midpoint of the first referenced edge, pulled
  away from the body.
- **Revolve**: an arc about the resolved axis.

The handle and the tool window edit the same field, so the field updates live
with no sync protocol. A length is clamped at the field minimum (0.01).
Escape during a drag restores the grab value and is consumed, so it does not
also cancel the preview. A readout floats at the handle being dragged.

## Snapping: one polarity

**Snap is the default, and Ctrl frees it, everywhere a drag snaps at all.**
`src/ui/DragMath.h`:

- `SnapValue(v, step, capture, snap)` is a **capture window**, not a
  quantizer: a value within `capture` of a multiple of `step` sticks to it,
  and everything between stays reachable (37° is as reachable as 45°), which
  is what lets snapping be the default without taking a value away.
- `SnapAngleDeg` / `SnapAngleRad`: multiples of 45° with a 5° capture — one
  round set at every scale and in every unit.
- `SnapLength`: multiples of 0.5 document units with a 0.1 capture. A literal,
  and flagged as one: 0.5 is round for a 3 mm fillet and arbitrary for a 2 m
  beam.

Users: the preview handles (angle or length), the CAM placement spin, a free
occurrence rotate (which snaps the **increment**, because a free occurrence's
starting orientation is arbitrary), and a drag within a mate's rotational DOF
(which snaps the **absolute** value, because the mate's own frame is the zero
— a revolute joint lands on 90° from its seat). A ball joint snaps the turn.
A primitive's ghost, dragged in its reference's frame, snaps to geometry
instead of round numbers — the face's centre and outline
([tool-sessions.md](tool-sessions.md#primitives)). Drags that do not snap (assembly translation, the section offset) still do
not: the rule is about polarity, not coverage.

## Mates: flip and rotate

The mate tool window offers **Flip** (which side: reverses B's Z against A's)
and, for every type but Ball, **Rotate 90°** (`###mate_rotate90`, which way
up: a quarter turn about the connector axis, wrapped into [0, 360) so four
clicks return to the start) plus an **Angle (deg)** field. Both edit
`Mate::rotation`, which is composed for every mate type
(`T(dx, dy, offset) · Rz(rotation) · R_ball · Flip`) and always serialized, so
a fastened mate's twist solves and round-trips. The inspector shows the angle
for a fastened mate too, so a twist set in the tool window can be read and
undone later. Ball is excluded because its twist folds into its ball
rotation. The prompt reads "Accept, Flip, Rotate, or Cancel". Over RPC,
`add_mate` / `set_mate` take `rotation` / `rotation_deg`.

## Simulation load drag

`AppendSimulationBCGlyphs` publishes one handle per force load. Dragging maps
arrow length to magnitude **relatively**:
`magnitude = start * (1 + (t - startParam) / length)`, clamped at zero — the
arrow is a view-scaled glyph, not a ruler, and an additive step would behave
differently on a 5 N and a 5 kN load. Dragging clears the load's
`magnitudeExpr` (a dragged value is a number, not a formula, and otherwise the
next regeneration would revert it). Pressure loads are a field of short arrows
with no shaft to grab and are not draggable.

## Tests

- `handles/extrude_twist_field_and_arc` (the Twist field, then the arc
  beside the arrow: the hover names the arc, the drag snaps to 90° and
  leaves the distance alone), `handles/two_sided_extrude_has_two_arrows`
  (the back arrow drags `distance2`).
- `handles/hover_highlights_the_arrow` (hover sets and clears
  `hoveredHandle`), `handles/gizmo_hover_names_the_axis` (each axis reports
  its own id, and only the hovered axis's drawn colour changes),
  `handles/extrude_arrow_drag_sets_distance`,
  `handles/revolve_arc_drag_sets_angle` (reads its grab point off the drawn
  handle, and drags negative because the default 360° is the clamp ceiling),
  `handles/a_preview_handle_snaps_unless_ctrl` (the same drag lands on 270
  plain and near 272 with Ctrl).
- `fillet/fillet_body_edge_via_click` (a missed handle falls through to edge
  picking).
- `section/offset_arrow_drags_the_cut`, `section/offset_arrow_drags_on_an_assembly`.
- `assembly/mate_tool_rotates_the_part` (the button moves geometry, and four
  clicks return to 0), `assembly/a_mate_rotate_snaps_to_the_common_angles`,
  `assembly/a_ring_drag_unwraps_past_half_a_turn`,
  `cam/a_placement_is_dragged_in_the_viewport`.
- `sim/*` force-arrow cases.

## Not built

Handles on a committed feature (they exist only while previewing; an edit
session would need a JSON baseline at grab and one modify command on release),
a shell-thickness handle, cursor
changes and hover tooltips on handles, a length snap derived from the part.
