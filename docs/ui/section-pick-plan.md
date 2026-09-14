# Picking through a section view

## The problem

A section is a fragment-level discard. `SectionPlanes()` (ui/ViewActions.cpp)
hands up to four world planes to every pass — mesh, outline, SSAO, shadow,
ray tracer — and a fragment dies where `dot(n, p) > w` (render/ClipPlanes.h).

`FaceSelector` never learned about them. `PickFace` returns the nearest
triangle along the ray and `PickEdge` the nearest polyline, both over the
whole mesh. So the material a section cuts away is still fully pickable:
hovering the revealed interior highlights the invisible near half, and a click
selects it. On an assembly that is the common case — you section to reach a
part inside, and every hover lands on the housing you just cut open.

The sketch slice (`sketchSlice`, same plane array) has the same hole, and it
is worse there: the slice exists precisely so you can reach geometry buried
inside material.

## What a pick should answer

**Nothing on the removed side is pickable.** The kept region along a ray is
the intersection of the enabled half-spaces, which on a line is one interval
`[tEnter, tExit]`. A hit outside it is a hit on something nobody can see.

**The cut face is the body it cuts.** Where the ray crosses the plane *inside*
a solid, what the viewer sees there is that body's section cap. The pick must
land on that body — otherwise sectioning a hollow part hands back whatever
happens to be behind it.

**A cap names no element.** The cap is not a B-Rep face; it has no entry in
the element map and no stable name. So a cap hit resolves to a Body or an
Occurrence and refuses to be a Face — it is never silently substituted by the
face the ray entered through, which would put a sketch on a cylinder wall the
user believes is the flat cut.

## The shape of the fix

### 1. The picker carries the planes

`FaceSelector::SetClipPlanes(std::span<const glm::vec4>)` keeps the enabled
ones. `SyncDocumentRenderers` pushes them every frame, ahead of its own
version gate and its cap-rebuild gate — that function is already the one place
that registers meshes with the picker, so the picker cannot disagree with what
was drawn, and it runs in the windowed loop, the offscreen loop and the RPC
paths alike. The planes must be pushed even while a widget or the section
arrow is being dragged, when cap geometry deliberately is not rebuilt.

`FaceSelector.h` stays free of `render/` — it takes a span of planes, the way
`Measure::SectionCaps` takes a vector of them.

### 2. The clip test is two comparisons

`PickFace` computes `[tEnter, tExit]` once per pick from the planes and the
ray. A triangle hit is visible exactly when `tEnter <= t <= tExit`, so the
per-triangle cost is two float compares and no dot products — this loop is
~2.4 ms a frame on the OM615 (docs/performance/hover-pick.md) and must not
grow. An empty interval means everything is cut away: an early miss.

`PickEdge` tests the closest point on the segment against the planes directly
(the point is near the ray, not on it), placed after the pick-radius reject so
it only runs for candidates already under the cursor.

### 3. The cap is inferred, not built

No cap geometry enters the picker. While scanning a mesh's triangles,
`PickFace` remembers the **last boundary crossed before `tEnter`** — the
largest `t < tEnter` — and whether that triangle was front-facing. Entering
last means the ray was inside that solid where it met the plane, so the
surface visible there is that body's cap: a hit at `tEnter`, on that mesh,
normal = the plane's, `cap = true`.

Last-crossing-orientation rather than crossing parity on purpose: a ray
through the shared edge of two coplanar-adjacent faces reports two hits at the
same `t`, which flips a parity count and does not disturb an orientation test.

It needs a closed solid, so `AddMesh` takes a `solid` flag, cached on
`BodyTessellation` from `CADShape::SolidCount()` (once per shape, not per
sync). An imported open shell gets no inferred cap rather than a phantom one
floating in front of the part.

### 4. A cap hit is a body, never an element

`RayHit` gains `bool cap`. Four sites turn a `RayHit` into a named element and
each refuses a cap: the Face branch of `Candidates` (ui/SelectionPipeline.cpp
— Occurrence and Body branches keep it, which is the reported bug),
`PickAssemblyFace` (mate connectors), the sketch-plane pick
(ui/SketchInteraction.cpp), and the Project tool (ui/SketchDimensions.cpp).

### 5. A cap lights with the body it belongs to

A cap is a separate instance in `sectionCapMeshes`, so an occurrence's
highlight and hover skipped it — and on a sectioned part the cut face is most
of what you can see of it, which made a correct highlight read as landing on
something else. Caps therefore record whose they are (`occurrenceId`, and the
body index for a part tab), and the selection/hover projection lights them
with the rest of that occurrence. On a part tab a cap pick resolves to a Body,
which had no hover visual at all: it now lights the body and its caps, the way
an assembly lights a whole occurrence. Body hover fires only when nothing
nearer names an element, which is exactly the cap case.

## Files

- `src/cad/FaceSelector.{h,cpp}` — planes, the interval, cap inference, the
  `solid` flag, `RayHit::cap`.
- `src/ui/RendererSync.cpp` — push the planes per frame; pass `solid` at both
  `AddMesh` sites.
- `src/ui/AppActionsInternal.h`, `src/ui/TessellationCache.cpp` —
  `BodyTessellation::solid`.
- `src/ui/SelectionPipeline.cpp`, `src/ui/MateActions.cpp`,
  `src/ui/SketchInteraction.cpp`, `src/ui/SketchDimensions.cpp` — refuse a
  cap where an element name is required.
- `src/ui/AppState.h`, `src/ui/ViewActions.{h,cpp}`,
  `src/ui/SelectionPipeline.cpp` — `SectionCapMesh` identity, and the caps
  lit with their body.

## Verification

- `tests/test_faceselector.cpp`: a cube sectioned in half — a pick from the
  cut side returns the cap at the plane (`cap == true`, the body's mesh, the
  distance the plane is at) and not the near wall; from the other side the
  plane is behind the geometry and nothing changes; a pick aimed through the
  removed corner where no material meets the plane returns the far wall, not a
  cap; every plane disabled reproduces today's answers exactly; an edge behind
  the cut is not pickable while one on the kept side still is; a mesh with
  `solid = false` gets no cap.
- A GUI case in the `section` category (`a_cut_picks_what_you_can_see`): two
  occurrences stacked along the view, clicked through the viewport — the near
  one before the cut, the far one after it, at the same pixel. It fails
  against the unwired build, which is the point of writing it.
- `tools/test-fast.sh`, then the full headless suite and
  `./tools/gui-tests-xvfb.sh`.
- `tests/render_pixel_smoke.py` is untouched by this — nothing here changes
  what is drawn.

## Not in this round

**The cap is not itself pickable geometry.** The hit is inferred from the
solid the ray is inside, which answers for the body and for the plane's own
surface, and that is all a cap can mean. Registering the cap meshes with the
picker would buy face-level truth ON the cut — picking one cap of several
overlapping bodies at the same point, or naming a cut face as an element —
and costs an identity table read by every consumer of a pick. Nothing wants
that yet.
