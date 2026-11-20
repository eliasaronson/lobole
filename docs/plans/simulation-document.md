# The simulation document

Simulation gets a document of its own, as CAM did, with the mesh as a step
you can see, and results and meshes kept with the project.

## The problem

- **A study lives inside the thing it analyses, twice over.** A part study
  is in `Document::GetStudies()`, an assembly study in
  `AssemblyDocument::GetStudies()`, built by two planners
  (`fem::PlanStudy`, `fem::PlanAssemblyStudy`) with different abilities: a
  pin and a thermal study work on a part only, a topology run and an async
  mesh are refused on an assembly, and contact with a neighbour exists on
  an assembly only. Editing a load moves the part's save stamp. A study of
  one part held in two ways, or of the same part in two configurations, is
  two unrelated studies that mesh twice.
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

A fifth document kind, `fem::SimDocument` (`ui::DocKind::Simulation`, a tab
of its own), shaped like `cam::JobDocument`:

```
SimDocument
  subject        a part or an assembly, by stable id, and its configuration
                 (0 = the active one): a reference, never a copy
  geometry       analysis-only changes: suppressed features (per part),
                 symmetry cuts
  roles[]        on an assembly: the participants table (meshed, bonded,
                 rigid, contact, ignored), as fem::Participant is now
  mesh           size, per-face and per-participant refinements, the
                 stored mesh and its report
  studies[]      fem::Study: type, loads and supports, cases, solver,
                 converge, nonlinear, thermal -- without the mesh fields
  results        per study, as now
```

- One planner. A part subject is an assembly of one meshed participant, so
  `PlanStudy` and `PlanAssemblyStudy` become one, and what was refused on
  one side for want of the other's code (a pin, a thermal study, topology,
  an async mesh on an assembly) is either built by the merge or refused
  with its real reason.
- An anchor is `{participant, ElementRef}`, as a CAM target is
  `{jobPartId, ElementRef}`; one that does not say which participant when
  the name is ambiguous is refused.
- A subject that is deleted, or does not build in the named configuration,
  is reported on the document (`missing`, `why`), never substituted.
- Several simulation documents may name the same subject.
- The simulation command on a part or an assembly (`part.simulation`,
  `assembly.simulation`) opens the subject's simulation document, making
  one first where there is none, so a quick check is still one click.
- Requirements (`study_probe`, `study_result`), probes, the topology-to-
  part flow and the `Study` RPCs name a study by its stable id, which it
  keeps through the move; `study` parameters accept the ids they accepted.

Studies in existing files migrate on open (`vNEXT`): each part or assembly
with studies gets one simulation document holding them. A saved result is
adopted where its hash still matches the migrated study's; where the one
planner hashes differently it reads stale, as any result whose inputs
moved does, and is solved again when asked. The part- and assembly-level
study lists are then gone from the model, not kept as a second home.

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
  document (`simulations/<id>-<slug>/…`), out of the history as study
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

The left panel shows what exists; the toolbar acts; nothing is edited in
the panel (docs/ui/panels.md). Today's floating Simulation window, which
holds the type, the mesh size, every load tool, Run and the results at
once, is taken apart along that line.

**The tree** (the Model dock on a simulation tab), one row per thing, each
with its icon from `CommandIcon` and a state shown quietly:

```
Subject      bracket  (config: long)
Geometry
  suppressed  Hole pattern 2
  symmetry    XZ plane
Mesh         41 820 tets            (a dot: current / stale / not built;
                                     a warning count when there are any)
Studies
  Static 1   solved  FoS 2.4
    Fixed    back face
    Force    120 N  lug
    Result   von Mises
  Modal 1    not run
```

- A row says one thing: its name, and one short value in the dimmed
  column (a load's magnitude, a mesh's size, a study's headline number).
  No buttons and no fields in rows; a state is a small dot in the icon
  column in the colours the study list already uses, not a word, with the
  word on hover.
- Groups with nothing in them are not drawn (no empty "Geometry" header on
  a study with no suppression). Studies fold; a folded study shows its
  state and headline only.
- Clicking a row selects it and highlights what it stands on in the
  viewport; double-click or its menu opens its editor in the tool window.
  Selection, multi-select and Delete follow the shared list behaviour
  (docs/ui/selection.md).
- On an assembly subject each participant is a row under Subject with its
  role as a per-row menu, as a CAM part row has its Configuration menu:
  the one inline control, because a role is a single choice and a table of
  them is read as a whole.

**The toolbar**, left to right in the order the work goes, in groups with
separators: Suppress, Symmetry | Mesh, Refine | the supports | the loads |
Check, Run | Result field, Probe, Export. Supports and loads are two
drop-down buttons showing the last-used kind, not a dozen buttons. A tool
that cannot apply yet is disabled with its reason on hover.

**The tool window** is the shared `###ToolWindow` every other tool uses
(docs/ui/tool-sessions.md): one tool's fields at a time, and for Mesh and
Check the report -- a list of findings, each a row that frames its place
in the viewport on click.

**Result controls sit on the legend** in the viewport, since they act on
it: the field, the case or mode, the deformation scale, the clamp. The
legend is the only persistent result chrome; a study's numbers (maxima,
factors, reactions) are read in the tool window when its Result row is
opened.

**Glyphs in the viewport** stay as they are for loads and supports. While
a simulation tab shows a mesh or a result, load labels are hidden unless
the load's row is selected, so a picture of a result is not covered in
text.

### 7. The agent surface

One convention for expressions on every number here
([expressions everywhere](expressions-everywhere.md)). The MCP workflow
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
3. **The document**, in steps that each land green. The core is built
   ([the simulation document (core)](../simulation/README.md#the-simulation-document-core):
   `SimDocument`, the one planner, the mesh stored and shared); what is left:
   1. **Serialization and migration** (`vNEXT`): the document saved and
      opened, its stored meshes beside its results, and the studies of every
      part and assembly moved into one simulation document each, as
      [the document](#1-the-document) says.
   2. **The tab and [the window](#6-the-window)** (tree, toolbar, tool
      window, legend) on the document, and the app's runs going through
      `RunSimStudy`.
   3. **The RPC move**: the study RPCs on the document, with the study ids
      kept.
   4. **Docs and generators**, and the part- and assembly-level study lists
      deleted.
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
- `src/core/Document.{h,cpp}`, `src/core/Occurrence.h` (the study lists
  leave), `src/core/io/SerializeStudies.cpp`, a new
  `SerializeSimulation.cpp`, the migration and its goldens.
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

- The migration: every golden with a study, and `study_reopen_smoke`'s
  project, opens as a simulation document with every study, load and
  requirement in place; each saved result reads current or stale, never
  current against inputs it was not solved for, and a stale one solves to
  the answer it had.
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
