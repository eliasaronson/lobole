# R4 — Offline raytraced "Render" button — implementation plan

The KeyShot-style money shot: an on-demand, present-quality still (path-traced
soft shadows, AO, real materials, environment reflections) rendered into an
offscreen image and saved to PNG. The interactive raster viewport is **not**
touched — RT runs only when the user clicks Render. Real-time hybrid RT (6b)
stays deferred.

Estimated **4–6 focused days**. First-light debugging (a black image with no
validation error) is the dominant risk, so the plan front-loads a ground-truth
audit and builds up from one triangle.

## What already exists (grounded in code, Jul 2026)

The `RayTracingRenderer` scaffold is ~40% real:
- **Real**: capability detection, RT extension/function loading, the RT pipeline
  (`CreateRTPipeline` — descriptor set layout, 4 shader stages/groups,
  `vkCreateRayTracingPipelinesKHR`), the shader binding table
  (`CreateShaderBindingTable`). All four shaders compile to
  `libraries/shaders/*.spv` and `Initialize()` runs at startup.
- **Shaders**: `raygen.rgen` (basic — 1 ray/pixel, writes an `rgba8` storage
  image, no jitter/accumulation), `closesthit.rchit` (138 lines — already has a
  shading model + a shadow ray), `miss.rmiss`, `shadow.rmiss`.
- **Stubbed**: `AddGeometry` (only counts vertices — no GPU buffers),
  `CreateBottomLevelAS`/`CreateTopLevelAS` (fill the Vulkan structs but never
  call `vkBuildAccelerationStructuresKHR`), `CreateRTDescriptorSets` (one log
  line), `Render()` (empty). Nothing calls the render path.

**Already cleared for R4** (done in R1–R3 + the validation pass):
- Device creation requests Vulkan 1.2, so the RT extensions' dependencies and
  the SPIR-V 1.4 shaders are valid; the SBT buffer allocates with
  `DEVICE_ADDRESS_BIT`. A validated startup is **0 VUIDs** (`1b9c7f4`).
- `ImGui_ImplVulkanH_SelectPhysicalDevice` prefers a discrete GPU → the RTX 4060
  (RT "support detected" in the log). The Arc iGPU is not selected. *Verify*
  with a device-name log; likely no fix needed.
- **Materials** (R1): `MaterialProperties` (baseColor/metallic/roughness/
  specular/reflectance) + the per-feature slug. `RTMaterial` already mirrors
  these fields — the closest-hit shader can shade with real materials.
- **IBL** (R3): `IBLTextures` exposes irradiance / prefilter / BRDF sampled
  textures. **Reuse opportunity**: the RT closest-hit can sample the *same*
  environment maps for ambient/reflection, so the offline render and the raster
  viewport agree on lighting — no separate RT environment to build.
- **Readback** (`Screenshot.cpp`): the staging-buffer → PNG plumbing (host-
  visible buffer, `vkCmdCopyImageToBuffer`, `stbi_write_png`) is a template for
  writing the RT output image to disk.

## Progress

- **R4.0–R4.2 SHIPPED** (`c6b6fef`): device confirmed (RTX 4060); SBT math
  audited sound; `AddGeometry` uploads real vertex/index buffers; **real
  BLAS-per-body + TLAS build works** (`SyncRayTracingScene` + `rt_build` RPC →
  `has_tlas=true`, **0 VUIDs**). The highest-risk piece (AS build) is done.
- **Remaining**: R4.3 descriptors + `rgba32f` output image, R4.4 trace/
  accumulate + shader rewrite, R4.5 the button + `render_still` RPC. Note the
  descriptor architecture needs revisiting for N bodies — the scaffold's
  layout has a single vertex/index binding; use buffer arrays (or a combined
  buffer + per-instance offsets) so the closest-hit shader can fetch attributes
  for the hit body via `gl_InstanceCustomIndexEXT`.

## Milestones

### R4.0 — Ground truth (~0.5 day) — done
Was: verify device, audit SBT. Result: RTX 4060 selected; SBT sound.

### R4.0 (original detail)
Before writing new code, de-risk the untested scaffold.
- Log the selected device name; confirm it's the RTX 4060 (not Arc/llvmpipe).
  If a machine ever picks the wrong one, add a discrete-RT-capable preference
  pass over `vkEnumeratePhysicalDevices`.
- Audit the SBT region math (`m_raygenSBT`/`m_missSBT`/`m_hitSBT`
  stride/size/alignment) against `shaderGroupHandleSize` /
  `shaderGroupBaseAlignment` — written blind, never executed.
