# Rendered stills

`render_still` and the toolbar's Render dialog ("Render Still") produce a
picture of the model at a chosen size, in one of two modes:

- **path** (the default) — a path-traced still: soft shadows, AO, real
  materials, environment reflections, refraction through glass. Needs a GPU
  with the ray-tracing extensions.
- **quick** — the model as the viewport draws it (materials, the three
  lights, shadow map, SSAO, outlines), at the requested size, in about a
  second. Needs a renderer, not ray tracing.

## `render_still`

| param | meaning | default |
|---|---|---|
| `path` | output PNG | `render.png` |
| `width`, `height` | pixels | 1920 x 1080 |
| `mode` | `path` \| `quick` | `path` |
| `fit` | frame the model for THIS picture's aspect first | false |
| `samples` | path samples per pixel | 512 |
| `seed` | RNG seed; (scene, samples, seed) reproduces the image | 1 |
| `ground` | shadow-catcher ground plane | true |
| `backdrop` | `studio` \| `environment` | `studio` |
| `outlines` | edges over the image, as the viewport draws them | true |
| `supersample` | quick only: 1-4 | 2 |

An unknown `mode` or `backdrop` is refused naming the accepted values. The
path tracer's parameters (`samples`, `seed`, `ground`, `backdrop`,
`outlines`) are **refused** in quick mode rather than ignored. `supersample`
outside 1-4 is refused. A picture that is almost entirely one colour is still
saved, with a warning in the reply.

