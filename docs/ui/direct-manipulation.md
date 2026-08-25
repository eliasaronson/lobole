# Direct manipulation in previews

Status: **SHIPPED 2026-07-19** (`9479582`) — preview-scoped handles are wired
for blind/symmetric extrudes, fillets/chamfers, and revolves. The dialog and
handle edit the same pending-feature parameter, then acceptance creates one
undoable `AddFeatureCommand`.

## Follow-up: input correctness and coverage

The initial feature is complete. The former P0 input regression was fixed:
`HandlePreviewHandleInput` now returns `false` before touching `HandleDrag`
when hit testing misses, so `ViewportInput` falls through to selection and
armed tools. `fillet_body_edge_via_click` asserts that contract for a live
fillet preview.

Remaining coverage: add a revolve-handle drag GUI case.

Scope: the plan.md backlog item "Direct manipulation in previews" — drag an
arrow/handle in the viewport to set a feature parameter (Onshape/SolidWorks
style), as the natural U3 follow-on. The dialog and the handle edit the same
parameter: the handle is a second widget over the same feature field, not a
second code path.

## Historical pre-implementation inventory

At planning time, every piece of machinery existed but nothing was wired
together:

- **Preview flow** (`PreviewFeature.cpp`): a pending feature lives in the
  document (`state.pendingFeatureId`), regenerates and renders live;
  `DrawPreviewDialog` calls `DrawFeatureParamWidgets(state, f, idx,
  /*commit=*/false)` — while previewing, edits mutate + `Regenerate()` with
  **no undo entry**; accept re-adds through one `AddFeatureCommand`. So for a
  preview-scoped handle, undo correctness is already free.
- **Dialog side** (`FeatureEditor.cpp`): `ExprDim` reads `&ext->distance`
  every frame — any external mutation shows in the field next redraw. That is
  the whole "same param" guarantee; no sync protocol needed. `HandleEdit`
  already regenerates per-frame during a field drag, so a handle drag costs
  the same as dragging the dialog's `DragScalar`.
- **Drag math** (A2.5 gizmo, `AssemblyActions.cpp` ~400-740): `RayAxisParam`
  (ray→axis closest coordinate), `RayPlane`, `RotateAngle`, `PointRayDist`,
  `GizmoScale` (camera-distance sizing), `GizmoHitTest` (grab = 0.14×scale),
  plus the grab/`startParam`/delta drag lifecycle in `HandleGizmoInput` and
  `AppState::GizmoDrag`. All in an anonymous namespace — needs hoisting.
- **Input routing** (`ViewportInput.cpp`): the gizmo gets first shot at clicks
  *only on the assembly branch* (line ~250); a live drag continues
  off-viewport; a hit consumes the click so it never reaches
  `HandleSelectionInput`. The part-tab path has no such hook — misses fall
  through to the pipeline, so armed slots (U3) and edge picking keep working
  untouched.
- **Rendering**: `UI.cpp` (~384) builds a dedicated `gizmo` line channel →
  `SketchRenderer::SetGizmoLines`, drawn depth-test-off on top
  (`m_gizmoPipeline`). On part tabs it's currently always empty
  (`BuildGizmoOverlay` early-outs without `state.assembly`) — free to use.
- **Anchor data**: `SketchFeature::plane` (`SketchPlane::GetNormal/
  PlaneToWorld`), `ExtrudeFeature::regions` (U2 outer-loop entity ids),
  `FilletFeature::edges` + `DocumentRenderMesh::edgePolylines` (edgeIndex
  order), `ResolveAxis` for `RevolveFeature::axis`, `ui::WorldToScreen`
  (SketchInteraction.h) for tests.

## Design

1. **v1 feature set**: extrude `distance` (Blind + Symmetric), fillet/chamfer
   `radius`, revolve `angleDeg`. Shell `thickness` and extrude `distance2` /
   `thinThickness` deferred — same mechanism, just more anchors. One handle
   per preview; no handle when the param is expression-driven
   (`distanceExpr` etc. non-empty) — a drag would silently clobber the
   expression, so the handle simply doesn't appear (the field shows "fx").
2. **v1 lifetime = preview only**: handles exist iff `HasPendingFeature`.
   Re-editing a committed feature keeps today's Properties-panel flow;
   extending handles there is v2 (see 8).
3. **Shared math**: hoist `RayAxisParam`, `RayPlane`, `RotateAngle`, `PerpTo`,
   `PointRayDist`, `GizmoScale` from `AssemblyActions.cpp` into a small
   `src/ui/DragMath.h` used by both the gizmo and the new code. No behavior
   change to A2.5.
4. **New module `src/ui/PreviewHandles.cpp/.h`** (mirrors the gizmo pair):
   - `BuildPreviewHandleOverlay(state, lines)` — appends the handle geometry
     for the pending feature; called in `UI.cpp` next to `BuildGizmoOverlay`,
     same channel.
   - `HandlePreviewHandleInput(state, ray, clicked, down, released)` — the
     gizmo's grab/drag/release contract; called from `ViewportInput.cpp` on
     the **part-tab** path, before `HandleSelectionInput`, only when a
     preview is pending. Active drags continue off-viewport.
   - `AppState::HandleDrag { active, featureId, axisOrigin, axisDir,
     startValue, startParam }` next to `GizmoDrag`.
