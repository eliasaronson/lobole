# Assemblies A2.5 — interactive manipulation (drag gizmo) — design

> Status: **signed off 2026-07-13** — all four phases approved (Elias: "all four
> phases"). A2 shipped numeric-first (occurrences placed
> by panel fields); A3 shipped mates. A2.5 is the interactive layer both wanted
> but deferred so it could be built once: a **3D drag gizmo** to move/rotate an
> occurrence directly, the **OutlineRenderer per-mesh model** that makes a live
> drag cheap (and removes A2's vertex-bake stopgap), **drag-within-mate** (spin
> a revolute by dragging), and **connector inference snap points** (the deferred
> A3 UX item). They share one pick-handle → mouse-delta → live-transform layer.

## Why now, and why together

The roadmap grouped these because they share infrastructure:

- A **live-transform update path** — mutate a render instance's model matrix each
  frame with no geometry re-upload or resync. `MeshRenderer::SetInstanceTransform`
  already does this; the OutlineRenderer does **not** (it renders every mesh at
  identity and A2 bakes the transform into outline vertices as a stopgap).
- A **screen-space interactive-handle** layer — ray-test a manipulator, capture a
  drag, map mouse motion to a constrained transform. The free-occurrence gizmo,
  drag-within-mate, and inference hover-markers are three consumers of it.

Build that once. The pieces phase cleanly (below).

## The seams (from code exploration)

| Piece | State today |
| --- | --- |
| `MeshRenderer` per-instance model | **Done** — `mat4 model` push constant, `SetInstanceTransform` mutates in place, no resync (`MeshRenderer.cpp:287-318`). The reference pattern. |
| `OutlineRenderer` per-mesh model | **Missing** — `OutlineMesh.transform` field + `SetMeshTransform` exist but feed only the CPU silhouette calc; both pipelines have **no push constant**, `model` is a shared per-frame UBO forced to `identity` (`OutlineRenderer.cpp:1048-1087`). Shaders read `ubo.model` (binding 0). |
| A2 stopgap | `SyncAssemblyRenderers` bakes the occurrence transform into outline vertices (`AppActions.cpp:1090-1104`). |
| Viewport drag state | `ViewportDrag {None,Orbit,Pan,Zoom,Roll}` — camera-nav only, middle mouse (`AppState.h:43`, `ViewportInput.cpp:77-113`). Left-click → pick/select. Gizmo needs a new drag state + an early-return consumer like `HandleSketchViewport`/`HandleEdgePickInput`. |
| Ray / camera | `FaceSelector::ScreenToWorldRay(x,y,w,h,cam)` + `Ray{origin,dir}`; `Camera::GetPosition/GetForward/GetDistance/GetView/Proj`. **No screen-space-size helper** (and no `GetFOV` getter) — a gap to fill for a constant-size gizmo. |
| Overlay lines | `SketchRenderer::SetOverlayLines(LineVertex3D)` — depth-**tested** (can be occluded), biased forward. A gizmo that must stay grabbable needs a **no-depth-test** line channel. |
| Occurrence→instance map | `state.documentMeshes` (`DocumentRenderMesh{meshId,outlineId,bodyIndex,occurrenceId}`); iterate by `occurrenceId` (as `SelectOccurrence` does). `FaceSelector::UpdateMeshTransform` re-places pick geometry cheaply. |

## Phase 1 — OutlineRenderer per-mesh model (foundation)

Mirror `MeshRenderer`: add a `mat4 model` **push constant** to the outline and
wireframe pipeline layouts, move `model` out of the shared UBO into the push
constant, and `vkCmdPushConstants(mesh->transform)` before each `vkCmdDrawIndexed`
in `RenderOutlines`/`RenderWireframes`. Update `wireframe.vert`/`outline.vert` to
read `model` from a `layout(push_constant)` block (view/proj stay in the UBO).
Then `SyncAssemblyRenderers` uploads **untransformed** outline vertices and calls
`SetMeshTransform(outlineId, OccurrenceMatrix(occ))` — the vertex-bake loop is
deleted. (`OutlineMesh.transform` still also feeds the silhouette calc, unchanged.)

