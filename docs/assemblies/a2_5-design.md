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
|---|---|
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
