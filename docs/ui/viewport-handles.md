# Viewport handles — one hover, three owners

> Status: **plan, then built 2026-08-20.** From a question with a feature
> request inside it: "we have arrows to drag things in a number of places — do
> they use a common interface, should they, and I would like hover
> highlighting so I get feedback on which arrow is selected."

## What exists

Four places have drag arrows in the user's mind; the code has **three**, and
one of those is not interactive at all:

| | owner | state | hit test | drawing |
|---|---|---|---|---|
| extrude / fillet / revolve | preview handles | `HandleDrag` | inline in `HandlePreviewHandleInput` | `BuildPreviewHandleOverlay` |
| assembly move, mate DOF, explode offset | gizmo | `GizmoDrag` | `GizmoHitTest` | `BuildGizmoOverlay` |
| section offset | *grafted onto the preview handle* — `HandleDrag::sectionId` | same | same |
| simulation force / pressure | force: `SimulationState::LoadDrag` (added 2026-08-20); pressure: none | inline in `HandleLoadDragInput` | `AppendSimulationBCGlyphs` |

So: the *math* is already shared (`ui/DragMath.h` — `RayAxisParam`,
`PointRayDist`, `RayPlane`, `RotateAngle`, `GizmoScale`; 10 uses in
`AssemblyActions.cpp`, 8 in `PreviewHandles.cpp`), and both overlay builders
append to the same line channel, drawn together in `UI.cpp`. The
*interaction* is duplicated, and the section handle riding on `HandleDrag`
with an extra id field is the visible strain.

The two hit tests are structurally identical, which is what makes this worth
unifying rather than a forced abstraction:

```
linear:  t = clamp(RayAxisParam(...)); PointRayDist(origin + dir*t, ray) < grab
rotate:  RayPlane(...); |length(hit - origin) - R| < grab
```

## What is shared, and what deliberately is not

**Shared: which handle is under the cursor, and how it is drawn.**

```cpp
struct ViewportHandle {
  uint64_t id;              // stable across frames; the hover key
  HandleKind kind;          // Linear (arrow) | Rotate (arc)
  glm::vec3 origin, dir;    // arrow base + direction, or arc centre + normal
  float length;             // arrow length, or arc radius
  glm::vec3 color;
};
int HitTestHandles(std::span<const ViewportHandle>, const Ray&, float grab);
```

Every owner emits its handles into `AppState::viewportHandles` once per frame,
in the same pass that already computes their geometry for drawing. Hover is
then one hit test against that list, and one `AppState::hoveredHandle` id.

**Not shared: the drag payloads.** A preview handle drags one scalar feature
parameter (mutate + regenerate, no undo entry). The gizmo drags an occurrence
transform, or a mate DOF, or an explode offset (snapshot at grab, solve on
release). A section drags view state and touches no document at all. Merging
those into one struct produces the `sectionId` graft at three times the size —
a union of everything with flags deciding which half is live. Each owner keeps
its own grab bookkeeping; the shared layer answers *which* handle, not *what
happens to it*.

That also gives the section handle a first-class emitter instead of a special
case, and leaves an obvious place for simulation loads when they become
draggable.

## Hover

`AppState::hoveredHandle` (0 = none), recomputed each frame in `ViewportInput`
from the mouse ray **when no drag is active** — today the hit test only runs
on click, which is exactly why nothing can know what is under the pointer
before you press.

Drawing: the hovered handle draws in a brighter tint of its own colour, and
the actively-dragged one brighter still. Colour per handle, not one accent for
all, so the gizmo's X/Y/Z stay distinguishable while hovered.

