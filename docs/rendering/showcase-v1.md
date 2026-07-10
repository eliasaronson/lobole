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

## Phase R3 — Image-based lighting (~2–3 days) — SHIPPED (`517cf51`, IBL precompute; + integration)

The "premium render" jump; also builds the codebase's first texture path.

**Shipped**, with one deliberate deviation from the outline below: the precompute
runs on the **CPU** (`IBLPrecompute`, in `designer_core`), not GPU compute. No
compute-pipeline infra existed and CPU keeps the Vulkan surface minimal + the
split-sum math unit-testable and PPM-dumpable. Multithreaded, with a binary disk
cache so the multi-second convolution runs once per HDR. `IBLTextures` uploads
the maps as `R16G16B16A16_SFLOAT` / `R16G16_SFLOAT` sampled textures (env sampler
wraps u / clamps v for the equirect back-seam; LUT clamps both), and `mesh.frag`
does the split-sum ambient (irradiance·albedo diffuse + prefilter·(F·A+B)
specular), with the analytic key/fill/rim lights dialed to 0.5 on top. Bundles
`brown_photostudio_02_1k.hdr` (Poly Haven, CC0). Verified: `test_ibl` (BRDF
corner ≈ (1,0), bright irradiance, decreasing mips) + dumped maps eyeballed +
a validated render (0 VUIDs) showing gold/chrome picking up the environment.
GPU-compute prefiltering remains a later optimization.

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

## Phase R4 — Offline render button (RT still) (~4–6 days) — PLANNED

Finish the scaffold into a KeyShot-style "Render" button; the raster viewport is
untouched. **Detailed, code-grounded plan: [r4-offline-render.md](r4-offline-render.md)**
(R4.0 ground truth → R4.1 geometry upload → R4.2 BLAS/TLAS → R4.3 descriptors +
`rgba32f` accumulation image → R4.4 trace/accumulate → R4.5 button + `render_still`
RPC).

Prerequisites already met by R1–R3 + the validation pass: Vulkan 1.2 / SBT
device-address / extension-dep VUIDs fixed (0 VUIDs at startup, `1b9c7f4`);
discrete-GPU selection (RTX 4060); `RTMaterial` ← `MaterialProperties` (R1); and
— new — the RT closest-hit can sample the **same IBL maps** (R3) for ambient, so
the offline render and the raster viewport agree on lighting. The hard part is
still the acceleration-structure build; first-light debugging (black image, no
VUID) dominates the risk, so the plan builds up from one triangle.

## Order & payoff

R1 → R2 → R3 → R4, ~9–12 focused days total. Each phase ends demoable:
R1 "parts have real materials", R2 "no more jaggies + finished-looking
highlights", R3 "product-render lighting", R4 "the money shot button".
R1 must precede R4 (RT shading reads the same materials); R2/R3 are
independent of R4 but make its output (and every screenshot) better.

After this arc, 6b (real-time hybrid RT) remains in [plan.md](plan.md), gated
on the `main.cpp` render-path cleanup + the offscreen-viewport refactor noted
in R2.
