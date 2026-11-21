# The simulation document: what is left

Simulation has a document of its own, a tab with its tree, toolbar and tool
window, its mesh drawn and reported, its studies checked before they run,
results and meshes kept with the project, and a geometry step of its own
([the simulation document](../simulation/README.md#the-simulation-document-core),
[the geometry step](../simulation/README.md#the-geometry-step)). What is
left is the cases the loads cannot yet state and a result kept whole.

## The problem

- **Two common load cases cannot be stated**: a bearing load on a bore,
  and a part with no support at all (free-free modes).
- **A saved result's interior is gone**, so nothing can be cut, probed
  inside or re-plotted after a reopen without solving again, and a design
  sweep refuses study requirements
  (docs/requirements/README.md#design-sweep), the quantity most worth
  sweeping.

## The shape of the fix

### 1. Loads

- **A bearing load**: a total force on a bore as a cosine-distributed
  normal pressure over the loaded half, in `AssembleFaceLoads` beside
  Torque and Remote.
- **A free-free modal study**: a negative shift in `SpdShiftInvert`; rigid
  modes labelled; each mode's effective mass per direction.

### 2. Results

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

Every number these add takes the one convention for expressions
([numbers that are expressions](../variables/README.md#numbers-that-are-expressions)).

## Stages

5. **Bearing load, free-free modal, `keep: full`, study sweeps**
   ([1](#1-loads), [2](#2-results)). Merges on its own with the suites
   green.

## Files

- `src/core/fem/SimStudy.{h,cpp}`, `FemSolver.cpp` (the bearing load, the
  shift), `FemResults`.
- `src/core/io/SerializeSimulation.cpp` (a full result).
- `src/ui/StudyPanel.cpp`, `src/remote/RemoteSimulation.cpp`,
  `RemoteRequirements.cpp` (sweeps) and their `describe` rows.
- `python/lobole/mcp.py` (the prompt).
- `docs/simulation/README.md`, `docs/requirements/README.md`,
  `docs/versioning/format.md`, `docs/scripting/rpc.md`.

## Verified by

- Bearing load: resultant equals the force, no net moment about the axis,
  a lug against the bearing stress the pin tests use. Free-free: six rigid
  modes near zero and a beam's first elastic frequency against its closed
  form.
- `keep: full`: a reopened result sectioned and probed inside answers as it
  did before the save.
- A sweep of wall thickness against factor of safety, reused on rerun.
- The headless, GUI and pygui suites; the action matrix and
  [the reference cases](../simulation/README.md#reference-cases).
