# Designer Roadmap

## Vision

An open-source parametric CAD application in the spirit of Onshape/SolidWorks, with four
differentiating goals:

1. **Transformation checkpoints** — named points in the model's regeneration history that
   can be inspected, rolled back to, and branched from.
2. **Variables and expressions** — dimensions defined as expressions over global parameters
   (`length = width * 2`).
3. **Scripting** — a Python REPL / API that can do everything the GUI can.
4. **Git-based versioning** — models stored in a diffable, mergeable text format.
5. **LLM-friendly by design** — AI assistants use the *same interface as humans*: the same
   command layer, the same document format, the same feedback (queries, screenshots). This
   helps both in developing the application and in future AI-assisted modeling.

### The one architectural decision behind all of it

Goals 1–5 are all the same decision: a **headless core with a serializable command/document
layer**, where the GUI is just one client.

- Every UI action becomes a *command* that edits a *document* (a feature list), never the
  geometry directly.
- The document regenerates the B-Rep deterministically from scratch.
- The document serializes to human-readable text.

From this fall out: undo (command replay/inversion), checkpoints (points in the feature
history), variables (an expression pass during regeneration), scripting (the command layer
*is* the API), git (text format), LLM interface (same command layer + headless mode), and
testability (geometry ops unit-testable without a GUI).

## Current state (July 2026)

Working: STEP import → OpenCASCADE tessellation → Vulkan rasterizer (mesh + outlines),
camera controls, materials. CADKernel has primitives, booleans (not exposed in UI), STEP/STL I/O.

Not working / partial:
- Face selection: picking ray is vertically mirrored vs. the display (missing Vulkan Y-flip
  in the render path), and tessellation stores no B-Rep-face → triangle mapping, so a "face"
  is a single triangle. Highlighting is a stub.
- Sketching: plane/UV scaffolding and element input exist, but sketches are never rendered
  and nothing consumes them (no extrude).
- Vertex normals hardcoded to (0,0,1) — shading is fake.
- No feature history, variables, undo, scripting, or tests.
- Dead code: `Application`, `SimpleApp`, `Viewport`, `ColorPicker`, `FaceHighlighter`;
  1600-line `main.cpp` with global state; Vulkan boilerplate copy-pasted across renderers.
- Rendering is flat: hardcoded normals, no AO/shadows/anti-aliasing/tone-mapping, a single
  directional light. `RayTracingRenderer` (BLAS/TLAS, RT pipeline, SBT, `vkCmdTraceRaysKHR`)
  and its shaders are fully scaffolded and the device enables the RT extensions, but
  `Render()` is never called and its output is never composited — the viewport is 100%
  rasterized (`MeshRenderer` + `OutlineRenderer` + `SketchRenderer`). This is the Phase 6
  starting point, not dead weight.

## Short term

1. ~~**Restore & stabilize the build**~~ *(done — submodules, documented apt packages;
   CI still open)*
2. ~~**Testing infrastructure**~~ *(done — Catch2 unit tests against `designer_core`,
   GUI tests via ImGui Test Engine, all under ctest)*
3. **Purge dead code** — delete the uncompiled leftovers, the duplicated View menu, the
   committed test binary. *Keep* `RayTracingRenderer` — it's non-functional today but is the
   seed of Phase 6; see there.
4. ~~**Fix face selection**~~ *(done — consistent Vulkan projection in camera+picker, real
   B-Rep face mapping with surface normals, per-face highlight via push constants, viewport
   input through ImGui so tests/automation can drive orbit/pan/zoom/select)*
5. ~~**Render sketches + implement extrude**~~ *(done — SketchFeature + ExtrudeFeature
   (new body / add / cut), sketch rendering, plane/face picking, the full
   sketch → extrude → sketch-on-face loop)*
6. **Sketch constraints** (next) — PlaneGCS for coincident/parallel/dimension.

## Long term

### Phase 1 — Parametric document model (the heart)
A feature list (sketch, extrude, boolean, transform, …) that regenerates the B-Rep
deterministically, serialized as human-readable text (JSON or a small DSL). All UI actions
route through commands. Gives undo, checkpoints, and a diffable format immediately.

*Largely built:* `src/core/` has `Document` (feature list + deterministic regeneration,
stable feature ids, `RegenContext` for feature→feature references), features (Box, Sphere,
Cylinder, STEP Import, **Sketch**, **Extrude** with new-body/add/cut), `Command`/`CommandStack`
(undo/redo incl. `ModifyFeatureCommand`), and JSON project serialization v2 (`DocumentIO`).
The UI is a docking workbench (`src/ui/`): Model panel with a Default-geometry section and
feature history, Properties with **live parameter editing**, a `SketchRenderer` (lines +
translucent plane quads), and full sketch interaction (pick plane/face → draw → extrude).
The complete Onshape loop works: sketch → extrude → sketch on a face → cut. Remaining before
this phase is "done": persistent references (**topological naming**) so a sketch on a face
follows that face when upstream edits move it — currently planes are baked at creation.

⚠ Hard problem to design for up front: **topological naming** — how "the face I filleted"
stays identified after an upstream edit reorders faces. Study FreeCAD's RealThunder branch
and SolveSpace before designing persistent references. Getting this wrong is the #1 way
parametric kernels die.

### Phase 2 — Variables & expressions
Dimensions as expressions over named parameters, evaluated during regeneration. Cheap once
Phase 1 exists, nearly impossible before it.

### Phase 3 — Scripting + the LLM interface
Expose the command layer as an API: embedded Python REPL and/or an MCP server with commands
like `create_sketch`, `add_line`, `extrude`, `query_faces`, plus screenshot/render feedback.
Humans and LLMs become genuinely equivalent clients; every scripted session is a replayable
test case. Headless mode makes geometry operations unit-testable.

*Started early:* `--remote <port>` serves JSON-RPC (see README): semantic feature/document
commands + human-parity UI actions through the test engine + live screenshots.
`tools/designer_remote.py` is the client. Remaining: MCP wrapper, Python REPL, headless mode.

### Phase 4 — Git integration
With a text format this is mostly product design: repo-per-project, feature-level merging
(consider one file per feature/part — feature lists merge far better than monolithic files),
UI for history and branches.

### Phase 5 — Sketch constraints
A real constraint solver (coincident, parallel, dimensions) is what makes it feel like
Onshape. Don't write one — embed **PlaneGCS** (FreeCAD's solver, LGPL) or SolveSpace's
solver as a library.

*Done (v1):* PlaneGCS vendored (`deps/planegcs`, LGPL) + a `SketchSolver` adapter. Constraints
(coincident, horizontal/vertical, parallel, perpendicular, equal, distance, radius) in the
sketch model, serialized (format v3). Sketch-entity picking + drag-to-resolve; a constraints
toolbar. Solve runs in the interaction layer (never in Regenerate). Remaining: angle
constraints, arcs, auto-constraint inference, whole-line/circle dragging.

### Phase 6 — Rendering & visual fidelity
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

### CAD UI (done)
Traditional-CAD redesign on the dark theme: icon toolbar (Font Awesome), **preview →
accept/cancel** feature creation (green ✓ / red ✗, Onshape-style), default planes visible by
default and auto-hidden with geometry, world-origin gizmo. The 3D viewport renders into the
central dock node (centered between panels).

### Continuous
Incrementally dismantle `main.cpp` and the tripled Vulkan boilerplate (extract a
`VulkanContext` + shared renderer utilities) — opportunistically while touching code, not as
a big-bang rewrite. The prototype rasterizer is good enough to carry the parametric core work.
