# Sketch reference images (underlays) — design & plan

> Status: **IN PROGRESS.** Import a raster image (PNG/JPEG) into a sketch as a
> scalable underlay, calibrate its scale by picking two points and typing the
> real distance between them, then trace geometry over it.
>
> - **Phase 0 (model + serialization) — DONE & verified.** `sketch::ReferenceImage`,
>   `CalibrateImage`, format **v49**, document-level asset store (bytes survive
>   undo), base64 single-file + `assets/` directory backends, regen-hash
>   exclusion, v49 golden, `tests/test_reference_image.cpp` (16 cases). Full
>   suite green.
> - **Phase 1 (core ops + RPC/MCP) — DONE & verified.** RPCs `add/calibrate/
>   set/delete_reference_image` (RemoteModeling.cpp), MCP wrappers
>   (designer_mcp.py). Driven end-to-end against a headless app instance:
>   import→calibrate→set→save→reload round-trips in both formats.
> - **Phase 2 (rendering) — DONE & verified.** `UnderlayRenderer` + `underlay.vert/frag`,
>   per-asset device-local textures (decode via stb, cached, `vkDeviceWaitIdle`
>   teardown), textured quad on the sketch plane fed from UI.cpp. Verified by
>   screenshot on an Xvfb `--hidden` instance: image renders upright, calibrate
>   rescales (4→30 units), opacity fades, sketch grid draws on top, no validation
>   errors. Two bugs found & fixed in the process: the `screenshot` re-render
>   path (main.cpp:1466) had its OWN renderer list that omitted the underlay; and
>   the texture V needed flipping (pixel +Y down vs plane +V up).
> - **Phase 3 (GUI), Phase 4 (polish) — TODO.**
>
> NOTE: real `kVersion` was 48 (not 46 as an old memory said); this feature is
> **v49**. Build with `nix-shell --run 'ninja -C build <target>'` (glm comes
> from the nix env). Reference-image RPC is testable via `designer --headless
> --remote <port>` (no GPU) — the model/calibration path needs no renderer.

## Decisions (Elias, 2026-07-21)

1. **Calibrate = scale, with optional axis-align.** Two picked points + a typed
   distance set the scale. A checkbox additionally rotates the image so the line
   between the two points becomes horizontal (or vertical). Default off.
2. **Storage = copy into project + base64 in single-file.** `.dsgn` directory
   projects copy the pixels into `assets/`; single-file `.json` interchange
   inlines the pixels as base64 so the file stays self-contained.
3. **Multiple images per sketch.** A `std::vector` of independently
   placed/scaled/toggled images.

## Data model

An underlay is a **sketch-level attachment, not a sketch `Entity` variant** — it
carries no curve, contributes nothing to the profile, the constraint solver, or
the regen geometry, and it must never extrude. Adding it to the `Entity`
`std::variant` would force churn through every `std::visit`/`if constexpr` chain
for zero benefit. Instead, new fields on `SketchFeature`
(`src/core/features/SketchFeature.h`):

```cpp
namespace sketch {
struct ReferenceImage {
  uint32_t id = 0;             // stable id within the sketch (1-based)
  std::string asset;           // filename in <project>/assets/, e.g. "3-2.png"
  glm::vec2   origin{0.f,0.f};  // plane-UV position of the image's (0,0) pixel corner
  double      rotation = 0.0;  // radians CCW about origin
  double      unitsPerPixel = 1.0; // plane units per image pixel (the calibration)
  double      opacity = 0.5;   // 0..1
  int         widthPx = 0, heightPx = 0; // cached pixel dims (size without a decode)
  bool        hidden = false;
};
}
// on SketchFeature:
std::vector<sketch::ReferenceImage> images;
uint32_t nextImageId = 1;
```

Placement is a rigid 2D affine (origin + rotation + uniform scale); it is **not**
a solver point — like construction geometry it is excluded from profiles and the
solve. The four plane corners are:
`origin + R(rotation) * (pixel * unitsPerPixel)` for pixel in
`{(0,0),(W,0),(W,H),(0,H)}`, drawn via `SketchPlane::PlaneToWorld`.

