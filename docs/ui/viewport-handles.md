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
| simulation force / pressure | **none** — `ArrowGlyph` in `StudyPanel.cpp` draws them, nothing picks them | | |

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

Simulation loads still draw through `ArrowGlyph` and are still not draggable.
Making them draggable is now a matter of emitting handles and writing one
grab payload, which is the point of the seam.

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