Deliberately not in v1: cursor changes (an ImGui cursor is set per frame from
several places and would need its own arbitration) and hover tooltips showing
the value (the drag readout already covers the value; a tooltip competes with
the pick pipeline's own hover preview).

## What shipped, and what it did not touch

The click hit tests were deliberately **left where they are**. Each owner still
runs its own — they compute grab-time bookkeeping the shared test knows
nothing about (`startParam` for a preview handle, a snapshot + mate DOF for
the gizmo) — and swapping them would have risked the drag behaviour for no
gain the request asked for. The shared list answers "which handle", which is
all hover needs; the two tests use the same geometry and the same grab radius,
so the highlight cannot promise a grab the click would miss.

Simulation loads shipped one round later and were exactly the promised size:
`AppendSimulationBCGlyphs` publishes one handle per force load and draws it
through `HandleTint`, and `HandleLoadDragInput` is the grab payload — about
50 lines. No shared code changed to accommodate a fourth owner, which is the
evidence the seam was cut in the right place.

The drag maps arrow *length* to magnitude relatively: `magnitude = start * (1
+ (t - startParam) / length)`, clamped at zero. Not absolute (the arrow's
length is a view-scaled glyph, not a Newton ruler, so "drag to 2x the drawn
length" is the only reading with a meaning), and not additive (an absolute
step would behave differently on a 5 N and a 5 kN load). One
`ModifyStudyCommand` is pushed on release from a snapshot taken at grab, so
the whole drag is one undo entry, and the load's `magnitudeExpr` is cleared —
dragging a value that a variable drives has to break that link or the next
regeneration silently reverts the drag.

Pressure loads deliberately stay undraggable: their glyph is a *field* of
short arrows over the face rather than one arrow with a tip, so there is no
shaft to grab. They would need a different affordance, not this one.

## Drawing shared too (2026-09-02)

The plan above says "shared: which handle is under the cursor, and how it is
drawn", and only the first half was built. Each owner kept drawing its own
arrow: the gizmo and the preview handle as 1 px `LINE_LIST` segments in the
no-depth "gizmo" channel (two copies of the same arrow code, rings of 40 and
48 chords), the FEM force arrow at 2.2 px through the thick primitive
pipeline with a 27° head. Three drawers, three looks, and the single-pixel
ones read as cheap next to everything else the viewport draws thick.

Now `ui/ViewportHandleDraw.h` is the ONE drawer. An owner publishes its
`ViewportHandle` and draws nothing; `DrawViewportHandles` walks
`state.viewportHandles` once per frame — after the last owner has published —
applies `HandleTint` against `hoveredHandle` and `ActiveHandleId(state)`, and
emits every arrow and ring as thick anti-aliased primitives (`kHandleHalfPx`,
4 px) into the on-top primitive channel, so a handle stays visible and
grabbable through the geometry it moves. The consequence worth having: what
highlights is what is drawn *by construction* — both read the same record —
instead of by three drawers agreeing with a hit test.

What stays with the owners are decorations, not handles: the section's plane
outline and the preview arrow's base tick (drawn in the handle's tint through
the same thick helper, into the same channel), and the floating value readout.
The mass-properties CoG cross and the FEM peak marker are not handles and keep
the 1 px no-depth line channel. `BuildGizmoOverlay(state, lines)` became
`PublishGizmoHandles(state)`; `BuildPreviewHandleOverlay` takes the prim
vector its decorations go into. The tests that used to read grab points off
the emitted line segments read the published handle instead, which is now the
same thing, and `gizmo_hover_names_the_axis` compares the drawer's output.

Not `VkPipelineRasterizationStateCreateInfo::lineWidth`: widths above 1 need
the optional `wideLines` feature (lavapipe and several drivers lack it) and
are not anti-aliased. The primitive pipeline already existed for exactly this.

**Found by the first 4 px stroke: every thick line in the app was a twisted
rope.** `sketch_prim.vert` took the normal from posThis→posOther, which runs
the other way for a segment's far-end vertices, and `AppendPrimLine` undid
that by negating `corner.x` at that end — correct for the POSITION, but the
fragment shader's cross-section coordinate (`vLocal.x`, the anti-aliasing
distance) kept the raw sign, so `+1` at one end met `-1` at the other on the
same edge and the opaque core of the quad was an hourglass. At the 3 px the
sketch lines wear the pinch is a pixel and reads as slight unevenness; at 4 px
a ring of 48 chords reads as rope, and at 12 px (the diagnostic width) each
chord is a plain X. The shader's normal is now canonical per segment (flipped
when the screen direction points -x), the helper passes the true side at both
ends, and every consumer — sketch entities, selection, measure, CAM backplot,
FEM glyphs, handles — gets full-width strokes from the same fix. The
`--offscreen` harness could not have shown this: it draws no `SketchRenderer`
output at all (not even the origin triad), so the screenshots came from
`headless-preview.sh --gui` under Xvfb.

## Found immediately after, by the hover it added

The section arrow **highlighted on an assembly tab and then did nothing when
dragged**. The handle was published and drawn there (it is the one handle an
assembly has), so hover worked — but the block that routes clicks to handles
lived only in the part path, and the assembly branch ends with
`HandleSelectionInput(...); return;`. The click never reached it and fell
through to the picker.

That asymmetry existed before this change and was invisible: with no hover,
"the arrow does nothing on assemblies" and "there is no arrow on assemblies"
look identical. Routing is now one `RouteHandleInput` called from both tab
kinds, and the new test drags the arrow on an assembly — verified to fail
with the routing removed.

## Verification

- The existing drag GUI tests are the safety net: `section` (the arrow takes
  the click), the gizmo drags, the preview-handle drags. All pass unchanged.
- `handles/hover_highlights_the_arrow`: with a pending fillet (a box has no
  handle — only features with a headline scalar do), moving onto the shaft
  sets `hoveredHandle` to that handle's id, and moving away clears it. A
  highlight that never turns off is worse than none.
- `handles/gizmo_hover_names_the_axis`: all three axes, each hovered in turn,
  each reporting its OWN id — not whichever is first. It then builds the
  overlay with and without a hover and compares the vertex colours, so the
  assertion reaches the drawn output rather than stopping at the state: the
  geometry is identical, only the hovered axis's lines brighten, and X stays
  red rather than becoming one shared accent.