**Wins on its own**, independent of the gizmo: removes the per-occurrence vertex
copy, and a transform change becomes a matrix update instead of a re-upload. This
is the self-contained, lowest-risk piece and lands first.

## Phase 2 — the translate/rotate gizmo (free occurrence)

**Geometry.** A standard combined manipulator anchored at the selected
occurrence's origin, **constant screen size** (computed from camera distance,
since there's no pixel helper): three axis **arrows** (translate along X/Y/Z),
three **rotation arcs** (rotate about X/Y/Z). Plane-translate handles (drag in a
plane) are a nice-to-have, deferrable within the phase. Drawn through a **new
no-depth-test line channel** (`SetGizmoLines` on SketchRenderer, its own pipeline
with `depthTestEnable=FALSE`) so handles are always visible and grabbable.

**Interaction.** A new `HandleGizmoInput` early-return consumer in
`ViewportInput`, running before the pick/orbit blocks:

- On left-click, ray-test the handles (cylinder/torus proximity to the ray).
- On grab: `SnapshotAssembly` once; record the axis + the grab point.
- Per frame while dragging: intersect the ray with the axis line (translate) or
  the rotation plane (rotate), compute the delta, apply to a **scratch** copy of
  the occurrence's `position`/`orientation`, and push it live via
  `SetInstanceTransform` + `SetMeshTransform` (phase 1) for every `documentMeshes`
  entry of the occurrence — **no `BumpVersion`, no resync**.
- On release: write the final transform into the occurrence, then
  `SolveActiveAssembly` (one `BumpVersion` → one resync; mates re-solve;
  `FaceSelector` geometry rebuilt) — a single clean undo step.

The gizmo shows only for a selected, **non-`fixed`** occurrence. A fixed
occurrence shows a grounded indicator, not handles.

## Phase 3 — drag-within-mate

The same drag layer, constrained to a mated occurrence's **remaining DOF**: a
revolute shows only its rotation arc (drag → writes `mate.rotation`), a slider
only its axis arrow (→ `mate.offset`), cylindrical both, planar the in-plane
arrows + spin, ball the rotation arcs. Instead of writing the occurrence
transform, the drag writes the **mate's DOF field** and calls `SolveActiveAssembly`
— so the mechanism moves and everything downstream re-solves. This reuses phase
2's handle geometry + ray-mapping wholesale; only the "what the delta writes"
target differs (occurrence transform vs mate DOF).

## Phase 4 — connector inference snap points

The deferred A3 UX item (see a3-design "why connector frames"). When picking a
face for a mate connector (the A3 mate tool), hovering also offers **snap
points** derived from nearby sub-geometry: a circular edge's **center**, a
straight edge's **midpoint**, a **vertex/corner**, the **face center** (today's
only option). Picking one sets the connector frame's origin there instead of the
face centroid — one click lands a well-placed frame (the thing that makes the
Onshape model pleasant). Additive on disk: the anchor gains a **qualifier**
alongside its `ElementRef` (e.g. `{face:{…}, snap:"edge-center", edge:"<name>"}`),
defaulting to the centroid when absent. Reuses the hover-marker overlay + the
edge/vertex picking (`FaceSelector::PickEdge` exists) from the same interactive
layer. Format bump when it lands (v26).

### Phase 4b — snap-marker polish — shipped (2026-07-24)

While the mate tool awaits a connector, it now resolves the same transient
candidate that a click would resolve and draws a screen-stable cross at that
connector origin. The toolbar retains its picking instruction and additionally
reports `Snap: hole center`, `Snap: edge midpoint`, or `Snap: face center`.
Hole centers and edge midpoints both persist through the existing named
`snapEdge`; the nearest candidate wins, so moving to the middle of a face still
selects its centroid. Midpoints use a cyan marker while hole centers remain
amber. The marker clears on a miss, leaving the viewport, or when the live mate
preview begins; the click resolves a fresh pick and never trusts hover state.
This remains compatible with v26 `snap_edge` serialization and needs no format
bump.

