# Renderer instancing — design (for sign-off)

> Status: design for Elias's sign-off. This is the last remaining gate to
> assemblies A2 (occurrences) — see [../assemblies/plan.md](../assemblies/plan.md)
> prerequisites. It reshapes the renderer's core data model (`Mesh` →
> asset + instance), touches picking and outlines, and has one real
> architecture choice in it, so it wants a decision before code.

## What this is for

Today one body = one `Mesh` = one dedicated vertex/index buffer pair = one
draw (`MeshRenderer.h:34-58`, one `vkCmdDrawIndexed(..., instanceCount=1, ...)`
per mesh at `MeshRenderer.cpp:295`). An assembly with 40 occurrences of one
screw would tessellate and upload the screw 40 times. The assemblies plan
calls the fix — one tessellation per part, shared across N occurrences with
per-instance transform + material — "the big memory/perf lever (10–100× on
repeated parts)".

Precision on the lever: it is **tessellation + upload sharing**, not draw-call
batching. CAD-scale assemblies (hundreds of occurrences) are nowhere near
draw-call-bound in a pipeline this simple. That observation drives the main
recommendation below.

## Current state (verified)

- **Per-mesh data** (`MeshRenderer.h:34-58`): CPU vertex/index copies, one
  device-local buffer pair per mesh, `modelMatrix`, `materialParams`
  (metallic/roughness/specular/reflectance), `highlight` (uvec4 vertex
  ranges: selected `[x,y)`, hovered `[z,w)`).
- **Per-draw plumbing**: everything per-mesh rides a 112-byte push constant
  `{mat4 model, uvec4 highlight, vec4 material, vec4 clipPlane}`
  (`MeshRenderer.cpp:268-273`, `mesh.vert:19-28`). One shared UBO holds
  camera + lights only. No per-mesh descriptor sets.
- **Albedo is baked into vertex colors**: `SetMeshMaterial`
  (`MeshRenderer.cpp:190-206`) rewrites every vertex's `color` to the
  material's base color; `CADMeshConverter` stamps the body palette color the
  same way. This is the one per-body channel that lives *inside* the shared
  tessellation — the direct blocker for sharing it.
- **`clipPlane` is global** (same value for every mesh, pushed each frame
  from `AppState.sectionPlane` at `main.cpp:1188-1192`) yet travels in the
  per-mesh push constant. Free space if reclaimed.
- **Highlight uses `gl_VertexIndex` ranges** (`mesh.vert:57-60`) — valid
  per-draw, so it survives anything that keeps one draw per instance; it
  breaks under `instanceCount > 1` unless made per-instance.
- **Picking is a CPU raycast** (`FaceSelector.cpp:175-229`): one `MeshData`
  per mesh with its own CPU geometry copy and a baked transform; brute-force
  every mesh × every triangle; hit → `meshId` → `state.documentMeshes` →
  `bodyIndex`.
- **Outlines**: `OutlineRenderer` mirrors the one-mesh-one-upload model with
  its own buffers and per-mesh transform.
- **Sync is full-rebuild** (`ui::SyncDocumentRenderers`,
  `AppActions.cpp:683-747`): any document version bump → remove every mesh,
  re-tessellate every body, re-upload. Part-tab switches force this via
  `syncedDocumentVersion = UINT64_MAX` (`AppActions.cpp:518`), so inactive
  parts have zero GPU residency and switching pays full re-tessellation.
- **Prior art in-repo**: the ray tracer already builds one TLAS instance per
  body with `instanceCustomIndex` → per-body attributes
  (`RayTracingRenderer.cpp:1185-1284`) — the same shape as this design,
  though it pre-transforms geometry and shares no BLAS yet.

## Design

### Data model: split `Mesh` into asset and instance

```cpp
// MeshRenderer-owned
struct MeshAsset {            // one per distinct tessellation (per part body)
    std::vector<Vertex3D> vertices;   // CPU copy (picking, updates)
    std::vector<uint32_t> indices;
    VkBuffer vertexBuffer, indexBuffer;   // uploaded ONCE
    ...memory...
    uint32_t refCount;        // instances referencing this asset
};
struct MeshInstance {         // one per occurrence
    AssetId   asset;
    glm::mat4 transform;
    glm::vec4 baseColor;      // albedo — moves OUT of vertex color
    glm::vec4 materialParams; // metallic, roughness, specular, reflectance
    glm::uvec4 highlight;     // per-instance selected/hovered vertex ranges
    bool visible = true;
};
```

`Vertex3D` drops its `color` attribute (position + normal only, 36 → 24
bytes); albedo becomes per-instance state. This is what makes N occurrences
with different materials share one buffer, and it deletes the
`SetMeshMaterial` rewrite-every-vertex path outright. (If per-face color
ever returns, it comes back as a face-indexed lookup, not baked vertices.)

### Draw path: one draw per instance, per-instance push constants

Render loop: for each visible instance → push
`{model, highlight, material, baseColor}` → bind the asset's buffers (skip
when unchanged from previous instance; sort instances by asset) →
`vkCmdDrawIndexed(instanceCount=1)`.

