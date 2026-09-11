# Rendering & visual fidelity — plan

> **Status 2026-07-14: the showcase arc is SHIPPED** — R1 PBR materials
> (`d206a26`), R2 4×MSAA + ACES (`310b833`), R3 IBL from a studio HDR
> (`517cf51` + `d9a8272`), R4 offline path-traced render button with
> shadow-catcher ground plane (`c6b6fef` → `de3d896`), exposed as the
> `render_still` RPC + UI modal. Phase detail: [showcase-v1.md](showcase-v1.md)
> / [r4-offline-render.md](r4-offline-render.md). **Renderer instancing**
> (asset/instance split, the assemblies A2 gate) also shipped (`6a99c38`):
> [instancing-design.md](instancing-design.md). Assemblies A4 additionally
> gave the mesh renderer its **first transparency** (the ghost pipeline:
> alpha blend, depth-write-off second pass, per-instance alpha) — the
> pipeline glass now does reuse: a transmissive material is drawn there in
> the viewport and refracted in the still ([glass-plan.md](glass-plan.md)).
>
> **2026-07-19: SSAO + key-light soft shadows — SHIPPED**, closing items 3 and
> the soft-shadow half of 5 ([ssao-soft-shadows.md](ssao-soft-shadows.md)).
> S3's quality knobs shipped too, as *persisted user preferences*
> (`shadows` / `ssao` / `shadowMap` in `core/io/Settings.h`) rather than the
> session-only View menu the sub-plan proposed — see [../ui/settings.md](../ui/settings.md).
>
> **Remaining** (item numbers below): TAA (later, needs a G-buffer) and **6b
> real-time hybrid RT** — still gated on the `main.cpp` render-path cleanup
> (not started: `main.cpp` is ~1300 lines, no extracted `VulkanContext`; the
> 2026-08-24 architecture review extracted the *helpers* into
> `render/VulkanCommon` but deliberately not the structure). Of 6b's two
> preconditions, PBR (✓) is met; the cleanup is not.
>
> **2026-08-11: outline/wireframe interaction perf — SHIPPED.** Orbiting
> lagged on many-body assemblies (CPU-side per-frame silhouette recompute +
> mid-frame `vkDeviceWaitIdle` in `OutlineRenderer`). Fixed: precomputed
> edge adjacency (`OutlineEdges.{h,cpp}`) + flat settle-pass reclassify +
> per-frame-in-flight edge buffers — 131-mesh Waveshare assembly reclassifies
> in 0.8 ms, only on camera settle. Plan + numbers:
> [outline-perf.md](outline-perf.md).
>
> **2026-08-20: frames-in-flight + frame timing — SHIPPED.** Rotating the
> camera wobbled on the Intel GPU because per-frame GPU resources were sized
> to 2 while Mesa gives 3 swapchain images
> ([frames-in-flight.md](frames-in-flight.md)). And the class of question
> behind it — "is this frame slow because of us or because we are waiting?" —
> is now answerable: a phase split (update | ui | wait | record | present)
> with avg/p50/p95/max, reachable by `--perf-hud`, Ctrl+Shift+F or the
> `frame_stats` RPC, and **off by default**
> ([frame-timing.md](frame-timing.md)).

Most open-source CAD looks cheap, and that alone makes it unpleasant to use. The fix is
mostly *not* raytracing — it's a modern raster pipeline done properly. Ordered by
perceptual-win-per-unit-effort, so the cheap high-impact work lands before the expensive
work. Everything through 6.5 works on any GPU; 6.6 needs RT hardware.

~~Prerequisite: real vertex normals~~ — **done** (shipped with the face-selection fix):
`CADKernel` tessellation emits exact B-Rep surface normals with a triangle-average fallback,
and `CADMeshConverter` passes them through. (The albedo-guess shading this paragraph
originally described was replaced by real per-body materials in R1.)

1. ~~**Anti-aliasing**~~ — **SHIPPED** (R2, `310b833`): 4× MSAA through the shared
   `main.cpp` pass + every renderer's pipelines. TAA remains a later option if/when a
   G-buffer exists.
2. ~~**PBR materials + sane defaults**~~ — **SHIPPED** (R1, `d206a26`): metallic-roughness
   in `mesh.frag` fed by real per-body `CADMaterials` properties (the albedo-guess
   heuristic is gone); per-instance since the instancing split (material rides push
   constants, `SetInstanceMaterial`).
3. ~~**SSAO**~~ — **SHIPPED** (S2, `03c5db6`): contact shadows in corners and crevices,
   from a depth+normal prepass. `SSAORenderer`; quality is a persisted preference
   (off | half | full). The readability win it promised, and the reason a mechanical
   part now reads as solid.
4. ~~**Tone mapping (ACES)**~~ — **SHIPPED** (R2): ACES filmic (Narkowicz fit) in
   `mesh.frag`, highlights roll off instead of clipping.
5. ~~**IBL environment + soft shadows**~~ — **SHIPPED**, in two halves. R3
   (`517cf51`+`d9a8272`): diffuse irradiance + prefiltered specular + BRDF LUT precomputed
   from a bundled studio HDR, sampled in `mesh.frag` (split-sum). S1 (`5f4ac15`):
   PCF soft-shadow maps from the key light (`ShadowRenderer`, 1024/2048 by preference).

   > 6.1–6.5 together get ~80% of the "whoa, this looks good" reaction at a fraction of the
   > cost and risk of raytracing, and they work everywhere. Do them first.

6. **Raytracing** — the cherry on top, not the foundation. Two deliverables, in order:
   - ~~**6a. Offline "render" button**~~ — **SHIPPED** (R4, `c6b6fef` → `de3d896`):
     path-traced still with accumulation, real materials, environment lighting, and a
     shadow-catcher ground plane; synchronous `render_still` RPC (reproducible via a
     fixed seed) + a progress-modal Render button in the UI. See
     [r4-offline-render.md](r4-offline-render.md).
   - **6b. Real-time RT lighting (hybrid)** — RT shadows/AO/reflections composited under the
     raster viewport. Higher risk. The scaffolding does most of the plumbing; the hard,
     un-built 30% is: keep the **TLAS refit/rebuilt on every geometry edit** (extrude, sketch
     edit, regen); composite the RT storage image into the ImGui viewport; handle resize;
     reconcile with the color+depth pass and the raster overlays. Budget a few focused days
     to first correct image, more to make it robust across the edit loop.

   **Not full-screen / pure path tracing.** That's the right call for a voxel engine (uniform
   grid → trivial DDA ray-march, no BVH, globally consistent lighting for free) but not for
   B-Rep: our geometry is triangle meshes needing a BVH (the `VK_KHR_acceleration_structure`
   path), and our 2D overlays (sketch lines, gizmo, edge outlines, selection, dimensions) are
   cheap in raster and awkward in a pure-RT pipeline. **Hybrid** — raster for interaction +
   overlays, RT for lighting — is what every DCC/CAD tool does, for that reason.

   Preconditions before 6b is worth starting: PBR (6.2) so RT and raster shading agree, and
   the `main.cpp` render-path cleanup below (RT adds a lot of GPU state to a path whose
   device-lost history shows it's already touchy about in-flight resources).

