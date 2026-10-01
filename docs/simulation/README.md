# Simulation (FEM)

Linear structural analysis of parts and assemblies: static stress and
displacement, and natural frequencies. Topology optimization is the third
kind of study and has its own page, [../topopt/README.md](../topopt/README.md).

## The model

A **study** (`fem::Study`, `src/core/fem/FemStudy.h`) is a document-level
object, not a `Feature`. It consumes the finished bodies and produces no
geometry, so it lives beside the parameter table (`Document::GetStudies()`,
`AssemblyDocument::GetStudies()`) and never enters regeneration, rollback or
suppression. On a part the list is read-only: an edit is a command
(`ModifyStudyCommand` → `ReplaceStudy`), which moves the part's save stamp
though not its version, so it reads unsaved
([versioning](../versioning/README.md#unsaved-changes)); a solve writes the
study's transient fields through `StudyForRun`. Its analysis `type` is
`static`, `modal` or `topology`.

A study holds:

- `meshSize` (mm; 0 = auto, the body's bounding-box diagonal / 15) and
  per-face `refinements` (a target element size on one face).
- `loads`: each a `Fixed` support, a `Force` (a total vector in newtons,
  spread as uniform traction over the face) or a `Pressure` (MPa along the
  inward normal, positive pushes into the part).
- `gravity` (static only; m/s² in world axes, 0 = off; needs a material
  density) and `numModes` (modal only; default 6).
- `loadCases` (static and topology): forces and pressures grouped into
  cases, each solved on its own over the same supports
  ([load cases](../topopt/README.md#load-cases)).
- `shapeFrom` (static only): solve a topology study's result instead of
  meshing the body
  ([a static study of the optimised shape](../topopt/README.md#a-static-study-of-the-optimised-shape)).
- `participants` (assembly studies only, below).

Sizes and magnitudes follow the value + expression convention, so a load can
read `bolt_preload * 1.5`. Expressions are evaluated lazily at mesh/solve
time (`EvaluateStudyExprs`), not during regeneration.

Every face is an `ElementRef` resolved at mesh time (`fem::ResolveFace`,
the same lookup `ShellFeature` and `Measure` use). A reference whose face no
longer exists is a study error naming the load, never a silent skip. A face
may be given as `{feature_id, element}` or the element name alone; the body
that holds that name is then found, because the name's own prefix is the
feature that created the element, which after a boolean is not the body's
`created_by`.

A force with no direction pushes into the face it was put on
(`fem::DefaultForceDirection`, the negated outward normal). The GUI placement
and `add_load` both call it, so clicking and scripting agree.

### Units

The solver works in mm–N–MPa (lengths and displacements mm, forces N,
stresses and moduli MPa) with mass in tonnes, so frequencies come out in Hz.
The material library (`src/cad/CADMaterials.h`) stores `youngGPa`, `poisson`,
`yieldMPa` and `density` (kg/m³), each 0 when unknown. A body whose material
has no stiffness is refused ("has no stiffness data"); a modal study or a
gravity load on a material with no density is refused too. The library's
default material has none of these, so an unassigned body cannot be analysed
by accident.

### Results and staleness

Mesh and results are **derived**. They live in a `fem::StudyRuntime` cache
stamped with the document's `Version()` at solve time, and are saved with
the project beside their study, with the input hash that produced them
([studies as jobs](#studies-as-jobs)). Any later change makes them
**stale**: the panel badges them and `study_results` / `list_studies`
report `stale: true`. Nothing is ever re-solved automatically — meshing and
solving take seconds to minutes and stay an explicit act; an explicit run
whose inputs are unchanged is answered from the stored result. A study
that has never been run carries its setup and no answers; `study_results`
refuses it with "study has not been run".

## Studies as jobs

Every long study -- a motion study, a static, modal or topology solve, a
topology check, a mesh-only run -- runs as a job with an id, and a result is keyed by a hash
of exactly what produced it. This page holds it for all four kinds; the
motion side is also in
[assemblies](../assemblies/README.md#motion-studies).

**A job id.** `BackgroundJob::id` (`src/ui/BackgroundJob.h`) is handed out
by `StartBackgroundJob` from a session counter and never reused. An async
`run_study`, `run_motion_study` or `mesh_study`, and a `check_topology`
that has something to solve, answers `job: <id>` (and `kind`). `study_status {job}` reports a running job (kind, study, label,
fraction, elapsed, cancellable, whether it outlives its client) and, once
it has ended, how: `outcome` done, cancelled or failed, with `error` or
`cancel_reason`; the registry keeps the last 64 ended jobs for that
(`AppState::finishedJobs`). `cancel_study {job}` cancels any study job,
motion included. Both work from any tab. An id that names no study job,
running or ended, is refused naming the ones there are; an ended job is
refused a cancel; a CAM or import job is refused, pointing at
`cancel_background_job`. `background_jobs` lists every job with its id.

**Owned by its client.** The RPC session numbers each connection
(`RemoteServer`, a number never reused, unlike the fd), and the pump sets
`AppState::rpcConnection` around each dispatch. A study job started over
RPC is owned by that connection, and when the connection closes the pump
cancels it (`CancelJobsOwnedBy`, reason "its client disconnected") --
unless the call passed `outlive_client: true`. A synchronous motion run
polls the same thing (`AppState::rpcCallerGone`) between samples and pair
checks, so a client that hangs up mid-run stops it. Jobs the GUI starts,
and CAM passes (cached and paid for), have no owner. A script that starts
an async study and exits without `outlive_client` has stopped its study.

**What cancelling does.** A topology run stops after the iteration it is
on and keeps the best shape so far, marked cancelled. A motion run stops
between samples, or between pair checks, and keeps the leading samples
whose every check landed. A static, modal or mesh job stops at its next
boundary -- after the meshing; the factorization has no interruption
points, so a cancel during it lands when it ends -- and drops its answer.

**Results keyed by inputs.** A run computes a 64-bit input hash
(`src/core/InputHash.h`; FNV-1a) over the exact B-Rep of the bodies it reads
(BinTools bytes without triangulation, memoized per shape identity), their
names, materials and the library's numbers for them, thread regions and face
names, and:

- an FEM study (`fem::StudyInputKey`, `fem::AssemblyStudyInputKey`,
  `src/core/fem/FemInputKey.cpp`): the study as saved with its expressions
  EVALUATED (so a variable feeding a load is an input by its value), its
  name and id aside; on an assembly, each taking-part occurrence as saved
  and its placed bodies;
- a motion study (`motion::InputKey`): the definition (name and id aside,
  the panel's Preview or Validate override included), every occurrence with
  its placement, every mate with its current value -- a run starts from the
  pose the document is in, so a scrub or a `leave_at` moves the next run's
  inputs --, relations, configurations and the active one, through every
  sub-assembly, and every placed (part, configuration).

A version string leads each hash (`motion/1`, `fem/1`); a change to what a
run computes bumps it. The solver a topology run picks (GPU or CPU) is not
an input. A finished whole run is kept by its hash in the session
(`AppState::motionResultsByInput`, 32; `studyResultsByInput`, 8; a cancelled
or failed run is not kept), and `Start…Run`/`RunMotionNow` ask first: a match
-- the study's own result, or one of the session's -- is installed as the
study's fresh result and nothing starts. The reply says `reused: true`,
every result carries `input_hash`, and `recompute: true` computes anyway
(Shift+Run in the panel, which also says "Reused" when it did). A reuse
answers before the one-run-at-a-time check: it starts nothing.

**Saved with the project.** Every result is written beside its study with
its hash: a part's or an assembly's static, modal or topology result as
`studies/<id>-<slug>.result.json` (topology since v33 and its hash since
v148, the others v153), a motion result as `motion/<id>-<slug>.result.json`
(v153). What is kept is what the panel and the RPCs read -- the mesh, each
node's displacement and stress, the modes, the maxima, a shape study's cut
and the result it was cut from, the topology density, every motion sample's
poses, mate values and clashes -- never a solver's scratch. A reopened
project's run of the same inputs is answered from it (`reused: true`), and
a result whose saved hash no longer matches the reopened inputs reads stale
rather than current-because-loaded. The checks:

- a part's results are checked as it loads (`LoadDir`), and again after
  the app regenerates it once more against the project's variables
  (`fem::RestampByInputs`, from `SyncProjectScope` and every open): that
  regeneration moves the version a result was stamped at, which made a
  reopened topology result read stale and `check_topology` refuse it;
- an assembly's FEM and motion results need the parts their hash reads, so
  the app checks them when the assembly is opened or its tab comes up
  (`ui::AdoptStoredResults`, `ui::AdoptSavedMotionResults`); until then an
  assembly study's saved result reads stale.

**The B-Rep is hashed as geometry**, not as OCC's bookkeeping on it
(`inputhash::ShapeDigest`): a copy of the shape, its TShapes' `Modified`,
`Checked` and `Free` flags set one way, written as text without UV points,
every `-0` read as `0`. Meshing a face -- Netgen for a study, BRepMesh for
the viewport -- clears its `Checked` flag and its cached UV points, and a
shape read back from the project's baked geometry carries a `+0` where the
regenerated one had a `-0` (a circle's axis); the pan/tilt yoke's envelope,
meshed in place by its topology run and saved meshed, reopened hashing
otherwise, and its 80 s run was paid again on every reopen. A topology run
now meshes a copy, as a static plan does.

The `[studyjobs]` tests prove the hash moves on each input class -- geometry,
material, mates, a placement, relations, settings, loads, an expression's
value, configurations -- and on nothing else (`tests/test_motion.cpp`,
`tests/test_study_inputs.cpp`); that a result of each kind -- static, modal,
topology, motion -- reopens with its hash, current, its part hashing as it
did, and reads stale once an input moves; and that a part meshed in place,
or with a bore, hashes the same after a reopen.
`tests/study_jobs_smoke.py`, `fem_async_smoke.py`, `topo_smoke.py` and
`topo_build_smoke.py` drive the ids, cancels, disconnects and reuse over the
RPC; `tests/study_reopen_smoke.py` saves a project with a study of each kind
and a topology check, opens it again, and has each one reused.

## Meshing

`fem::Mesh` (`src/core/fem/FemMesher.{h,cpp}`) wraps Netgen's C++
`OCCGeometry` interface: it meshes a `TopoDS_Shape` directly, attributes
every surface element to its B-Rep face (the plain C nglib API cannot), and
produces second-order tetrahedra through an explicit `MakeSecondOrder()`, so
midside nodes lie on the true surface. Node order is canonical tet10 (corners
0–3, then edges (0,1)(1,2)(0,2)(0,3)(1,3)(2,3)); boundary triangles are tri6.
Mesh statistics carry node and element counts and the worst normalized tet
quality.

Netgen is optional. The wrapper is its own static library, `fem_mesher`,
because Netgen's build requirements (the `PARALLEL`/MPI defines and
`-fabi-version=17` the nixpkgs package was built with — a mismatch corrupts
the heap) must not leak into the rest of core. Without Netgen the app builds
and runs, and meshing returns "built without FEM support". Netgen writes
triangulation onto the faces it meshes, so `PlanStudyMesh` / `PlanStudy`
deep-copy each shape on the main thread and the worker meshes the copies;
the document's shape is never written under the renderer.

A mesh failure is reported as the study error, naming the face when Netgen
says which, and its shortest edge when that is under a micrometre: the usual
cause, and one no picture and no BRepCheck shows: two faces meant to
coincide that miss by a few micrometres leave a zero-area face bounded by
edges that short (the pan/tilt yoke's arm and base did, while sketch planes
were single precision). There is no defeaturing step: suppress cosmetic features before meshing.

## The solver

`src/core/fem/FemSolver.{h,cpp}` and `FemSolverInternal.h` (the element math
shared with topology optimization), namespace `fem`, GUI-free. Gated
`HAVE_FEM_SOLVER` on Eigen and `HAVE_SPECTRA` on Spectra (modal only).

- **Static** (`fem::Solve`, or `fem::SolveCases` for several load cases on
  one factorisation): tet10 isotropic elasticity, 4-point Gauss rule,
  triplet assembly into a sparse K, fixed nodes eliminated (all three DOF),
  consistent nodal loads from face tractions (equal splitting is visibly
  wrong on tet10), `SimplicialLDLT`. The factorization can report success on
  a singular K, so the residual `‖Ku − F‖` is checked and an
  under-constrained model is refused as such. Stresses are evaluated at each
  element's nodes and averaged to the mesh nodes weighted by element volume;
  von Mises per node, and the maxima with their locations.
- **Modal** (`fem::SolveModal`): `K φ = ω² M φ` with a consistent mass
  matrix, lowest `numModes` by Spectra shift-invert. It needs a fixed support,
  so K is positive definite: only the lower triangle is assembled (the only
  half Spectra reads) and `(K − σM)⁻¹` is factorized with `SimplicialLDLT`
  behind `SpdShiftInvert` rather than Spectra's stock `SparseLU` path, which
  would store an L and a U for a symmetric matrix. Mode shapes are normalized
  to max |u| = 1.
- **Several materials in one mesh**: both solvers take one `Material` per
  mesh domain (`FemMesh::tetDomain`), which is what a bonded assembly group
  needs.

Per-part bodies are independent: a part study meshes and solves each body it
references separately, with no contact between them, and `study_results`
reports one entry per body. Only bodies that carry a load or refinement are
meshed; each needs a fixed support ("has loads but no fixed support" is
checked before any factorization).

`study_results` for a static study reports `max_von_mises_mpa` and
`max_displacement_mm` (`{value, at, body}`), the per-body breakdown, and a
factor of safety against the material's yield strength when one is known.
With load cases the top level is the worst over them and `cases` lists each
one's maxima and factor of safety. The
factor travels with a receipt whose basis is *predicted*, and which is
unverifiable when the result is stale or no body has a yield strength.

### Threads

`RunStudy` is `PlanStudy` (main thread: expressions, face resolution,
validation, shape copies) + `MeshPlannedStudy` + `SolvePreparedStudy`
(plain data, safe on a worker). `run_study {async: true}` and the Run button
mesh and solve off the frame as a background job; `mesh_study {async: true}`
meshes only. Each is a job with an id
([studies as jobs](#studies-as-jobs)); a cancel stops a static or modal job
at its next boundary (after the meshing; the factorization has no
interruption points) and drops its answer. One study run at a time. A
finished job stores its result into the document it ran on even if another
tab is active.

### Probing a face

The maxima say how far the worst node moved, somewhere on the part. A
mechanism asks a different question: what did the face that carries
something do? A bearing seat moves its bearing, and the bearing its shaft,
as one body, so `probe_study {id, face, at?}` fits the face's nodes with a
small-displacement rigid motion (`fem::FitFaceMotion`):
`u(p) = translation + rotation × (p − centroid)`, least squares, each node
weighted by the area of the boundary triangles it stands for. It reports the
translation (mm), the rotation vector (radians), and the RMS of what the
fit leaves, which is the face's own distortion. Given `at` (a point in
part coordinates), it carries the motion there: where a rigid thing held by
the face goes.

The pan/tilt head's yoke is the use it was built for
(`generators/pantilt/yoke_loads.py`): the camera is not in the study, so
the lens's deflection is the two bearing bores' translations (the shaft's
line), the ledge's motion carried to the worm's axis (the wheel's turn), and
the lens's lever from the tilt axis.

A study with load cases is probed in one, `case` (its name or index; the
first when omitted). It reads a part study's static result; it refuses a stale one (a face named
now may not be the face that was meshed), an assembly study (its mesh is
the imprinted fuse, whose faces are not the parts'), and modal or topology
ones. Tests: `[probe]` in `tests/test_fem.cpp` (a rigid motion recovered
exactly; a cantilever's end face turning by the tip slope, P L² / 2EI).

## Assembly studies

A study on an `AssemblyDocument` names **occurrences**, each in one role
(`fem::Participant`, top-level occurrences; a sub-assembly takes part whole):

| role | cost | meaning |
|---|---|---|
| `meshed` | mesh + solve | the part being analysed; exactly one per study |
| `bonded` | mesh + solve | meshed too, and glued to the meshed part where they touch, with its own material |
| `rigid` | one boolean | a support; its contact patch can be clamped |
| `ignored` | nothing | not in the analysis (also: any occurrence not listed) |

Left out and rigid bracket the truth: a neighbour left out is a lower bound
on stiffness, the same neighbour rigid is an upper bound, and bonded lies
between them. `study_results` carries the role table so a reader knows which
bound each neighbour holds.

Every anchor on an assembly study names its occurrence (`occurrence` on a
load or refinement). A fixed support may instead anchor on a **contact**
(`add_load {type: fixed, contact_with}`): the patch where a rigid participant
touches the meshed group. That patch exists in neither part, so it is
re-derived every time the study meshes, and a clamp on a rigid support that
touches nothing meshed is refused by name. Being rigid does not clamp
anything by itself; the clamp is a load.

How it is built (`src/core/fem/AssemblyStudy.cpp`, `fem::PlanAssemblyStudy`):

- **The imprint never touches a part.** Placed bodies are copied, and one
  general fuse (`BOPAlgo_Builder`) runs over the copies with
  `SetNonDestructive` — an OpenCASCADE boolean writes to its arguments. A
  split written into a part would reach its drawings, CAM and exports.
- **The fuse is fuzzy** (`SetFuzzyValue(1e-3)` mm). Placed parts meet only as
  exactly as their placements were solved — a loop closes to 1e-5 mm, a
  rotation rounds, a placement typed by hand is off by what was typed — so
  faces that should coincide can sit apart; a strict boolean finds nothing
  between them and the assembly meshes as loose pieces, which looks exactly
  like a working study.
- **A bonded group is one conforming mesh.** The fuse imprints the shared
  faces and keeps each solid, and Netgen meshes a shared face once, so the
  tets on both sides share its nodes and load crosses the joint without
  constraint equations. An unfused compound of the same solids meshes as
  separate components with every interface node doubled. The members'
  imprinted solids go into one compound whose faces are indexed the way the
  mesher indexes them (`TopExp::MapShapes`). Which mesh domain is which member
  is decided by classifying tet centroids against the members, not by
  trusting the mesher's numbering. The group reports as one body.
- **Bonding is declared, not inferred.** T-nuts, drivers and press fits are
  not glue, and fusing a line-to-line fit produces slivers that fail to mesh.
- **Refusals before meshing**: not exactly one meshed occurrence; a meshed,
  bonded or rigid pair that interpenetrates (checked with `interference`;
  `ignored` is the way out); a bonded member sharing no face with the group; a
  contact anchor whose support does not touch.
- Anchors on split faces are mapped onto the imprinted faces; a force's total
  is divided among them by area.

Assembly results are stale against the assembly's version **and** every
participating part's version (`StudyRuntime::solvedPartVersions`). An edit to
an assembly study is one full-state undo snapshot and one version bump. The
result cache (`fem::StudyRuntimeCache`) is shared between the document and
its undo snapshots so a snapshot does not copy meshes. Topology
optimization is refused on an assembly; `mesh_study {async}` is refused there
(`run_study {async}` works).

## Results display

`FieldRenderer` (`src/render/FieldRenderer.{h,cpp}`) has its own pipeline and
vertex format `FieldVertex {position, displacement, scalar}`: the boundary
triangles of the solved mesh, warped in the vertex shader by a push-constant
deformation scale (so the slider needs no re-upload) and coloured through an
analytic Turbo colormap. It shares the scene's clip planes, so a section
cuts a stress plot like everything else. While a field is shown it stands in
for the solved body (or every solved occurrence of an assembly study).
Fields: von Mises, displacement magnitude, a modal mode shape, and the
topology views. The legend names the study and analysis, the quantity with
quarter ticks, the deformation factor, the mode and frequency of a modal
result, and where the maximum is, marked in the viewport by a cross.

`set_result_display` colours the raster viewport, which is what `screenshot`
captures. `render_still` renders the model, not the field.

Boundary conditions are drawn in the viewport (`src/ui/StudyPanel.cpp`):
each BC face is tinted through the mesh renderer's face highlight in its
family's colour; a force is one resultant arrow at the face; a pressure is a
field of arrows over the face, each along its own triangle's inward normal,
on a grid spaced from the view-scaled glyph length; a fixed face gets anchor
hatches; gravity is one arrow from the centre of mass of the meshed set,
drawn in the see-through channel because that point is inside the solid.
Labels with value and unit sit at the glyphs and can be switched off. On an
assembly study a rigid support is drawn as a ghost with its contact faces
tinted as a fixture, and bonded seams in the bond colour.

## GUI

- A part enters simulation from the toolbar (command `part.simulation`); the
  Model dock lists the studies, one row per study with its state (solved,
  stale, failed, not run, solving) and Delete in the row menu. The selected
  study is edited, run and read in the floating **Simulation** window.
- On an assembly tab, `assembly.simulation` opens the Simulation tool window,
  a viewport mode beside explode and motion; it holds the study list and a
  participants table with a role per row and **Clamp here** on rigid rows.
- In the window: analysis type, mesh size, material check, gravity (a
  checkbox that sets world-down 9.81 m/s²), and **Boundary conditions**: arm
  Fixed, Force or Pressure and click a face; magnitude and an optional custom
  direction apply to the next placement. A placed force can be dragged in the
  viewport to change its magnitude (one undo step on release). Run shows the
  shared progress bar. Results: mesh size, max von Mises, max displacement,
  field selector and deformation slider; for modal, the natural frequencies
  (selecting one shows its mode shape) and a mode-shape toggle. The factor of
  safety is reported over RPC only.
- Clicking an occurrence other than the meshed one on an assembly study is
  refused in the panel.

## RPCs

All in `src/remote/RemoteSimulation.cpp` (`SimulationMethods` table). They
act on the active part or assembly; a drawing or job tab is refused, naming
where studies live. A verb that acts on a study takes `id` (or `study`); a
verb that names a study for context (`add_load`, `add_refinement`,
`set_result_display`) takes `study`.

| method | what it does |
|---|---|
| `add_study {type?, name?}` | `type`: static \| modal \| topology (`kind` accepted) |
| `remove_study`, `list_studies` | list reports type, mesh size, loads, `has_results`, `stale`, participants |
| `set_study` | `mesh_size`, `type`, `gravity` [x,y,z] m/s², `num_modes`, `participants`, `load_cases`, `shape_from`, topology parameters |
| `add_load {study, type, face \| contact_with, occurrence?, magnitude?, direction?, case?}` | returns the load id |
| `remove_load {study, id}` | by load id |
| `add_refinement {study, face, size, occurrence?}`, `remove_refinement` | local element size, mm |
| `mesh_study {id, async?}` | per-body nodes, elements, worst quality |
| `run_study {id, async?, recompute?, outlive_client?}` | mesh + solve; returns the `study_results` summary when synchronous, `job` (its id) when async; unchanged inputs answer `reused: true` from the stored result ([studies as jobs](#studies-as-jobs)) |
| `study_status {id \| job}`, `cancel_study {id \| job}` | progress of an async job, or by `job` any study job's (motion included) and how it ended; cancel stops it |
| `study_results` | static: maxima, per-body, factor of safety; modal: `bodies[i].frequencies_hz` |
| `probe_study {id, face, at?, case?}` | how one face moved, as a rigid body; carried to `at` ([#probing-a-face](#probing-a-face)) |
| `enter_simulation {study?}`, `exit_simulation` | the GUI mode |
| `set_result_display {study?, field, deform_scale?, mode?, case?, threshold?}` | `field`: von_mises \| displacement \| shape \| density \| none; `case`: which load case a static study shows |
| `check_topology {study, threshold?, recompute?, outlive_client?}` | a topology result's shape and solid design domain, statically, per case, as a job ([the topopt page](../topopt/README.md#a-static-study-of-the-optimised-shape)) |

## Tests

- `tests/test_study_inputs.cpp` (`[studyjobs]`): the input hash moves on
  each input class and on nothing else; a saved topology result reopens
  with its hash, fresh, and stale when saved against other inputs.
- `tests/study_jobs_smoke.py`: job ids, a job cancelled with its
  connection and one that outlives it, a cancel by id, an unknown id
  refused, a hung-up synchronous run stopping, reuse, and `leave_at` in one
  solve.
- `tests/test_fem.cpp` (`[fem]`): element-level checks (tet10 stiffness and
  stress without a mesher), mesher cases, and analytic benchmarks — uniaxial
  bar σ = F/A, cantilever deflection against Euler–Bernoulli, a Lamé
  thick-wall cylinder, modal f₁ against beam theory, self-weight deflection. It asserts physics with tolerances and
  never golden meshes, because Netgen output is not bit-stable across
  versions. Also: a mesh spans two solids only when they share the face; the
  assembly cases (clamped nodes on the interface only, the refusals by name,
  staleness across documents, the shared cache, a bonded pair with two
  materials and an answer softer than the rigid one).
- `tests/fem_smoke.py`, `tests/fem_async_smoke.py`: the RPC surface
  headless, including an assembly tab; the async one also a reused run, a
  job's status by id and a cancel by id. Both skip (exit 77) on a build without
  Netgen, which is how the stock-Ubuntu CI jobs build; run them locally.
- GUI (`src/app/guitests/GuiTestsSimulation.cpp`, category `sim`): force
  direction and arrow placement, pressure as a field, face tints, gravity at
  the centre of mass, a static run against beam theory, the assembly study
  window and its clamp, the list-row editor.
- `tests/rpc_gate_smoke.py` checks the study verbs are accepted on an
  assembly tab.

## Not done

- Contact that can separate or slide, and nonlinear analysis (plasticity,
  large deformation), thermal, buckling, fatigue. The escape hatch if they
  are ever wanted is an external solver such as CalculiX, not growing this
  one past linear.
- Free-free modal analysis (a modal study needs a support).
- Shell and beam elements; adaptive refinement.
- Defeaturing for analysis. Mesh size is set by the smallest features, so a
  whole cabinet of 16 mm panels with fastener holes meshes to millions of
  DOF; per-panel studies are the practical route until holes can be
  suppressed for the analysis shape.
- Per-participant mesh size on a bonded member applies to its faces only; the
  study's global size still governs the interior.
- Result export (VTU).
