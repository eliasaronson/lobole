# Rendering showcase v1 — materials → MSAA/ACES → IBL → offline render button

Implementation plan for the first arc of [plan.md](plan.md): items 2 (PBR
materials), 1 (MSAA), 4 (tone mapping), 5 (IBL), and 6a (offline raytraced
render). Goal: screenshot-worthy renders on demand, with a visible win at the
end of every phase. Explicitly **not** in scope: 6b real-time hybrid RT
(gated on this arc + the `main.cpp` render-path cleanup) and surface textures
(triplanar — deferred; see the OpenSCAD/texture discussion in scripting/UI
plans).

## Code facts this plan is grounded in (verified July 2026)

- Real surface normals already ship: `CADKernel.cpp` emits exact B-Rep normals
  with a triangle-average fallback; `CADMeshConverter` copies them through.
- `mesh.frag` already does three lights + Blinn-Phong + Fresnel + a
  Reinhard-ish curve — but **metallic/roughness are guessed from albedo
  brightness/hue**. Albedo arrives as per-vertex color (`mesh->color` →
  `vertex.color`); push constants carry only the highlight range
  (`MeshRenderer.cpp:248`).
- `CADMaterials.h` already defines `MaterialProperties { baseColor, metallic,
  roughness, specular, reflectance, name }` and a 16-entry `MaterialLibrary`
  (aluminum → gold → plastic → glass …) — unplumbed.
- Everything is single-sample (`VK_SAMPLE_COUNT_1_BIT` in the `main.cpp` pass,
  `MeshRenderer`, and ImGui init). The scene renders directly into the
  swapchain backbuffer + shared D32 depth (`main.cpp:438`), same pass as ImGui.
- RT: all four RT shaders **are compiled** by CMake (`raygen.spv` etc. →
  `libraries/shaders/`), `RayTracingRenderer::Initialize()` runs at startup,
  and the RT pipeline + SBT creation code is real. The stubs are: `AddGeometry`
  (no GPU buffers), `CreateBottomLevelAS`/`CreateTopLevelAS` (structs filled,
  `vkBuildAccelerationStructuresKHR` never called), `CreateRTDescriptorSets`
  (one log line; no output image, no camera UBO), `Render()` (empty), and no
  caller anywhere.
- `--remote` already renders offscreen screenshots windowless
  (`Screenshot.cpp`) — the natural output path for the render button.
- Hardware: RTX 4060 Laptop (RT cores) **plus** an Intel Arc iGPU that also
  advertises the RT extensions — device selection must prefer the discrete
  NVIDIA GPU explicitly (verify in R4.0; an iGPU pick would work but crawl).

## Phase R1 — Real materials (~1.5–2 days) — SHIPPED (`d206a26`)

Replace the color-guess with the library that already exists. Shipped as
"feat: per-feature PBR materials": material slug on `Feature` (serialized v10),
stamped onto bodies during regen; `mesh.frag` uses real metallic/roughness +
reflectance-driven dielectric F0 (fragment-stage push constant); Properties
combo + `set_material` RPC + `get_state` body material; unit + remote_smoke
tests; chrome-vs-plastic verified visually. One scope note: albedo comes from
`MaterialProperties::baseColor` on assignment (custom per-body color override
is a later nicety, not needed for R1).

- **Model**: a feature/body gains a `materialType` (library enum, serialized as
  its name string; format v10 with default `DEFAULT` on load). Assignment is a
  `ModifyFeatureCommand` → undoable, regen-safe.
- **Renderer**: extend the mesh push constants with a
  `vec4(metallic, roughness, specular, reflectance)`; `mesh.frag` drops the
  brightness/hue heuristics and uses them. Albedo stays per-vertex color
  (fed from `MaterialProperties::baseColor` unless the user set a custom
  color).
- **UI**: material combo (the 16 library entries, grouped Metals/Other) in
  `PropertiesPanel`; live preview via the existing preview→accept flow.