The RPC is synchronous. It pulls the renderer projection first
([README.md#offscreen-mode-and-lazy-projection](README.md#offscreen-mode-and-lazy-projection)),
so it works on a `--offscreen` instance.

## Framing

`fit_view` frames the geometry for the *viewport's* aspect; a still projects
at `width / height`. When the still is narrower than the viewport, a
fit-then-render crops — and offscreen the viewport aspect is whatever the
framebuffer happens to be. So:

- `render_still {fit: true}` fits at the still's own size before rendering;
- `fit_view {width, height}` frames for a picture of that size (both or
  neither; one alone is refused);
- `get_state.camera` (and `view`) report `fov_deg` and `aspect`, so a script
  can reproduce the projection and check its own framing.

`ui::FitCameraToDocument(state, forAspect)` (`src/ui/ViewActions.cpp`) is the
one fit; `forAspect = 0` means the camera's own. `tests/headless_smoke.py`
fits for three very different picture shapes and projects the part's bbox
corners through the reported camera at that aspect, asserting all land in
NDC — and first asserts that a plain `fit_view` does *not* already frame a
tall picture, so the check cannot pass vacuously.

The GUI dialog does not fit: it renders the view the user framed, and its
default size is 1920x1080 whatever the viewport's shape, so a still can crop
a part that is whole on screen.

## The dialog

The Render toolbar button (`view.render`) opens a modal with a resolution
list (3840x2160 down to 640x480, default 1920x1080), a mode choice, a sample
slider (16-1024, default 512), a progress bar and Cancel. On a device without
ray tracing it opens anyway, with "Path traced" disabled and quick forced.

- **Path** runs as `AppState::renderJob`, pumped one batch (up to 32
  samples) per frame by `PumpRenderJob` from `ui::PollBackgroundJobs`, so the
  app keeps drawing. The GUI uses seed 1.
- **Quick** captures on the main thread (the only part that needs the
  renderers, a few hundred ms), then downsamples and encodes the PNG on a
  worker thread with progress and Cancel.

## The path tracer

`RayTracingRenderer` builds one BLAS per body occurrence (pre-transformed to
world space; also one for each section cap and the ground plane) and a TLAS
with `instanceCustomIndex` = geometry index. `rt_build` builds them on its
own; `render_still` builds them from the raster tessellation cache.

**Still API**: `BeginStill` (output image, UBO, SSBOs, descriptor set) →
`RenderBatch` (N traces per one-shot submit, accumulating into an `rgba32f`
image) → `ReadbackStill` (divide by N, ACES, gamma, PNG) → `EndStill`.
`RenderStillBlocking` (`RendererSync.cpp`) is the synchronous form the RPC
uses.

**Descriptor set** (`raygen.rgen`, `closesthit.rchit`): 0 TLAS, 1 `rgba32f`
accumulation image, 2 camera/light UBO, 3 `GeometryInfo[]` SSBO, 4
`RTMaterial[]` SSBO, 5-7 the IBL irradiance / prefilter / BRDF maps (shared
with the raster viewport, so the two agree on lighting), 8 the full-resolution
environment, 9 the `rgba32ui` outline G-buffer. Closest-hit reaches each
body's vertex and index data through `GL_EXT_buffer_reference2` addresses in
`GeometryInfo[gl_InstanceCustomIndexEXT]`, so there are no descriptor arrays;
`std430` float/uint references with stride 4 match the 6-float `Vertex3D`.
The shaders compile with `glslc --target-env=vulkan1.2`. Raygen push
constants: `sampleIndex`, `frameSeed`, `auxPass`, `auxRow0`, `auxHeight`.

**Shading.** Raygen jitters each camera ray (free anti-aliasing) and seeds a
PCG RNG per pixel and sample. Closest-hit shades with the body's material:
the key light with a cone-jittered shadow ray (soft shadows), unshadowed fill
and rim, one cosine-weighted AO ray, and split-sum IBL ambient. Near-mirror
surfaces (roughness below 0.15) blend in the full-resolution environment for
crisp reflections.

**The still is lit for the picture, not for working.** Its key light is
derived from the camera — 40° to its left and 45° up — with a fill 60° to the
other side and a rim from behind, so it stays off-axis from any viewpoint.
The viewport's fixed rig (`conv::kKeyLightDir`) sits close to the default
view direction, which is fine where outlines and SSAO define shape but
flattens a traced still and throws its shadow behind the model.

**Backdrop.** By default (`studio`) the camera sees a neutral vertical sweep
where it sees no geometry; the HDRI still lights the model and fills its
reflections, but is not the picture (a low-resolution photograph stretched
behind a model has no scale relationship with it). `environment` shows the
HDRI itself.

**Ground.** The shadow catcher is a large quad just under the scene's
bounds, flagged `RT_GEOM_SHADOW_CATCHER` in `GeometryInfo.flags`; it shows
the backdrop darkened by key-light shadow and contact AO, so parts are
grounded without a visible floor. It is opaque from both sides, so it is left
out when the camera is below it.

**Glass** is traced as a path: raygen loops up to 8 segments, and
closest-hit hands each continuation back instead of tracing it, so path
length does not add recursion depth. Shadow and AO rays pass through glass
tinted. See [materials.md#glass](materials.md#glass).

## Quick mode

`AppState::captureRaster` (a `RasterStill` request) is installed by
`main.cpp` in both the windowed and the `--offscreen` loop — a capability
only `main.cpp` can provide, reachable from `ui/` and `remote/`.
`CaptureRasterStill`:

1. points the scene renderers' viewport and the SSAO targets at k·W x k·H,
   projects the camera at W/H, and scales line and outline widths by k;
2. clears the selection and hover tints, and settles the silhouettes;
3. records the prepasses and **only the mesh, field and outline renderers**
   (no sketch, underlay or CAM stock) into `CaptureScreenshot`'s own
   attachments;
4. restores every size it changed, so the next frame is the frame it would
   have been.

`supersample` k (1-4, default 2) renders k times wider and taller, on top of
the pipelines' MSAA, and `DownsampleToPng` (`render/Screenshot.cpp`) averages
it down in linear light, with an optional progress and cancel flag. k is
lowered, with a note in the reply, if k·W passes the device's image or
framebuffer limit.

## Outlines on a traced still

A traced still finds its edges as discontinuities in a primary-ray G-buffer
(body id, face normal — binding 9), and averages the edge mask into coverage,
so lines are anti-aliased at any resolution:

- The G-buffer is traced **4x4 per output pixel, in strips of rows**: raygen
  takes a row offset and the aux height, and a strip holds at most 4 M
  texels (64 MB), so memory does not depend on the still's size. The scale
  drops below 4 only if 4·width passes the device's largest image.
- Each strip carries a **halo** of rows above and below — as many as the edge
  test and the line-width dilation reach — so a seam computes exactly what a
  whole-image pass would.
- The edge test, dilation and block average are a pure function,
  `render/StillOutlines.{h,cpp}` (CPU, in `designer_core`). The dilation
  radius comes from the line width in aux pixels, so a 1 px line is about
  1 px at any scale.
- The G-buffer is written from the first hit only, so glass does not add
  edges behind it.

The still's radiance and G-buffer readbacks use host-cached memory
(`vkutil::CreateReadbackBuffer`), as screenshots do.

## Tests

- `tests/render_quick_smoke.py` — on an `--offscreen` instance: a quick
  still of exactly the requested size, not one colour, in seconds; a
  path-tracer parameter in quick mode is refused naming it; the viewport's
  own screenshot is byte-identical before and after.
- `tests/offscreen_smoke.py` — same seed, byte-identical traced PNG.
- `tests/render_still_reach_smoke.py`, `tests/render_glass_smoke.py` — skip
  (77) without ray tracing.
- `tests/test_still_outlines.cpp` — a diagonal boundary at 4x gives
  fractional coverage, a 1 px line covers about one pixel, and strip-by-strip
  coverage equals one whole-image pass.
- `tests/headless_smoke.py` — framing for a picture's aspect.
- The GUI test `render/modal_enabled_on_an_assembly` drives a quick render
  through the dialog to the file.
