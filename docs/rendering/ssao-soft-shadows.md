# SSAO + key-light soft shadows

Status: DRAFT — awaiting sign-off.

Scope: the two raster-polish items left open by the showcase arc
([plan.md](plan.md) items 3 and the soft-shadow half of 5). Per
[roadmap.md](../roadmap.md), the `main.cpp` render-path cleanup gates **only**
real-time hybrid RT (6b) — this work is explicitly *not* gated on it, and the
design below is shaped so it doesn't accidentally become a dismantle-main.cpp
project.

---

## Where we actually are

- **One render pass for everything.** `main.cpp` overrides ImGui's color-only
  swapchain pass with `g_DepthRenderPass`: MSAA color (transient, up to 4×) +
  MSAA `D32_SFLOAT` depth + resolve-to-backbuffer, one subpass. `FrameRender`
  records Mesh → Field → Outline → Sketch → ImGui inline. The depth attachment
  is `STORE_OP_DONT_CARE` and multisampled — **nothing can sample scene depth
  today**, which is the structural gap SSAO has to fill.
- **A key light already exists.** `mesh.frag` runs three analytic directionals
  (primary/fill/rim, hard-coded in `MeshRenderer::UpdateCamera`, world-space —
  primary is `normalize(1, 1.5, 1)` at intensity 1.8) at 0.5× on top of the R3
  IBL split-sum ambient. "Key-light shadows" therefore means **shadowing the
  existing primary light** — no new light needs inventing; fill/rim/IBL stay
  unshadowed (they are the soft fill that keeps shadows from going black).
- **No material AO exists** — materials are four scalars (metallic/roughness/
  specular/reflectance). SSAO is the *only* AO term; it multiplies the
  ambient/IBL contribution, nothing to composite with.
- **Cross-cutting state that new passes must respect:** the section clip plane
  (`fragClipDist` discard, shared by Mesh/Field), per-instance ghost
  transparency (alpha-blend, depth-write-off second pass), and the
  `UniformBufferObject` std140 convention (mat4/vec4 members only — the
  float-then-vec4 trap documented in `OutlineRenderer.h`).
- **Screenshot path:** `CaptureScreenshot` re-records the same renderer
  callbacks into its own MSAA framebuffer + readback (dev-watch / MCP
  `screenshot`). Headless runs have no renderer at all, so nothing there moves.

## Design

Two new self-contained renderer classes following the existing pattern
(`Initialize(device, physDevice, renderPass, queue, queueFamily, commandPool,
samples)` / `Render(cb, frame)` / `Shutdown`): **ShadowRenderer** and
**SSAORenderer**. `main.cpp` only gains pass sequencing + barriers before
`vkCmdBeginRenderPass`; the touchy swapchain/present code is untouched. Plain
fragment/depth passes — no extensions beyond core 1.0-level features, so the
same device support as today.

Per-frame order becomes:

1. **Shadow pass** (ShadowRenderer): depth-only pass into a 2048² `D32_SFLOAT`
   shadow map from the primary light's direction. Cascade-free: CAD scenes are
   compact, so fit an ortho frustum to the scene bbox every frame (the same
   fit-to-document math the aspect-aware camera fit already does) — no
   peter-panning from slack far planes, no cascades to tune. Vertex-only shadow
   variant of `mesh.vert` reusing the instance loop + push-constant model
   matrix; applies the clip-plane discard (sectioned-away material must not
   cast) and **skips ghost/transparent instances** (they don't write depth in
   the main pass either).
2. **Depth+normal prepass** (SSAORenderer, non-MSAA): full-res `D32` depth +
   `RGBA8` view-space normal color target, opaque mesh instances only, same
   clip discard, same ghost skip. Recommended over resolving the main pass's
   depth: that depth is never stored, depth resolves need
   `VK_KHR_depth_stencil_resolve`/renderpass2 churn in exactly the file we're
   avoiding, and SSAO wants normals anyway. Cost is a second geometry pass, but
   it's vertex-bound and CAD meshes are small; this is also the seed of the
   G-buffer TAA will eventually want.
3. **SSAO + blur** (SSAORenderer): half-res `R8` AO — hemisphere kernel, ~16
   samples, 4×4 tiled rotation noise — then a depth-aware bilateral blur to
   kill the noise pattern without bleeding across edges. Fullscreen-triangle
   fragment passes.