- **Remote parity**: `set_material(feature_id, name)` RPC + `remote_smoke`.
- **Curate**: sanity-pass the library values against PBR references (metals:
  albedo ≈ F0, roughness 0.1–0.4; plastics: albedo + metallic 0) — the values
  predate the shader that will finally read them.
- Tests: serialization round-trip; GUI test assigns steel and asserts the
  feature's material; visual eyeball via `/run`.

## Phase R2 — MSAA + ACES (~1–1.5 days) — SHIPPED (`310b833`)

The #1 "looks cheap" fix and the "looks finished" curve. Shipped as "feat: 4x
MSAA + ACES tone mapping": whole-pass 4x MSAA with a resolve to the backbuffer
(sample count from device limits, 1x fallback), sample count threaded into every
scene pipeline + ImGui + the offscreen screenshot pass, ACES filmic curve in
`mesh.frag`. Before/after edge crops confirm the staircase is gone. Two notes:
(1) the render pass keeps color=0/depth=1 so FrameRender's clears don't branch,
resolve appended at index 2; (2) validated clean — after installing
`vulkan-validationlayers` and clearing the pre-existing errors (see below), a
validated run reaches the render loop and screenshots with **zero** VUIDs,
confirming the MSAA BeginRenderPass → resolve → present → screenshot path.