**Regen-hash exclusion (must verify):** images live in the sketch feature JSON,
but toggling opacity or nudging an underlay must not invalidate the regen cache
and rebuild downstream extrudes. Confirm how the regen cache keys the feature
(around `Feature::VisitExprs` / feature hashing) and keep the `images` array out
of the hashed geometry — cosmetic-only, like selection state.

## Calibration math (pure, headless-testable)

Given the image's current placement and two clicked plane-UV points `p1`, `p2`,
and a desired real distance `D`:

- `f = D / length(p2 - p1)` → `unitsPerPixel' = unitsPerPixel * f`.
- Keep `p1` fixed: recover its pixel coord under the *old* placement,
  `pix1 = R(-rotation) * (p1 - origin) / unitsPerPixel`, then set
  `origin' = p1 - R(rotation') * pix1 * unitsPerPixel'`.
- **Optional align:** `rotation' = rotation - atan2(p2-p1) + targetAxisAngle`
  (targetAxisAngle = 0 for horizontal, π/2 for vertical); else `rotation' =
  rotation`.

This is the whole feature's "smarts" and it is a dozen lines with no GPU or OCC
dependency — it gets unit tests first.

## Serialization  (`src/core/io/`)

- Bump `kVersion` **46 → 47** in `SerializationInternal.h` (comment: "v47 =
  sketch reference images").
- In the SketchFeature arm of `FeatureToJson`/`FeatureFromJson`
  (`SerializeFeatures.cpp`), read/write an `"images": [...]` array of the struct
  above (scalars + vec2, same helpers as entities). The `asset` **filename** is
  always stored; pixel bytes are handled by the two storage backends below.
- **Directory format** (`DocumentIO::SaveDir`/`LoadDir`): copy each image's bytes
  to `<project>.dsgn/assets/<sketchId>-<imgId>.<ext>`; load reads them back.
  Small, git-diffable, mirrors the STEP-assembly extractor that already writes
  per-part files into the project folder.
- **Single-file `.json`**: a document-root `"assets"` object mapping
  `filename → base64(bytes)`, encoded on Save / decoded on Load, so the export
  stays self-contained.
- **Import-before-save:** importing when `projectPath` is empty forces a
  Save-As first (exactly like `OpenStepAssemblyFileDialog` today), giving us the
  `assets/` folder to copy into.
- **Golden-file policy (mandatory, enforced by `tests/test_golden.cpp`):** ship
  in the same commit — the v47 loader path, a frozen `tests/golden/v47-reference-image.json`
  (single-file/base64 form) + `.expect.json`, and a round-trip test. Add a
  directory-format round-trip too (Save→Load asset survives).

## Rendering  (new `UnderlayRenderer`)

`SketchRenderer` re-uploads host-visible vertex buffers every frame and its
descriptor set is UBO-only — a persistent GPU texture doesn't fit it. Add a
small dedicated `UnderlayRenderer` (sibling of `SketchRenderer`) instead:

- Reuse `IBLTextures.cpp:UploadImage()` + `stbi_load` (`thirdparty/stb_image.h`,
  already vendored) to decode + upload a device-local `VkImage`/view/sampler per
  image. Cache by asset filename.
- New shaders `src/shaders/underlay.vert` / `underlay.frag`: position + UV in,
  sample `sampler2D`, multiply alpha by a push-constant opacity.
- One `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` descriptor, one TRIANGLE_LIST
  pipeline, alpha blend on, **depth test on / write off**, drawn **before** the
  sketch lines so geometry sits on top.
- Only the **active sketch's** non-hidden images are drawn (fed each frame like
  `g_SketchRenderer` is from `main.cpp`).
- **Device-lost guard:** destroy textures behind `vkDeviceWaitIdle` on sketch
  switch / image delete / document close. This project has crashed before from
  freeing in-use GPU resources — do not skip the wait.
- Wire into `main.cpp` beside `g_SketchRenderer` (global, init, per-frame camera
  push, render call, `g_App.underlayRenderer`).

