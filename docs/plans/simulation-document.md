# The simulation document

Simulation gets a document of its own, as CAM did, with the mesh as a step
you can see, and results and meshes kept with the project.

## The problem

- **The mesh is not a thing.** The GUI goes from a mesh-size number to Run;
  `mesh_study` answers counts and the worst quality over RPC and nothing is
  drawn. Nobody sees the elements, a sliver, a thin wall meshed one element
  through, or where the refinement went, until the answer looks wrong. A
  mesh is rebuilt by every study on the same geometry, and by every reopen
  (a static result keeps its boundary surface only:
  docs/simulation/README.md#what-a-saved-result-keeps).
- **The analysis has no geometry of its own.** There is no way to suppress
  fastener holes or logos for the analysis only ("Defeaturing for
  analysis" in Not done: a cabinet of 16 mm panels meshes to millions of
  DOF), and no pre-mesh report of features smaller than the element size.
- **Setup problems arrive one at a time**, at `run_study`'s refusal.
- **Results show less than they hold.** The display has von Mises,
  displacement, mode shapes, density, temperature and heat flux. The
  principal stresses, the across-layers stress a printed part fails by,
  the factor of safety, plastic strain and contact pressure are numbers in
  a panel; the legend has no range clamp and no over-yield colour; there is
  no export for an outside post-processor.
- **A saved result's interior is gone**, so nothing can be cut, probed
  inside or re-plotted after a reopen without solving again, and a design
  sweep refuses study requirements
  (docs/requirements/README.md#design-sweep), the quantity most worth
  sweeping.

## The shape of the fix

### 1. The document

Built ([the simulation document](../simulation/README.md#the-simulation-document-core)),
but for the analysis-only geometry it holds (below) and its window
([6](#6-the-window)).

### 2. The geometry step

- **Suppress for analysis.** `geometry.suppressed` lists features of the
  subject (per part on an assembly); the analysis shape is the subject
  regenerated without them, cached by the regeneration cache's chain hash.
  A load or refinement on a face that a suppressed feature made is refused
  naming the feature. The subject itself is never changed.
- **Symmetry cut.** A plane (a datum or a flat face) cuts the analysis
  shape and puts a roller on the cut
  ([the roller](../simulation/README.md#rollers-held-directions-and-cylindrical-supports));
  results draw mirrored on request.
- **The geometry check** (`check_sim_geometry`, and a list in the panel):
  edges, faces and gaps smaller than the element size they will be meshed
  at, each with the feature that made it, so a person or an agent knows
  what to suppress or refine before meshing.

### 3. The mesh step

- **Mesh is an action with a result you look at.** `mesh` (the existing
  `mesh_study` job, on the document) builds the mesh and stores it. The
  viewport draws it: element edges over the shaded body, an optional colour
  by quality (the scaled Jacobian the fold check already computes,
  `FemMesher.cpp`) or by size, and a section through it showing the
  interior tets cut. The worst elements are listed and a click frames one.
- **Its report** lists everything at once: counts, the worst quality and
  where, walls meshed one element through, elements straightened by the
  fold check, faces whose refinement did not take, mesh volume against the
  B-Rep volume (refused past a tolerance derived from the deflection), and
  for a bonded group that every interface face is shared.
- **One mesh, reused.** Every study of the document solves on it, unless
  the study sets a mesh size of its own, which gives it a mesh of its own,
  stored and reported beside the document's. A `converge` run's local
  refinement is a child mesh of the one it started from, kept with that
  study's result. The mesh is keyed by a hash of the analysis shape and the
  mesh settings, so editing a load never re-meshes and editing the part
  marks the mesh, and the results on it, stale.
- **Run still works from nothing.** Running a study with no current mesh
  meshes first, as now; the order geometry, mesh, look, loads, run is the
  one the panel lays out, not one it enforces, because loads are anchored
  on faces and survive a re-mesh.

### 4. Setup

- **`check_study`** (read-only) returns every problem a run would refuse,
  at once, each with the call that fixes it: no support, a torque on a
  face that is not a cylinder, a material without a modulus or density, an
  anchor that no longer resolves, a load region with no facets, a stale
  mesh. The panel shows the same list beside Run.
- **Loads the common cases need:** a bearing load (a total force on a bore
  as a cosine-distributed normal pressure over the loaded half, in
  `AssembleFaceLoads` beside Torque and Remote), and a free-free modal
  study (a negative shift in `SpdShiftInvert`; rigid modes labelled; each
  mode's effective mass per direction).
- The [roller, held and cylindrical supports](../simulation/README.md#rollers-held-directions-and-cylindrical-supports)
  are tools in today's Simulation window, and appear here as tools.
- A reply's `applied_total` and `reaction_total`, so a balance is one read.

### 5. Results

- **Kept whole when asked.** A study's stored result keeps the boundary
  surface as now by default (it is what the panel and the RPCs read). With
  the mesh stored on the document, `keep: full` on a study also stores the
  interior field against it, so a reopened result can be sectioned and
  probed inside. Results and meshes stay sidecars beside the simulation
  document (`simulations/<folder>/`), out of the history as study
  results are (docs/versioning/README.md#saved-results-stay-out-of-the-history).
  The mesh row and each Result row show their size on disk on hover, and
  **Clear stored meshes and results** (`clear_sim_storage`) removes them,
  leaving everything current to be rebuilt on the next run;
  a mesh is written compressed if a compressor is linked by then, raw
  otherwise.
- **Fields:** max/mid/min principal stress, the stress across layers and
  the interlayer factor on a printed part (`layer_normal`), factor of
  safety, equivalent plastic strain, contact pressure; each with its unit
  in the legend.
- **Legend:** a range clamp with over and under colours, "above yield" as
  a preset, markers at the maximum and the minimum.
- **VTU export** (`export_study_result`): the tet10 mesh
  (`VTK_QUADRATIC_TETRA`) with displacement, the stress tensor, von Mises,
  temperature; written to a file, never returned inline.
- **Sweeps.** `design_sweep` accepts `study_probe` and `study_result`
  requirements: each point regenerates, re-meshes and solves as a job,
  reused by input hash, under a time budget stated up front and refused
  when `converge: always` would make it unbounded.

### 6. The window

Built: the tree, the toolbar and the shared tool window
([panels.md](../ui/panels.md#simulation-tab-the-model-panel)), and clearing
stored meshes and results from the UI
([clearing](../simulation/README.md#clearing-stored-meshes-and-results)).
What is left of it:

- **Result controls on the legend**, since they act on it: the field, the
  case or mode, the deformation scale, the clamp. The legend is then the
  only persistent result chrome, and the Result row's settings keep the
  study's numbers (maxima, factors, reactions) and its probes.
- **The geometry step's rows and tools** with stage 4: a Geometry group in
  the tree (suppressed features, the symmetry plane) and Suppress,
  Symmetry and Refine on the toolbar, before Mesh.
- **The mesh as the simulation's**: one Mesh row under the subject once the
  document's mesh size is set on it (`set_simulation {mesh_size_mm}`), each
  study's row only where it meshes apart.
- **Sizes on disk on hover** of the mesh and Result rows, and in the clear
  storage question.
- **Load cases, kept faces and keep-outs in the list selection**, so Delete
  and Shift reach them as they reach studies and loads.

### 7. The agent surface

One convention for expressions on every number here
([numbers that are expressions](../variables/README.md#numbers-that-are-expressions)). The MCP workflow
prompt gains a short "judging a study" section: read `check_study`, the
mesh report, `convergence.status`, `peak_at_support_edge`, the balance and
`stale`, and name what the model leaves out.

## Stages

Each merges on its own with the suites green.

1. **The mesh step on today's model**: the mesh drawn, its report, the
   sanity checks, `check_study`. No format change. This is the part a user
   sees first and it does not wait for the document.
2. **Result fields, the legend, VTU export.** No format change beyond the
   display settings.
3. **The document**, in steps that each land green. Built: the core
   ([the simulation document (core)](../simulation/README.md#the-simulation-document-core):
   `SimDocument`, the one planner, the mesh stored and shared), its storage
   and the move of old files' studies on open
   ([how a simulation is saved](../simulation/README.md#how-a-simulation-is-saved),
   [moving studies](../simulation/README.md#moving-studies-into-a-simulation)),
   the tab ([the simulation tab](../simulation/README.md#the-simulation-tab))
   with the app's runs and every study RPC on it, its tree, toolbar and
   tool window ([6](#6-the-window)), and the part- and
   assembly-level study lists gone from the model and the format. What is
   left:
   1. **[The window](#6-the-window)**'s remainder: the legend's controls,
      the mesh as the simulation's row, sizes on hover.
   2. **The new RPCs** `add_simulation`, `list_simulations`,
      `set_simulation {mesh_size_mm, participants, configuration}` and
      `clear_sim_storage` (per-study participant tables come with
      `add_simulation`), then the generators, the docs and the screenshots.
4. **The geometry step**: suppress for analysis, the geometry check, the
   symmetry cut.
5. **Bearing load, free-free modal, `keep: full`, study sweeps.**

Stages 1 and 2 stand on today's model and start at once. Stage 3 runs
[the reference cases](../simulation/README.md#reference-cases), so the
merge of the two planners is checked against published targets, not only
our own tests.

## Files

- New `src/core/fem/SimDocument.{h,cpp}`; `src/core/fem/FemStudy.{h,cpp}`,
  `AssemblyStudy.cpp` (merged into the one planner), `FemMesher.{h,cpp}`,
  `FemInputKey.cpp`, `FemSolver.cpp`, `FemResults`.
- `src/core/io/SerializeSimulation.cpp` (the geometry step's fields).
- `src/render/FieldRenderer.{h,cpp}` (fields, clamp), a mesh-edge overlay
  registered in `src/app/GpuOwners.cpp` (`kRows`,
  `RegisterSceneRenderers`).
- `src/ui/StudyPanel.cpp` (split into the tree, the tools and the legend
  controls), `SimulationActions.cpp`, `src/ui/Icons.cpp`, the tab bar and
  project panel for the new kind; `src/remote/RemoteSimulation.cpp`,
  `RemoteRequirements.cpp` (sweeps) and their `describe` rows.
- `python/lobole/` (the SDK's study objects, `mcp.py`'s prompt).
- `docs/simulation/README.md`, `docs/architecture.md` (five kinds),
  `docs/requirements/README.md`, `docs/versioning/format.md`,
  `docs/scripting/rpc.md`.

## Verified by

- One planner: every existing FEM test passes through it; a part study and
  the same part as a one-participant assembly give the same hash and the
  same answer; a pin and a thermal study on an assembly solve.
- The mesh: a GUI test meshes, sees edges drawn (`render_pixel_smoke` gains
  the mesh view), frames the worst element; a planted hole in the mesh
  fails the volume check; editing a load leaves the mesh current, editing
  the part marks mesh and results stale.
- Suppress for analysis: a plate with fastener holes meshes to a stated
  fraction of the elements with them suppressed, the part unchanged; a
  load on a suppressed hole's face is refused by name.
- `check_study` returns three planted problems in one reply.
- Bearing load: resultant equals the force, no net moment about the axis,
  a lug against the bearing stress the pin tests use. Free-free: six rigid
  modes near zero and a beam's first elastic frequency against its closed
  form.
- Fields: principal stresses of a bar and a pressurised cylinder against
  closed forms; the clamp and markers in a screenshot test.
- VTU: read back with a small parser, node order checked on a curved
  element.
- A sweep of wall thickness against factor of safety, reused on rerun.
- The window: a GUI test builds a study by toolbar and viewport clicks
  alone, with no field in the tree; screenshots of the tab at the default
  and the minimum window size, with a long study, show no clipped or
  elided row value and no empty group; `widget_ids_lint`.
- The headless, GUI and pygui suites per stage; the action matrix after
  stage 3.