- Keep validation on (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`) for the
  whole phase.

### R4.1 — Geometry upload (~1 day)
Make `AddGeometry` real and mirror the scene.
- Vertex + index buffers per body with usage
  `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR | SHADER_DEVICE_ADDRESS |
  STORAGE_BUFFER` (STORAGE so the closest-hit can read attributes by index),
  device-address-allocated (the `CreateBuffer` fix from `1b9c7f4` already sets
  the flag when the usage bit is present).
- `SyncFromScene(document, documentMeshes)`: mirror `SyncDocumentRenderers`
  (`AppActions.cpp`) — same tessellated `Vertex3D` + indices per body, plus the
  body transform and material. Build `RTMaterial` from the body's
  `MaterialProperties` (reuse the R1 slug → `MaterialLibrary` lookup).
- Keep the RT geometry list rebuildable (clear + refill), since Render is
  on-demand — no incremental tracking.

### R4.2 — Acceleration structures (~1–1.5 days, the hard part)
Real BLAS/TLAS build (the classic subtle-bug surface).
- Per body: `vkGetAccelerationStructureBuildSizesKHR` → allocate AS buffer +
  scratch buffer (device-address) → `vkCmdBuildAccelerationStructuresKHR` on a
  one-shot command buffer → `vkGetAccelerationStructureDeviceAddressKHR`.
  (Compaction is a later optimization; skip for v1.)
- TLAS: one `VkAccelerationStructureInstanceKHR` per body (transform =
  body transform, `instanceCustomIndex` = body index for material/attribute
  lookup, `accelerationStructureReference` = that body's BLAS address); upload
  the instance array to a device-address buffer; build the TLAS.
- Build once per Render click (rebuild-on-demand). Verify with a single
  hardcoded triangle before feeding the real scene.

### R4.3 — Descriptors + output image (~0.5–1 day)
- **Accumulation image**: `rgba32f` storage image at render resolution (not the
  `rgba8` the raygen currently declares — bump it). High precision so many
  samples accumulate without banding.
- Camera UBO (viewInverse/projInverse/viewPos + light + the IBL toggle), TLAS,
  the per-body vertex/index/material storage buffers, and the IBL samplers
  (irradiance/prefilter/BRDF) — extend `CreateRTPipeline`'s descriptor layout
  (currently AS + image + UBO + vtx + idx) with the material buffer + 3 IBL
  samplers. Implement `CreateRTDescriptorSets` for real (pool + set + writes).

### R4.4 — Trace + accumulate (~1–1.5 days)
- **Raygen**: jitter the camera ray per sample (sub-pixel → free AA);
  accumulate into the `rgba32f` image across N samples; sample index + RNG seed
  via push constant.
- **Closest-hit**: shade with the real material (from the per-body `RTMaterial`
  by `instanceCustomIndex`), cast a soft shadow ray toward the key light
  (jittered over a small solid angle → soft shadows), sample a cosine-hemisphere
  AO ray, and add IBL ambient by sampling the shared irradiance/prefilter maps
  (parity with raster). This is direct lighting + AO + IBL, not full bidirectional
  path tracing — enough for the CAD money shot, far simpler to get right.
- **Dispatch**: `vkCmdTraceRaysKHR` in batches (e.g. 32 samples/submit, default
  256 total) so the GPU isn't monopolized in one submit; progress reported
  between batches.
- Final: tonemap (ACES, shared with `mesh.frag`) + gamma the accumulation image
  into an 8-bit image, then reuse the `Screenshot.cpp` readback → PNG. Note this
  is the RT output image, **not** the raster render pass.

### R4.5 — The Render button + RPC (~0.5 day)
- Toolbar "Render" (Font Awesome camera) → modal: resolution, sample count,
  live progress bar, Cancel. Runs the trace, shows the result in a window, saves
  a PNG. Non-blocking-ish (batch between UI frames, or a simple modal "rendering…").
- Remote parity: `render_still(width, height, samples, path)` RPC — the LLM
  gets the money shot too ([scripting/plan.md](../scripting/plan.md)).

## Verification
- Validation-clean throughout (layer now installed).
- Build up: hardcoded triangle → one real body → full scene.
- Remote `render_still` smoke: render 64×64 / low samples, assert the PNG is
  non-black and (given a fixed seed + sample count) hashes deterministically.
- The showcase still: chrome + gold + black-plastic parts on the studio
  environment, compared against the raster viewport for lighting agreement.

## Risks & mitigations
- **Black image, no VUID** — the signature RT failure. Mitigate: validation on,
  triangle-first, and a debug raygen mode that writes the ray direction / a flat
  hit color to isolate AS-miss vs shading bugs.
- **AS build correctness** — device addresses, scratch alignment, instance
  transforms (row-major `VkTransformMatrixKHR` vs glm column-major — transpose).
  The most bug-prone step; budget accordingly.
- **Accumulation precision / seed** — use `rgba32f`; vary the RNG per sample and
  per pixel so batches don't correlate.
- Keep RT strictly on-demand and off the frame loop — no impact on the shipped
  raster path if RT misbehaves.

## Explicitly out of scope (→ 6b, later)
Real-time hybrid RT in the viewport (TLAS refit per edit, compositing into the
ImGui dock, resize). Gated on the `main.cpp` render-path cleanup + an
offscreen-viewport refactor. R4 deliberately avoids all of that by rendering
into its own image on demand.