Covered by GUI test `assembly/mate_hover_marker`: it verifies miss clearing,
face-center versus midpoint ranking, midpoint-reference persistence on click,
and preview clearing. `FaceSnapPoints exposes box-edge midpoint snaps`
covers the kernel edge enumeration (renamed with the perf fix below, which
folded three whole-shape walks into one). Vertex/corner snapping was
deferred here "because `ElementMap` does not yet provide persistent vertex
names" — see 4c, where that turns out to be the wrong reason.

### Phase 4c — corner snapping — SHIPPED 2026-08-25

**That deferral reason above is wrong, and it cost a year.** It assumes a
corner needs a persistent VERTEX name. It does not: a corner of the picked
face is where two of that face's boundary edges meet, and both of those are
already named. The anchor's existing `snapEdge` is itself a derived point
with a stable identity taken from a named element — the origin is "that
edge's centre, else its midpoint". A corner is the same trick with one more
named ref, not a toponaming change.

The same conclusion was reached independently from the other side in
[../inspect/measure-v2.md](../inspect/measure-v2.md), whose M5 deferred
vertex picking for the identical stated reason. Two workstreams, one
mis-stated blocker.

**Measured on the real kernel** (throwaway spike, 2026-08-25), which is what
makes this a design rather than a hope:

| shape | face | bounding edges | named | pairs sharing 1 vertex | sharing 2+ |
|---|---|---|---|---|---|
| box | any | 4 | 4 | **4** | 0 |
| cylinder | the wall | 4 | 4 | 4 | **1** |
| cylinder | a cap | 1 | 1 | 0 | 0 |

Two things fall out. On prismatic geometry — which is what anyone is
snapping a mate to — every corner is unambiguous and every bounding edge
carries a name. And the ambiguous case is NOT hypothetical: two boundary
edges sharing two vertices appears on the very first curved face tried. So
the refusal rule is needed on day one, and a design that had not measured
this would have shipped a corner that silently picks one of two points.

**Design.** `ConnectorAnchor` gains a second `ElementRef snapEdge2`. When
both are set, the frame origin is the vertex the two edges share; when they
share none or two, the anchor falls back to `snapEdge`'s own characteristic
point rather than guessing which corner was meant.

1. **Kernel** — `CADShape::FaceCorners(faceIndex, out)`, beside
   `FaceSnapPoints`: every vertex of the face where exactly two of its
   bounding edges meet, with that vertex and both edge indices in the
   `ElementMap::edgeNames` order. Plus `CornerPoint(edgeA, edgeB, out)` for
   resolving a stored anchor. ~55 lines. **Built as one VERTEX->EDGE
   ancestor pass, not pairwise** — O(edges), which is also what keeps the
   +0.16 ms below true.
2. **Inference** — `ResolveAssemblyFacePick`'s candidate loop already ranks
   every snap point by distance to the hit; corners join it as more
   candidates. ~25 lines, no new ranking.
3. **Schema** — `snap_edge2` in SerializeAssembly (2 sites), **kVersion 64 →
   65**, one golden. The fixed cost of any format change here.
4. **Frame** — `ConnectorLocalFrame` resolves the pair to the shared vertex.
   ~15 lines.
5. **UI** — a `SnapType::Corner`, its marker colour, and `Snap: corner` in
   the toolbar, all beside the two that exist. ~10 lines.

Tests, all present: a box's face offers 4 corners and every reported pair
resolves back to one point; a cylinder's cap offers none (one closed edge
meets nothing) and its wall's ambiguous pair is dropped;
`assembly/mate_hover_marker` hovers a corner and checks the frame lands on
it within 1e-3; the v65 golden round-trips both edges.

**Two things the build found.** The measure side arms `Connector` to reach
the corner snap, and the Connector candidate is pushed FIRST — so offering
a non-corner snap there would have put a candidate the measure tool cannot
use at the head of the list and silently eaten the click. `Candidates()`
therefore drops the non-corner snaps when measure is the armed context, and
the mate tool still sees all four. And a corner carries its first edge in
`ref`, so the measure readout initially took the ANGLE between two corners'
edges and re-anchored the dimension onto them: two diagonal corners of a
2 mm face read as 2 mm instead of 2.83 mm. A point has no direction; both
the angle and the re-anchoring are now suppressed for it.