4. **Main pass** unchanged in structure. `mesh.frag` gains: set 0 binding 4 =
   AO texture (sampled at gl_FragCoord × scale, multiplied into the
   ambient/IBL term only — direct light is occluded by the shadow map, not
   AO), binding 5 = shadow map (`sampler2DShadow`, 3×3 PCF for the soft
   penumbra + normal-offset bias against acne) attenuating **only the primary
   light's** contribution. UBO grows `mat4 lightViewProj` + one `vec4` of
   params (AO strength, texel size, toggles) — std140-clean by construction.

Because AO/shadow are applied *inside* `mesh.frag`, nothing composites over
the other pass consumers: outlines, sketch overlay, FieldRenderer, gizmos and
ImGui are untouched. The FEM field overlay gets no AO/shadows (it isn't in the
prepass) — acceptable; it's a false-color plot, not a beauty render. Ghosts
receive shadows/AO on surfaces behind them but neither cast nor occlude.

**MSAA interaction:** AO is computed at 1× from the prepass and sampled by
MSAA fragment invocations. Silhouette-edge AO can alias slightly; the
bilateral blur plus the fact that AO modulates only ambient makes this
invisible in practice, and it avoids per-sample SSAO entirely.

**Controls:** View menu toggles for SSAO and Shadows (default on) + a quality
setting (Off / Half-res / Full-res SSAO; shadow map 1024/2048). Session/ini
state only — **no file-format change, no golden bumps**.

**Screenshot path:** hoist steps 1–3 into a `RecordScenePrepasses(cb)` helper
called both by `FrameRender` and inside `CaptureScreenshot`'s command buffer
before its render pass begins, so remote screenshots (dev-watch, MCP) show
identical AO/shadows. `render_still` (offline PT) already has real shadows —
no interaction.

**Resize/rebuild:** prepass + AO targets are recreated alongside
`SetupDepthBuffer`'s targets on swapchain rebuild; the shadow map is
resolution-independent and survives. All destruction behind wait-idle — the
OutlineRenderer in-use-buffer device-lost (`1422695`) is the cautionary tale.

## Risks

- **Render-graph churn in `main.cpp`.** The file's device-lost history says
  it's touchy about in-flight resources. Mitigation is the shape of the
  design: all new VkImages/pipelines/descriptors live inside the two renderer
  classes; `main.cpp` adds ~30 lines of pass recording + two image barriers
  (shadow/AO write → fragment sample) and target recreation on resize. No
  `VulkanContext` extraction required; if this still turns into whack-a-mole,
  `DEV_WATCH_VALIDATE=1` sync validation is the proven diagnostic.
- **Shader divergence:** the shadow/prepass vertex shaders duplicate
  `mesh.vert`'s instance transform + clip logic; a comment pins them together
  (same convention as the IBL UV mapping pinned to `IBL.cpp`).
- **Bias tuning** (acne vs peter-panning) is empirical — budget a dev-watch
  screenshot loop for it; normal-offset bias makes it a one-knob problem.

## Tests

- **Headless-assertable:** shaders compile at build time (glslc, like every
  existing shader); `gui_smoke` (`designer --gui-tests`, hidden windows, real
  GPU) exercises `Initialize` of both new renderers — pipeline/pass creation
  failures fail the suite. Add a GUI test that toggles SSAO/shadows and the
  section plane through a few frames and asserts the app survives (no
  device-lost, screenshot RPC still succeeds) — this catches barrier/layout
  bugs, which is where Vulkan work actually breaks.
- **Screenshot-verified (dev-watch, port 8799):** AO visible in pocket
  corners, shadow contact under a part, penumbra softness, no acne at default
  bias, section view casting correctly, ghosts unshadowed — visual review, not
  asserts.
- **Perf budget:** stays interactive at 4K. Measure frame time on the bracket
  demo assembly before/after; expected cost ≈ one extra geometry pass + two
  half-res fullscreen passes + a 2048² depth pass — well under 2 ms on the
  dev GPU. Off-toggles double as the escape hatch if a weak GPU disagrees.

## Order & effort

1. **S1 Shadow map** (~1 session): ShadowRenderer + UBO/`mesh.frag` change.
   Smallest item, and it debugs the "new pass before the main pass" plumbing
   (barriers, resize, screenshot hook) that SSAO then reuses twice.
2. **S2 SSAO** (~1–2 sessions): prepass + AO + blur + `mesh.frag` term.
3. **S3 polish** (~half session): View-menu toggles/quality, bias/strength
   tuning via dev-watch, perf numbers recorded here.

Both items are independent of the hybrid-RT track and leave TAA's G-buffer
seed (the depth+normal prepass) behind as a bonus.
