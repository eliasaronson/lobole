# Rendering & visual fidelity — plan

Most open-source CAD looks cheap, and that alone makes it unpleasant to use. The fix is
mostly *not* raytracing — it's a modern raster pipeline done properly. Ordered by
perceptual-win-per-unit-effort, so the cheap high-impact work lands before the expensive
work. Everything through 6.5 works on any GPU; 6.6 needs RT hardware.

Prerequisite for all of it: **real vertex normals**. Shading is fake today (normals
hardcoded to `(0,0,1)` in the mesh path). OpenCASCADE tessellation already provides per-face
normals — plumb them through `CADMeshConverter` → `MeshRenderer` before anything below will
look right.

1. **Anti-aliasing** — the #1 "this looks cheap" tell. Start with MSAA (a render-pass +
   pipeline change; note the current pass is color+D32 single-sample, so this touches the
   shared depth-buffer setup in `main.cpp`). TAA later if/when a G-buffer exists.
2. **PBR materials + sane defaults** — metallic-roughness model in `mesh.frag`, a small
   material library that reads as real metal/plastic. Replaces the current flat Lambert-ish
   shading. Ties into `CADMaterials`.
3. **SSAO** — contact shadows in corners and crevices. Enormous readability win for
   mechanical parts specifically (you *see* the geometry). Needs a depth (and ideally normal)
   prepass to sample against.
4. **Tone mapping (ACES)** — a final full-screen pass so highlights roll off instead of
   clipping to flat white. Cheap, and it's what makes everything else look "finished."
5. **IBL environment + soft shadows** — a studio HDRI for image-based lighting (diffuse
   irradiance + prefiltered specular) plus soft-shadow maps from the key light. This is the
   "premium product render" look. Bundle one or two neutral studio HDRIs.

   > 6.1–6.5 together get ~80% of the "whoa, this looks good" reaction at a fraction of the
   > cost and risk of raytracing, and they work everywhere. Do them first.

6. **Raytracing** — the cherry on top, not the foundation. Two deliverables, in order:
   - **6a. Offline "render" button** — a present-quality still (path-traced soft shadows,
     AO, reflections, real materials) triggered on demand, output through the existing
     offscreen path (`Screenshot.cpp`). Low risk: reuses `RayTracingRenderer` *without*
     touching the interactive loop. This is the KeyShot-style money shot.
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

