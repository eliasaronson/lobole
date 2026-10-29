# Simulation (FEM)

Structural analysis of parts and assemblies: static stress and
displacement, natural frequencies and linear buckling, and on CalculiX
large deformation and snap-through ([nonlinear static](#nonlinear-static))
and contact ([contact](#contact)). Topology optimization is the third
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
`static`, `modal`, `topology` or `buckling` ([buckling](#buckling)).

A study holds:

- `meshSize` (mm; 0 = auto, the body's bounding-box diagonal / 15) and
  per-face `refinements` (a target element size on one face).
- `converge` (a static or buckling study's): `auto`, `always` or `never`,
  whether a run refines round its peak stress until it settles
  ([a peak that has settled](#a-peak-that-has-settled)).
- `loads`: each a `Fixed` support, a `Pin` support (a rigid pin in a bore
  that holds where the part presses on it, [pin supports](#pin-supports)),
  a `Contact` support (the face rests on the ground or on a rigid
  occurrence, pushed and never pulled, with friction, solved on CalculiX:
  [contact](#contact)),
  a `Force` (a total vector in newtons,
  spread as uniform traction over the face), a `Pressure` (MPa along the
  inward normal, positive pushes into the part), a `Torque` (N·mm about an
  axis) or a `Remote` force (newtons acting at a point off the face)
  ([torque and remote loads](#torque-and-remote-loads)).
- `gravity` (static and buckling; m/s² in world axes, 0 = off; needs a
  material density) and `numModes` (modal: default 6; buckling: its number
  of factors, default 3).
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

### Torque and remote loads

A shaft, a key, a knob or a gear under torque, and a load at a point away
from the face that carries it (a lever's end, a bracket's hole), are loads of
their own rather than forces picked tooth by tooth.

- **Torque** (`magnitude` N·mm, right-handed about the axis): the traction
  `t = (T/J) â × (x − o)`, with `J = ∫ r² dA` the face's polar moment about
  the axis. That is exactly `T` about the axis, and no net force on a face
  symmetric about it. The axis is `axisOrigin` + `axisDirection`; with none,
  the face lends its own, which only a **cylindrical** face has
  (`fem::CylindricalFaceAxis`: the surface's axis through the face's
  middle, signed so its largest component is positive). A torque on any
  other face without an axis is refused, naming what it needs: by
  `add_load` before the study changes, by the panel's click, and by the run
  for a file that carries one (`fem::CompleteLoadGeometry`). An off-axis
  face (one of a knob's flats) carries `T` about the axis plus the force
  that puts its share there; a set of faces symmetric about the axis
  carries no net force.
- **Remote** (`magnitude` N along `direction`, at `point`): the face
  carries the force F at the point p the way an RBE3 spreads it,
  `t(x) = F/A + (I⁻¹ M) × (x − c)` with `M = (p − c) × F` and `I` the face's
  area "inertia" `∫ (|r|² E − r rᵀ) dA` about its centroid `c`: the face's
  share of the force plus the linear traction that carries the moment. A
  remote load without a point is refused, on load and by `add_load`.

Both are assembled in `AssembleFaceLoads` (`FemSolverInternal.h`), which the
static solve and topology optimisation share, at the same Gauss points that
integrate `J`, `A`, `c` and `I`, so the totals come back exact. On an
assembly study the axis and point are world coordinates; a face the imprint
splits is solved as ONE face again (`MeshPlannedStudy` gathers a load's
pieces into one `FaceLoad`), so the field is the unsplit face's. A force is
still split by area, which is the same thing for a uniform traction.

The study's input hash covers both through the serialized study
([studies as jobs](#studies-as-jobs)). Tests: `tests/test_fem_loads.cpp`
(`[loads]`): the nodal loads sum to `T` about the axis to 1e-9; a shaft's
end twists by `TL/GJ` and its surface carries `√3·Tr/J`; a torque on a
cylinder's side through `RunStudy` twists the end by `TL/2GJ`; a remote
force's nodal loads are `F` and `(p − c) × F`, and the reactions at the
clamp answer them; the refusals.

### Pin supports

A part hung on a pin is not clamped: the pin pushes on the side of the bore
the load presses onto, the other side is free to lift away, and the part
turns on it. Clamping the bore (`fixed`) glues it all round, holds the far
side in tension and puts a clamp's edge on the bore's rims. A `pin` support
(`LoadType::Pin`; SolidWorks' *bearing fixture*, Ansys' *cylindrical* or
*compression only* support) holds it the way the pin does.

- **On a cylindrical face only**: a bore, or a shaft in a bearing. Its axis
  is the face's (`fem::CylindricalFaceAxis`, set by `CompleteLoadGeometry`
  at the run). Any other face is refused, naming what is accepted, by
  `add_load`, by the panel's click and by the run. A pin takes a `region`
  like any support ([loads on part of a face](#loads-on-part-of-a-face)).
- **What it holds.** The pin is rigid and does not move. At each node of
  the face it holds the RADIAL displacement (from the axis) and the AXIAL
  one, and leaves the slide ROUND the axis free: the part turns on it
  without friction. `fixRotation` (`fix_rotation: true`) stops the face
  turning as a whole -- the sum of its nodes' slides round the axis, each
  weighted by its share of the face's area and its radius, is tied to zero
  -- while each node may still slide. On a whole bore that is a pure turn;
  on part of one (a region) it holds some slide across the axis too.
- **Compression only.** The radial direction is signed into the material by
  the face's outward normal, so one rule serves a bore and a shaft: the pin
  may push the part, never pull it. Which facets it pushes on is found by
  iterating linear solves (`SolveWithPins`, `src/core/fem/FemStudy.cpp`):
  every facet in contact first; then a facet whose nodes' radial reactions
  sum to a pull leaves the contact, and one out of it whose nodes move into
  the pin on average comes back, until the set holds still (30 rounds at
  most, else a study error; a lever takes three to eight). Decided per
  facet, not per node, because a tri6 under a uniform pressure loads its
  corners not at all, and a varying one a little either way. A facet on
  the contact's edge that flips each round is let go: once the set
  repeats, only the facets every state of the cycle had in contact stay,
  and that is solved once more. An answer whose contact still pulls (more
  than a thousandth of what it pushes) is refused. Each load case finds its
  own contact.
- **In the solver** (`fem::SolveCases` with `fem::Supports`): a node a pin
  holds takes its DOF in its own frame (into the material, round the axis,
  along it; `K' = Tᵀ K T` per element, loads rotated the same way, the
  answer rotated back), so a held direction is eliminated exactly as a
  fixed DOF is, and K stays symmetric positive definite. A tie (`fem::Tie`)
  eliminates its largest entry's DOF in terms of the others.
- **A mechanism is refused, never solved.** Before each solve the held
  directions are tested against the six rigid motions (`FreeRigidMotion`:
  the 6x6 Gram matrix of the rows `[d, (x − c) × d]`, a tie the sum of its
  rows): a rank under six names the free motion. A lever on ONE pin free to
  turn: "free to turn about an axis along … through …: pin N turns freely,
  so the part needs a second support, or fix_rotation on the pin"; two
  pins on one axis the same. A load that lifts the part off its pins (the
  contact shrinks until nothing holds it): "the loads pull it off its pins".
- **What it reports.** Every static result lists what each support carried
  (`FemResults::reactions`, the internal force less the applied load summed
  over the support's nodes, each node counted once): its force on the part
  in newtons, its moment about the frame's origin in N·mm (`moment_n_mm`:
  the part's frame, world on an assembly study), and for a pin the area of
  its face in contact, of the face's. A lever's two pins carry its statics,
  force and moment.
- **What it costs.** Each round is a solve: a pinned study takes as many
  factorizations as rounds, where a clamped one takes one.
- **Not modelled.** Clearance: a pin smaller than its bore presses on a
  narrower arc and raises the bearing stress, which this underestimates.
  And a pin over a LENGTH of shaft (a bushing's sleeve, a band) holds it
  along that length, so it takes moment as a clamp does: two such pins
  share an overhung load as their stiffness says, not by the lever rule
  over their middles. The woodcutter's crank, leaned on outside the left
  bushing, rests on that bushing alone, its moment taken along the sleeve
  (`moment_n_mm` shows it); a bushing with clearance would tilt in its bore
  and let the far one carry its share. The bending moment between the load
  and the near pin is the overhang's either way.
  Friction. A pin on a modal study (a mode has no load to press the part
  onto it), a topology study (it freezes the faces its fixed supports
  hold) or an assembly study (held by fixed faces and rigid contacts) is
  refused, naming why. A pin beside a CalculiX [contact](#contact) is
  refused too: its contact set is found in-house, which cannot hold the
  other ([contact and pins](#contact-and-pins)).
- **The GPU declines it.** The femgpu SpMV applies each element matrix
  relative to its first node's translation, which holds only when every node
  is in x, y, z; a pinned study above the iterative threshold solves on the
  CPU's PCG, and the result's `solver` says "GPU declined: a pin support
  holds nodes in rotated frames, which the GPU solve does not take".

### A peak on a support's edge

A clamp's edge is a singularity: the stress where a fixed face meets a free
one grows as the mesh is refined, so the study's peak, and the
`factor_of_safety` read from it, can be a property of the mesh at the clamp
rather than of the part. Every static result checks
(`fem::SupportEdgeZones`, `fem::PeakAwayFromEdges`): a support's EDGE is
its rim, the facet edges it shares with boundary facets it does not hold
(a clamp's rim, a bore's rims). A node lies ON that edge when it is within
the support's **reach** of the rim, in millimetres, derived from the
support (`fem::SupportAwayMm`):

- a pin: a quarter of its bore's radius;
- a fixed face: a quarter of its facets' largest extent along their
  principal axes (5 mm for a cantilever's 20 x 2 mm root).

A mesh coarser than that uses at least one element (every node of a tet
that has a rim node), as the rule did before it was a distance. Measured by
elements alone, "away" shrank as the mesh refined and crept toward the
singularity with it: a clamped plate's 40.7 MPa at the auto size became
44.7 at 0.4 mm. Measured by reach it is 40.6, 41.0, 41.2 and 41.3 MPa at
auto, 1.5, 0.75 and 0.4 mm (1.6 % over the four), against 41.25 for
M c / I at 5 mm from the root. The
quarter of the clamped face's SMALLEST side (its 2 mm thickness) was tried
first and crept as before (40.7, 43.2, 44.2): the stress a clamped member
carries changes along it over the size of the section, not of its
thinnest side.

If the peak von Mises node lies in a support's zone, the result carries
`supportEdgePeak`: which support, and the largest von Mises outside every
zone. Each static body keeps its zones (`supportEdgeZone`, saved with the
result), so a field read later is flagged the same way: `study_results`
with `layer_normal` reads the stress across the layers away from the edge
too (`max_across_layers_away_mpa`). `study_results` reports it as
`peak_at_support_edge` {support, type, max_von_mises_away_mpa, away_at,
max_across_layers_away_mpa?, across_layers_away_at?,
factor_of_safety_away, governing?} per body, per case and for the top-level
peak: `factor_of_safety_away` is the governing (smaller) of the factor
against yield and the factor across the layers, and `governing` names
which. The factor of safety's receipt says its peak sits on a support's
edge. The headline number is unchanged; it is no longer silent, and the
convergence check reads the away value instead
([a peak that has settled](#a-peak-that-has-settled)).

A printed PLA lever on two Ø5 mm bores, pushed down at its end (`[pin]`,
"a lever on two pins"), its bores meshed at 1.0 and then 0.5 mm: clamped,
the peak grows with the mesh (11.5, then 12.3 MPa) and is flagged on a
bore's rim; pinned, it is the lever's bending at the second bore and holds
still (11.3, then 11.4 MPa, against M c / I = 10.5).

### Loads on part of a face

A drum turning on a stretch of an arbor, a rod's small end on the middle of
a wrist pin, a pin held where it is pressed into its bosses: the load is on
PART of a face that is not a face of its own. A load or a support takes
`region` (`fem::LoadRegion`, v183):

- `{normal, from_mm, to_mm}`, a **band**: the face between the planes
  `n·p = from_mm` and `n·p = to_mm` (`n` the unit normal, mm in the part's
  frame); or `{center_mm, radius_mm}`, the face **within a radius** of a
  point.
- It is applied to the face's **mesh facets whose centroids lie in it**
  (`MeshPlannedStudy`): a force's whole `magnitude` spread over them, a
  pressure on them, a support's nodes theirs; a torque or a remote load is
  the field over them. The band's edge falls inside a row of facets, so the
  resultant's place is the mesh's: a finer mesh puts it closer.
- **Refused**: a zero normal, a band whose `to_mm` is not past `from_mm`, a
  radius that is not positive, a key that is neither form (naming both) —
  by `add_load` and on load; a region on an assembly or topology study
  (a topology study freezes whole faces); and at the run, a region that
  holds none of its face's facets, naming the load.
- The generators load a seat this way rather than modelling a step on the
  shaft to make it a face: the clock's barrel arbor, the engine's wrist pin
  (`generators/_lib.py`'s `pin_strength`).

The serialized study carries it, so it is in the input hash. Test:
`tests/test_fem_loads.cpp`, "a pull on half a cantilever's end bends it by
its eccentricity": a pull along the axis on the upper half of the end face
drops the tip by `F (h/4) L² / 2EI`, within 10 %; on the whole face, not at
all; a region off the face is refused, and so is an empty band.

### Units

The solver works in mm–N–MPa (lengths and displacements mm, forces N,
stresses and moduli MPa) with mass in tonnes, so frequencies come out in Hz.
The material library (`src/cad/CADMaterials.h`) stores `youngGPa`, `poisson`,
`yieldMPa`, `interlayerMPa` (a printed material's strength across its
layers, [printed layers](#printed-layers)) and `density` (kg/m³), each 0
when unknown; `describe` lists them as `materials`. A body whose material
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
that has something to solve, answers `job: <id>` (and `kind`); so does
`check_requirements {async}`, whose job (kind `requirements`) runs studies
of its own ([requirements](../requirements/README.md#checking-is-a-job)),
and `design_sweep {async}`, whose job (kind `design_sweep`) regenerates
copies of the model ([design sweep](../requirements/README.md#design-sweep)).
`study_status {job}` reports a running job (kind, study, label,
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
boundary and drops its answer: after the meshing, or within a few
iterations of an iterative solve ([the solver](#the-solver)); a
factorization has no interruption points, so a cancel during one lands
when it ends.

**Results keyed by inputs.** A run computes a 64-bit input hash
(`src/core/InputHash.h`; FNV-1a) over the exact B-Rep of the bodies it reads
(memoized per shape identity), their names, materials and the library's
numbers for them, thread regions and face names, and:

- an FEM study (`fem::StudyInputKey`, `fem::AssemblyStudyInputKey`,
  `src/core/fem/FemInputKey.cpp`): the study as saved with its expressions
  EVALUATED (so a variable feeding a load is an input by its value), its
  name and id aside; on an assembly, each taking-part occurrence as saved
  and its placed bodies;
- a motion study (`motion::InputKey`): the definition (name and id aside,
  the panel's Preview or Validate override included), every occurrence with
  its placement, every mate with its current value -- a run walks from the
  pose the document is in to its first sample, and where that walk begins
  picks a linkage's branch, so a scrub or a drag moves the next run's
  inputs --, relations, configurations and the active one, through every
  sub-assembly, and every placed (part, configuration). A `leave_at` is
  the exception: the pose it writes in comes from the result's own samples,
  on the path the result describes, so `ApplyMotionSample` re-keys that
  study's result to the posed assembly's hash (`RekeyAfterLeave`,
  `src/ui/MotionActions.cpp`) -- the next `run_motion_study {leave_at}` is
  reused, the result is not stale, and a project saved there reopens with
  it current. The hash it was run for stays in the session's store, so an
  undo back to that pose is answered too. Another study on the assembly is
  not re-keyed: its start pose did move.

A body's B-Rep is hashed as the project reads it back: written and read
through BinTools (the baked geometry's format), with OCC's bookkeeping
flags set one way, the bytes of BinTools' format 1 (no cached UV points).
Reading is not idempotent -- a plane's frame is rebuilt from its normal
and X direction as it is read, and for some frames the X flips a last
place between two values on every read (the engine's cam pulley has one) --
so the shape is read back until a read repeats an earlier one, and the
smallest digest of that cycle stands for it: the regenerated shape and the
one a reopen reads are one input. Up to 32 reads: most shapes settle in two
or three, but the engine's one-piece camshaft (tapered lobes) drifts for
more than eight, and hashed within eight it opened every requirement over
the engine stale.

A version string leads each hash (`motion/3`, `fem/3`); a change to what a
run computes bumps it. The solver a run picks (GPU or CPU, direct or
iterative) is not an input: every one is held to the same residual. A finished whole run is kept by its hash in the session
(`AppState::motionResultsByInput`, 32; `studyResultsByInput`, 8; a cancelled
or failed run is not kept), and `Start…Run`/`RunMotionNow` ask first: a match
-- the study's own result, or one of the session's -- is installed as the
study's fresh result and nothing starts. The reply says `reused: true`,
every result carries `input_hash`, and `recompute: true` computes anyway
(Shift+Run in the panel, which also says "Reused" when it did). A reuse
answers before the one-run-at-a-time check: it starts nothing.

**Saved with the project.** Every result is written beside its study with
its hash: a part's or an assembly's static, modal or topology result as
`studies/<id>-<slug>.result.json` and `.result.bin` (topology since v33 and
its hash since v148, the others v153, the two files v154), a motion result
as `motion/<id>-<slug>.result.json` and `.result.bin`. What is kept is what
the panel and the RPCs read back, and nothing else
([what a saved result keeps](#what-a-saved-result-keeps)). A reopened
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
(`inputhash::ShapeDigest`): the shape as the project reads it back (written
and read through BinTools, the baked geometry's format), its TShapes'
`Modified`, `Checked` and `Free` flags set one way, written as text without
UV points, every `-0` read as `0`. Meshing a face -- Netgen for a study, BRepMesh for
the viewport -- clears its `Checked` flag and its cached UV points, and a
shape read back from the project's baked geometry carries a `+0` where the
regenerated one had a `-0` (a circle's axis); the pan/tilt yoke's envelope,
meshed in place by its topology run and saved meshed, reopened hashing
otherwise, and its 80 s run was paid again on every reopen. A topology run
now meshes a copy, as a static plan does. And reading a direction
normalises it again: a unit vector that came off a rotation reads back a
unit in the last place away (a 6005's chamfer line, `-0.70710678118654746`
saved and `...757` read), so the pan/tilt head's pan and tilt studies, whose
hash reads every bearing, read stale after every reopen until the hash read
the shape as it comes back.

The `[studyjobs]` tests prove the hash moves on each input class -- geometry,
material, mates, a placement, relations, settings, loads, an expression's
value, configurations -- and on nothing else (`tests/test_motion.cpp`,
`tests/test_study_inputs.cpp`); that a result of each kind -- static, modal,
topology, motion -- reopens with its hash, current, its part hashing as it
did, and reads stale once an input moves; that a part meshed in place,
or with a bore, hashes the same after a reopen; and that a motion study on
an assembly with a relation, its bearing parts saved and read back with the
project, reopens current.
`tests/study_jobs_smoke.py`, `fem_async_smoke.py`, `topo_smoke.py` and
`topo_build_smoke.py` drive the ids, cancels, disconnects and reuse over the
RPC; `tests/study_reopen_smoke.py` saves a project with a study of each kind
and a topology check, opens it again, and has each one reused.

### What a saved result keeps

A result is reproducible from its inputs, so it is kept small: what a panel
or an RPC reads back from it, at the precision derived data has, and
nothing rebuilt from something else. `fem::KeepWhatIsRead`
(`src/core/fem/FemSolver.h`) cuts a solved runtime down to that, and every
stored result passes through it (`Document::StoreStudyResults`,
`AssemblyDocument::StoreStudyResults`) -- so the result a run leaves in
memory is the one a reopen reads back, value for value.

| kind | kept | left out |
|---|---|---|
| static, modal | the boundary surface: its tri6 triangles and their faces, the nodes they use (and the peak nodes), each of those nodes' displacement, von Mises stress and stress tensor per load case, or its mode shapes; the maxima; the solved mesh's node and tet counts | the tets and every interior node: the contour, the deformed shape, a face's motion (`probe_study`) and the peak markers all read the surface |
| topology | the whole tet10 mesh, its boundary, the per-tet density, the summary | -- the iso-surface, the STL export and a static study of the shape cut tets |
| motion | every sample's poses, mate values, clashes and unknown contacts; the pair counts | -- |

**Float, in a binary file.** Fields are float -- derived bulk data is
([the model is double](../conventions.md#the-model-is-double)) -- rounded
when the result is stored, not only when it is written. The arrays go to
`<base>.result.bin` as raw little-endian `f32`, indices as `u16` when every
one fits and `u32` (or `i32`, where a cut's triangle lies on no face, -1)
otherwise; `<base>.result.json` keeps the metadata, `input_hash`, and a
reference {`type`, `offset`, `count`} for each array
(`ResultArrayWriter` / `ResultArrayReader`,
`src/core/io/SerializationInternal.h`). A motion placement is the model's,
so it stays `f64`: each distinct matrix once, and each sample's poses as
indices into them -- an occurrence that holds still through a run is one
matrix. The build links no zstd, so nothing is compressed. A topology
result's density digest, which a static study of the shape keys on, is
taken over the density as float, so a run's digest and a reopened one's
agree.

**Written once.** The bin goes first and the json, which carries
`bin_bytes`, second; a json whose bin is missing or another size (a torn
save, one file copied without the other), or a result that does not read,
drops that result and the study runs again. The open reports it as a
derived drop, never a failed load
([a lossy load](../versioning/README.md#a-lossy-load)). Each file is skipped when its bytes are the ones
on disk, so a save whose result has not changed writes neither and leaves
its modification time alone. An autosave writes no result: its sidecars are
the single-file form, which carries none
([autosave](../versioning/README.md#autosave-and-recovery)). And the
project's history keeps none
([saved results stay out of the history](../versioning/README.md#saved-results-stay-out-of-the-history)).

**What it buys.** The pan/tilt yoke's projects, saved by v153 and again by
v154 (`scratch/pantilt`):

| result | kind | v153 | v154 |
|---|---|---|---|
| yoke, 3 g (151 168 nodes, 3 cases) | static | 52.0 MB | 3.88 MB (13.4x) |
| yoke_solid, 3 g | static | 33.1 MB | 2.20 MB (15.0x) |
| yoke envelope: shape | static of a topology result | 19.2 MB | 1.87 MB (10.3x) |
| yoke envelope: solid | static of a topology result | 22.9 MB | 2.16 MB (10.6x) |
| yoke envelope (67 509 nodes) | topology | 6.69 MB | 2.95 MB (2.3x) |
| pan 360 | motion | 0.60 MB | 0.38 MB (1.6x) |
| tilt 90 | motion | 0.43 MB | 0.09 MB (5.0x) |

`yoke_envelope.lob`'s results went from 135 MB to 13.5 MB and
`yoke_topo.lob`'s from 86 MB to 6.5 MB; the rest of each project -- baked
geometry above all -- is untouched (147.6 MB to 26.2 MB for the envelope's
tree). A topology result keeps its volume and shrinks least; a static one
was nine-tenths interior. Every result reopened as it was saved, current
or stale alike; the two shape studies, current under v153, read stale
once, because their saved hash names the topology result by a digest of
its double density.

`tests/test_study_inputs.cpp` reopens each kind and compares what came back
with what the run stored, keeps a static result to its surface as float
with its peak markers on their nodes, saves an unchanged result twice and
once more after a reopen without touching either file's time, and has an
autosave write no result.

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

Netgen meshes as well as gmsh at a comparable node count (worst tet within
±0.1 of gmsh's on five parts; gmsh's default optimiser left slivers on thin
plates), and second-order tets with midside nodes on the true surface bend
right with ONE element through a thin wall: a cantilever plate 60 x 20 x t
at the auto size (4.2 mm), t = 2, 1 and 0.8 mm, has its tip deflection
within 0.9 % and its mid-span bending stress within 0.6 % of a converged
mesh. A thin wall needs no special sizing for a static study.

**Refinements** (`MeshSettings::faceSizes`, a study's `refinements`) go
through Netgen's shape properties: each sets
`OCCGeometry::GetProperties(face).maxh` before the geometry is built (its
constructor copies each face's properties then), the route Netgen's python
`face.maxh` takes, and Netgen grades that size into the face's edges and
the volume behind it. Any refinement also takes Netgen's `grading` from 0.3
to 0.2. The legacy `OCCGeometry::SetFaceMaxH` array sized the face's own
triangles and nothing else, so a fine face met coarse tets head on. A
steel plate 100 x 40 x 5 with a Ø10 hole in tension, its bore refined at
the auto global size (a converged mesh gives 24.55 MPa there):

| bore refinement | before: worst tet, peak | now: nodes, worst tet, peak |
|---|---|---|
| 0.3 mm | 0.014, **36.79 MPa** (+50 %, a fake) | 55.8 k, 0.188, 24.55 MPa |
| 0.2 mm | "volume meshing failed" | 122 k, 0.381, 24.56 MPa |
| 0.15 mm | fails | 202 k, 0.332, 24.57 MPa |

The properties live in a STATIC table keyed by shape
(`global_shape_property_indices`), shared by every mesh in the process,
and `OCCGeometry::MeshFace` adds an entry for every face it meshes,
refined or not. So `fem::Mesh`:

- holds one mutex from setting the properties until the volume mesh and its
  second order are done: two study jobs meshing at once would otherwise
  write the table together (our faces are deep copies, so their keys never
  collide, but the table is not thread-safe);
- takes its entries out on every way it returns, a failure's and an
  exception's included. Netgen has no remove call, so each entry of ours is
  reset to the default properties and the indexed map is rebuilt without
  our shapes. Otherwise the table grew with every run, held every meshed
  copy alive, and a refinement left on a shape sized its next mesh.
  `fem::NetgenPropertyEntries` counts what is left of a shape, for the
  tests.

A mesh failure is reported as the study error, naming the face when Netgen
says which, and its shortest edge when that is under a micrometre: the usual
cause, and one no picture and no BRepCheck shows: two faces meant to
coincide that miss by a few micrometres leave a zero-area face bounded by
edges that short (the pan/tilt yoke's arm and base did, while sketch planes
were single precision). There is no defeaturing step: suppress cosmetic features before meshing,
or, on an imported part, remove them with Delete Face
([modeling](../modeling/README.md#delete-face)).

## A peak that has settled

A peak at a hole or a fillet read at the auto size is about 5 % low (the
plate above: 23.41 MPa against 24.55; a 26 -> 20 bar's r 2 shoulder: 19.83
against 20.83), and shrinking the GLOBAL size does not find it: Netgen's
curvature sizing already sets the size there, so steps of 5, 3, 2, 1.5 and
1 mm moved the plate's peak under 2 % each while it stayed low, and it
arrived only at 0.75 mm over the whole part (353 k nodes, 40 s). Refining
the face the peak is on converges in two steps at 3.5-11 k nodes in a few
seconds. So a static run refines where its peak is
(`fem::SolveConverged`, `src/core/fem/FemStudy.cpp`):

1. Take the governing peaks of each body, the worst over its load cases:
   von Mises, and for a printed material (one with an interlayer strength)
   the stress across the layers along the run's `layer_normal`, or without
   one the largest principal stress, which bounds it in every direction. A
   peak that lies on a support's edge is a singularity and never settles,
   so its away value is used instead
   ([a peak on a support's edge](#a-peak-on-a-supports-edge)).
2. Find the B-Rep face each peak node lies on: of the faces its facets
   belong to, the smallest (the bore rather than the flat it meets); an
   interior node takes the nearest boundary node's. Every boundary node
   within 90 % of the peak (off the support edges, for an away value) adds
   its face too: a bar with two shoulder fillets peaks on both alike, and
   refining one only moved the peak to its twin, which read as settled
   (0.7 % on one step, 3.7 % low).
3. Halve that face's element size, starting from the size its elements have
   now (the mean edge length of its facets), and re-mesh and re-solve.
4. Stop when every governing peak moves by less than 2 %, or after four
   refinements.

The refinements belong to the run: they are never written into the study.
A study's `converge` says when it runs, and `run_study {converge}`
overrides it for one run; any other value is refused, naming the three:

- `auto` (the default) runs it while the first solve took less than the
  `converge_auto_s` preference (10 s by default; Preferences > Simulation,
  `set_settings`), and keeps refining while each step does: a step costs at
  least what the last one did, so one after a step that took longer is not
  started, and the result says `stopped: "time"` with the time that step
  took.
- `always` runs it whatever it costs.
- `never` skips it, and the result says the peak was not checked, so a peak
  never checked does not look like one that was.

Taking the run's own time needs no estimate: the study that just ran is the
measurement. The mode and the layer normal are inputs (`fem::RunInputKey`),
so a run with them is not answered from a stored result without.
`study_results` (and a synchronous `run_study`) reports `convergence`
{status: converged | not converged | not checked, converged, stopped?
(time | steps | failed), stopped_seconds?, why?, reason?, last_change_pct,
layer_normal?, steps [{refined [{body, face, size_mm}], nodes, seconds,
peaks [{body, quantity, away_from_support_edge, mpa}]}]}, saved with the
result. The factor of safety's receipt says whether its peak converged, and
a requirement read from an unsettled peak (the loop ran out, stopped for
time, or never ran) is judged on the value it has and carries
`not_converged` with the reason, listed again under its own heading
([requirements](../requirements/README.md#evidence)). The Simulation panel has a
Converge peak combo on a static study and a line under its result.

Measured, `always` at the auto size:

| part | refinements | nodes at the end | peak | converged | handbook |
|---|---|---|---|---|---|
| hole plate | 2 | 11.7 k | 23.41 -> 24.62 MPa | 24.55 | 24.22 (Heywood) |
| shoulder fillet | 2 | 17.7 k | 19.83 -> 20.78 MPa | 20.83 | 20.48 (Peterson, ±3-5 %) |

5-7 s each. It also catches the kind of fake a bad mesh made: a peak
that jumps 50 % on one step and back on the next does not converge, and is
flagged.

What it costs: each step re-meshes and re-solves every body of the study,
on the study's own solver: a CalculiX study runs ccx once a step.
On the hole and the fillet that is a few seconds a step. A pin-supported
study solves on the CPU (the GPU declines a pin) once per contact round,
and a shaft is often ONE cylindrical face under its pins and its loads, so
halving that face refines the whole shaft: the woodcutter's camshaft
crank end took 9 s at its generator's 2 mm and 9 minutes at 1 mm. That is
why the default is `auto`: it reports such a study unconverged with its
reason instead of making every run wait.

The woodcutter's leaned-on pin studies (crank end, crank, handle pin) do
not settle within a 300 s step: the crank's von Mises away from its pins
moves 3 % on its second refinement (66.2, 66.0, 64.0 MPa), but its stress
across the layers and the other two parts' peaks move 15-85 % a step. The
peaks they chase are not a support's rim: a compression-only pin's
contact ends inside its band, and the camshaft's hand load is a band that
runs to the shaft's end, where its across-layers peak sits (13.6 MPa at
2 mm, 25.2 at 1 mm). Neither edge is a support's, so neither is read
away, and the result says "not converged" with the step it stopped at. A static
study of a topology result solves the result's own tets, which cannot be
refined, and reports "not checked" with that reason.

## Buckling

A part that fails by buckling passes a static study with a comfortable
factor of safety: a slender strut, a thin printed wall, a rib, a web or a
tube looks finished. A **buckling** study (`type: buckling`,
`fem::AnalysisType::Buckling`) answers by what factor the loads can grow
before the part loses stability, and in what shape. It is solved in-house,
with no outside dependency; [CalculiX](#calculix) is its test oracle and a
second answer on request.

### The method

`fem::SolvePreparedStudy` (`src/core/fem/FemStudy.cpp`), for each body:

1. **The base state** is the study's linear static solve at factor 1, on
   whichever solver a static study of its size would use (direct, PCG or
   GPU). A pin's contact set is the one the
   [pin iteration](#pin-supports) settles on, then held bilaterally: linear
   theory scales the load without turning it, so the set holds at every
   factor, and the result's `notes` say so. Gravity is part of the load set
   and is scaled with the rest, and the notes say that too.
2. **The geometric stiffness** K_G (`fem::SolveBuckling`,
   `src/core/fem/FemSolver.cpp`) is assembled from the base state's stress:
   at each of a tet10's four Gauss points, with stress tensor σ, each pair of
   nodes adds ∫ ∇N_a · σ · ∇N_b dV to the 3×3 identity block. CalculiX's C3D10
   uses the same rule, so the same mesh gives the same matrix. A held DOF
   leaves K_G exactly as it leaves K: both are scattered through one
   reduction of the supports (the pins' frames and ties included).
3. **The eigenproblem** −K_G φ = μ K φ is solved for the largest |μ|, with
   λ = 1/μ: K is the B matrix (positive definite once held) and K_G may be
   indefinite, which Spectra's `SymGEigsSolver` in Cholesky mode takes as it
   is (`Spectra::SparseCholesky`, a `SimplicialLLT` of K's lower triangle).
   The largest positive μ give the smallest positive factors, which govern;
   a negative one is the factor under the reversed load. When every factor
   found is negative, the largest algebraic μ is looked for on its own; when
   there is none either, the body answers `buckling_factor: null` with the
   reason ("nothing in this load buckles"), never a number picked for it.

`num_modes` is the number of factors and defaults to 3
(`fem::kBucklingFactorsDefault`): a symmetric section has two equal first
modes. `load_cases` are refused on a buckling study, by `set_study`, the run
and the file reader: a factor scales one load set, so each way the part is
loaded is a study of its own.

**Memory.** The direct factor of K is the limit, and it fills in faster than
the DOF count grows. `fem::BucklingMemoryEstimate` is calibrated from measured
runs (the steel column at 5 to 1.35 mm, peak resident memory over the
process's start: 5.7k DOF 45 MB, 31k 186 MB, 66k 423 MB, 143k 1.17 GB, 205k
2.01 GB; the hidden `[buckling-calibrate]` case prints them): 1.1 × (60 MB +
39 B × DOF^1.45), above every measured run. A run it says will not fit is
refused before anything is solved, naming both numbers: "a buckling solve of
N degrees of freedom would need about X GB, and Y GB is free". (A modal study
has no such estimate.)

### What the result says

Each body gets `buckling_factors` (every factor found, signed, ascending by
magnitude), `buckling_factor` (the smallest positive one) and its mode shapes,
stored as modal ones are (`ModeShape::factor` beside `frequencyHz`) and shown
with the deform slider and the mode list. The base state's peak von Mises and
displacement come beside them (`base_state`).

**The governing number.** Elastic buckling alone over-promises in three ways,
and each correction is derived from the part (`ReadBuckling`,
`src/core/fem/FemStudy.cpp`):

- **Imperfection.** A curved wall buckles at a fraction of its linear load.
  The knockdown is NASA SP-8007's, γ = 1 − 0.901 (1 − e^(−φ)) with
  φ = √(R/t)/16, where R is the radius of curvature of the cylindrical,
  conical (r / cos α) or spherical B-Rep face the first mode's motion peaks
  on, and t is the wall there, measured through the mesh along the inward
  normal: of the nodes inside one face moving at least nine tenths of the
  peak, the thinnest wall. A wall with R/t of at least 10 (thin-shell
  theory's bound, and where SP-8007's tests begin) is a shell of its kind;
  anything thicker is a `member` and a flat face is `flat`, both with γ = 1:
  plates and struts are not imperfection-sensitive the way shells are, and
  the interaction below covers what imperfection does to a stocky strut.
  `knockdown` {value, class, face, radius_mm, wall_mm}.
- **Yield.** `yield_factor` = yield ÷ the base state's peak von Mises (each
  node against the weakest material of the tets that use it). For a printed
  material also `interlayer_factor` = interlayer strength ÷ the peak tension:
  the largest principal stress, which bounds the stress across the layers in
  every direction, or with `layer_normal` the stress across them
  (`interlayer_from`).
- **Interaction.** A member near the transition between yielding and
  buckling fails below both. The factor that governs is Rankine–Gordon's,
  1/λ_gov = 1/(γ λ_b) + 1/λ_yield, with λ_yield the smaller of the yield and
  interlayer factors: `governing_factor`, with its three inputs beside it.
  With no positive buckling factor it is λ_yield; with no strength, γ λ_b.

`factor_of_safety` on a buckling study is its governing factor, and so is
the requirement quantity `buckling_factor` (`study_result`, used with
`at_least`), which carries the `not_converged` warning a static peak does.
A sweep lists it as not swept, as it does every study requirement.

**Honesty flags**, the counterpart of the
[support-edge flag](#a-peak-on-a-supports-edge):

- `mode_at_load_edge` {load, type, face}: the first mode's peak motion lies
  at a support's or a load face's edge (within that face's reach, as the
  support-edge flag measures it), and the mode is local there -- every node
  moving half the peak or more lies within twice that reach of the peak and
  within a quarter of the part's size. That is crippling where the load
  enters, not member buckling. A column's free end moves most too, but its
  whole length moves with it, so a column is not flagged.
- `through_thickness`: the first mode bends a wall with one element through
  it (`wall_layers`, round(wall / element size) where the motion peaks). It
  is a warning, not a refinement, because measured it reads close: below.
- **Base-state convergence** follows the study's `converge`, as a static
  study's does ([a peak that has settled](#a-peak-that-has-settled)): its
  peaks are the base state's, each refinement re-solves the buckling too,
  and `convergence` is reported the same way.

The receipt's claim is "governing buckling factor >= 1", basis `predicted`,
oracle `in_house/buckle` (`calculix/buckle` on CalculiX). Its note says that
linear buckling is an upper bound the knockdown and the interaction correct,
and that snap-through -- an arch or a curved wall loaded across its curve --
needs a nonlinear run: a static study with `nonlinear {large_deformation:
true, snap_through: true}` ([nonlinear static](#nonlinear-static)).

### Solver choice

`auto` and `in_house` solve buckling here. `calculix` solves the base state
as a static CalculiX study does (the answer checked against our K), then runs
a second deck: `fem::WriteDeck` with a `*BUCKLE` step for `num_modes` factors
(accuracy 1e-10, where ccx's default is 1 %), under the base state's
supports and contact set (`fem::BucklingOnCalculix`). The factors and each
mode's shape are read from the `.dat` (`fem::ReadDatBuckling`; ccx prints the
modes there as well as in the `.frd`, to seven digits), and the same keys are
reported with oracle `calculix/buckle`. `export_study_inp` on a buckling study
writes that deck.

### Measured

**Closed forms** (`tests/test_fem_buckling.cpp`, `[buckling]`, in-house, so
they run where ccx is absent):

| case | answer | closed form |
|---|---|---|
| Euler, fixed–free, 10×10×200 steel, 2.5 mm, 10 204 nodes | 10.2876 | π²EI/4L² = 10.2808 (+0.07 %) |
| the same 10×20: strong over weak axis | 3.968 | 4 (I ratio; shear takes the rest) |
| wide cantilever plate, 60×20×1, 2 mm, 4 479 nodes | 67.313 | Et³/12(1−ν²) per width: 67.786 (−0.70 %) |
| thin cylinder, R/t 25, capped, 2 mm, 33 283 nodes | 730.98; knockdown 0.758 (`cylinder`, the bore's R 24.5 over a 0.98 mm wall) | classical Et/(R√(3(1−ν²))): 760.55 (−3.9 %); SP-8007 at that R/t, exactly |
| stocky strut, 10×10×128 at 10 kN | buckling 2.508, yield 1.895, governing 1.079 | Rankine–Gordon, to 1e-12 |
| pure tension | `null`, "nothing in this load buckles" | |

**Through the thickness**, measured before anything was built on it: the
60×20×1 mm plate's first factor at one element through the wall and at two
(a refinement on both faces sized to half the wall; the hidden
`[buckling-measure]` case, `LOBOLE_MEASURE="b L size refine; ..."`).

| mesh | elements through | nodes | factor | against two layers |
|---|---|---|---|---|
| 3 mm | 1 | 2 173 | 67.683 | +1.33 % |
| 2 mm | 1 | 4 479 | 67.313 | +0.77 % |
| 1.5 mm | 1 | 7 630 | 67.141 | +0.51 % |
| 1 mm | 1 | 17 108 | 67.022 | +0.33 % |
| 2 mm, faces at 0.7 | 1 | 33 073 | 66.891 | +0.14 % |
| 2 mm, faces at 0.5 | 2 | 97 399 | 66.800 | |

One layer is within 2 % of two at every size, and what moves it is the
in-plane size, not the layers (one layer with faces at 0.7 mm is 0.14 % from
two), while two layers cost 22 times the nodes and 450 times the time. So a
buckling study does not refine through the wall: the `through_thickness`
warning is the only consequence.

**Against CalculiX on the same mesh** (`tests/test_fem_calculix.cpp`,
`[buckling][calculix]`, solver `auto` against solver `calculix`; the first
three factors):

| study | nodes | in-house | ccx 2.23 | worst rel. diff. |
|---|---|---|---|---|
| column, clamped, pushed and weighed | 2 458 | 10.2909, 10.3401, 83.8821 | 10.29088, 10.34013, 83.88213 | 4.7e-7 |
| bracket on a pin held from turning | 2 775 | 21.2170, 187.756, 372.208 | 21.21698, 187.7564, 372.2085 | 2.3e-7 |
| shaft under a torque and a remote load | 4 254 | 48.9415, 49.1254, 294.871 | 48.94147, 49.12543, 294.871 | 6.0e-8 |
| steel column bonded to aluminium (assembly) | 2 936 | 6.27630, 6.27748, 79.6168 | 6.276302, 6.277484, 79.61677 | 6.4e-8 |

The same element and rule are used on both sides, so this measures K_G and
the eigensolve, not the physics; the agreement is the seven digits ccx
prints.

**Cost**: the column at 1.28 mm, 216 921 DOF (Netgen's nearest to 240k),
three factors, mesh to modes: 177 s of CPU on a P-core and 602 s pinned to an
E-core, peak 2.12 GB against an estimate of 2.41 GB. The `[slow]` cost check
bounds it at twice the E-core's, 1200 s. A direct factor of a tet10 mesh is
what it costs: a static study that size solves iteratively in a fraction of
it, and the eigensolve cannot.

Tests: `tests/test_fem_buckling.cpp` (the closed forms, the refusals -- load
cases, a run over the memory free -- and the result's round trip),
`tests/test_fem_calculix.cpp` (the cross-check), `tests/buckling_smoke.py`
(a column through the RPCs alone: the study, a `buckling_factor`
requirement unchecked then checked, the factor, the receipt),
`refused_calls_smoke` (load cases on a buckling study, an unknown analysis
type).

## The solver

`src/core/fem/FemSolver.{h,cpp}` and `FemSolverInternal.h` (the element math
shared with topology optimization), namespace `fem`, GUI-free. Gated
`HAVE_FEM_SOLVER` on Eigen and `HAVE_SPECTRA` on Spectra (modal and
buckling).

- **Static** (`fem::Solve`, or `fem::SolveCases` for several load cases on
  one K): tet10 isotropic elasticity, 4-point Gauss rule, triplet assembly
  into a sparse K, fixed nodes eliminated (all three DOF), consistent nodal
  loads from face tractions (equal splitting is visibly wrong on tet10).
  Every answer is held to `‖Ku − F‖ ≤ 1e-6 ‖F‖`, recomputed on the
  assembled K whichever solver produced it. That is how an
  under-constrained model is refused: an LDLT factorization can report
  success on a singular K.
  - Supports beyond fixed nodes (a pin's) hold nodes in their own frames
    ([pin supports](#pin-supports)); each support's reaction is the
    internal force less the load over its nodes.
  - **The linear solver** is chosen by K's size
    (`SolveOptions`, `src/core/fem/FemSolveOptions.h`). Up to
    `kIterativeAboveDofs` (20k) free DOF it is `SimplicialLDLT`. Above it, a
    direct factor of a tet10 mesh fills in steeply, so each case is solved
    iteratively:
    - On the GPU when the build has it and a hardware device exists, under
      topology's rules (`LOBOLE_TOPO_GPU`, the preferred device): the same
      mixed-precision refinement
      ([../topopt/README.md](../topopt/README.md#the-gpu-solver)), from a
      cold start, through `TopoLinearSolver::SetupDomains` (each tet its
      domain's material) and `SolveStrict` (to 1e-7, or not an answer).
    - A GPU that is lost mid-solve (`VK_ERROR_DEVICE_LOST`) fails the
      run, "the GPU was lost during the solve; run it again", rather than
      starting over on the CPU
      ([../topopt/README.md](../topopt/README.md#the-gpu-solver)).
    - Otherwise, or when the GPU declines or falls short, on the CPU:
      preconditioned conjugate gradients with an incomplete-Cholesky
      preconditioner, to 1e-8, written out so it can be cancelled and so a
      stall (no tenfold gain in 2000 iterations, the signature of a
      singular K) is refused as under-constrained rather than run to the
      cap.
  - **Measured** on a 100 x 20 x 20 beam, CPU only: 33.5k DOF, LDLT 4.9 s
    and PCG 1.7 s; 202k DOF, 423 s against 26 s. A printed gear's sun at
    111k tets (512k DOF), meshing included: PCG 120 s, the GPU 31 s. At 47k
    tets, which LDLT took 15 minutes and 9 GB to factor, the GPU took 11 s.
  - `study_status.solver` names the solver a running job chose once its
    mesh said how big K is, and the stored result names the one that ran,
    with `GPU declined: <why>` when a GPU was offered and not used. Stresses are evaluated at each
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
meshed; each needs a support, fixed or pin ("has loads but no fixed
support" is checked before any factorization).

`study_results` for a static study reports `max_von_mises_mpa` and
`max_displacement_mm` (`{value, at, body}`), the per-body breakdown with
each support's `reactions`, and a factor of safety against the material's
yield strength when one is known, flagged when its peak sits on a
support's edge ([a peak on a support's edge](#a-peak-on-a-supports-edge)).
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
at its next boundary (after the meshing, inside an iterative solve; a
factorization has no interruption points) and drops its answer. One study run at a time. A
finished job stores its result into the document it ran on even if another
tab is active.

### Probing a face

**Stress.** `probe_study` also reports the von Mises stress ON the face
(`fem::FaceStressOf`): `von_mises_max_mpa`, the largest nodal value over the
face's boundary triangles, at `von_mises_max_at_mm`, and
`von_mises_mean_mpa`, the area-weighted mean of the field the triangles
interpolate. A study's global maximum often sits in a sharp re-entrant
corner, where it grows as the mesh is refined -- a property of the mesh, not
the part. The stress a designer wants is at a tooth root, a fillet, a bore's
edge, and a face is where to read it. A requirement holds it with
`study_probe {quantity: von_mises_max | von_mises_mean}`
([measures](../requirements/README.md#measures)). Tests: `[probe]` in
`tests/test_fem_loads.cpp` (a cantilever's clamped face reads `Mc/I`; the
free end reads less than the global peak).

**Motion.** The maxima say how far the worst node moved, somewhere on the part. A
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

### Printed layers

A printed part is not isotropic where it breaks. Its stiffness across the
layers is within 10-20 % of along them (Bambu PLA Basic's datasheet: 2.16
against 2.68 GPa), but its strength is not: Prusament PLA's datasheet gives
an interlayer adhesion of 17 ± 3 MPa against a tensile yield of 51 printed
flat. So the study stays isotropic and the allowables carry the direction.

- **The tensor is kept.** A static result keeps each node's whole stress
  (`FemResults::stress`, Voigt order, float like every field; v172), not
  just its von Mises. Everything below is read from it, after the solve.
- **The largest principal stress.** `study_results` reports
  `max_principal_mpa` {value, at, body} and `probe_study` `principal_max_mpa`
  (at, mean): the largest TENSILE principal stress, which a brittle material
  breaks on. Von Mises is symmetric in tension and compression, so it reads
  a beam's compressed side as hard as its stretched one; it stays the yield
  check (`factor_of_safety`).
- **The stress across the layers.** `layer_normal` [x, y, z] on
  `study_results` or `probe_study` is the build direction in the part's
  frame. They add the largest normal stress on the layer planes, `n·σ·n`
  (`max_across_layers_mpa`, `across_layers_max_mpa`), and `study_results`
  `interlayer_factor_of_safety` against the material's `interlayerMPa`
  (pla 17, petg 18 MPa, both Prusament's datasheets; describe's `materials`
  lists every material's numbers). The direction is a question, not a
  stored field: the stress does not depend on how the part was printed, only
  which allowable applies does, so one solve answers every orientation.
  Refused on a non-static study, an assembly study (its parts are each
  printed their own way), a study whose bodies have no interlayer strength,
  and a result saved before v172.

`generators/hook/` is the use it was built for: one solve of a wall hook,
read for a flat print and an upright one, each with a predicted breaking
load to test on a luggage scale. Tests: `[stress]` in `tests/test_fem.cpp`
(known tensors; a cantilever's top fibre reads `Mc/I` in tension along the
beam and nothing across it, its bottom no tension), the round trip in
`tests/test_study_inputs.cpp`, the RPCs and their refusals in
`tests/fem_smoke.py`.

**A printed pin.** `generators/_lib.py`'s `pin_strength(d, part, face,
load_n, direction, seat, layer_normal, region=…)` is the hook's pattern for
a pin: one static study of the pin's part as printed, held at its seat and
loaded on its face (each optionally a [region](#loads-on-part-of-a-face)),
read with `layer_normal`; it returns `interlayer_factor_of_safety` and the
stresses for a receipt, and a factor under 2, across the layers or
against the yield, stops the run. The Strandbeest's crank pins, handle,
joint pins and frame pivots (under its legs' statics), the clock's barrel
arbor and the engine's wrist pin state theirs in their receipts.

## CalculiX

A static study can be solved on CalculiX (`ccx`) instead of in-house:
`set_study {solver: calculix}`, or the panel's **Solver** combo. It is the
same study -- the same Netgen mesh, loads, supports and materials, the same
results and checks -- answered by a second, independent program. The code is
`src/core/fem/Calculix.{h,cpp}`. A [buckling](#buckling) study solves there
too, as its cross-check, and a static study solved past linear -- large
deformation, snap-through -- solves only there
([nonlinear static](#nonlinear-static)), as does a study with a
[contact](#contact) and one past yield ([plasticity](#plasticity)).
Thermal is what is left ([the plan](../plans/calculix-bridge.md)).

### The solver choice

A study's `solver` is `auto`, `in_house` or `calculix` (`fem::Solver`;
absent from the file when `auto`, v204). `auto` and `in_house` solve every
linear analysis in-house; a [nonlinear](#nonlinear-static) study, or one
with a [contact](#contact), on `auto` solves on CalculiX
(`fem::EffectiveSolver`), and on `in_house` is refused. `calculix` solves a **static** or **buckling** study only:
on a modal or topology study it is refused (`fem::SolverRefusal`) by
`set_study`, by the panel, by the file reader and by the run, never quietly
solved in-house. The
serialized study carries the key, so the input hash moves with it only when it
is not `auto`: every result saved before keeps its hash.

### Finding ccx

`fem::FindCalculix` looks, in order:

1. the `calculix_path` setting (Preferences > Simulation, which shows the
   version found);
2. `LOBOLE_CCX`;
3. a `ccx` shipped beside the app (`<prefix>/lib/lobole/ccx`, or `ccx.exe`
   beside `lobole.exe`). The GUI AppImage (`usr/lib/lobole/ccx`, Ubuntu's
   2.21), the Flatpak (`/app/lib/lobole/ccx`, 2.23 built in it) and the
   Windows zip and installer (`bin\ccx.exe`, dhondt.de's 2.23) carry one
   there; the `.deb`, the AUR package and Nix leave it to the distribution
   ([packaging](../packaging/README.md#the-bundled-calculix));
4. `ccx` on PATH.

It runs `ccx -v` once per path, size and modification time, reads "This is
Version 2.NN", and refuses anything older than 2.21, the oldest version the
suites run (Ubuntu 24.04's; the Nix shell has 2.23). A path the setting or
`LOBOLE_CCX` names is the one asked for: when it is not a ccx it is refused by
name, never passed over for the next. `ping`'s `features.calculix` is `{path,
version}` of the ccx a study would run now, or null. With none, `run_study`
is refused before anything starts, in one sentence naming every way to get
one: `sudo apt install calculix-ccx`, Nix's `calculix-ccx`, the dhondt.de
Windows zip, or the setting. Opening a project and reading a saved CalculiX
result never need ccx; a run whose inputs match a stored result is answered
from it, as any run is.

### The deck

The bridge reuses everything up to the solve, so a load means exactly what it
means in-house: `PlanStudy` on the main thread, `MeshPlannedStudy` on the
worker, and the `StudyJobBody` it makes. `fem::WriteDeck` writes, per body
and load case:

| ours | `.inp` |
|---|---|
| nodes (mm), tet10 | `*NODE`; `*ELEMENT, TYPE=C3D10`, one ELSET per `tetDomain` (an assembly's bonded members). The node order is ours; a tet Netgen orients negatively has its corners 1 and 2 swapped, with their midsides |
| material per domain | `*MATERIAL` / `*ELASTIC` (MPa) / `*DENSITY` (t/mm³) / `*SOLID SECTION` |
| force, pressure, torque, remote load, region, gravity | the nodal forces the in-house solve applies (`fem::NodalForces`: the same Gauss rules), as `*CLOAD`; every load kind and `region` maps identically |
| fixed (and a rigid participant's clamp) | `*BOUNDARY n, 1, 3` |
| pin: a node held radially and axially in its own frame | the pin's nodes in a cylindrical `*TRANSFORM` about its axis, the held directions as `*BOUNDARY` in it, and those nodes' loads given in it |
| pin `fix_rotation` (`fem::Tie`) | one `*EQUATION` over the tangential directions, the tie's own coefficients, its largest first |
| output | `*NODE PRINT, GLOBAL=YES` U to the `.dat`, and `*NODE FILE` U to the `.frd` for a person to look at |

A pin pushes and never pulls, so which of its facets it holds is found by the
in-house iteration (`SolveWithPins`) on the same loads, and that contact set is
held in the deck (`fem::StaticSupportsOf`). A node on two pins' faces is
refused: one node has one frame. `export_study_inp {id, path, body?, case?}`
writes the deck a study would run, meshing it there and needing no ccx (one
deck per body and per load case; `NotInBatch`).

### Running it

- **The job** is the study's own (`StartStudyRun`, or the synchronous
  `run_study`): the worker meshes, then for each body and case writes the deck
  and runs `ccx -i job` through `proc::PipedChild` with its working directory
  set (`proc::ChildOptions`).
- **The run directory** is `$XDG_CACHE_HOME/lobole/calculix/<pid>-<n>/`, never
  inside the project. It is removed when the run succeeds or is cancelled,
  and kept and named in the refusal when it fails. A run sweeps the
  directories of processes that are no longer running, on Windows as on
  Linux.
- **Threads.** `OMP_NUM_THREADS` and `CCX_NPROC_*` are the machine's worker
  count. The nixpkgs ccx factors with SPOOLES on one core whatever they say.
- **Cancel.** `cancel_study`, or the starting connection closing, kills the
  process within 20 ms; its directory goes with it. On Windows too: the
  child is a `CreateProcessW` process that is polled, not waited for
  ([packaging](../packaging/README.md#windows)).
- **Deadline.** `run_study {timeout_s}` kills ccx past it and refuses the run,
  naming the limit. It is refused on an in-house study. Without it a run goes
  until it is done or cancelled.
- **Memory.** SPOOLES factorises in core, and an out-of-memory host takes the
  agent with it, so a run whose estimate exceeds `MemAvailable` is refused
  before ccx starts, naming both numbers. `study_status` reports
  `memory_estimate_bytes` once the mesh is made. The estimate
  (`fem::CalculixStaticMemoryEstimate`) is 55 MB + 166 B × DOF^1.3: a
  direct factor of a 3D mesh fills in faster than the DOF count grows. It was
  fitted to ccx 2.23's peak resident memory on a 10 × 10 × 200 steel column
  meshed by our pipeline (the hidden `[calculix-calibrate]` case prints
  them), and bounds every run from above:

  | mesh | DOF | ccx peak | estimate | ccx time |
  |---|---|---|---|---|
  | 5 mm | 5 658 | 57 MB | 68 MB | 0.2 s |
  | 3.5 mm | 16 029 | 62 MB | 104 MB | 0.6 s |
  | 2.5 mm | 30 321 | 116 MB | 166 MB | 1.3 s |
  | 1.8 mm | 65 577 | 272 MB | 358 MB | 2.6 s |
  | 1.4 mm | 196 821 | 1.18 GB | 1.32 GB | 15.7 s |
  | 1.2 mm | 240 717 | 1.48 GB | 1.70 GB | 20.0 s |
- **Errors.** A ccx that writes an `*ERROR` line, or exits non-zero, is
  refused with its first `*ERROR` line verbatim (else its last output) and the
  run directory.

### Reading the answer back

Only U is read, from the `.dat`: ccx prints it with seven significant digits
(`E13.6`; the `.frd` has six). `SolveCases` takes it as a given answer
(`SolveOptions::given`) and holds it to the in-house rule against our own
assembled K -- same element, same Gauss rule, the supports in the same frames:
`‖Ku − f‖ ≤ 1e-6 ‖f‖`, plus the floor the printed precision puts under an
exact answer, `‖|K| |u|‖ · 5e-7 / ‖f‖`. That floor is wide: seven digits
are rounding noise of high frequency, which K amplifies, and an exact answer
printed so leaves `‖Ku − f‖ / ‖f‖` of 1e-4 to 1 (measured below), so the norm
alone could not tell a wrong deck from rounding. The check that can is the
**energy balance**, `|u · (Ku − f)| ≤ 1e-6 |u · f|` plus `4 · 5e-7 · Σ|u||f|`
over `|u · f|`: rounding moves it by `δ · f`, first order in the precision,
and a deck that differs from the study moves it first order in the
difference. A deck whose material is 0.1 % stiffer than the study's is
refused by it (`[calculix]`). A direction the study holds that the answer
moves is refused too. Then stresses, reactions, the support-edge flag,
the largest principal stress, printed layers and probes are computed by the
code an in-house answer goes through. A deck that differs from the study (a
load lost, a wrong frame, a material swapped) fails this check rather than
reading as an answer.

### Results and receipts

- `study_results` names `solver` ("calculix 2.23"; the result saves it, so a
  reopened one still says so), and `calculix` {`residual`, `residual_floor`,
  `energy_residual`} for a run this session made.
- A CalculiX answer is a **second, independent answer, not a stronger one.**
  Its factor of safety's receipt keeps basis `predicted`, with oracle
  `calculix/static` and the version. When the session holds an in-house result
  of the same study (its input hash with `solver: auto`), `study_results` adds
  `in_house_agreement` {`max_displacement_rel`, `max_von_mises_rel`}: reported
  beside the answer, never folded into the claim.

**Measured** (`[calculix]`, the nixpkgs ccx 2.23, each study meshed once and
solved both ways):

| study | nodes | peak displacement, rel. diff. | peak von Mises, rel. diff. | ‖Ku − f‖/‖f‖ | energy |
|---|---|---|---|---|---|
| column, clamped, pushed sideways and weighed | 10 204 | 2.6e-7 | 2.0e-7 | 1.2 | 1.1e-6 |
| lug on a pin held from turning (a tie) | 8 316 | 6.8e-11 | 2.9e-7 | 1.9e-3 | 8.0e-9 |
| lever on two free pins | 8 316 | 2.3e-7 | 1.8e-6 | 4.0e-3 | 2.9e-8 |
| shaft under a torque and a remote load | 5 835 | 1.4e-8 | 2.3e-7 | 1.3e-3 | 2.9e-9 |
| a pull on part of a face (`region`) | 5 947 | 2.8e-7 | 2.2e-8 | 2.6e-3 | 9.6e-9 |
| steel bonded to aluminium (assembly) | 2 353 | 1.1e-7 | 4.1e-8 | 8.1e-5 | 5.5e-9 |
| a hand-made mesh, no Netgen | 525 | 5.7e-8 | 2.1e-7 | 4.0e-3 | 1.1e-8 |

The agreement is the precision ccx prints at; the column's norm residual
exceeds `‖f‖` because its load (100 N sideways and its weight) is small
against the internal forces the rounding disturbs.

Tests: `tests/test_fem_calculix.cpp` (`[calculix]`): the same-mesh agreement
above; a hand-made mesh with no Netgen (which runs on every CI image);
refusals with a stand-in ccx (a shell script) -- none found, its `*ERROR`
line and kept directory, `timeout_s`, cancel leaving no process and no
directory, the memory estimate, an answer that does not solve our K; the
`.dat` reader; the result's `solver` round trip. `tests/calculix_static_smoke.py`
drives it over the RPCs alone, gated on `ping`'s `features.calculix`, and runs
ccx on the exported deck; `refused_calls_smoke` holds the refusals to
changing nothing.

## Nonlinear static

A part that bends a long way, or a curved wall loaded across its curve, is
not answered by a linear study: the first stiffens or softens as it moves,
and the second snaps through below its linear buckling factor, which linear
buckling cannot see. A static study takes `nonlinear` (`fem::Nonlinear`,
`src/core/fem/FemStudy.h`) and is then solved on [CalculiX](#calculix)
(`fem::NonlinearOnCalculix`, `src/core/fem/Calculix.cpp`); `auto` picks it
and `in_house` is refused, since the in-house solver is linear.

`set_study {nonlinear: {large_deformation?, plasticity?, increments?,
snap_through?, imperfection_mm?, deflection_limit_mm?}}` (null for linear;
[plasticity](#plasticity) has its own section); the panel has **Large
deformation**, **Plasticity** and **Snap-through (peak load)** checkboxes
under **Converge peak**. Refused, by `set_study`, the panel, the file reader
and the run (`fem::NonlinearRefusal`): without `large_deformation: true` or
`plasticity: true` (or a contact), on any analysis but static, with `solver: in_house`, with load cases (a nonlinear
answer is not a sum of cases, and a snap-through run scales one load set),
an unknown key, `increments` under 1, a seed or limit without
`snap_through`, a negative seed, a limit that is not positive, and
`converge: always` on a snap-through run.

### Large deformation

`*STEP, NLGEOM` and `*STATIC`, the load stepped up from 0 to the study's,
the step's largest increment its load over `increments` (default 10;
`fem::kNonlinearIncrements`). Each increment is solved on the part as it has
deformed. The loads (`WriteDeck` with `DeckNonlinear`):

- a **force** and **gravity** keep their direction (dead loads): the nodal
  forces the in-house solve applies, as `*CLOAD`, scaled by the step;
- a **pressure** follows its face as it turns: `*DLOAD P<n>` on each
  loaded facet's tet face (a boundary tri6 maps to the face S1-S4 of the
  tet that has its corners, in the order the deck writes them);
- a **torque** or **remote** load is a reference node at its axis origin or
  point carrying the force and moment its in-house traction sums to (so
  the two are statically the same), spread over its face by
  `*COUPLING`/`*DISTRIBUTING`;
- a **pin** is held where the in-house solve of the same load presses on
  it, as a [buckling](#buckling) study holds it, and the notes say so.

The answer's stress is the Cauchy stress of the deformed part
(`fem::LargeDeformationFields`): the Green-Lagrange strain from U through
the St Venant-Kirchhoff law ccx's linear elastic material is under NLGEOM,
pushed forward, averaged to the nodes as the linear recovery is. The factor
of safety is yield over its peak, with the receipt saying it was a
large-deformation solve.

A run that stops converging before its load (ccx's "increment size smaller
than minimum", or "solution seems to diverge") is **refused** with the
fraction of the load it reached, ccx's words, and the hint that the part
may lose its stiffness there (a snap-through run reports that peak) or the
load steps too far; nothing is stored. A converged increment that **jumps**
to another branch -- far more compliant than the increment before and the
one after, kSnapJumpRatio (4) times each -- is a snap on the way: the run
goes on to its load, and the result carries `snapped` {from_factor,
to_factor} and a warning that the fields are of the branch it landed on.

### Snap-through

`snap_through: true` asks for the load factor the part stops carrying more
at. The run:

1. solves the study's load linearly and its first [buckling](#buckling)
   mode in-house (the buckling memory estimate refuses one that will not
   fit), and refuses a load that buckles nothing, which has no mode to seed;
2. seeds the shape with that mode scaled to `imperfection_mm`, by default
   a tenth of the wall the mode bends (`wall_mm`, measured as the knockdown
   measures it; a thousandth of the part's size where none is measured). A
   stated 0 is the perfect shape;
3. steps every load up together, under load control, to a cap of twice the
   linear factor, stopping at the first of:
   - **limit_point**: the solve stops converging where the part's
     stiffness has run out (the last increment's compliance at least four
     times the first's -- otherwise it is refused as non-convergence). The
     peak lies between the last converged factor and that plus the
     smallest increment ccx tried (1e-5 of the cap at the least);
   - **snap**: a converged increment jumped to another branch, as above;
   - **deflection_limit**: the motion passed `deflection_limit_mm`, by
     default a tenth of the part's bounding-box diagonal. An elastic column
     does not peak -- its load rises slowly past the Euler load on the
     elastica -- so for a stable path this is the answer: the factor at
     which the part has moved that far, interpolated.
4. A snap or a deflection limit is bracketed again: the run repeats with
   that increment cut in ten and goes on past it to the cap, so a candidate
   the finer increments do not bear out lets the run continue (at most four
   ccx runs). The cap reached with none of these is `peak_at: none` with the
   reason.

ccx 2.23 has **no arc-length method**: `*STATIC, RIKS` is read as a plain
`*STATIC` with "parameter not recognized", so the path past a peak is not
followed. Load control past a limit point either stops converging (the
limit point) or converges on the far branch in one increment (the snap,
which is why the jump test exists: a shallow arch at eps 3 jumped its crown
from 1.7 to 6.5 mm in one increment, past its limit, and converged).

The result's `nonlinear` per body: `linear_factor`, `imperfection_mm`,
`deflection_limit_mm`, `max_factor`, `peak_at`, `peak_factor`,
`peak_bracket`, and `first_yield_factor` (the factor the peak von Mises
first reaches yield at on the path, each increment's Cauchy stress against
the weakest material at the node). The study's `factor_of_safety` is the
governing factor, the peak or the first yield when that comes first, and
its receipt claims "peak load factor >= 1", oracle `calculix/nonlinear`,
basis `predicted`. Its fields are of the increment at the peak.

`converge` (a peak that has settled): a large-deformation study refines
round its stress peak as a static one does, a ccx run a step. A
snap-through run's answer is a load factor, which refining round a stress
peak does not settle: `auto` reports it not checked with that reason, and
`always` is refused.

### The run

- **Progress.** The watcher reads the `.sta` every 200 ms; the load reached
  over the load the run goes to is `study_status`'s `load_fraction` and
  the job's bar.
- **Increments.** Each increment's U and the supports' RF are read from
  the `.dat` as ccx writes them (it flushes per increment), a whole block
  at a time; a run is stopped (killed) as soon as an event says it has
  what it needs.
- **Equilibrium.** ccx's residual criterion is tightened from its 0.5 % of
  the mean force to `kNonlinearResidual` (1e-6: on a shallow arch to its
  snap that took 98 Newton iterations where the default took 103). At the
  factor reported the supports' total reaction is checked against the load
  applied there -- the dead loads scaled, a pressure integrated on the
  deformed faces, each reference node's force: `equilibrium` {residual_criterion,
  imbalance, tolerance}, the imbalance over the load carried. Past
  `kNonlinearBalance` (1e-3) the answer is refused as a deck that differs
  from its study. Measured imbalances are 1e-9 to 1e-8.
- **Memory.** Refused as a static run is, before ccx starts, naming both
  numbers (`fem::CalculixNonlinearMemoryEstimate`): a tenth over the static
  estimate. Measured on the 10 x 10 x 200 column's large-deformation deck
  (a 2 kN sideways pull, ten increments; the hidden
  `[calculix-nl-calibrate]` case), the nonlinear peak is 1.02-1.10 times
  the linear one, which the static estimate bounds by as little as 7 %:

  | mesh | DOF | ccx peak | estimate | ccx time |
  |---|---|---|---|---|
  | 5 mm | 5 658 | 58 MB | 74 MB | 4.1 s |
  | 3.5 mm | 16 029 | 68 MB | 114 MB | 16.8 s |
  | 2.5 mm | 30 321 | 124 MB | 183 MB | 40 s |
  | 1.8 mm | 65 577 | 290 MB | 394 MB | 109 s |
  | 1.4 mm | 196 821 | 1.24 GB | 1.45 GB | 667 s |
  | 1.2 mm | 240 717 | 1.55 GB | 1.87 GB | 921 s |

  Time is what grows: ten increments of a few Newton iterations each, a
  SPOOLES factor every iteration on one core, cost 20 to 46 times the
  linear run's.
- `export_study_inp` writes a large-deformation study's deck; a
  snap-through one's is refused, since its seed and cap are found when it
  runs.

A tet whose curved midside nodes turn its Jacobian negative (Netgen's
sliver with all four corners on a cylinder's surface, seen on a 12 mm shaft
at 3 mm) is refused by ccx, linear or not ("nonpositive jacobian"), where
the in-house solver integrates |det J| and does not notice; another mesh
size meshes it without one.

### Measured

`tests/test_fem_calculix.cpp` (`[nonlinear]`, ccx 2.23):

| case | answer | closed form |
|---|---|---|
| the elastica: a 5 x 5 x 100 steel cantilever, a dead tip load at PL²/EI = 1, 2.5 mm, 1 911 nodes, 10 increments, 5 s | tip 0.30089 L along the load, 0.05619 L back | Bisshopp-Drucker 0.30172 (-0.27 %), 0.05643 (-0.44 %); linear 1/3 |
| a shallow circular arch between pins, 200 x 4 x 4 mm, eps 4 (rise 4.475), a uniform pressure on its crown, 4 mm, 7 484 nodes, 146 s (`[slow]`) | limit point at 0.47088 [0.47088, 0.47094] | shallow-arch theory's 0.47115 (-0.06 %); linear buckling 1.0316 |
| a 10 x 10 x 200 column, 1 kN, seeded 1 mm, 5 mm, 1 911 nodes, 15 s (`[slow]`) | deflection limit (20 mm) at 9.836, 4.48 % below | linear 10.297; Southwell's 1 - a/delta: 5 % |
| the same, seeded 0.1 mm, 20 s | 10.275, 0.21 % below | 0.5 % |
| a shaft under a torque, a remote load and a pressure, a thousandth of a working load, 3 854 nodes | 9.5e-5 of the peak motion from the linear in-house answer; von Mises 5e-6 | the linear answer |

The arch's closed form is Timoshenko and Gere's (sec. 7.6) for a sinusoidal
pin-ended arch, its first harmonic: a circle of rise e is the sine of rise
32e/pi³, a uniform load q its harmonic 4q/pi, and with r = h/sqrt(12) and
eps = e1/r the limit is at xi² = (eps² - 4)/3, symmetric below
eps = sqrt(22). The arch's linear stiffness is 4.5 % under that theory's
(the pins' bearing compliance, measured at 8 mm wide), so a closer match
than that is partly luck; the test holds it to 5 %. The arch snaps at 46 %
of its linear buckling factor.

Tests: those, plus the refusals (each `NonlinearRefusal` case; a run with a
stand-in ccx that converges 40 % of the load and stops, refused with that
fraction and nothing stored), the result's round trip, and the memory
calibration (hidden, `[calculix-nl-calibrate]`); `tests/nonlinear_smoke.py`
over the RPCs (a column's large-deformation deck and run with its progress,
then its snap-through peak below the linear factor and its receipt);
`refused_calls_smoke` (in-house, an unknown key, no large_deformation,
modal, load cases, converge always on a snap, a run that stops converging).

## Contact

A part that rests on something -- a block on a floor, a ball on a plate --
is held only where it presses: rock it and one edge comes away. A `fixed`
support glues the face and holds it in tension; a contact holds it the way
the floor does, pushing and never pulling, with friction or without. A
contact is nonlinear by nature (which part of the face carries the load
depends on the load), so a study with one solves on [CalculiX](#calculix)
only, through the [nonlinear](#nonlinear-static) path: `auto` picks it,
`in_house` is refused. The code is `src/core/fem/Calculix.cpp` (the deck
and the run), `FemStudy.cpp` (the part study's ground, the mesh) and
`AssemblyStudy.cpp` (the pairs on an assembly).

### Two kinds

- **A contact support** (`LoadType::Contact`; `add_load {type: contact,
  friction?}`, the panel's **Contact** tool, an assembly row's **Rest on
  it**). It is a support: it holds the part, its row in `reactions` is what
  it carried.
  - On a part study, or on a face of the meshed occurrence of an assembly
    study, the face rests on **the ground**: the plane a flat face lies on.
    A curved face is refused, naming the assembly route. The ground is a
    box on the face's outer side (`fem::GroundUnder`), reaching past the
    face by a quarter of its larger side on every side and as deep, meshed
    at that size, every node fixed: it is rigid, so its mesh has only to
    hold the plane, which any mesh of a flat face does.
  - On an assembly study with `contact_with` a rigid participant, the
    meshed occurrence rests **on that occurrence**, where their faces face
    each other. Its bodies are meshed twice as coarse as the meshed body
    (`kRigidCoarsen`), their contact faces at the body's own size so a
    curved partner keeps the shape of the face it meets, every node fixed.
- **A contact participant** (role `contact`, with `friction`): meshed apart
  from the meshed occurrence, in its own material, and in contact with it
  where their faces face each other -- two parts that touch, press, slide
  and lift off. Loads, supports and refinements go on its faces as on the
  meshed one's (`occurrence` names it). Its mesh joins the body's as
  domains of its own, sharing no node, so the result reports one body, as
  a bonded group does. This lifts the one-meshed-occurrence rule, for
  CalculiX studies only. A contact participant is in contact with one body
  of the meshed occurrence and nothing else (not another participant, not
  a rigid one); a contact beside bonded participants is refused, because
  the bonded group is meshed as one fused shape whose contacts are not
  looked for.

`friction` is the coefficient (unitless, 0 or more; 0 and the default is
frictionless). Refused, by `add_load`, `set_study`, the panel, the file
reader and the run (`fem::ContactRefusal`): in-house, on any analysis but
static, with load cases (where a contact closes depends on the whole load,
so its answer is not a sum of cases), with `snap_through` (the seed is an
in-house buckling mode, and the in-house solver cannot hold a contact),
with `converge: always` (below), beside a pin (below), on a study of a
topology shape, with a `region`, and with a friction below 0.

### Which faces

Two faces are a pair when they come within 5 % of the smaller body's
bounding-box diagonal of each other (`kContactClearanceFraction`) and face
each other there: their outward normals more than 60 degrees opposed at
the nearest points (`kContactFacing`). The whole of each face is paired,
so a contact that closes beyond where the parts first touch is held. The
assembly's fuse is not what finds them: it imprints the faces two parts
SHARE, and a ball on a plate touches at a point, a roller along a line --
they share none. The facing test keeps a block standing on a plate from
pairing its sides, which meet the plate along an edge without facing it.

### The deck

The nonlinear deck ([the run](#the-run)) with, per pair:

- `*CONTACT PAIR, TYPE=SURFACE TO SURFACE`: face-to-face penalty contact,
  the body's facets the slave surface, the partner's the master; a rigid
  partner's nodes and tets written after the mesh's, `*BOUNDARY` on all of
  them.
- `*SURFACE BEHAVIOR, PRESSURE-OVERCLOSURE=LINEAR` with the slope (the
  penalty, pressure per overclosure) `K = 5 E / h` (`fem::ContactPenalty`,
  `kContactPenalty`): `E` the softer side's modulus, `h` the mean edge of
  the body's contact facets. An overclosure is then a fifth of what the
  same pressure strains one element by, a compliance at the contact that
  shrinks with the mesh. ccx's own default is for node-to-face contact
  only -- 50 times the first material's modulus whatever the element,
  which reads as per millimetre and so is bound to the units -- and
  face-to-face contact has none; its manual's range is 5 to 50 times the
  modulus. Measured on the Hertz case below (the ball on a rigid flat, its
  cap at a/6), the soft end is the closest:

  | penalty | contact radius | peak pressure | ccx run |
  |---|---|---|---|
  | 5 E / h | +0.94 % | +1.70 % | 153 s |
  | 15 E / h | -1.64 % | +3.36 % | 169 s |
  | 50 E / h | -3.26 % | +4.04 % | 199 s |

  The stiffer the penalty, the more the pressure gathers on the
  integration points that close first. The kern case below reads the
  same at every one of them (-0.04 %).
- `*FRICTION` with `friction` and the stick slope `K / 100`
  (`kStickSlopeRatio`), ccx's manual's advice ("about 100 times smaller
  than the spring constant"; its default, the first modulus over 2, is
  again unit-bound). A frictionless pair writes none.
- Without `large_deformation` the steps are plain `*STATIC` (no NLGEOM)
  and every load is the nodal forces the in-house solve applies; with it,
  the large-deformation deck. The default `increments` is 10.
- `*CONTACT PRINT` each increment: each pair's statistics (CF: the force
  and moment on the body's side, the area in contact, the normal and shear
  force) and the pressure at every contact integration point (CSTR).
- A run with a contact does not look for snaps: a contact closing or
  opening changes the stiffness by design.

### A part held by a contact alone

A part whose only support is a contact -- a ball on a plate, a block on
the floor -- has nothing holding it until its contact closes. A ball
touching at a point closes nothing in the first iteration and falls
through its partner under its first increment of load (measured: 2.2 m on
the Hertz case below, its contact never found). Every node of such a part
(a part of the mesh with no fixed node and no pin) takes springs to the
ground in each direction:

- **settling springs** (`fem::SettleSprings`) through the first step, the
  first tenth of the load (or the first increment, when that is less): all
  together they would move the part a tenth of the mean edge of the body's
  contact facets under the whole load of that step (`kSettleMotion`), so
  its contact closes and takes the load as it does. They are removed
  (`*MODEL CHANGE, TYPE=ELEMENT, REMOVE`) for the rest of the load.
- **weak springs** all through: `1e-6 E L` summed over the part's nodes, `L`
  its bounding-box diagonal (`kStabilization`), holding what a frictionless
  contact leaves free with nothing pushing it there: a block on a
  frictionless floor under a load straight down is free to slide, and
  nothing makes it.

What the weak springs carry at the factor reported, over the load, is the
result's `stabilization`, and it is part of the equilibrium check. Past
`kNonlinearBalance` (1e-3) the answer is refused: the springs are holding
a part nothing else holds -- a frictionless contact under a sideways load
-- and the refusal names friction or another support as the way out.
Measured: 3e-6 to 6e-6 of the load on every case below.

### What a contact reports

Per body, `contacts` (`StudyBodyResult::ContactResult`, saved with the
result): for each pair, its support's load id or its participant's
occurrence, the partner's name ("ground" or the occurrence), the friction
and penalty, the area of its faces on the body's side and how much of it
is closed (ccx's area in contact; the rest is open), the force and moment
(about the frame's origin) the partner exerts on that side, its normal and
shear parts, and the pressure: the largest at any contact integration
point, and the mean over the closed area. A contact support's `reactions`
row carries the same force and moment and its closed area of its face's.
The equilibrium check counts each rigid partner's reaction, so a contact
that lost load fails it. The fields are the small-deformation stress
(`fem::SmallDeformationFields`) without `large_deformation`, the Cauchy
stress with it. The panel lists each contact's closed area, force and
peak pressure.

### Converging on a contact

`converge` ([a peak that has settled](#a-peak-that-has-settled)) refines
the faces a stress peak lies on, one ccx run a step, and a contact's peak lies on a contact face, whose partner is meshed apart
from it: refining one side of a contact and not the other does not settle
it. `auto` reports it not checked with that reason, and `always` is
refused.

### Contact and pins

A [pin](#pin-supports) is the in-house solver's one compression-only
contact: a rigid pin on a bore's axis, frictionless, never meshed, its
contact set found by iterating linear solves (`SolveWithPins`). A CalculiX
contact is the general case -- a flat face on the ground, any face on a
rigid occurrence or a meshed one, with friction -- solved by ccx's penalty
method. They do not mix: a pin's contact set is found in-house, which
cannot hold a contact as well, so a study with both is refused. A pin
stays the support for a part on a pin in a linear in-house study, and the
base state a buckling study holds.

### Measured

`tests/test_fem_calculix.cpp` (`[contact]`, ccx 2.23):

| case | answer | closed form |
|---|---|---|
| a 20 x 20 x 40 steel block on a frictionless ground, 4 kN down e = b/12 off its axis (a remote load), 4 mm, 2 468 nodes, 2 increments, 2.2 s | the whole base closed (399.99 of 400 mm²), peak pressure 14.994 MPa, the ground's resultant 1.66590 mm off the axis | inside the kern: closed, P/A (1 + 6e/b) = 15.000 MPa (-0.04 %), e = 1.66667 (-0.05 %) |
| the same, e = b/4, 3.4 s | 277.7 mm² closed, peak 26.4 MPa, resultant at 4.99752 mm | past the kern (b/6) it lifts off; e = 5 (-0.05 %). A rigid block on a Winkler bed would close 3 (b/2 - e) b = 300 mm² |
| a steel ball, R 10, on a rigid flat (a rigid occurrence), 29.3 kN for a = R/10, its cap refined to a/6, 11 371 nodes, 10 increments, 153 s (`[slow]`) | contact radius 1.0094 mm, peak pressure 14 230 MPa | Hertz: a = 1 mm (+0.94 %), p0 = 13 992 MPa (+1.7 %) |
| a 20 x 20 x 30 steel block, a contact participant (friction 0.2), standing on a clamped aluminium slab, 3 kN down 3 mm off its axis, 5 mm, 2 197 nodes, 3.4 s | 2 999.99 N at 2.99991 mm; 399.4 mm² of the block's 400 closed | statics: P at e (-0.003 %) |

The weak springs carried 3e-6 to 6e-6 of the load in each. Two meshed parts
converge with the mesh as any contact does, and refining a face refines
all of it: a steel ball on a steel block, the ball's cap and the block's
whole top refined to a/2.5, read the radius 15 % short (18 278 nodes, 453
s, at a penalty of 50 E / h), where the rigid flat needs no refining at
all.

The kern case is exact for a frictionless floor: a prism under a linear
end traction keeps its sections plane, its base presses everywhere with
the linear pressure while e <= b/6, and the penalty, uniform over the
face, keeps it linear. Past the kern the base lifts, and statics alone
puts the floor's resultant on the load's line.

## Plasticity

A part loaded past its yield does not fail there: the yield spreads, the
part carries more by it, and it stays bent once the load is off. A linear
study reports a factor of safety under 1 and stops. `nonlinear.plasticity:
true` (`fem::Nonlinear::plasticity`) solves the study past yield, on
[CalculiX](#calculix), through the [nonlinear](#nonlinear-static) path:
`auto` picks it and `in_house` is refused. On its own it is solved at small
deformation (plain `*STATIC`); with `large_deformation` under NLGEOM, and
with a [contact](#contact) beside it. The panel has a **Plasticity**
checkbox under **Large deformation**.

### Hardening curves

Each material runs along its own hardening curve
(`CADMaterials::MaterialProperties::hardening`, `fem::Material::hardening`):
the von Mises stress against the equivalent plastic strain, from (yield, 0),
written as `*PLASTIC` (isotropic hardening, ccx's default). A curve is in
the library only where a source is cited beside it:

| material | curve | source |
|---|---|---|
| `steel` (structural, f_y 250 MPa, E 200 GPa) | linear hardening, the stress-strain slope E/100 past f_y (in plastic strain H = E/99 = 2020 MPa), to 5 % plastic strain: (250, 0), (351.0, 0.05) | EN 1993-1-5:2006 Annex C.6(2), Figure C.2 c), "elastic-plastic with linear strain hardening"; the 5 % is C.8(1) Note 1's recommended principal strain limit |

Every other material has none, and a plastic study on it is **refused** --
by the run, before anything is meshed (`fem::HardeningRefusal`), naming the
material and the ones that have a curve -- never run perfectly plastic,
which carries no load past yield that the part does carry, nor on a guessed
slope. Stainless steel (EN 1993-1-4's Ramberg-Osgood model) and the
aluminium alloys (EN 1999-1-1 Annex E) have published curves that are not
cited here yet. The printed plastics (PLA, PETG, TPU) get none: their
datasheets give a yield and a break and nothing between, so a plastic study
of a printed part is refused with "has no hardening curve".

Past the end of its curve CalculiX holds the stress at the last point. A
result whose plastic strain passes it (`strain_limit`, the smallest last
strain of the body's curves) is warned: there it is the data's
extrapolation, not the material's -- and for steel it is past the 5 %
limit EN 1993-1-5 C.8 puts on a plated structure anyway.

The study's input hash takes each body's curve when, and only when, the
study is plastic, so every other study kept its hash when steel gained one.

### Reading the answer

With plastic strain in it, the stress cannot be recomputed from U, as the
elastic answers' is ([reading the answer back](#reading-the-answer-back),
the [large-deformation](#large-deformation) Cauchy stress): it depends on
the path. The deck prints CalculiX's own at every Gauss point of the body,
each increment (`*EL PRINT, ELSET=EBODY` S, PEEQ: the Cauchy stress, and the
equivalent plastic strain), and `fem::IntegrationPointFields` carries it to
the nodes: each tet's four points (ccx's C3D10 rule is our `kTetGauss`,
point k nearest written corner k, mapped back through a swapped tet's
corners; ccx prints xz before yz) fitted with the linear field through
them, read at its corners and midsides, and averaged by element volume as
the linear recovery is. A tet10's elastic stress is linear, so below yield
this is the elastic stress of the same U, which `[plasticity]` holds to
(3.8e-4 of the peak, the rounding of the U read back at seven digits). The
plastic strain is read the same way, never below 0.

### Unloading

A run that reaches its load takes it off again in one more step
(`*CLOAD, OP=NEW` and `*DLOAD, OP=NEW`, ramped down over as long as it took
to put on; supports and contacts stay). The displacement at its end is what
the yield left: `residual_displacement`, saved with the result. A run whose
unloading stops converging is refused (the deformation it leaves is not
known); a snap-through run is not unloaded.

### Collapse

ccx 2.23 has no arc-length method, so the path past a peak is not followed
([snap-through](#snap-through)). Under load control a part that runs out of
stiffness stops converging; when its last increment is at least four times
as compliant as its first, as a [snap-through](#snap-through) run's limit
point is, a plastic run reports it as its answer rather than refusing it:
the part **collapsed** at that factor, between it and the smallest
increment ccx tried beyond (`collapse_bracket`). The fields are of the last
converged increment, where a mechanism is forming and the plastic strain is
unbounded. Nothing is unloaded. The study's `factor_of_safety` is then the
collapse factor, its receipt claims "collapse load factor >= 1" (oracle
`calculix/nonlinear`, basis predicted). To find a collapse load above the
working load, load the study past it: the run answers the fraction.

A plastic run that carries its load keeps yield over the peak von Mises as
its factor of safety (under 1 once it yields, and how far under says how
far past yield the peak is); `max_plastic_strain` says how much it yielded,
and is a requirement quantity of `study_result`
([requirements](../requirements/README.md)).

`converge`: a plastic run's stress is held down by the yield where it
concentrates, and the plastic strain that concentrates there is not
refined round: `auto` reports it not checked with that reason, and
`always` is refused.

Refused, by `set_study`, the panel, the file reader and the run, where
`nonlinear` is (`fem::NonlinearRefusal`): `in_house`, any analysis but
static, load cases (a plastic answer depends on its path, not a sum of
cases), and with `converge: always`; and by the run, a material without a
hardening curve.

### What a plastic result reports

Per body, `max_plastic_strain`, `max_residual_displacement_mm` (absent on a
collapse), and in `nonlinear` `plasticity` {`unloaded`, `strain_limit`,
`collapsed`, `collapse_bracket`?}; `first_yield_factor` is where the path
first yielded (from ccx's own stress), and is not warned of as an elastic
run's is. The top level has `max_plastic_strain` and
`max_residual_displacement_mm` {value, at, body}, and `nonlinear` {kind:
plasticity, collapse_factor?, collapse_bracket?, body?}. The panel shows
the peak plastic strain, what is left once unloaded, and a collapse.

### Measured

`tests/test_fem_calculix.cpp` (`[plasticity]`, ccx 2.23):

| case | answer | closed form |
|---|---|---|
| a 10 x 10 x 40 steel bar on a frictionless ground, pressed to 300 MPa, 5 mm, 610 nodes, 6 increments, 3.4 s | plastic strain 0.024724 to 0.024748 at every node, von Mises 299.947 to 299.996; unloaded, 0.98946 mm shorter and 0.123683 mm wider | (300 - 250) / H = 0.02475 (-1.1e-3 at the least); L eps_p = 0.99 (-5.5e-4), b eps_p / 2 = 0.12375 (-5.4e-4) |
| a 6 x 8 x 60 steel cantilever under its yield, 3 mm, 1 313 nodes, 0.7 s | ccx's stress, carried to the nodes, within 3.8e-4 of the peak of the elastic stress of the same U; peak von Mises the same to 1e-5; nothing left once unloaded | the elastic answer |
| a 10 x 10 x 60 bar, clamped, bent by 1.25 M_p through a head twice its section, the curve stated flat, 3.4 mm, 2 516 nodes, 17.5 s (`[slow]`) | collapses at 0.80803 [0.80803, 0.80804] of it: M = 63 128 N mm, the hinge mid-bar | M_p = f_y b h^2 / 4 = 62 500 N mm (+1.0 %; +0.42 % at 2.5 mm, 6 670 nodes, 70 s) |

The bar is exact but for the weak springs that hold a part on a contact
alone ([a part held by a contact alone](#a-part-held-by-a-contact-alone)):
plastic flow shortens it by a millimetre, and the springs carried 1.5e-4 of
the load there, which near the base is 0.05 MPa of stress and so 2.6e-5 of
plastic strain on a hardening modulus of 2020 MPa.

The collapse case's closed form is for perfect plasticity, so the test
states the curve flat on its plan; the library's steel hardens, and at
small deformation has no collapse. How the moment goes in decides what is
measured:

- a tip force on a clamped cantilever collapses 13.4 % high at L = 10 h
  (2.5 mm; 16.7 % at 5 mm) and 6.4 % at 30 h: its moment peaks at the
  clamp, whose face holds the section from spreading across its width --
  towards plane strain, whose yield is 2/sqrt(3) = 1.155 times -- so the
  hinge forms where the moment is lower;
- a moment put in as the linear traction of pure bending on the bar's own
  end face collapses at that face, 17 % low: the dead traction there
  passes f_y from 2/3 M_p on, which no stress field of a perfectly plastic
  material can carry;
- the same moment through a head twice the bar's width and depth leaves
  the bar's middle the weakest place, uniform in moment, and collapses at
  M_p.

## Assembly studies

A study on an `AssemblyDocument` names **occurrences**, each in one role
(`fem::Participant`, top-level occurrences; a sub-assembly takes part whole):

| role | cost | meaning |
|---|---|---|
| `meshed` | mesh + solve | the part being analysed; exactly one per study (a `contact` participant is solved beside it) |
| `bonded` | mesh + solve | meshed too, and glued to the meshed part where they touch, with its own material |
| `rigid` | one boolean | a support; its contact patch can be clamped |
| `contact` | mesh + solve | meshed apart from the meshed part, in its own material, and in contact with it where their faces face each other; solved on CalculiX ([contact](#contact)) |
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
anything by itself; the clamp is a load. A contact support may anchor on a
rigid participant the same way (`add_load {type: contact, contact_with}`):
the meshed part rests on it, pushed and never pulled
([contact](#contact)).

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
  bonded, contact or rigid pair that interpenetrates (checked with
  `interference`; `ignored` is the way out); a bonded member sharing no face
  with the group; a contact anchor whose support does not touch; a contact
  participant that faces no face of the meshed part, or faces two of its
  bodies; a contact beside bonded participants.
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
Fields: von Mises, displacement magnitude, a modal or buckling mode shape,
and the topology views. The legend names the study and analysis, the
quantity with quarter ticks, the deformation factor, the mode and frequency
of a modal result (its load factor for a buckling one), and where the
maximum is, marked in the viewport by a cross.

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
  participants table with a role per row, **Clamp here** and **Rest on
  it** (a [contact](#contact) support on it) on rigid rows, and a
  **Friction** field on contact rows.
- A static study's window has a **Solver** combo (Auto, In-house,
  CalculiX with the version found, or "not found"): CalculiX stays choosable
  without ccx, and Run then refuses in the window's red text with how to
  install it ([CalculiX](#calculix)).
- In the window: analysis type, mesh size, material check, gravity (a
  checkbox that sets world-down 9.81 m/s²), and **Boundary conditions**: arm
  Fixed, Force, Pressure, Torque, Remote, (on a static part study) Pin or
  (on a static study) Contact and click a face; magnitude and an
  optional custom direction apply to the next placement. **Torque** takes
  "Torque (N·mm)" and an **Axis**: "From the face (cylinder)", or the choices
  a Revolve's axis offers (X/Y/Z, datum axes, construction lines, custom).
  With no axis chosen a click on a cylinder takes its axis, and a click on
  any other face is refused in the panel's red line and places nothing. An
  untyped torque seeds from the part's weight times the clicked face's
  radius. **Remote** takes "Force (N)", a direction and "At (mm)"; an
  untyped point is the clicked face's centroid pushed out along its normal
  by a tenth of the body's bounding diagonal, and the point stays editable
  in the load row's menu. A torque is drawn as a circular arrow about its
  axis, a remote load as an arrow at its point with thin lines to the face.
  **Pin** takes "Hold it from turning" (unticked: free to turn); a click on
  a face that is not a cylinder is refused in the red line and places
  nothing; a pin is tinted and hatched as a support, its row saying "free
  to turn" or "held from turning". **Contact** takes "Friction (μ)"; a
  click on a face that is not flat is refused in the red line and places
  nothing, and on a flat one it places a contact support on the ground,
  hatched as a support, its row saying "on the ground, friction 0.25". A
  solved contact study lists each contact's closed area, normal and shear
  force and peak pressure, and what its stabilization carried
  ([contact](#contact)). A placed force can be dragged in the
  viewport to change its magnitude (one undo step on release). Run shows the
  shared progress bar. Results: mesh size, max von Mises, max displacement,
  field selector and deformation slider; for modal, the natural frequencies
  (selecting one shows its mode shape) and a mode-shape toggle; for
  buckling, the **Buckling factors** list (selecting one shows its mode),
  the governing factor with the knockdown and its class, the yield and
  interlayer factors, and the flags as text. The factor of safety is
  reported over RPC only.
- **Probes** (a solved static part study): arm **Probe**, hover a face to
  read its max and mean von Mises at the cursor, click to pin it. Each pin
  is a row, "<face>  max 2.1  mean 1.4 MPa" in the case shown, with a small
  marker at the face's peak in the viewport; its menu has Remove and **Add
  requirement**, a `study_probe` requirement on that face's peak, bounded
  at most the body's material yield (at most today's value without one).
  Pins are panel state; the requirement is what is saved.
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
| `add_study {type?, name?}` | `type`: static \| modal \| topology \| buckling (`kind` accepted) |
| `remove_study`, `list_studies` | list reports type, mesh size, loads, `has_results`, `stale`, participants |
| `set_study` | `solver` (auto \| in_house \| calculix: [CalculiX](#calculix)), `mesh_size`, `type`, `converge` (a static or buckling study's: auto \| always \| never, [a peak that has settled](#a-peak-that-has-settled)), `gravity` [x,y,z] m/s², `num_modes` (a modal study's frequencies or a buckling study's factors: refused on another unless the same call sets the type, which is applied first), `participants` (a `contact` one takes `friction`: [contact](#contact)), `load_cases`, `shape_from`, `nonlinear` (a static study's, solved on CalculiX: [nonlinear static](#nonlinear-static)), and `volume_fraction`, `filter_radius`, `max_iterations` (a topology study's, refused the same way, naming `type: topology`) |
| `add_load {study, type, face \| contact_with, occurrence?, magnitude?, direction?, case?, region?, fix_rotation?, friction?}` | returns the load id; `type` contact is a compression-only support on the ground under a flat face, or on a rigid participant (`contact_with`), with `friction` ([contact](#contact)); `type` torque takes `torque_nmm` and `axis_origin_mm` + `axis_dir` (omitted on a cylinder), remote takes `force_n`, `direction?` and `point_mm` ([torque and remote loads](#torque-and-remote-loads)); pin takes a cylindrical face and `fix_rotation?`, on a static part study ([pin supports](#pin-supports)); `region` puts it on part of its face ([loads on part of a face](#loads-on-part-of-a-face)) |
| `remove_load {study, id}` | by load id |
| `add_refinement {study, face, size, occurrence?}`, `remove_refinement` | local element size, mm |
| `mesh_study {id, async?}` | per-body nodes, elements, worst quality |
| `run_study {id, async?, recompute?, outlive_client?, timeout_s?, converge?, layer_normal?}` | mesh + solve, and on a static study settle its peak ([a peak that has settled](#a-peak-that-has-settled)): `converge` overrides the study's for this run, `layer_normal` is the build direction whose across-layers peak a printed part's check settles; returns the `study_results` summary when synchronous, `job` (its id) when async; unchanged inputs answer `reused: true` from the stored result ([studies as jobs](#studies-as-jobs)); `timeout_s` bounds a CalculiX run ([CalculiX](#calculix)); a nonlinear run that stops converging is refused with the load it reached ([nonlinear static](#nonlinear-static)) |
| `export_study_inp {id, path, body?, case?}` | the CalculiX deck a static study would run, meshed now, no ccx needed ([the deck](#the-deck)); a large-deformation study's NLGEOM deck, a snap-through one's refused ([nonlinear static](#nonlinear-static)) |
| `study_status {id \| job}`, `cancel_study {id \| job}` | progress of an async job, or by `job` any study job's (motion included) and how it ended -- a nonlinear run's `load_fraction` ([the run](#the-run)); cancel stops it |
| `study_results {id, layer_normal?}` | static: maxima, the largest principal stress, per-body, each support's `reactions`, factor of safety, `peak_at_support_edge` ([a peak on a support's edge](#a-peak-on-a-supports-edge)), `convergence` ([a peak that has settled](#a-peak-that-has-settled)), the `solver` that ran (on CalculiX also its residual and the agreement with an in-house run: [results and receipts](#results-and-receipts)); with `layer_normal`, the stress across those layers, its factor of safety and its away value `max_across_layers_away_mpa` ([printed layers](#printed-layers)); modal: `bodies[i].frequencies_hz`; buckling: per body `buckling_factors`, `buckling_factor`, `knockdown`, `yield_factor`, `interlayer_factor`, `governing_factor`, the flags, and at the top `governing_factor` = `factor_of_safety` ([buckling](#buckling)); nonlinear: per body `nonlinear` (the path, the equilibrium check, a snap-through run's peak) and at the top `nonlinear` {kind, peak_factor, governing_factor}, a snap-through run's `factor_of_safety` its governing factor ([nonlinear static](#nonlinear-static)); a contact: per body `contacts` (each pair's closed and open area, pressure and force) and `nonlinear.stabilization` ([contact](#contact)) |
| `probe_study {id, face, at?, case?, layer_normal?}` | the face's von Mises and largest principal stress, max (and where) and mean, and with `layer_normal` the stress across the layers; how it moved, as a rigid body; carried to `at` ([#probing-a-face](#probing-a-face)) |
| `enter_simulation {study?}`, `exit_simulation` | the GUI mode |
| `set_result_display {study?, field, deform_scale?, mode?, case?, threshold?}` | `field`: von_mises \| displacement \| shape \| density \| none; `mode`: a modal or buckling study's mode shape; `case`: which load case a static study shows |
| `check_topology {study, threshold?, recompute?, outlive_client?}` | a topology result's shape and solid design domain, statically, per case, as a job ([the topopt page](../topopt/README.md#a-static-study-of-the-optimised-shape)) |

## Tests

- `tests/test_study_inputs.cpp` (`[studyjobs]`): the input hash moves on
  each input class and on nothing else; a saved topology result reopens
  with its hash, fresh, and stale when saved against other inputs; a shape
  whose reads flip a plane's frame hashes as itself read back.
- `tests/study_jobs_smoke.py`: job ids, a job cancelled with its
  connection and one that outlives it, a cancel by id, an unknown id
  refused, a hung-up synchronous run stopping, reuse, `leave_at` in one
  solve, and every later `leave_at` reused while a real edit runs again.
- `tests/test_fem.cpp` (`[fem]`): element-level checks (tet10 stiffness and
  stress without a mesher), mesher cases, and analytic benchmarks — uniaxial
  bar σ = F/A, cantilever deflection against Euler–Bernoulli, a Lamé
  thick-wall cylinder, modal f₁ against beam theory, self-weight deflection. It asserts physics with tolerances and
  never golden meshes, because Netgen output is not bit-stable across
  versions. Also: a mesh spans two solids only when they share the face; the
  assembly cases (clamped nodes on the interface only, the refusals by name,
  staleness across documents, the shared cache, a bonded pair with two
  materials and an answer softer than the rigid one).
- `tests/test_fem_pin.cpp` (`[pin]`): a pinned lug's reaction answers the
  pull, on about half its bore, its bearing stress against
  (4/π) F/(d t) and the hole's sides against the net section F/((w − d) t);
  a lever on two pins whose reactions, force and moment, are its statics,
  its clamped peak
  growing with the mesh and flagged while the pinned one holds (`[slow]`);
  a cantilever's clamp-edge peak flagged and a hole's peak not; the
  refusals (a flat face, one free pin, two on one axis, a load that lifts
  the part off, modal and topology); the iterative solve against the
  direct one, and the GPU declining.
- `tests/test_fem_converge.cpp` (`[converge]`, `[mesher]`): Netgen's
  property table left empty after a mesh, a failed one included, a shape
  refined then meshed plain giving the plain mesh, and two meshes at once
  each giving its own; the hole plate's bore refined to 0.3, 0.2 and 0.15
  mm meshing with no tet worse than 0.1 and reading 24.55 MPa within 1 %
  (`[slow]`); the hole and the fillet converging within 1 % of their
  converged values and their handbooks' tolerance in at most three
  refinements; `never` not checked and `auto` stopping for time; a clamped
  cantilever settling away from its clamp; the clamp's away value within 3
  % across auto, 1.5 and 0.75 mm (`[slow]`); a support's reach; the stress
  across layers read away from a clamp, before and after the result is cut
  to its surface.
- `tests/test_fem_buckling.cpp` (`[buckling]`): Euler's column and its
  strong axis, a wide plate's stiffness, a thin cylinder's classical load
  and its SP-8007 knockdown (`[slow]`), Rankine-Gordon on a stocky strut, a
  pull that buckles nothing, the refusals, the saved result's round trip,
  and the 217k-DOF cost check (`[slow]`); `tests/buckling_smoke.py` over
  the RPCs ([buckling](#buckling)).
- `tests/test_fem_calculix.cpp` (`[contact]`): a block on the ground
  lifting off only past its kern, a steel ball on a rigid flat against
  Hertz (`[slow]`), a block standing on a slab as a contact participant,
  the refusals, the ground under a flat face only, and the contact rows'
  round trip; `tests/nonlinear_smoke.py` (a column standing on the ground
  over the RPCs), `refused_calls_smoke`, and the GUI's
  `sim/a_contact_rests_a_flat_face_only` ([contact](#contact)).
- `tests/test_fem_calculix.cpp` (`[plasticity]`): a bar past yield, under
  its yield the elastic stress, a bar's collapse at M_p (`[slow]`), the
  refusals and the result's round trip; `tests/nonlinear_smoke.py` (a column
  pulled past yield over the RPCs, its requirement, PLA refused) and
  `refused_calls_smoke` ([plasticity](#plasticity)).
- `tests/test_fem_calculix.cpp` (`[nonlinear]`): the elastica, a shallow
  arch's snap, a seeded column's approach to its linear factor (`[slow]`
  both), a small load against the linear answer, the refusals and the
  result's round trip; `tests/nonlinear_smoke.py` over the RPCs
  ([nonlinear static](#nonlinear-static)).
- `tests/fem_smoke.py`, `tests/fem_async_smoke.py`: the RPC surface
  headless, including an assembly tab; the async one also a reused run, a
  job's status by id and a cancel by id. Both skip (exit 77) on a build without
  Netgen, which is how the stock-Ubuntu CI jobs build; run them locally.
- GUI (`src/app/guitests/GuiTestsSimulation.cpp`, category `sim`): force
  direction and arrow placement, pressure as a field, face tints, gravity at
  the centre of mass, a static run against beam theory, the assembly study
  window and its clamp, the list-row editor; a torque on a cylinder, a
  torque refused on a flat face and placed with an axis, a remote load's
  default point, a pin refused on a flat face and placed on a cylinder, a
  probe pinned and made a requirement.
- `tests/rpc_gate_smoke.py` checks the study verbs are accepted on an
  assembly tab.

## Not done

- Thermal, fatigue. Thermal comes through [CalculiX](#calculix), as
  [large deformation, snap-through](#nonlinear-static), [contact](#contact)
  and [plasticity](#plasticity) did, not by growing this solver past linear
  ([the plan](../plans/calculix-bridge.md)).
- Hardening curves for any material but steel ([plasticity](#plasticity)):
  a plastic study on one is refused until a cited curve is in the library.
  Kinematic hardening (a part loaded back and forth), and a plastic strain
  field in the viewport (the panel and the RPCs report its peak).
- Contact between two contact participants, between a contact participant
  and a rigid occurrence, beside bonded participants, a curved face on the
  ground (it rests on a rigid occurrence instead), and an interference fit
  (parts that overlap are refused).
- The path past a peak: ccx 2.23 has no arc-length method (`*STATIC, RIKS`
  is read as a plain `*STATIC`, with a warning), so a snap-through run
  reports the peak and not the branch after it.
- Free-free modal analysis (a modal study needs a support).
- Shell and beam elements; adaptive refinement over the whole field (the
  convergence check refines only the faces its peaks lie on).
- Defeaturing for analysis. Mesh size is set by the smallest features, so a
  whole cabinet of 16 mm panels with fastener holes meshes to millions of
  DOF; per-panel studies are the practical route until holes can be
  suppressed for the analysis shape.
- Per-participant mesh size on a bonded member applies to its faces only; the
  study's global size still governs the interior.
- Result export (VTU).
