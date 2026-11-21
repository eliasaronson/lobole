# The simulation document: what is left

Simulation has a document of its own, a tab with its tree, toolbar and tool
window, its mesh drawn and reported, its studies checked before they run,
and results and meshes kept with the project
([the simulation document](../simulation/README.md#the-simulation-document-core)).
What is left is the geometry the analysis sees, the cases the loads cannot
yet state, a result kept whole, and the window's remainder.

## The problem

- **The analysis has no geometry of its own.** There is no way to suppress
  fastener holes or logos for the analysis only ("Defeaturing for
  analysis" in Not done: a cabinet of 16 mm panels meshes to millions of
  DOF), and no pre-mesh report of features smaller than the element size.
- **Two common load cases cannot be stated**: a bearing load on a bore,
  and a part with no support at all (free-free modes).
- **A saved result's interior is gone**, so nothing can be cut, probed
  inside or re-plotted after a reopen without solving again, and a design
  sweep refuses study requirements
  (docs/requirements/README.md#design-sweep), the quantity most worth
  sweeping.
- **The window keeps result chrome apart from the legend** it acts on, and
  has nowhere for the geometry step.

## The shape of the fix

### 1. The geometry step

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

### 2. Loads

- **A bearing load**: a total force on a bore as a cosine-distributed
  normal pressure over the loaded half, in `AssembleFaceLoads` beside
  Torque and Remote.
- **A free-free modal study**: a negative shift in `SpdShiftInvert`; rigid
  modes labelled; each mode's effective mass per direction.

### 3. Results

- **Kept whole when asked.** `keep: full` on a study also stores the
  interior field against the stored mesh, so a reopened result can be
  sectioned and probed inside. A `converge` run's local refinement is a
  child mesh of the one it started from, kept with that study's result. A
  mesh is written compressed if a compressor is linked by then, raw
  otherwise.
- **Sweeps.** `design_sweep` accepts `study_probe` and `study_result`
  requirements: each point regenerates, re-meshes and solves as a job,
  reused by input hash, under a time budget stated up front and refused
  when `converge: always` would make it unbounded.

### 4. The window's remainder

- **Result controls on the legend**, since they act on it: the field, the
  case or mode, the deformation scale, the clamp. The legend is then the
  only persistent result chrome, and the Result row's settings keep the
  study's numbers (maxima, factors, reactions) and its probes.
- **The geometry step's rows and tools**: a Geometry group in the tree
  (suppressed features, the symmetry plane) and Suppress, Symmetry and
  Refine on the toolbar, before Mesh.
- **Sizes on disk on hover** of the mesh and Result rows, and in the clear
  storage question.
- **Load cases, kept faces and keep-outs in the list selection**, so Delete
  and Shift reach them as they reach studies and loads.

Every number these add takes the one convention for expressions
([numbers that are expressions](../variables/README.md#numbers-that-are-expressions)).

## Stages

Each merges on its own with the suites green.

4. **The geometry step** ([1](#1-the-geometry-step)): suppress for
   analysis, the geometry check, the symmetry cut, with the window's
   remainder ([4](#4-the-windows-remainder)).
5. **Bearing load, free-free modal, `keep: full`, study sweeps**
   ([2](#2-loads), [3](#3-results)).

## Files

- `src/core/fem/SimDocument.{h,cpp}` (the geometry step's fields),
  `src/core/fem/SimStudy.{h,cpp}`, `FemSolver.cpp` (the bearing load, the
  shift), `FemResults`.
- `src/core/io/SerializeSimulation.cpp` (the geometry step's fields, a full
  result).
- `src/ui/SimTree.cpp`, `src/ui/StudyPanel.cpp` (the legend's controls),
  `SimulationActions.cpp`, `src/ui/Icons.cpp`;
  `src/remote/RemoteSimulation.cpp`, `RemoteRequirements.cpp` (sweeps) and
  their `describe` rows.
- `python/lobole/mcp.py` (the prompt).
- `docs/simulation/README.md`, `docs/requirements/README.md`,
  `docs/versioning/format.md`, `docs/scripting/rpc.md`.

## Verified by

- Suppress for analysis: a plate with fastener holes meshes to a stated
  fraction of the elements with them suppressed, the part unchanged; a
  load on a suppressed hole's face is refused by name.
- Bearing load: resultant equals the force, no net moment about the axis,
  a lug against the bearing stress the pin tests use. Free-free: six rigid
  modes near zero and a beam's first elastic frequency against its closed
  form.
- `keep: full`: a reopened result sectioned and probed inside answers as it
  did before the save.
- A sweep of wall thickness against factor of safety, reused on rerun.
- The window: the legend's controls in a screenshot test; a GUI test
  suppresses a feature from the toolbar; `widget_ids_lint`.
- The headless, GUI and pygui suites per stage; the action matrix and
  [the reference cases](../simulation/README.md#reference-cases).