Push-constant budget: `clipPlane` moves to the shared UBO where it belongs
(it's global), freeing its 16 bytes for `baseColor`. The block stays
64+16+16+16 = **112 bytes**, comfortably under the 128-byte guaranteed
minimum. UBO note: append `vec4 clipPlane` after the existing vec4s —
`UniformBufferObject` is std140-clean (all mat4/vec4) and must stay that
way; the `float`-then-`vec4` padding trap is documented at
`OutlineRenderer.h:93-102`.

**Explicitly deferred: true GPU instancing** (`instanceCount = N` with an
SSBO or instance-rate attributes indexed by `gl_InstanceIndex`). It saves
draw calls we don't need saved, and it breaks the `gl_VertexIndex` highlight
scheme, per-instance visibility, and per-instance push data — three
redesigns for a bottleneck we don't have. The asset/instance split is the
memory lever and is forward-compatible: if a profile of a real 100+-part
assembly ever shows draw submission as the cost, batching becomes a
contained change inside `MeshRenderer::Render`. (Same philosophy as the
assemblies plan's allocator rule: profile before cleverness.)

### Picking: shared geometry, ray in instance space

`FaceSelector` splits the same way: one geometry record per asset
(triangles, `triangleFaces`, face ranges, edge polylines — extracted once),
plus a list of `{instanceId, transform, inverseTransform}`. `PickFace`
transforms the ray into each instance's local space with the cached inverse,
intersects against the shared triangles, and compares hit distances in world
space. Hits gain an instance dimension: `RayHit{instanceId, faceIndex, ...}`.
This replaces today's one-CPU-copy-per-mesh model and makes
`UpdateMeshTransform`'s re-extract-everything path (`FaceSelector.cpp:46`)
a pure matrix update.

### Outlines

`OutlineRenderer` gets the identical asset/instance split (shared edge
buffers, per-instance transform). Its transform already flows through a UBO;
the per-instance value moves to a push constant to match the mesh path.

### Sync layer and ids

`SyncDocumentRenderers` keeps its shape (version-gated full resync) but the
bookkeeping changes: `DocumentRenderMesh` (`AppState.h:30-37`) maps
`bodyIndex` → `{assetId, instanceId, outlineInstanceId}`. For a single part
document nothing else changes — each body is one asset with one instance,
and every current call site (`ViewportInput.cpp:29-48` highlight wiring,
`RemoteServer.cpp:1000-1003` RPC picking) migrates from `meshId` to
`instanceId` mechanically.

When A2 lands, the assembly document's sync creates one asset per referenced
part body and one instance per occurrence; a pick returns
`instanceId` → occurrence. That mapping is A2's code, but this design is what
makes it a loop over occurrences instead of a renderer rewrite.

**Out of scope, enabled for later**: keeping inactive part tabs' assets
resident (kill the full re-tessellate on tab switch, `AppActions.cpp:518`)
and lazy/LRU part loading — both fall out naturally once assets are
refcounted, both deferred to keep this change reviewable. Same for the RT
path: the TLAS can eventually share one BLAS per asset with real instance
transforms instead of pre-transformed geometry (`RayTracingRenderer.cpp:
1186-1198`); nothing here precludes it.

### What this does NOT change

No document format change (pure renderer + picking refactor — no version
bump, no golden-file churn). No shader lighting changes. Pattern features
are unaffected: they fuse instances into real B-Rep bodies at model time
(`BodyMode.h:27-32`), which is a modeling semantic, not a rendering one —
patterned geometry stays one body, one asset.

## Phases

1. **MeshRenderer split** — asset/instance structs, refcounted asset
   destroy, per-instance draw loop, albedo → push constant, `clipPlane` →
   UBO, `Vertex3D` loses `color`. Single-part rendering is pixel-identical
   (verify via gui_smoke screenshots).
2. **FaceSelector + OutlineRenderer split** — instance-space picking,
   shared outline buffers; migrate `documentMeshes` bookkeeping and the
   highlight/RPC call sites.
3. **Tests** — a unit/GUI test that creates one asset with two instances at
   different transforms + materials, verifies picking resolves each
   instance, and screenshots. (A2 will then exercise this for real.)

Phase 1 and 2 are one PR each; both are behavior-preserving for single-part
documents, which is what makes this safe to land before A2 exists.

## Decisions that need you

1. **Per-instance draws vs. true GPU instancing now.** — **DECIDED
   (2026-07-12): per-instance draws** with shared buffers; batch later only
   if a profile of a real large assembly shows command submission as the
   cost. Rationale: push constants live in the command stream (zero CPU/GPU
   sync — the sync burden is on the batched side's per-instance buffer),
   and per-draw record cost only matters at tens of thousands of draws,
   far past CAD-assembly scale.
2. **Dropping `Vertex3D.color`.** — **DECIDED (2026-07-12): remove.**
   Albedo moves to per-instance state; vertices go to 24 bytes; the vertex
   input layout, converter, and `SetMeshMaterial` are updated rather than
   carrying a dead attribute.
3. **Scope check.** — **DECIDED (2026-07-12): defer.** Inactive-tab GPU
   residency stays out of scope; natural follow-up once assets are
   refcounted.