5. **Anchors & handle shapes** (all sized by `GizmoScale`, accent-colored,
   distinct from the XYZ gizmo palette):
   - *Extrude*: arrow along `plane.GetNormal()` from the profile centroid —
     centroid of the selected regions' outer loops (via SketchProfile loop
     points), falling back to the sketch entities' UV bbox center. Arrow
     base sits on the current end cap (`centroid + N·distance`; Symmetric:
     `+N·distance/2`) so the tip tracks the geometry while dragging.
   - *Fillet*: sphere-ish cross marker at the midpoint of the first
     referenced edge's polyline (`edgePolylines[edgeIndex]`, mesh found by
     `bodyIndex` — the SyncSelectionHighlights convention). Drag axis: from
     the body bbox center through the edge midpoint, projected ⟂ to the
     edge tangent — "pull away = bigger".
   - *Revolve*: rotate arc around the resolved axis (`ResolveAxis` needs a
     `RegenContext`; add a small UI-side resolver for the three global axes +
     sketch-line + datum-axis cases, reading the already-regenerated datum's
     origin/dir), radius from the profile centroid.
6. **Pixels → parameter**: on grab store `startValue` and
   `startParam = RayAxisParam(ray, axisOrigin, axisDir)` (revolve:
   `RotateAngle`); per-frame the param becomes
   `startValue + (t - startParam)` mapped through the end-mode inverse
   (Symmetric: ×2; revolve: radians→degrees), clamped to the field minimum
   (0.01, `DragDim::kMin`). Mutate the feature field directly +
   `doc.Regenerate()` — byte-identical to what the dialog's drag does.
7. **Feedback & snapping**: the dialog field updates live for free (see
   above). Additionally a floating readout ("12.500") renders at the handle
   tip via the ImGui foreground draw list, like the measure readout. Values
   round to display precision (0.001 / 0.1°) by default; holding **Ctrl**
   snaps to coarse increments (0.5 length units, 5°). Esc during a drag
   aborts the drag — restore `startValue`, regenerate, consume the Esc so it
   does *not* cancel the preview (ordering in ViewportInput's Esc ladder).
8. **Undo**: v1 needs nothing — preview mutations aren't on the stack, and
   accept commits the whole feature as one `AddFeatureCommand`. v2
   (committed-feature handles) must follow the `HandleEdit` recipe: JSON
   baseline at grab, one `PushApplied(ModifyFeatureCommand)` on release —
   never Execute (the ReplaceFeature dangling-pointer class from the
   dims work), never per-frame commands.

## Open questions

- Snap polarity: Ctrl-to-snap (proposed) vs snap-by-default with
  Shift-for-fine (Onshape). Cheap to flip; want a decision at sign-off.
- Fillet drag axis on concave/interior edges can point oddly; fallback to
  the camera-right projection if the bbox heuristic degenerates?
- TwoSided extrude: second arrow for `distance2` in v1, or wait? (Proposed:
  wait — it's mechanical once the first arrow exists.)
- Does the fillet handle stay useful when regen fails at large radii (OCC
  blend rejection)? Proposal: keep the last good mesh on screen (already the
  failure behavior) and let the readout keep tracking the dragged value.

## Tests

Precedent: `GuiTests.cpp` already drives raw viewport drags with
`MouseMoveToPos`/`MouseDown`/`MouseUp` ("dims/label_drag",
"drag/move_whole_line") and projects world→screen via `ui::SketchUVToScreen`
/ `ui::WorldToScreen`.

- GUI: sketch a rectangle, start an extrude preview, project the handle tip
  (`WorldToScreen(centroid + N·distance)`), drag along the projected axis;
  assert `ext->distance` increased ≈ the world delta, the preview is still
  pending, then accept → `FeatureCount()==1` and one undo step.
- GUI: Esc mid-drag restores the grab value and the preview survives; a
  click away from the handle changes nothing (pipeline fall-through).
- GUI: fillet preview on a box edge — drag, assert `radius` changed; a
  missed-handle ray returns false without activating a drag, preserving the
  blend-picker fall-through contract.
- Screenshot-verified on dev-watch (not asserted): arrow/arc aesthetics,
  floating readout, snap feel, no z-fighting with the model.

## Implementation record and remaining order

Completed in `9479582`: shared `DragMath`, preview-handle module and wiring,
extrude arrow, fillet/chamfer marker, revolve arc, Esc/snap behavior, and GUI
coverage for extrude and fillet.

Remaining order:

1. ~~Add a revolve drag GUI case.~~ **DONE 2026-08-25** —
   `handles/revolve_arc_drag_sets_angle`. It reads its grab point off the
   drawn handle rather than recomputing the arc (`PerpTo` and the axis
   resolution are file-local in PreviewHandles.cpp, and a test that
   re-derives them can drift from what is on screen), and it drags NEGATIVE
   because the default angle is 360 — the clamp ceiling, so a positive drag
   is swallowed and would pass against a handle that did nothing.
2. Consider the deferred two-sided-extrude and shell-thickness handles only
   when users need them.

No format change or golden bump is required; the assembly gizmo remains
unchanged beyond the shared math extraction.
