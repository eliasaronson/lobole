# The hover pick tested every triangle in the scene, every frame

Measured 2026-09-07 on the OM615 (76 parts, 241 occurrences) and the
air-cooled engine (33 leaf occurrences), RTX 4060 Laptop, windowed under Xvfb.

## Where it came from

It was second place in the profile that found the bounding-box memo
(docs/performance/shape-bounds-memo.md) and became first place once that
landed — which is what happens to second place when first place leaves, not a
regression.

```
OM615, assembly tab, cursor over the model
  41.85%  FaceSelector::PickFace          (28.94% of it RayTriangleIntersect)
  41.24%  ui::(anonymous)::Candidates
  41.25%  ui::HandleSelectionInput
```

`ui` was 13.26 ms a frame, about 5.9 ms of it the pick.

## Three things were wrong

**It ran unconditionally.** `HandleSelectionInput` calls `Candidates` every
frame the cursor is over the viewport and not dragging — moved or not.

**It was a linear scan with no bound.** `PickFace` looped every triangle of
every mesh, and a MISS cost exactly as much as a HIT: moving the cursor off
the model changed nothing measurable, because the ray still tested all 241
occurrences.

**It ran two or three times per frame.** `Candidates` picks a face for the
occurrence branch, and again for the face/body branch, and `PickEdge` picks
one internally as its occlusion reference. A part tab arms Edge|Face|Body, so
it paid for two full face sweeps plus an edge sweep.

## What changed

**A per-mesh world AABB, tested first.** It is accumulated in `ExtractFaces`,
which is already the one function that runs when either the geometry or the
placement changes — so the box cannot disagree with what a pick will test, and
`UpdateMeshTransform` cannot leave a stale one behind. It covers triangles AND
edge polylines: a mesh can carry edges and no triangles, and bounding only the
triangles gave those an empty box, which a prune reads as "nothing here". That
made every edge of every such mesh unpickable — caught by the existing
`PickEdge down a bore` test, which is exactly what it is for.

**The ray goes into mesh space; the vertices stay put.** The loop used to
apply three `mat4 x vec4` per triangle, about as expensive as the intersection
test they fed. The local direction is deliberately NOT renormalised, so the
parameter `t` remains the one along the WORLD ray: distances stay comparable
between meshes of different scale, and the hit point comes straight back from
the world ray. The hit normal returns through the inverse-transpose times
`sign(det)` — `cross(Ma, Mb) = det(M) M^-T cross(a, b)`, so the sign is what
keeps a MIRRORED placement answering exactly as the world-space vertices did.

**One face pick per frame**, shared by the occurrence branch, the face/body
branch and `PickEdge`'s occluder (a new overload takes it).

## Result

| | `ui` before | after |
|---|---|---|
| OM615 assembly tab | 13.26 ms | **7.36 ms** |
| OM615 part tab | 13.16 ms | **7.73 ms** |

`PickFace` falls from 41.85% of process CPU to 28.6%, and because the frame's
CPU shrank with it that is ~5.9 ms to ~2.4 ms — 2.5x. `PickFace`'s own
non-intersection cost (the per-triangle matrix work) falls from 12.90% to
4.03%.

## What is still there

`RayTriangleIntersect` is 24.5% of the profile. The AABB prune removes the
meshes the ray comes nowhere near, but a ray down a dense assembly still
crosses many boxes, and inside a mesh there is no acceleration at all — every
triangle of every crossed part gets tested. **A per-mesh BVH is the next
step**, and unlike the prune it needs a build and an invalidation story;
`collide::Mesh` in core/MeshCollide.cpp already has a triangle BVH (BuildNode
/ Descend) built for shape-vs-shape, which is the obvious thing to reuse
rather than write a second one.

Not done, deliberately: caching the pick on (mouse position, camera, geometry
version) so a stationary cursor costs nothing. It would have removed the whole
cost in the measurements above — which are taken with a still cursor — but a
stale hover highlight is a worse failure than the milliseconds it saves, and
the case that matters is a cursor that IS moving.

## A quirk this pinned rather than fixed

A MIRRORED placement reports an INWARD face normal. That is pre-existing:
the hit normal has always been the cross product of the placed triangle's
edges, and a mirror reverses winding. `ExtractFaces` builds `Face::normal` the
same way, so `GetFaceNormal` agrees and the selector is self-consistent. The
`sign(det)` factor above exists to reproduce it exactly.

It is worth its own look — sketch-on-face and `orient_to_face` read that
normal — but a performance change is the wrong place to alter what a mirrored
occurrence reports. `tests/test_faceselector.cpp` pins the current answer so
the next person meets it as a decision rather than as a surprise.