Headless builds simply skip the renderer — the model, calibration, and
serialization all work and are testable without a GPU (which I can't drive here).

## Phase 2 — rendering (DETAILED, awaiting approval)

New class **`UnderlayRenderer`** (`src/UnderlayRenderer.{h,cpp}`), a sibling of
`SketchRenderer` — NOT bolted onto it (SketchRenderer re-uploads host-visible
vertex buffers every frame and its descriptor set is UBO-only; a persistent
device-local texture doesn't fit). Mirrors SketchRenderer's Initialize / helper
shape (`FindMemoryType`, `LoadShader`, per-frame dynamic buffers, `ExecutableDir()
+ "/libraries/shaders/"`).

**Descriptors / pipeline layout**
- Set 0, binding 0: UBO `{mat4 view; mat4 proj;}` (vertex stage), per-frame host-
  visible + mapped, exactly like `SketchRenderer::Ubo` (minus the viewport vec4).
- Set 1, binding 0: `COMBINED_IMAGE_SAMPLER` (fragment stage) — **per texture**,
  not per frame.
- Push constant (fragment): `float opacity`.
- One pipeline: `underlay.vert`/`underlay.frag`, `TRIANGLE_LIST`, alpha blend on
  (`SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`, same as the quad blend), depth test ON /
  write OFF, cull NONE, drawn **before** the sketch lines so geometry sits on top.

**Vertex** `UnderlayVertex { glm::vec3 pos; glm::vec2 uv; }`. Each image is a
quad (6 verts) appended into one per-frame dynamic buffer; the renderer records
`(firstVertex, textureSet)` per image and draws them in order.

**Texture cache** keyed by asset name: `std::map<std::string, Texture>` where
`Texture { VkImage; VkDeviceMemory; VkImageView; VkDescriptorSet set1; int w,h; }`.
Created lazily on first sight of an asset via `stbi_load_from_memory(bytes)` →
RGBA8 → staging buffer → `vkCmdCopyBufferToImage` → device-local image + view →
allocate set 1 → write the sampler (one shared linear / clamp-to-edge sampler).
Asset names are unique per (feature,image), so a replaced image gets a new key —
no stale-texture invalidation needed.

**Per-frame API** `SetImages(std::vector<UnderlayDraw>)`,
`UnderlayDraw { std::string asset; const std::vector<unsigned char>* bytes;
std::array<glm::vec3,4> corners; float opacity; }`. `bytes` is only read on a
cache miss (a cheap pointer otherwise). `Render(cb, frame)` updates the UBO,
binds the pipeline + set 0, then per image binds set 1, pushes opacity, and draws.

**Texture lifetime (device-lost guard).** `ReleaseTextures()` does
`vkDeviceWaitIdle` then destroys every cached texture; call it on sketch exit,
document close, and document-version change. Never free a texture that a
frame in flight may still sample — this project has crashed from exactly that
(see [[assembly-two-extruded-devicelost]]).

**Feeding** (`src/ui/UI.cpp`, beside the existing `BuildSketchOverlay` block at
~424–483, and only when `state.sketchEdit.IsEditing()`): resolve the active
`SketchFeature` (by `activeSketchId`) and its `SketchPlane`. For each non-hidden
image, the four pixel corners `(0,0),(W,0),(W,H),(0,H)` map to plane-UV
`origin + R(rotation)·(vec2(px,py)·unitsPerPixel)`, then to world via
`SketchPlane::PlaneToWorld`; UVs are `(0,0),(1,0),(1,1),(0,1)`. `bytes` comes
from `doc->GetAsset(image.asset)`. Build the `UnderlayDraw` list → `SetImages`.
When not editing: `SetImages({})`, and `ReleaseTextures()` on the edit→non-edit
transition.

**main.cpp wiring** (beside `g_SketchRenderer`): global `g_UnderlayRenderer`,
`Initialize` after it, per-frame camera push, `Render` call **before** the
sketch renderer's (main.cpp:755), and expose `g_App.underlayRenderer`.

**CMake**: two `compile_shader(...)` calls for `underlay.vert`/`underlay.frag`,
added to the `shaders` custom target list (CMakeLists.txt:609); `UnderlayRenderer.cpp`
into `app_sources`.

**Verification (my responsibility).** Build `designer`; dev-watch relaunches the
8799 instance. Drive it (via `designer_remote.py`): new sketch, `add_reference_image`,
enter sketch edit, `screenshot` → confirm the image renders on the plane at the
right size; `calibrate` then re-screenshot to confirm it rescales; `set_reference_image`
opacity → confirm blend; delete → confirm it disappears without a validation
error (run with `DEV_WATCH_VALIDATE=1` to catch the free-in-flight case).

**Risks:** (1) device-lost on texture teardown — mitigated by the wait-idle
guard; (2) UV/V-flip (image top row vs. plane +V) — verify against a text-bearing
test image; (3) images behind an opaque body are depth-culled — acceptable for
v1 (underlays live on the sketch plane).

## GUI  (`src/ui/`)

- **Import**: clone `OpenStepFileDialog` → `OpenReferenceImageDialog`
  (`FileDialogs.cpp`) with a `png,jpg,jpeg,bmp` filter; Save-As-first when no
  project path. Menu item **Insert ▸ Reference Image…** and a sketch-toolbar
  button (`Toolbar.cpp:DrawSketchModeToolbar`). On import: decode for
  `widthPx/heightPx`, append a `ReferenceImage` centered on the plane origin at a
  sensible default scale, copy bytes into the project, mark dirty.
- **Calibrate tool**: new `SketchTool::CalibrateImage` (`AppState.h`) + handler
  branch in `HandleSketchViewport` (`SketchInteraction.cpp`). Pick p1, pick p2
  (reusing `RayToPlaneUV`), then a value popup — reuse the dimension tool's
  entry path (`DrawDimensionOnCreate`/`EditDimensionValue`) — plus an
  "align horizontal/vertical" checkbox. Apply the math above. Hover feedback and
  prompt text via `SketchToolPrompt`.
- **Images panel** (`SketchToolbar.cpp`): list the active sketch's images with an
  opacity slider, hidden toggle, delete, and a "Calibrate" button (arms the
  tool for that image). Move-by-drag is a stretch (reuse the existing drag
  session / gizmo); v1 can move via re-calibration + a numeric origin field.

## RPC / MCP  (headless-drivable)

Sketch domain lives in `src/remote/RemoteModeling.cpp`; MCP wrappers in
`tools/designer_mcp.py` (one `@mcp.tool` → `_call("method", ...)` each). Path is
a plain string param (agent supplies it; no dialog), so all of this is testable
headlessly.

- `add_reference_image(sketch_id, path, [origin], [units_per_pixel])` → image id.
- `calibrate_reference_image(sketch_id, image_id, p1, p2, distance, [align])`.
- `set_reference_image(sketch_id, image_id, {opacity|hidden|origin|rotation})`.
- `delete_reference_image(sketch_id, image_id)`.
- `list` surfaced via existing `get_state`/element listing.

## Phasing  (≈ 4–6 focused days)

- **Phase 0 — model + serialization (~1–1.5d).** Struct + fields, both storage
  backends, `kVersion` 47, golden + round-trip tests, regen-hash exclusion.
  *Fully headless-verifiable.*
- **Phase 1 — core ops + calibration + RPC/MCP (~1d).** Add/calibrate/set/delete
  with unit tests for the calibration math (scale + align variants); wire RPC +
  MCP. *Still headless — I can drive and verify this myself.*
- **Phase 2 — rendering (~1.5d).** `UnderlayRenderer`, `underlay.*` shaders,
  texture cache + lifetime, `main.cpp` wiring. *Needs your GPU to eyeball.*
- **Phase 3 — GUI (~1.5d).** Import dialog + menu/toolbar, calibrate tool,
  images panel.
- **Phase 4 — polish + tests + docs (~0.5d).** `gui_smoke` coverage, prompts,
  update this doc to SHIPPED, add a memory entry.

Rough-cut fallback (~2d): Phase 0 + a minimal Phase 2 + a dialog-box calibrate
(no interactive two-point pick) to prove the pipeline end-to-end.

## Risks / watch-items

- **Regen cache invalidation** from cosmetic underlay edits (see Data model).
- **GPU resource lifetime** — device-lost if textures freed while in flight.
- **Single-file bloat** — base64 pixels can make `.json` large; acceptable for
  interchange, and the `.dsgn` path keeps day-to-day projects lean.
- **Non-square pixels / EXIF rotation** — ignore EXIF in v1; document it.
