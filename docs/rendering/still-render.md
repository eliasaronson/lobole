# Rendered stills

`render_still` and the toolbar's Render dialog ("Render Still") produce a
picture of the model at a chosen size, in one of two modes:

- **path** (the default) — a path-traced still: soft shadows, AO, real
  materials, environment reflections, refraction through glass. Needs a
  hardware GPU with the ray-tracing extensions: on a software device
  (lavapipe), in safe mode, or without the extensions it answers `ok: false`
  with the reason (`ui::PathTracerRefusal`,
  [the device ladder](README.md#the-device-ladder)).
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
| `backdrop` | `studio` \| `environment` \| `sky` ([backdrops](#backdrops)) | `studio` |
| `sun_azimuth_deg` | sky only: the sun's bearing, counter-clockwise from world +X seen from above | where the studio key stands for this camera |
| `sun_elevation_deg` | sky only: the sun's height above the horizon, (0, 90] | 40 |
| `outlines` | edges over the image, as the viewport draws them | true |
| `supersample` | quick only: 1-4 | 2 |
| `line_weight` | quick only: edge width in px of the output, 0.25-4 | the viewport's (1) |

An unknown `mode` or `backdrop` is refused naming the accepted values. The
path tracer's parameters (`samples`, `seed`, `ground`, `backdrop`, the sun's
two) are **refused** in quick mode rather than ignored, and `line_weight` in
path mode. A sun parameter with any backdrop but `sky` is refused, as is an
elevation outside (0, 90]. A path reply names its `backdrop`; a sky reply
also carries the `sun` it was lit by, `{azimuth_deg, elevation_deg}`,
derived or not. `supersample` outside 1-4, a `line_weight` outside 0.25-4 and an
`outlines` that is not a bool are refused. A quick reply says `outlines`, and
the `line_weight` it drew when it drew lines. A picture that is almost
entirely one colour is still saved, with a warning in the reply.

A still renders a part or an assembly tab. On a drawing or a CAM job tab it
is refused naming the tab and its kind (`ui::StillTabRefusal`), in either
mode and in the dialog; "no geometry to render" is kept for a part that is
genuinely empty, and an assembly with no occurrences says that.

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
one fit; `forAspect = 0` means the camera's own. It frames the bodies'
TIGHT boxes (`ui::VisibleSceneBounds(..., tight)`): the conservative box
bounds a B-spline face by its control points, and a downloaded STEP bracket
of 62 x 36 x 78 mm framed as 164 x 345 x 156, the part a speck
(`test_actions` "the camera fit frames a B-spline part by its tight box").
A viewport fit keeps the
model out from under the tool window and the view cube
([../ui/README.md](../ui/README.md#viewport)); a fit for a picture does not,
because a still draws no overlays and gets the whole frame. `tests/headless_smoke.py`
fits for three very different picture shapes and projects the part's bbox
corners through the reported camera at that aspect, asserting all land in
NDC — and first asserts that a plain `fit_view` does *not* already frame a
tall picture, so the check cannot pass vacuously.

An assembly renders as the viewport shows it, the active explode included
(`SyncRayTracingScene` composes each occurrence's explode step as the raster
sync does): the still of an exploded view used to be the assembled head,
small in a frame `fit` had sized for the exploded one.

The GUI dialog does not fit: it renders the view the user framed, and its
default size is 1920x1080 whatever the viewport's shape, so a still can crop
a part that is whole on screen.

## The dialog

The Render toolbar button (`view.render`) opens a modal with a resolution
list (3840x2160 down to 640x480, default 1920x1080), a mode choice, an
Outlines checkbox, a progress bar and Cancel; quick adds the supersampling
and a Line weight slider (0.25-4 px of the still, starting at the viewport's
width, disabled with Outlines off), path a sample slider (16-1024, default
512) and the ground and backdrop choices. The dialog keeps its choices for
the session. On a device without ray tracing it opens anyway, with "Path
traced" disabled, its tooltip saying why, and quick forced.

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
image) → `ReadbackStill` (divide by N, the viewport's tone curve — `tone::ToDisplay`,
[tone mapping](README.md#tone-mapping) — PNG) → `EndStill`.
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

**The still is lit as the viewport is.** It takes the viewport's
[light rig](README.md#the-light-rig) for the same camera — key 40° to the
camera's left and 45° up, a fill 60° to the other side, a rim from behind —
and the environment turned the same way, so it stays off-axis from any
viewpoint and a still matches the screen it came from, Lock lights to model
included. The rig began as the still's own: the viewport's old world-fixed
key sat close to the home view direction, which flattened a traced still and
threw its shadow behind the model.

### Backdrops

What the camera sees where it sees no geometry, and what lights the model:

- **`studio`** (the default) — a neutral vertical sweep (`studioBackdrop()`
  in `miss.rmiss` and `closesthit.rchit`); the studio HDRI still lights the
  model and fills its reflections, but is not the picture (a low-resolution
  photograph stretched behind a model has no scale relationship with it).
- **`environment`** — the studio HDRI itself.
- **`sky`** — a procedural outdoor sky that backs the still AND lights it
  ([the sky](#the-sky)).

`RTStillDesc::backdrop` (`StillBackdrop`) carries the choice; the shaders
only need to know whether to draw the sweep (`params.w`), because
`environment` and `sky` both show the environment the bound maps were built
from.

### The sky

`IBLPrecompute::GenerateSky` (`src/render/IBL.cpp`) draws a 1024x512
equirectangular image in the HDRI's own convention: a zenith-to-horizon
gradient (deep blue overhead, pale haze at the horizon), a dry grey-brown
ground below a thin haze band, and a sun disc (drawn 1.5° across, larger than
the real sun's half degree so it survives the map's third of a degree a
pixel) with a glow around it. A low sun dims the sky and the ground. It is
deterministic: the same sun gives the same pixels.

`IBLPrecompute::BuildFromImage` convolves it exactly as the HDRI is
convolved (irradiance, prefilter chain, BRDF LUT), so the sky lights the
model through the same split-sum IBL. `SkyEnvironment` (`app/GpuOwners.cpp`), reached
as `AppState::skyEnvironment`, builds those maps the first time a sky still
asks and keeps them for the next still with the same sun (rounded to a
tenth of a degree, which the pixels then use too); a different sun replaces
them. Unlike the HDRI's, the sky's convolution is not cached on disk: it
takes about 0.2 s, and a file per sun direction would only pile up.

In a sky still the rig changes in two ways (`MakeStillDesc`): the **key
light is the sun** — its direction, with the soft shadow it already casts —
so the shadow falls away from the disc in the backdrop; and the environment
does **not** turn with the camera (`envRotation` is the identity), since a
sky belongs to the world. Fill and rim stay as the rig places them. The
disc and its glow are moderate on purpose: the key is the sun's direct light,
so the sky only has to give the IBL its bright side.

The sun's **elevation** defaults to 40°, a fixed value: an angle is not a
model-scale quantity, and a mid-morning sun gives short shadows that still
model the form. Its **azimuth** defaults to the bearing of the studio key
for this camera (Lock lights to model included) — over the viewer's left
shoulder, so the side being looked at is lit. The reply's `sun` reports both.

The GUI dialog offers studio and environment; the sky is the RPC's for now.
Underlays are not traced: a path still shows none of them, `show_always` or
not (a quick still does, [quick mode](#quick-mode)).

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
   projects the camera at W/H, and sets the wireframe to k x the line
   weight (or draws no edges, with `outlines: false`);
2. clears the selection and hover tints, and settles the silhouettes;
3. records the prepasses and **only the mesh, field, underlay and outline
   renderers** (no sketch or CAM stock) into `CaptureScreenshot`'s own
   attachments (`RecordModel`). The underlay renderer is handed only the
   reference images marked `show_always`
   ([sketching](../sketching/README.md#reference-images-underlays)), by
   `ui::SyncUnderlays` in `AimRenderersAt` — never the edited sketch's own,
   since a capture draws no sketch. That call is also what fills the
   renderer offscreen, where no frame loop runs. A motion video's frames
   take the same path;
4. restores every size it changed, so the next frame is the frame it would
   have been.

`supersample` k (1-4, default 2) renders k times wider and taller, on top of
the pipelines' MSAA, and `DownsampleToPng` (`render/Screenshot.cpp`) averages
it down, with an optional progress and cancel flag. k is lowered, with a note
in the reply, if k·W passes the device's image or framebuffer limit. The
background is the viewport's.

### Lines in a quick still

A quick still's edges are the viewport's in **output** pixels at any k: as
wide (the line weight, default the 1 px the viewport draws) and as dark.
They are the outline renderer's geometry, rasterized as lines, so they stay
straight; nothing is found in the image. Two things make that hold:

- **Width.** The wireframe pipeline takes its line width per draw
  (`VK_DYNAMIC_STATE_LINE_WIDTH`, `OutlineSettings::rasterLineWidth`): 1 in
  the viewport, k x the line weight in a capture k times larger, so the
  average down is the line weight. `app/GpuOwners.cpp` enables the device's
  `wideLines` feature wherever it has one. A width past the device's widest
  line is clamped, with a note in the reply; one below its narrowest is drawn
  that wide at the alpha of the coverage the thinner line would have had.
- **The average.** `DownsampleRgb` averages the *encoded* values, the space
  the viewport's own MSAA resolve averages in (its UNORM target holds what
  the shaders encoded), so the still's anti-aliasing is the viewport's
  filter on a finer grid.

Both used to be wrong at once: lines were drawn 1 px wide at k times the
size (the scaled `lineWidth` was the traced still's pen, which the raster
pipeline never read), so they averaged down to 1/k of a pixel, and the
linear-light average took a further third off a dark line on a light face.
Across the edges of a light-grey block, a line's darkness against the face
was a third of the viewport's at 2x and a ninth at 4x. Measured now as what
the lines take off the same picture drawn without them, it is within 2%
(peak) and 5% (summed across the line) of the viewport's at 2x and 4x, on
an RTX 4060, Intel Arc and llvmpipe.

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
  path-tracer parameter in quick mode is refused naming it, as are bad
  `line_weight` and `outlines` values; the viewport's own screenshot is
  byte-identical before and after. Then the lines, on a light-grey block at
  the viewport's size and camera: a 1x still IS the viewport's screenshot,
  byte for byte, so its `outlines: false` twin is the viewport without lines;
  what the lines take off their bare twin at 2x and 4x is within 10% of the
  viewport's, `outlines: false` leaves no line, and `line_weight` 2 and 0.5
  draw heavier and lighter ones.
- `tests/offscreen_smoke.py` — same seed, byte-identical traced PNG.
- `tests/render_still_reach_smoke.py`, `tests/render_glass_smoke.py` — skip
  (77) without ray tracing.
- `tests/render_sky_smoke.py` — a sky still of a block: the top of the frame
  is sky blue and the bottom ground, where the studio still of the same view
  is neither; the reply names the derived sun and an explicit one is used as
  given; an unknown backdrop is refused naming all three, a sun parameter
  without `sky` or in quick mode is refused, as is an elevation outside
  (0, 90]. Skips (77) without ray tracing.
- `tests/underlay_show_always_smoke.py` — a pure-red underlay with
  `show_always` is red in a quick still taken with no sketch in edit, and the
  same still without the flag has no red in it.
- `tests/test_still_outlines.cpp` — a diagonal boundary at 4x gives
  fractional coverage, a 1 px line covers about one pixel, and strip-by-strip
  coverage equals one whole-image pass.
- `tests/headless_smoke.py` — framing for a picture's aspect.
- The GUI test `render/modal_enabled_on_an_assembly` drives a quick render
  through the dialog to the file, with the Line weight slider disabled while
  Outlines is off and its value reaching the job.
