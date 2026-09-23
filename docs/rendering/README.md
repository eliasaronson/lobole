# Rendering

How the viewport, screenshots and rendered stills are drawn. The raster
viewport is Vulkan 1.2 with 4x MSAA, PBR materials, image-based lighting, a
key-light shadow map and SSAO; stills come either from that raster path
("quick") or from a hardware path tracer.

Topic pages:

- [frame-pacing.md](frame-pacing.md) — frames in flight, the swapchain, the
  frame-time instrument and GPU timestamps, the idle throttle.
- [still-render.md](still-render.md) — `render_still`: the path tracer, quick
  mode, traced outlines, framing a still.
- [materials.md](materials.md) — materials, procedural surface patterns,
  appearance overrides, glass.
- [gpu-debugging.md](gpu-debugging.md) — validation layers versus a RenderDoc
  capture, and which answers which question.

## Where the code is

| what | where |
|---|---|
| Vulkan instance/device, swapchain, render pass, frame loop, construction and teardown of every renderer, `--offscreen` loop, screenshot and quick-render capture | `src/app/main.cpp` |
| shared Vulkan helpers (`vkutil::CreateBuffer`, `CreateReadbackBuffer`, `LoadShaderModule`, single-time commands, allocation count) | `src/render/VulkanCommon.{h,cpp}` |
| the per-frame contract | `src/render/SceneRenderer.h` |
| scene renderers | `src/render/{Mesh,Outline,Field,Stock,Underlay,Sketch}Renderer.*` |
| prepasses | `src/render/ShadowRenderer.*`, `src/render/SSAORenderer.*` |
| IBL | `src/render/IBL.*` (CPU precompute, in `designer_core`), `src/render/IBLTextures.*` |
| path tracer | `src/render/RayTracingRenderer.*`, `src/shaders/{raygen.rgen,closesthit.rchit,miss.rmiss,shadow.rmiss,section.rahit}` |
| traced-still outlines | `src/render/StillOutlines.*` (pure CPU, in `designer_core`) |
| readback, PNG, supersample downsample | `src/render/Screenshot.*` |
| timing | `src/render/FrameTimer.h`, `src/render/GpuTimer.*`, `src/app/FramePacer.*`, `src/ui/PerfHud.*` |
| document → renderer projection | `src/ui/RendererSync.cpp` (`SyncDocumentRenderers`, `SyncAssemblyRenderers`, `SyncRenderersToDocument`, the still jobs) |
| picking | `src/cad/FaceSelector.*`, `src/ui/SelectionPipeline.cpp` |
| shaders | `src/shaders/*.{vert,frag,glsl}`, compiled by glslc at build time |

There is no `VulkanContext` object: the device, queue and pools are globals in
`main.cpp`. Every buffer and image takes its own `vkAllocateMemory` (no
sub-allocator); the startup log prints the device's
`maxMemoryAllocationCount` beside the selected GPU because the Vulkan floor
for it is 4096.

## Device selection

`main.cpp` scores every physical device: discrete 2, integrated 1, other 0,
and -1 for a device that cannot present to the window (asked through a
throwaway surface, because `glfwGetPhysicalDevicePresentationSupport` can
answer yes for a device that then fails). The `gpu_device` preference
(Preferences, `set_settings`) names a device by substring; one that cannot
present is not used, and the app says so in the log and in Preferences. The
log line `Selected GPU: …` names the choice. Ray tracing needs a device that
exposes the RT extensions; without them the path tracer is unavailable and
quick stills still work.

## A frame

Per frame, in one command buffer:

1. **Shadow pass** (`ShadowRenderer`) — depth-only map from the key light.
2. **SSAO chain** (`SSAORenderer`) — depth+normal prepass, AO, blur, and in
   the half tier a depth-guided upsample.
3. **Main pass** — one render pass: MSAA colour + MSAA `D32_SFLOAT` depth,
   resolved into the swapchain image; the scene renderers in registry order,
   then ImGui in the same pass.