**Validation errors fixed alongside R2** (`1b9c7f4`, all pre-existing, none from
materials/MSAA): the app was requesting Vulkan 1.0 implicitly → bumped
`VkApplicationInfo::apiVersion` to 1.2 (promotes the RT extensions' dependency
extensions to core + legalizes the SPIR-V 1.4 RT shaders); enabled
`fillModeNonSolid` for the wireframe pipeline; allocated the RT SBT buffer with
`VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`. Note: the app's own debug-report path is
still gated behind `#ifdef _DEBUG` (an MSVC-ism GCC never defines) — force the
layer with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` until CMake defines
`APP_USE_VULKAN_DEBUG_REPORT` for Debug builds.

- **MSAA 4×, whole-pass**: the swapchain render pass gains multisampled
  color + depth attachments with a resolve to the backbuffer
  (`main.cpp` pass + framebuffer + shared depth setup); every pipeline sets
  the sample count (`MeshRenderer`, `OutlineRenderer`, both sketch pipelines,
  wireframe, ImGui `init_info.MSAASamples`); `Screenshot.cpp`'s offscreen pass
  gets the same treatment so screenshots match the viewport. 4× is universally
  supported; query-and-clamp anyway.
  *Not* doing the render-to-offscreen-viewport-image refactor here — that's
  the 6b-era compositing change; whole-pass MSAA is the one-day version.
  ⚠ This touches the exact in-flight-resource territory with the device-lost
  history — build with validation layers on, test resize + dock/undock +
  screenshot early.
- **ACES**: replace the `x/(x+1.2)` curve in `mesh.frag` with the ACES fitted
  polynomial (keep the gamma step). A `sketch_prim`/overlay check that nothing
  double-tonemaps.
- Tests: full `gui_smoke` green (it exercises resize/screenshot paths);
  validation-layer-clean run; before/after screenshots via the remote for the
  eyeball diff.

## Phase R3 — Image-based lighting (~2–3 days)

The "premium render" jump; also builds the codebase's first texture path.

- **Texture infrastructure** (new, reusable): HDR load (`stb_image` — already
  vendored? if not, single header), `VkImage` + view + sampler creation with
  mips, staging upload. Lives beside the renderers as shared utility (a first
  brick of the eventual `VulkanContext` extraction).
- **Precompute** (startup, one-time): from one bundled studio HDRI (Poly Haven,
  CC0 — bundle 1–2 neutral ones) derive the classic split-sum set: 32×32
  diffuse irradiance cubemap, prefiltered specular mip chain, BRDF LUT.
  Compute shaders at startup (~ms, no offline tooling to maintain).
- **Shading**: `mesh.frag` gains three sampler bindings; the flat
  `fragAmbientColor` term becomes irradiance-based diffuse + prefiltered
  specular (the three analytic lights stay as key/fill/rim). Background stays
  the neutral gradient — IBL lights the part, no skybox (CAD-typical, less
  work).
- Tests: validation-clean; gui_smoke green; the money shot: chrome + brushed
  steel + black plastic side by side via the remote screenshot.

## Phase R4 — Offline render button (RT still) (~4–5 days)

Finish the scaffold into a KeyShot-style "Render" button. Raster viewport is
untouched — RT runs on demand into the offscreen path.

- **R4.0 Ground truth (half-day)**: partly done during R2's validation pass —
  `Initialize()` succeeds (RT pipeline + SBT create) and the SBT/device-address
  + extension-dependency validation errors are already fixed (`1b9c7f4`).
  Remaining: confirm the app picks the RTX 4060 and not the Arc iGPU (fix device
  selection to prefer a discrete RT-capable device), and audit the SBT region
  math against the spec (stride/size alignment) — it was written blind and has
  never executed a trace.
- **R4.1 Geometry upload (1 day)**: make `AddGeometry` real — vertex/index
  buffers with `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY` +
  `SHADER_DEVICE_ADDRESS` usage, fed from the same tessellated meshes the
  raster path uses; a `SyncFromScene()` that mirrors current bodies +
  transforms + materials (`RTMaterial` ← `MaterialProperties`, mapping from
  R1).
- **R4.2 Acceleration structures (1–1.5 days, the hard part)**: real BLAS
  build (sizes query → AS buffer + scratch → build submit → compaction later),
  one BLAS per body, TLAS over per-body instances. Rebuild-on-demand only
  (the button), no per-edit lifecycle — that's 6b.
- **R4.3 Descriptors + output (0.5–1 day)**: storage image at render
  resolution, camera UBO, material/vertex/index storage buffers, descriptor
  pool + set against the existing layout (extend the layout with the material
  buffer binding the closest-hit shader needs).
- **R4.4 Trace + accumulate (1 day)**: implement `Render()` —
  `vkCmdTraceRaysKHR` with barriers, progressive accumulation (sample index
  via push constant, jittered camera rays, cosine-hemisphere AO + soft shadow
  rays toward the key light in `raygen`/`closesthit` — direct lighting + AO,
  not full path tracing, matching the raster look but soft). N samples
  (default 256) submitted in batches so the GPU isn't hogged in one submit.
- **R4.5 The button (0.5 day)**: toolbar "Render" → modal with resolution /
  samples / progress → runs the trace → tonemapped result shown in a window +
  saved PNG (through `Screenshot.cpp` plumbing). Remote parity:
  `render_still(width, height, samples)` RPC — the LLM gets the money shot
  too ([scripting/plan.md](../scripting/plan.md)).
- Tests: a headless/remote render smoke (render 64×64, assert non-black and
  deterministic hash given fixed samples); validation-clean; the showcase
  render of a real part.
- **Risks**: first-light debugging dominates (black image with no validation
  errors is the classic RT failure mode — mitigate by starting from R4.0's
  audit and tracing one hardcoded triangle before the full scene); driver
  variance between the two GPUs (pin to NVIDIA).

## Order & payoff

R1 → R2 → R3 → R4, ~9–12 focused days total. Each phase ends demoable:
R1 "parts have real materials", R2 "no more jaggies + finished-looking
highlights", R3 "product-render lighting", R4 "the money shot button".
R1 must precede R4 (RT shading reads the same materials); R2/R3 are
independent of R4 but make its output (and every screenshot) better.

After this arc, 6b (real-time hybrid RT) remains in [plan.md](plan.md), gated
on the `main.cpp` render-path cleanup + the offscreen-viewport refactor noted
in R2.