### Cost — and a live defect this measurement found

Corners are free. Built as ONE `TopExp::MapShapesAndAncestors` VERTEX→EDGE
pass over the face (a vertex with exactly two incident face edges is a
corner named by that pair), they add **+0.16 ms/frame at their worst**, and
that worst is a plate with 400 holes.

The measurement found something else, though, and it is not hypothetical.
**The snap hover path is already quadratic and already too slow.**
`EdgeSnapPoint` calls `TopExp::MapShapes(m_shape, TopAbs_EDGE, …)` — mapping
every edge of the WHOLE BODY — on every call, and
`ResolveAssemblyFacePick` calls it once per boundary edge of the hovered
face. So hovering a drilled face costs O(face edges × body edges), every
frame, while the mate tool is armed:

| plate | body edges | drilled face's edges | hover today | corners add | map hoisted |
|---|---|---|---|---|---|
| 36 holes | 120 | 40 | 0.68 ms/frame | +0.011 | — |
| 144 holes | 444 | 148 | **10.0 ms/frame** | +0.058 | **0.16 ms** |
| 400 holes | 1212 | 404 | **88 ms/frame** | +0.162 | **0.62 ms** |

At 144 holes that is most of a 60 fps budget spent hovering; at 400 it is
about 11 fps, which reads as the app hanging while you look for a mate
point. A perforated plate is not an exotic part.

**Fixed 2026-08-25, before any corner work.** `CADShape::FaceSnapPoints`
replaces all three calls with one walk over one map, and
`ResolveAssemblyFacePick` consumes it directly: **0.097 ms on the 144-hole
face that cost 10 ms**, a ~100x improvement, verified against sabotage
(reverting to the per-edge map puts it back to 8.6 ms and fails the test).
`FaceCircleCenters` and `FaceEdgeIndices` are gone — subsumed, and their
tests re-pointed. `EdgeSnapPoint` stays for the solve, which resolves ONE
edge per mate and is not a hot path.

Guarded by `FaceSnapPoints stays linear on a drilled plate`
(test_cadkernel.cpp), which builds the 144-hole plate precisely because a
box cannot catch this: the cost is the PRODUCT of the two edge counts.

Same shape as the rest of this round's findings — the slow path is invisible
on the small test shapes and obvious on a real one.

## Decisions for sign-off

1. **OutlineRenderer gets a real per-mesh `model` push constant** (mirroring
   MeshRenderer), and the A2 outline-vertex-bake stopgap is deleted. Phase 1,
   self-contained.
2. **Gizmo = constant-screen-size translate arrows + rotate arcs** at the
   occurrence origin, drawn in a **new no-depth-test line channel** so handles
   stay grabbable through geometry. Plane-translate handles optional within the
   phase.
3. **Live-drag lifecycle:** snapshot once at grab → per-frame cheap
   `SetInstanceTransform`/`SetMeshTransform` (no resync) → `SolveActiveAssembly`
   on release (one undo step, one resync, mates re-solve). No per-frame
   `BumpVersion`.
4. **Gizmo shows only for a selected non-`fixed` occurrence.** Fixed → grounded
   indicator, no handles.
5. **Phasing + scope:** ship **phase 1 (per-mesh model) + phase 2 (free-occurrence
   gizmo)** as the A2.5 core now; **phase 3 (drag-within-mate)** and **phase 4
   (connector inference)** are clearly-scoped follow-ons that reuse the same
   layer (they can land right after, or when the mate/placement workflows call
   for them).

## Phasing summary

1. **Per-mesh model** (OutlineRenderer push constant; delete the bake). Verify:
   assemblies still render + outline correct; a `SetMeshTransform` moves the
   outline live.
2. **Free-occurrence gizmo** (no-depth line channel, `HandleGizmoInput`,
   translate+rotate, live drag → snapshot/solve). Verify visually via dev-watch
   (grab, drag, release, undo).
3. **Drag-within-mate.** 4. **Connector inference snaps.**