Steps 1-2 are `RecordScenePrepasses` (`main.cpp`), which the frame and every
screenshot path both call, so a capture has the same shadows and AO as the
viewport. The prepasses read the camera matrices and the shadow-caster list
out of the `MeshRenderer` (`SSAORenderer::RecordSSAOPasses(cb, frame,
MeshRenderer&)`) so they are guaranteed identical to what the main pass
uses.

Every framebuffer shares one depth image and one MSAA colour image. That is
correct because frames execute in order on one queue, **provided the render
pass's external dependency says so**: its `srcStageMask` includes
`LATE_FRAGMENT_TESTS` and its `srcAccessMask` names the colour and depth
attachment writes, so a frame waits for the previous frame's depth store.

Frames in flight, the swapchain image count and present mode are on
[frame-pacing.md](frame-pacing.md).

## Scene renderers

`SceneRenderer` (`render/SceneRenderer.h`) is the per-frame contract:
`SetViewport(x, y, w, h)`, `UpdateCamera(view, proj)`, `SetClipPlanes(...)`
(a no-op by default), `Render(cb, frameIndex)`, `Shutdown()`. Six classes
implement it: `MeshRenderer`, `FieldRenderer`, `StockRenderer`,
`OutlineRenderer`, `UnderlayRenderer`, `SketchRenderer`. The prepasses
(`ShadowRenderer`, `SSAORenderer`) and the path tracer are not scene
renderers.

`main.cpp` holds two lists and nothing else enumerates renderers:

- **`RegisterSceneRenderers()`** fills `g_Scene` (mirrored as
  `AppState::sceneRenderers`) **in draw order**: mesh, field, stock, outline,
  underlay, sketch. The order is content: the field and the stock stand in
  front of the mesh they replace, the outline goes over the solids it traces,
  reference images go under the sketch lines drawn on them. Every per-frame
  duty — viewport, camera, clip planes, and the three render blocks (the
  windowed frame, the windowed screenshot, the offscreen capture) — loops
  over this list.
- **`ShutdownSceneRenderers()`** resets every renderer before the device is
  destroyed. A renderer still alive at `CleanupVulkan` crashes at exit.

