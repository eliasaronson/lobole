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
- Dead code: `Application`, `SimpleApp`, `Viewport`, `ColorPicker`, `FaceHighlighter`,
  non-functional `RayTracingRenderer`; 1600-line `main.cpp` with global state; Vulkan
  boilerplate copy-pasted across renderers.

## Short term

1. **Restore & stabilize the build** — submodules for all vendored deps, documented apt
   packages, ideally CI that builds on a clean machine. *(in progress)*
2. **Purge dead code** — delete the uncompiled leftovers, the duplicated View menu, the
   committed test binary; park or delete `RayTracingRenderer`.
3. **Fix face selection** — the gateway feature; everything downstream (sketch-on-face,
   extrude, constraints, an LLM clicking on geometry) depends on it:
   - Apply the Vulkan Y-flip / 0–1 depth convention once, consistently, in renderers and picker.
   - Tag triangles with their `TopoDS_Face` ID during tessellation; compute real normals
     from the triangulation while there.
   - Implement real per-face highlighting.
4. **Render sketches + implement extrude** (`BRepPrimAPI_MakePrism` on a closed profile) —
   closes the first sketch → solid loop and forces the document-model design at the right time.

## Long term

### Phase 1 — Parametric document model (the heart)
A feature list (sketch, extrude, boolean, transform, …) that regenerates the B-Rep
deterministically, serialized as human-readable text (JSON or a small DSL). All UI actions
route through commands. Gives undo, checkpoints, and a diffable format immediately.

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

### Phase 4 — Git integration
With a text format this is mostly product design: repo-per-project, feature-level merging
(consider one file per feature/part — feature lists merge far better than monolithic files),
UI for history and branches.

### Phase 5 — Sketch constraints
A real constraint solver (coincident, parallel, dimensions) is what makes it feel like
Onshape. Don't write one — embed **PlaneGCS** (FreeCAD's solver, LGPL) or SolveSpace's
solver as a library.

### Continuous
Incrementally dismantle `main.cpp` and the tripled Vulkan boilerplate (extract a
`VulkanContext` + shared renderer utilities) — opportunistically while touching code, not as
a big-bang rewrite. The prototype rasterizer is good enough to carry the parametric core work.
