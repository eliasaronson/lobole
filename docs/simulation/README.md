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
- `loads`: each a `Fixed` support, a `Pin` support (a rigid pin in a bore
  that holds where the part presses on it, [pin supports](#pin-supports)),
  a `Force` (a total vector in newtons,
  spread as uniform traction over the face), a `Pressure` (MPa along the
  inward normal, positive pushes into the part), a `Torque` (N·mm about an
  axis) or a `Remote` force (newtons acting at a point off the face)
  ([torque and remote loads](#torque-and-remote-loads)).
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
  refused, naming why.
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
(`fem::FindSupportEdgePeak`): a support's EDGE is the nodes of its facets
that a boundary facet it does not hold also has (a clamp's rim, a bore's
rims); if the peak von Mises node is within one element of an edge (a node
of a tet that has an edge node), the result carries `supportEdgePeak`:
which support, and the largest von Mises farther than one element from
every support's edge. `study_results` reports it as `peak_at_support_edge`
{support, type, max_von_mises_away_mpa, away_at, factor_of_safety_away}
per body, per case and for the top-level peak, and the factor of safety's
receipt says its peak sits on a support's edge. The headline number is
unchanged; it is no longer silent.

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

A mesh failure is reported as the study error, naming the face when Netgen
says which, and its shortest edge when that is under a micrometre: the usual
cause, and one no picture and no BRepCheck shows: two faces meant to
coincide that miss by a few micrometres leave a zero-area face bounded by
edges that short (the pan/tilt yoke's arm and base did, while sketch planes
were single precision). There is no defeaturing step: suppress cosmetic features before meshing,
or, on an imported part, remove them with Delete Face
([modeling](../modeling/README.md#delete-face)).

## The solver

`src/core/fem/FemSolver.{h,cpp}` and `FemSolverInternal.h` (the element math
shared with topology optimization), namespace `fem`, GUI-free. Gated
`HAVE_FEM_SOLVER` on Eigen and `HAVE_SPECTRA` on Spectra (modal only).

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
  Fixed, Force, Pressure, Torque, Remote or (on a static part study) Pin
  and click a face; magnitude and an
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
  to turn" or "held from turning". A placed force can be dragged in the
  viewport to change its magnitude (one undo step on release). Run shows the
  shared progress bar. Results: mesh size, max von Mises, max displacement,
  field selector and deformation slider; for modal, the natural frequencies
  (selecting one shows its mode shape) and a mode-shape toggle. The factor of
  safety is reported over RPC only.
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
| `add_study {type?, name?}` | `type`: static \| modal \| topology (`kind` accepted) |
| `remove_study`, `list_studies` | list reports type, mesh size, loads, `has_results`, `stale`, participants |
| `set_study` | `mesh_size`, `type`, `gravity` [x,y,z] m/s², `num_modes` (a modal study's: refused on another unless the same call sets `type: modal`, which is applied first), `participants`, `load_cases`, `shape_from`, and `volume_fraction`, `filter_radius`, `max_iterations` (a topology study's, refused the same way, naming `type: topology`) |
| `add_load {study, type, face \| contact_with, occurrence?, magnitude?, direction?, case?, region?, fix_rotation?}` | returns the load id; `type` torque takes `torque_nmm` and `axis_origin_mm` + `axis_dir` (omitted on a cylinder), remote takes `force_n`, `direction?` and `point_mm` ([torque and remote loads](#torque-and-remote-loads)); pin takes a cylindrical face and `fix_rotation?`, on a static part study ([pin supports](#pin-supports)); `region` puts it on part of its face ([loads on part of a face](#loads-on-part-of-a-face)) |
| `remove_load {study, id}` | by load id |
| `add_refinement {study, face, size, occurrence?}`, `remove_refinement` | local element size, mm |
| `mesh_study {id, async?}` | per-body nodes, elements, worst quality |
| `run_study {id, async?, recompute?, outlive_client?}` | mesh + solve; returns the `study_results` summary when synchronous, `job` (its id) when async; unchanged inputs answer `reused: true` from the stored result ([studies as jobs](#studies-as-jobs)) |
| `study_status {id \| job}`, `cancel_study {id \| job}` | progress of an async job, or by `job` any study job's (motion included) and how it ended; cancel stops it |
| `study_results {id, layer_normal?}` | static: maxima, the largest principal stress, per-body, each support's `reactions`, factor of safety and `peak_at_support_edge` ([a peak on a support's edge](#a-peak-on-a-supports-edge)); with `layer_normal`, the stress across those layers and its factor of safety ([printed layers](#printed-layers)); modal: `bodies[i].frequencies_hz` |
| `probe_study {id, face, at?, case?, layer_normal?}` | the face's von Mises and largest principal stress, max (and where) and mean, and with `layer_normal` the stress across the layers; how it moved, as a rigid body; carried to `at` ([#probing-a-face](#probing-a-face)) |
| `enter_simulation {study?}`, `exit_simulation` | the GUI mode |
| `set_result_display {study?, field, deform_scale?, mode?, case?, threshold?}` | `field`: von_mises \| displacement \| shape \| density \| none; `case`: which load case a static study shows |
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

- Contact that can separate or slide between parts (a pin against its
  rigid pin is the one compression-only contact), and nonlinear analysis (plasticity,
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