A renderer that one construction path (windowed or `--offscreen`) never
built is skipped with a `LOG_WARN` naming it ("was never constructed in this
mode"), so the two paths cannot silently disagree.

**Adding a renderer**: derive from `SceneRenderer`, construct it in both
paths, add it to both lists. `tests/test_conventions.cpp` fails if a class
deriving `: public SceneRenderer` is missing from either function. The gate
exists because a renderer missing from the viewport loop draws full-window at
the wrong scale and offset — invisible under `--offscreen`, where the 3D
viewport *is* the whole framebuffer — and one missing from teardown crashes
at exit.

The typed pointers in `AppState` (`meshRenderer`, `outlineRenderer`,
`fieldRenderer`, …) stay: the `ui/` layer uses them for content (`AddMesh`,
`SetInstanceGhost`, `SetStaticLines`, …). The registry owns the per-frame
contract only. `MeshRenderer` also has duties nothing else has (shadow
casters, AO/shadow-map rebinds, asset management).

The viewport, camera and clip planes are still *set* on each renderer before
`Render` rather than passed to it; [../plans/renderer-scene-view.md](../plans/renderer-scene-view.md)
is the plan to pass them at `Render` instead.

## Meshes: assets and instances

`MeshRenderer` splits geometry from placement:

- **`MeshAsset`** — one tessellation: CPU vertex/index copies and one
  device-local buffer pair, uploaded once, reference-counted.
- **`MeshInstance`** — transform, `baseColor`, `materialParams`
  (metallic, roughness, specular, reflectance), `procParams` (surface
  pattern), selection and hover vertex ranges, `visible`, `ghost`.

`Vertex3D` is position + normal, 24 bytes; albedo is per instance, never per
vertex. Sketch and overlay geometry use `LineVertex3D {position, color}`.

Drawing is one `vkCmdDrawIndexed(instanceCount = 1)` per instance, rebinding
buffers only when the asset changes between consecutive instances. Everything
per instance rides a push constant:

```
mat4 model | uvec4 highlight | vec4 material | vec4 baseColor | vec4 procMaterial
```

**128 bytes — exactly the Vulkan-guaranteed `maxPushConstantsSize`.** There
is no room for another field: a new per-instance value must fit an existing
`vec4` or move to a buffer (the path tracer's id-indexed material SSBO is the
model). The block is summed by hand at the pipeline layout in
`MeshRenderer.cpp` and mirrored in `mesh.vert`; nothing asserts the two agree.

Global state — camera, lights, the section clip planes, shadow and AO
parameters — lives in the shared `UniformBufferObject`, which is std140-clean
by construction (mat4/vec4 members only; the float-then-vec4 padding trap is
documented in `OutlineRenderer.h`).

Selection and hover highlight use `gl_VertexIndex` ranges in `mesh.vert`
(selected `[x, y)` with a tint in the top bits of `y`, hovered `[z, w)`);
further selected ranges are extra sub-draws. This is why true GPU instancing
(`instanceCount > 1`) is not used: it would break the highlight ranges,
per-instance visibility and the per-instance push constant, for a
draw-call cost CAD-scale scenes do not have.

**Ghosts** (in-context parts, transparent bodies, glass) draw in a second
pass through a pipeline with alpha blending and depth writes off, at the
instance's alpha. Ghosts are not sorted, and they are excluded from the
shadow-caster and SSAO-prepass sets.

### The part render cache

A document edit rebuilds the renderers' view of the document
(`SyncDocumentRenderers` for a part tab, `SyncAssemblyRenderers` for an
assembly: remove every instance, re-add). What makes that cheap is
`AppState::partRenderCache` (`src/ui/TessellationCache.cpp`): per part (slot
id, document version) it keeps the tessellation and both renderer assets,
holding its own reference through `MeshRenderer::RetainAsset` /
`OutlineRenderer::RetainAsset`, so the assets survive the instance teardown
at the head of every resync. One asset serves every occurrence of a part.
Eviction is LRU against `kPartRenderCacheBudgetBytes` (256 MB,
`AppActionsInternal.h`) and never evicts what the current sync draws. A
visibility toggle is a full resync through this cache.

## Outlines

`OutlineRenderer` draws edges over the solids. The default mode (set in
`main.cpp`) is WIREFRAME: static edges plus view-dependent silhouettes.

- **Static edges** — boundary edges and feature edges (adjacent-face normal
  dot below 0.5, i.e. a dihedral over 60°) — come from
  `BuildEdgeAdjacency` in `render/OutlineEdges.{h,cpp}`, computed once per
  mesh at load. Internal edges of zero-area triangles are excluded.
- **Silhouettes** are reclassified from the precomputed adjacency (two flat
  passes: a facing test per triangle with the camera taken into mesh-local
  space, then a comparison per edge) — but only once the camera has been
  still for `OutlineSettings::silhouetteSettleMs` (120 ms of wall time).
  While the camera moves only static edges draw. `liveSilhouettes`
  reclassifies every frame instead. A settled view shows static edges plus
  silhouettes in one buffer.
- **Captures call `OutlineRenderer::SettleSilhouettes()` first**, so a
  screenshot taken right after a camera move still has its silhouettes.
- Edge index buffers are per frame slot, host-visible and written in place —
  no staging copy, no device wait during interaction — and created on demand
  per slot (`EnsureEdgeBuffer`), so unused slots cost nothing.

`tests/test_outline_edges.cpp` covers the adjacency and classification; a
hidden `[.outline-perf]` case is the benchmark.

## Lighting

- **Materials**: metallic-roughness PBR in `mesh.frag`, from per-body
  `CADMaterials` properties ([materials.md](materials.md)).
- **Three analytic directional lights** — key, fill, rim — from
  `conv::kKeyLightDir` / `kFillLightDir` / `kRimLightDir` and their
  colour/intensity constants in `core/Conventions.h` (key intensity 1.8),
  written into the UBO by `MeshRenderer::Render`. They add at 0.5x on top of
  the IBL ambient: `ambient + 0.5 * (shadow * key + fill + rim)`. Only the key
  light is shadowed.
- **IBL**: split-sum image-based lighting from a bundled studio HDRI,
  `assets/hdri/brown_photostudio_02_2k.hdr` (Poly Haven, CC0; the name is
  `RuntimePaths::kEnvironmentHdriFile`). `IBLPrecompute` runs on the CPU,
  multithreaded, and caches its result beside the HDR as `<hdr>.iblcache`:
  a 64x32 irradiance map, a 128x64 prefiltered specular map with 5 mips, and
  a 128² BRDF LUT, uploaded by `IBLTextures` as `R16G16B16A16_SFLOAT` /
  `R16G16_SFLOAT`. `IBLTextures::EnvironmentView()` is the full-resolution
  environment, used only by the path tracer. The viewport background is a
  solid clear colour from the theme, not the environment.
- **Tone mapping**: ACES filmic (Narkowicz fit) in `mesh.frag`.
- **Anti-aliasing**: 4x MSAA over the whole main pass, resolved into the
  swapchain image; `GetMaxUsableSampleCount` falls back to 2x, then 1x, from
  the device's colour and depth sample-count limits. ImGui and the screenshot
  pass use the same count.

### Shadows

`ShadowRenderer` renders a `D32` depth map from the key light direction,
1024² or 2048² (default 2048). No cascades: the orthographic frustum is
refitted to the scene's bounds every frame, which suits compact CAD scenes.
The shadow vertex shader applies the section clip discard (cut-away material
casts nothing) and draws only the opaque caster set (`GetShadowCasters`:
visible, non-ghost instances). `mesh.frag` samples it with 3x3 PCF; the
normal-offset bias (1.5 texels) is applied in `mesh.vert`. The map is
independent of the window and survives swapchain rebuilds.

### SSAO

`SSAORenderer` records, before the main pass:

1. a non-MSAA **depth + view-space normal prepass** at full resolution, over
   the same opaque caster set as the shadow map, with the same clip discard;
2. **AO** into an `R8` target at half or full resolution: a 16-sample
   normal-oriented hemisphere, rotated by per-pixel interleaved gradient
   noise (a tiled noise pattern's blur residual reads as stripes on curved
   surfaces at half resolution; the old noise texture stays bound only to
   keep the descriptor layout);
3. a **depth-aware bilateral blur** that compares neighbours against the
   locally predicted depth plane (from depth derivatives), so sloped surfaces
   blur and silhouettes do not;
4. in the **half** tier, a **joint-bilateral upsample** (`ssao_upsample.frag`)
   into a full-resolution `R8` target, guided by the full-resolution prepass
   depth: each pixel blends the four surrounding half-res texels by bilinear
   weight times depth similarity, and takes the depth-nearest texel when all
   four lie across a depth edge. The upsample target exists in both tiers so
   the descriptor set stays valid across a tier change.

`AOView()` is the upsample target in half mode and the blur target in full
mode; `mesh.frag` samples it by normalized UV and multiplies it into the
ambient/IBL term only (direct light is occluded by the shadow map). The AO
is sampled relative to the 3D viewport sub-rect, not the whole framebuffer.
Targets are recreated on swapchain rebuild. AO features narrower than about
two full-resolution pixels cannot be resolved by the half tier; `full` is
the reference.

The FEM field overlay gets no shadows or AO (it is not in the prepass).
Ghosts receive both and cast neither.

### Quality settings

Persisted preferences (Edit > Preferences, `core/io/Settings.{h,cpp}`, all
live-applied):

| key | values | default |
|---|---|---|
| `shadows` | bool | true |
| `ssao` | `off` \| `half` \| `full` | `half` |
| `shadow_map` | 1024 \| 2048 | 2048 |
| `idle_throttle` | bool | true ([frame-pacing.md](frame-pacing.md#idle-throttle)) |
| `gpu_device` | device name substring, `""` = automatic | `""` |

`set_render_options {shadows, ssao, shadow_map}` changes the same three for
the session only (not persisted); an out-of-range value is refused.

## Sections

The section clip planes (`set_section`, the Section tool) are in the shared
UBO and clip the mesh, field, stock, outline, shadow and SSAO-prepass
shaders, so a cut is consistent across the viewport, its shadows and its AO.
Cut caps are ordinary mesh instances; a cap of a transmissive body is drawn
as glass.

## Picking

Picking is a CPU ray cast in `FaceSelector` (`src/cad/FaceSelector.cpp`), one
geometry record per mesh.

- Each mesh carries a **world AABB**, accumulated in `ExtractFaces` — the one
  function that runs when either geometry or placement changes, so the box
  cannot disagree with what a pick tests. It covers triangles **and** edge
  polylines: a mesh can carry edges and no triangles, and a box over the
  triangles alone would make its edges unpickable. A ray that misses the box
  skips the mesh.
- The **ray goes into mesh space**, the vertices stay put. The local direction
  is deliberately not renormalised, so `t` stays the parameter along the
  world ray and distances compare across meshes of different scale. The hit
  normal returns through the inverse-transpose times `sign(det)`.
- **One face pick per frame**, shared by the occurrence branch, the face/body
  branch and `PickEdge`'s occlusion reference (`SelectionPipeline.cpp`).
- A **mirrored** placement reports an **inward** face normal, as
  `GetFaceNormal` does; `tests/test_faceselector.cpp` pins that answer.

The hover pick runs on every frame the cursor is over the viewport, so it is
the costliest CPU work in such a frame, and the per-triangle loop is kept to
what it is. A section's clip planes are half-spaces, so what they leave of a
ray is one interval, computed once per ray; each triangle hit then costs two
float compares against it rather than a dot product per plane.

There is no BVH inside a mesh: every triangle of every mesh the ray's box
test admits is tested. `collide::Mesh` (`core/MeshCollide.cpp`) has a
triangle BVH that could be reused. The same slab-test-first prune is used by
fastener axis traversal (`parts::TraverseAxis`).

## Offscreen mode and lazy projection

`--offscreen` runs the real renderer — materials, shadows, SSAO, the path
tracer when the device has it — with no window, no surface and no display
server. **It has no panels, so the 3D viewport is the whole framebuffer**;
anything that assumes full-window draws correctly here and wrong in the app,
and needs a windowed GUI test. `--headless` has no renderer at all.

Offscreen, the tick projects nothing into the renderers. Projection is
O(every body) — it removes and re-adds all of them, and a single-parameter
edit re-runs the feature tree so every body comes back as a new shape — so
doing it per edit makes every RPC pay for a picture nobody asked for.
Everything that reads the projection pulls it first through
`ui::SyncRenderersToDocument`:

- a capture (`screenshot`, each `render_sheet` tile, a quick still, a
  `frame_stats {renders}` burst) — `SyncOffscreenScene` in `main.cpp`, which
  also does the render-target maintenance only a frame needs (shadow/SSAO
  recreate and rebind, clip planes, deform scale);
- `select_face` and `orient_to_face`, which resolve element names through
  `documentMeshes`;
- `rt_build` and `render_still`, which read the raster tessellation cache.

`TickCamPlayback` stays on the tick (playback is wall-clock). The windowed
loop draws every frame, so it projects every frame.

**A stale projection must not be read.** After an edit `documentMeshes`
still has the right *length*, so a body index yields a plausible mesh id of
superseded geometry. Ask `ui::RenderMeshesCurrent()`. `get_state` does not
pull (it is the hot poll) and reports `bodies[].mesh` only while the
projection is current, omitting it otherwise. To pick a face after an edit
use the stable element name (`list_elements` → `select_face {element}`),
which pulls for itself.

`tests/offscreen_smoke.py` counts the app's own `Document synced` log lines:
edits with nothing looking produce none, the next capture exactly one, of the
current document; a second phase pins the omitted `bodies[].mesh`.

A new scene renderer is wired in `RegisterSceneRenderers()` /
`ShutdownSceneRenderers()` and nowhere else, for both modes.

## Screenshots

`CaptureScreenshot` (`render/Screenshot.cpp`) re-records the prepasses and
the scene renderers into its own MSAA framebuffer, then reads it back into a
host-cached buffer (`vkutil::CreateReadbackBuffer`: `HOST_VISIBLE |
HOST_CACHED`, invalidated after the copy when not coherent, falling back to
coherent-only memory). Uncached memory read a byte at a time is orders of
magnitude slower. The windowed `screenshot` writes the viewport as drawn,
including overlays; a still of only the model is `render_still {mode:
"quick"}` ([still-render.md](still-render.md)).

A camera move animates for about a second, so a capture taken straight after
one shows a frame part way through the turn. A remote `screenshot` is
therefore held while the camera is animating (`RemoteServer::WantsScreenshot`),
for at most 300 frames, and then taken anyway. The `view` RPC snaps rather
than animates; `menu_click`, the view cube and align-to-face do not, which is
why the wait sits where every capture passes through.

## RPCs

| method | what |
|---|---|
| `screenshot {path, view?, fit?}` | the rendered viewport to a PNG; needs a renderer |
| `render_sheet {path, views?, focus?}` | one PNG of up to six tiled named views with a numeric footer |
| `render_still {...}` | a quick or path-traced still ([still-render.md](still-render.md)) |
| `rt_build` | build the ray-tracing acceleration structures (`ok`, `geometry_count`, `has_tlas`) |
| `set_render_options {shadows, ssao, shadow_map}` | session render quality |
| `frame_stats {window, hud, reset, renders}` | frame and GPU pass timing ([frame-pacing.md](frame-pacing.md)) |
| `view`, `fit_view {width?, height?}`, `set_camera` | camera; `get_state.camera` reports `fov_deg` and `aspect` |
| `set_section` / `add_section` / `update_section` | section clip planes |
| `set_material`, `set_appearance` | the look of a body ([materials.md](materials.md)) |

## How it is tested

The Vulkan layer has no unit tests; it is covered by suites that drive a real
renderer, and by looking at the pictures.

- **GUI suite** (`tools/gui-tests-xvfb.sh`) — drives the windowed frame
  path, including the CAM simulation's viewport-offset assertion
  (`cam/simulation_plays_and_gives_it_back`), the frame-time HUD and the idle
  throttle.
- **`tests/render_pixel_smoke.py`** — re-renders a fixed scene and compares
  the PNG byte for byte against a baseline keyed to the exact driver
  (`tests/golden/render/`); a driver with no baseline is skipped locally and
  refused in CI (`--require-baseline`). CI's baseline belongs to the runner's
  llvmpipe: `tools/ci-local.sh --pixel` checks it and `--pixel-record` writes
  it. **Run it after anything touching `RendererSync.cpp` or a renderer.**
  Its scene assigns no materials, so a change in shared shader code shows
  here.
- **`tests/render_ao_smoke.py`** — a scene whose correct AO is confined to
  one place, asserting none appears anywhere else (catches any mapping error
  between the AO buffer and the viewport sub-rect), plus a bound between the
  upsampled half tier and native full.
- **`tests/offscreen_smoke.py`** — lazy projection, offscreen GPU timing, a
  deterministic traced PNG.
- `render_quick_smoke`, `render_still_reach_smoke`, `render_glass_smoke`
  ([still-render.md](still-render.md)).
- `remote_smoke` — the windowed screenshot path, the idle throttle's RPC
  wake, and the spare swapchain image.
- Unit: `test_outline_edges`, `test_faceselector`, `test_frame_timer`,
  `test_frame_pacer`, `test_ibl`, `test_still_outlines`, `test_materials`.
- **`tests/test_conventions.cpp`** — the scene-renderer registration gate.

## What it does not do yet

- **Real-time ray tracing in the viewport** (hybrid RT shadows/AO/reflections
  composited under the raster viewport). The path tracer runs only for
  stills. It would need a TLAS kept current across every edit, compositing
  into the ImGui viewport, and resize handling.
- **TAA** — needs a G-buffer; the SSAO depth+normal prepass is its seed.
- **An MSAA setting.** The sample count is fixed at the device maximum up to
  4x; on an integrated GPU geometry plus 4x MSAA is the floor of the frame.
- **Device-memory reporting** — no allocation count or heap budget is shown
  anywhere.
- **Per-occurrence appearance** and **per-face appearance** — see
  [materials.md](materials.md).
- A **BVH for picking** (see [Picking](#picking)).
- The path tracer builds one BLAS per body occurrence from pre-transformed
  geometry rather than sharing one per asset.
