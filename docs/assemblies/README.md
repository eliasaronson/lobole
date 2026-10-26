# Assemblies

An assembly is a document that places parts (and other assemblies) in space
and constrains them to each other. It sits in the same project as the parts it
places, as another tab.

Two topic pages carry the detail:

- [solver.md](solver.md) — connector frames, the mate types and their
  maths, the placement pass, and the numeric pass that closes mate loops.
- [relations.md](relations.md) — couplings between mates' degrees of
  freedom: gear (external or internal), chain, rack, screw, linear, cam,
  freewheel, click, worm, intermittent (a carry, a Geneva), and over
  several mates linear_n, epicyclic and planet.

## Projects and documents

A project holds several documents — parts, assemblies, drawings, CAM jobs —
each a tab. `project.json` lists them as typed `documents` entries, each with
a stable id; an assembly lives under `assemblies/<name>/`, a part under
`parts/<name>/`. Every document RPC acts on the active tab (`set_active_part`
switches it; `list_parts` lists every tab with its `type`, not only parts).

## The assembly document

`AssemblyDocument` (`src/core/Occurrence.h`) holds, in one id space (a single
never-reused counter, so "id 7" names exactly one thing in an RPC, a file or
an error):

| list | what it is |
|---|---|
| occurrences | placed parts or sub-assemblies |
| mates | joints between two occurrences ([solver.md](solver.md)) |
| relations | couplings between mates' DOFs ([relations.md](relations.md)) |
| exploded views | named display offsets |
| motion studies | one mate DOF driven over a range, clash-checked |
| configurations | occurrence/mate/relation suppression and held mate DOFs ([../configurations/README.md](../configurations/README.md)) |
| FEM studies | studies on the assembly ([../simulation/README.md](../simulation/README.md)) |

An assembly is its own document kind, not a feature list: a `Feature`
regenerates from earlier features of the *same* document, and an occurrence
references another document. So an assembly regenerates by a placement pass
(the solve), not by executing features.

On disk it shares the one `designer-document` format and `kVersion` with parts
(discriminated by `kind: "assembly"`), one file per item under
`occurrences/`, `mates/`, `exploded/`, `relations/`, `motion/`
and `studies/`, so disjoint edits merge without conflict. Field-level detail
is in [../versioning/format.md](../versioning/format.md).

## Occurrences

An `Occurrence` references a document by its stable slot id (`part_id`) —
never by name, so renaming or moving a part cannot break an assembly — with a
`ref_kind` of `part` or `assembly`. The kind is stored rather than inferred so
a dangling reference still knows what it was looking for.

Its placement is `position` + `orientation` (a quaternion): diffable and
shear-free. **An occurrence always stores an explicit transform — its last
solved placement** — so any file, or one whose mates were deleted, opens at
sane positions without running the solve; mates re-place non-`fixed`
occurrences on top of that. `fixed` grounds an occurrence (the solve's
anchor); `visible` hides it. `part_version` is reserved for a version pin and
is unused.

**Attachments.** An occurrence may ride on another (`attached_to`, with
`attach_offset` / `attach_rotation` in the parent's frame, or in its seat's,
below). This is how placed fasteners follow their plate and the hole they
sit in. It is a composition, not a mate: no solver sees
it, `ApplyAttachments` recomputes the rider's `position`/`orientation` after
the solve, it goes one level deep only, and the gizmo will not drag a rider
(its pose is derived). Deleting an occurrence deletes what rides on it. A
rider may also be SEATED on two named faces of its parent (`attach_seat`,
v157: a face it bears on and the cylinder or cone centring it — a
fastener's hole); its offset is then in the seat's frame, so it follows a
hole that moves inside its part, and a seat that no longer resolves is an
`attach_error` on the rider
([attached, and mated](../parts/README.md#attached-and-mated)).

Rendering is instanced: every occurrence of a part draws the part's one
tessellation with its own transform -- one per configuration and material it
is placed in (below).

### Configurations per placement

`configuration_id` says which configuration of the placed document an
occurrence uses; 0 is whatever that document has active. For a sub-assembly
that is a suppression set; for a part (v135) it is a different geometry: the
part regenerated in that configuration, a cached copy
(`ui::ConfiguredPartDocument`, the one drawing views and Derived features
read). `assembly::PartResolver` takes `(part, configuration)`, so `Flatten`,
mate connectors and the solve see each placement as placed, and
`FlatInstance::configurationId` says which. Every other read of a placed
part's geometry goes through `ui::OccurrencePartDoc` -- never
`PartDocById(occ.partId)`, which is the active configuration whatever the
placement asked for. A configuration the part does not have, or does not
build in, is refused when it is chosen, saying which feature fails.

`material` (v135) makes a placement of a part a material other than its
part's, every body of it: mass, the BOM, the renderers and STEP export read
it (`set_occurrence_material {id, material: slug | ""}`; `""` is the part's
own). One chess piece part placed as pawns and rooks, in maple and walnut, is
one part. A per-placement *appearance* override does not exist.

Keys follow: the renderer's tessellation cache and the in-context ghosts are
keyed by (part, configuration, material), a motion study copies each (part,
configuration) once, and staleness stamps fold in the part's configurations
revision -- editing a configuration a placement uses regenerates its copy
without moving the part's `Version`, and a configured copy carries the live
part's `Version`, so a key or a stamp on the part alone drew one
configuration as another.

## Sub-assemblies

An occurrence may place another assembly. By default the placement is
**rigid**: one solid unit at whatever placements its own document stores, and
the parent's solve never recurses into it. A placement can instead be
[flexible](#flexible-sub-assemblies), when the sub-assembly's own mechanism
moves in the parent. Mates, exploded views and the drag gizmo operate between
top-level occurrences either way, and on a flexible placement also reach its
moving parts.

`assembly::Flatten` (`src/core/AssemblySolve.h`) is the one tree walk. Every
consumer that wants geometry — render sync, BOM, mass, contact/interference,
export, drawings, ray tracing, section caps, in-context ghosts, auto-explode,
camera bounds — takes its leaf parts from it, each with a composed world
transform, a display path (`"Gangway/Board 03"`) and the top-level occurrence
it belongs to. Visibility and configuration suppression compose down the tree.
Flatten fails on a reference cycle (naming it) and past a depth of 8, and
skips a dangling reference rather than failing.

- A sub-assembly is a live reference: the parent shows whatever that document
  currently is. `set_occurrence_configuration` chooses which of its assembly
  configurations a placement uses (0 = its active one), so two placements of
  one assembly can differ -- as two placements of one part can
  ([above](#configurations-per-placement)).
- A top-level occurrence is one node for contact and interference, one
  explode step, one drawing reference.
- A mate may anchor on a face, a frame or the origin of something *inside* a
  placed sub-assembly: the connector's `inner_path` names the occurrence chain
  down to it. On a rigid placement the mate positions the top-level
  occurrence; on a flexible one, a path that enters at a moving part mates
  that part.
- Paths are read-only. `assembly::ResolvePath` takes an id chain (`"12/47"`)
  or an unambiguous name path; `interference`, `measure` and `frame` accept
  one, so a question can cross the boundary. Editing a sub-assembly's contents
  happens on its own tab.
- `add_occurrence` refuses a reference that would close a cycle
  (`WouldCycle`); `delete_part` refuses while any assembly places the
  document.
- Staleness is transitive: `SubtreeVersions` stamps every document version in
  the subtree, so an edit inside a sub-assembly re-solves and resyncs the top.

### Flexible sub-assemblies

A placement with `flexible` set (v129; `add_occurrence {flexible: true}`,
`set_occurrence_flexible`, or the Flexible checkbox in its editor) brings its sub-assembly's occurrences, mates and
relations into the parent's solve and motion studies, instanced for that
placement. An engine's chain then drives the injection pump's own camshaft
mate, and the pump is one occurrence rather than seventy.

**The state lives on the parent's occurrence.** A sub-assembly document is
shared by every placement of it, so nothing is written into it. The
occurrence keeps `inner_poses` (where each MOVING occurrence of the
sub-assembly stands, in the placement's frame) and `inner_mate_dofs` (every
inner mate's values as this placement has them). Two placements pose
independently, the parent's undo covers both, and the file re-solves the same
way. `Flatten`, `ResolvePath` and `ConnectorLocalFrame` place what is inside a
flexible placement at those poses, so render, contact, export, measurement
and mate connectors all see the unit as it stands.

**The solver is wrapped, not changed** (`src/core/AssemblyFlexible.cpp`).
`ExpandFlexible` copies the parent and, per flexible placement, adds:

- its moving occurrences, under synthetic ids from a range no document
  reaches. An occurrence moves when a mate chain reaches it from one that is
  fixed in the sub-assembly, or it rides on one that does;
- its mates and relations, remapped. A mate on anything else (the housing,
  what is bolted to it) lands on the root with an `inner_path`: it cannot
  move relative to the unit, so it folds in;
- the holds of the configuration the placement runs under.

`Solve` runs on that copy, and `CollapseFlexible` writes back the
placements, the inner poses, the inner mate values and the errors.
`Solve` skips all of this when nothing is flexible. `motion::Run` expands
once and runs every sample on the expanded copy.

**Naming an inner mate.** A mate is `{path, mate}`: the path is the flexible
placement (one occurrence id), the id is the sub-assembly's. Empty means this
document. A relation's sides (`add_relation {a_path, b_path}`, saved as
`mate_a_path`/`mate_b_path`), a motion study's driver (`driver_path`) and
`set_mate {id, path}` take it. `set_mate` with a path sets this placement's
pose of the mate and nothing else. `solve_assembly` reports every flexible
placement under `flexible`, with its mates by path.

**Configurations.** A flexible placement runs its mechanism under its own
`configuration_id` (0 = the sub-assembly's active one): that configuration
holds and suppresses its mates. The parent's configuration cannot, and
`set_configuration` refuses a `path` in `suppressed_mates` or
`mate_overrides`. The pump arrives with its rack held at full load.

**Refused, naming why.** A placement stays rigid, and says so on the solve
(`flexible[].error`) and at `set_occurrence_flexible`, when:

- its sub-assembly has nothing fixed to move against;
- it holds a freewheel or a click, whose history (`lead`) would be
  per-placement state. A relation into a flexible placement cannot be one
  either;
- it places something flexible itself, or the assembly is itself placed
  flexibly somewhere. Only one level moves.

**What sees the moving parts.** A motion study's samples carry a pose per
moving inner occurrence, keyed by its path `[placement, occurrence]`, and
inner mate values with their `path`. Clash checks key a flexible placement's
bodies per inner occurrence, so a plunger is checked against its barrel and
against the engine, and the rigid-pair rule compares those leaves' own
transforms. The GLB export writes one node per moving inner occurrence, and
live playback moves each one. A clearance requirement's input hash reads
the inner poses (the occurrence as saved). An exploded view moves the unit by its own step, and a moving part by
a step of its own on top (below).

**In the GUI** ([panels](../ui/panels.md#assembly-tab-the-model-panel)). The
occurrence's editor has the Flexible checkbox and the configuration picker;
both call `ui::SetOccurrenceFlexible` / `ui::SetOccurrenceConfiguration`
(`src/ui/FlexibleActions.cpp`), which is also what the two RPCs are, so a
refusal reads the same in the panel as over the socket, one undo step each.
The tree lists a flexible placement's moving parts and its mates that turn
or slide under its row, read-only. The study driver combo and the relation
editor's sides list those mates as "Pump / camshaft" (`ui::DofMateChoices`),
stored by path as the RPC stores them.

A moving part is selected ON ITS OWN, from its tree row or by clicking it in
the viewport: an occurrence selection item whose `id` is the placement and
whose `innerPath` is `[part]` (`get_state` reports it as `{kind:
"occurrence", id, path}`). The rest of the unit -- its housing, what is
bolted to it -- still selects the placement. `SelectedOccurrenceId(s)` skip
such an item, so Delete, Hide and the unit's gizmo never act on the whole
unit for a click on its crank. The selected part's gizmo is the ring or arrow
of the sub-assembly mate that places it (`ui::InnerMateDriving`): a drag
writes this placement's pose of that mate (`SetMateDofAt`, the write
`set_mate {id, path}` makes) and solves holding it, so the unit's mechanism
and whatever the parent chains to it follow, one undo step per drag. A turn
snaps as a top-level joint's does; a DOF the placement's configuration holds
does not move. Only what `set_mate` with a path can pose drags -- a turn and
a slide -- so a planar mate's in-plane slide and a ball show no handle.

**Exploded views** (v130). An explode step may name a moving part inside a
flexible placement as a mate is named: `{occurrence: <the sub-assembly's
id>, path: [placement]}`. Its offset adds to the placement's own step, so a
crank lifts out of a pump that has itself moved aside. `set_explode_step`
takes `path`, refusing a placement that is not flexible; with such a part
selected the explode gizmo writes that step. A path is ignored while the
placement is rigid (its parts explode as one block), and auto-explode stays
top-level.

## Mates and the solve

A mate joins two **connector frames**, one on each occurrence, with one mate
type: fastened, revolute, slider, cylindrical, planar or ball. The frame is
derived from the part at solve time (a face, a named Frame feature, or the part
origin), never stored, so a part edit that moves a hole moves the bolt mated to
it. The mate stores its DOF values, so a file re-solves to identical
placements.

The solve is closed-form for mate trees (direct frame alignment from the
anchors) and numeric only for components that contain a loop, where it closes
the loop in the mates' own joint coordinates and reports remaining DOF,
redundant-but-consistent mates, and how far a loop that cannot close misses.
All of it is in [solver.md](solver.md).

Units on the wire: `add_mate` and `set_mate` take a mate's angle as
`rotation_deg` only (a bare `rotation` is refused by name); answers carry
`rotation`, in radians, beside it. `offset`, `dx`, `dy` are mm.

**Re-solve triggers**, all through `ui::SolveActiveAssembly`: any mate or
occurrence edit, opening a project, activating an assembly tab whose parts
changed (`AssemblyPartsStale` compares per-document version stamps taken at
the last solve, so this works headless too), and every part regeneration
while editing in context. A mated, non-fixed occurrence's own transform is
overwritten by the next solve.

## Relations

A relation couples mates' degrees of freedom — `dof_b = ratio · dof_a +
phase` for gear, chain, rack, screw and linear; a lift table for a cam; a
one-way drive for a freewheel; a pawl for a click; one step a turn, standing
still between, for an intermittent relation (a carry); and Σ cᵢ · dofᵢ = constant
over several mates for linear_n and an epicyclic train (Willis, and each
planet's turn on its carrier). In a mate tree it is enforced by propagation
before placement; inside a loop it is a residual row in the loop solve.
Dragging or `set_mate` on a member moves the others. The occurrence panel
lists, edits and makes them (Add relation). See
[relations.md](relations.md).

## Motion studies

A `MotionStudy` drives ONE mate DOF from `from` to `to` in `samples` steps
(default 48), solves at each sample, and checks every occurrence pair for a
clash (`clash_mode` `mesh`, the default, or `exact`). The definition is
document state; the samples and the collision table are its result, saved
beside it and keyed by the run's inputs (below). A study may
name the configuration it runs under (`configuration`: an id, 0 = base,
omitted = whichever is active).

A study starts with every part already seated, so it says nothing about
whether a part could be put there: that is `check_insertion` and the
`insertion` requirement ([insertion](../inspect/README.md#insertion)),
which slide a part from clear to its seat along its mate's axis, and the
assembly's [sequence](#assembly-sequences), which puts the whole assembly
together in order.

- The range is stored in the DOF's own unit (radians / mm); the RPC takes
  degrees for a rotation and converts once.
- The mesh check meshes each body at 0.2% of its bounding diagonal unless
  the study states `clash_deflection_mm` (v167). A running gap finer than
  that — print-in-place gears at 0.1 mm a side, where a 100 mm ring meshes
  at about 0.2 mm — has triangles coarser than the gap they decide, and can
  read either way; state a deflection under the gap. The stated one is what
  the triangles are cut to, on a copy of each shape so the document's own
  triangulation is untouched ([mesh mode](../inspect/README.md#mesh-mode)),
  and the run's `deflection_mm` reports the coarsest mesh it decided on.
  It belongs to the mesh check only: on an `exact` study it is refused, and
  `set_motion_study {clash_deflection_mm: null}` derives it again. The
  Motion window's Validate always checks exactly, so it has no field there.
- The mesh check calls two faces seated on each other, no triangle
  piercing another, `touching`: listed per sample under `touching` (and
  kept with a stored result, v180), not a clash, as exact mode calls the
  same pair
  ([touching is not a clash](../inspect/README.md#touching-is-not-a-clash)).
  Any overlap is still a clash.
- Samples are walked in order, each solve starting from the last, so a closed
  loop stays on the branch it is on — and the first sample is WALKED to from
  wherever the document has the driver, at the study's own spacing, as the
  scrub walks: a jump from a crank left at 180 degrees to 0 landed a
  slider-crank on its mirrored branch, and the whole run followed it there.
  Between samples every free joint is watched, and a step too coarse for
  its seed is re-solved finer ([branch jumps](#branch-jumps)). A sample
  that does not solve is reported unsolved with its residual, never drawn
  in the last good pose.
- Pairs rigid to each other over the whole study — fastened, or carried by
  the same moving part — cannot be made to collide by the motion, so they are
  checked once, at the first sample, and skipped at every later one. The
  run solves every sample once, recording the placements, and that record
  decides which pairs move (the relative transform of two top-level
  occurrences changes at some sample); then every sample's checks run in
  one batch ([what a check costs](#clash-sweeps-what-a-check-costs)).
  The rigid check is always EXACT, whatever `clash_mode` says: a bolted
  joint's engaged thread and press fit are relieved only by an exact check.
  A clash
  found there is a standing clash: reported at sample 0 with `rigid: true`,
  listed again in `rigid_clashes`, left out of `seated_contacts` and
  `appeared`, and the study is not clean. The result says where the rigid
  pairs were checked (`rigid_checked_at_sample`), how many body pairs that
  cost (`pairs_rigid_checked`) and how many later checks were skipped
  (`pairs_rigid`); the table's last line says the same.
- Every clash says where it is, in the names `interference` uses: an exact
  check gives the overlap's `volume_mm3`, `centroid_mm` and
  `bbox_min_mm`/`bbox_max_mm`; a mesh check knows only that two bodies
  overlap and gives `within_min_mm`/`within_max_mm`, the region their boxes
  share.
- An exact check relieves what is meant to overlap — an engaged thread, a
  ring in its groove, a press fit
  ([designed overlaps](../inspect/README.md#designed-overlaps)) — and the
  run lists its press fits once per pair, the first sample that found them,
  as `press_fits` [{a, b, a_name, b_name, fit, interference_mm,
  max_interference_mm, volume_mm3}], saved with the result (v159). A mesh
  check relieves none and says so (`relieves`), except on a rigid pair,
  which is always checked exactly. A pair whose answer depends on more than
  its two shapes (a thread, a grip, a band on each side) is asked directly
  rather than from the [contact cache](#exact-contact-reuse-across-motion-runs),
  and the bands are inputs of the run's hash, so a tolerance edit makes a
  stored run stale.
- `run_motion_study` runs on a copy of the assembly, so the document is left
  where it was; `leave_at` (`"start"`, `"last"` or a sample index) poses
  the live assembly at a sample afterwards so the far end of the travel can
  be measured: the placements and moving mate values the run recorded there
  are written in and solved ONCE (`ui::ApplyMotionSample`; the reply's
  `left_solves` counts them), not walked to. `leave_at_deg` (a rotation
  study) or `leave_at_mm` (an offset study) leaves it at a driver VALUE
  between samples instead: the nearest sample's pose, then the one solve
  steps to the value from it, so a slider-crank lands on the branch the run
  found. A value outside the study's range, the other unit, `leave_at`
  beside it, or `leave_at` with `async` (there are no samples yet) is
  refused; a sample that did not solve has no pose to leave it in.
  `set_motion_pose {t}` scrubs (no undo step). `include_poses`
  returns every occurrence's placement and every moving mate's value per
  sample.
- A run is answered from a stored one when its inputs hash the same
  (`motion::InputKey`: the definition, every occurrence and placement,
  every mate with its current value, relations, configurations and the
  placed parts' geometry, materials and size bands): `reused: true`,
  nothing computed,
  `recompute: true` to run anyway. So `run_motion_study {leave_at}` after
  a run costs one solve -- and so does every one after it: a pose left from
  the result's own samples re-keys the result to that pose
  ([studies as jobs](../simulation/README.md#studies-as-jobs)), so measuring
  at a dozen samples does not run the study a dozen times, while a scrub,
  a drag or any edit is an input again. The result is saved with the project
  (`motion/<id>-<slug>.result.json` with its placements in `.result.bin`,
  v153/v154) and read back when the assembly
  is opened or its tab comes up: current when its inputs hash as they did,
  stale otherwise, so a reopened project's exact run is not paid for again
  ([studies as jobs](../simulation/README.md#studies-as-jobs)). `async: true` runs it as a job with an id
  (`job`), reported by `study_status {job}` and `motion_status`
  (`job`, `fraction`) and stopped by `cancel_study {job}`; it is cancelled
  when the connection that started it closes, unless `outlive_client: true`
  ([studies as jobs](../simulation/README.md#studies-as-jobs)).
- `export_motion` writes an animated GLB (metres, Y-up, 24 fps) and refuses
  when any sample is unsolved.

Code: `src/core/MotionStudy.{h,cpp}`, `src/remote/RemoteMotion.cpp`,
`src/ui/MotionActions.cpp`.

### Branch jumps

Each sample's solve starts from the last, and a step too large for that
seed can land on a mirror branch that closes every loop exactly: the Jansen
leg at 45 degrees a step does, at 360 degrees
([coupled loops](solver.md#coupled-loops)). Its residual is zero, so the
solve cannot tell. So the run watches every coordinate the loop solve
CHOOSES (each moving mate's rotation and offset, `motion::Run`'s
`BranchWatch`) -- not the driver's, and not one a relation sets as a
function of another (a gear, a cam, an intermittent, a screw, a member of a
relation over several mates), walked outward from the driver as
propagation walks it. Those have no branch to flip to: a tangent cam's
follower leaving its base circle after a dwell changes its rate at once,
and that is the lift table, not a jump (`tests/test_motion.cpp`, a cam
follower at 22.5 degrees a step).

- **The rate.** Along a branch a coordinate's rate, its move per unit of
  the driver, changes smoothly. A step whose rate for some coordinate moves
  by more than half that coordinate's largest rate so far (or of a tenth of
  the fastest coordinate's, for one that has barely moved) is re-solved
  from where it started at half the step, then halved again where needed,
  down to the study's `max_refine` halvings (default 6: a 64th of the
  step; 0 refines nothing). Where the rates hold again the steps grow back.
  The sample is the continued pose, marked `refined`, and the run counts
  them (`refined_samples`). The first step of a run, with no rate yet, is
  taken from the finest step up.
- **The jump that stays.** At the finest step the change is asked again
  scale by scale: the rate over the step against the rate over the same
  step back from where it started, and the same at twice the step. Along a
  branch the change is the curvature times the step and halves with it (a
  knee near straight, a slider at its dead centre); across a flip it
  doubles; at a kink, where the linkage passes a singular pose, it keeps
  two thirds or more in one of the steps either side. A change that keeps
  more than 0.6 of itself is a jump: the sample carries `branch_jump`
  {mate, name, path?, dof, its move and the move its rate gave, the driver
  value and the step}, the run counts `branch_jumps`, the table says
  `BRANCH JUMP`, the verdict says "Jumps branch at sample N", and the study
  is not clean, as an unsolved sample is not.
- **What it finds**: the Jansen leg at 45 degrees a step comes back on its
  built branch at every sample, with 6 of its 9 samples refined; at 2
  degrees nothing is refined. A parallelogram four-bar driven through its
  dead point, where all four links lie in line, leaves it as an
  anti-parallelogram, and the sample there says so: a mate that turned at
  the crank's rate stops (`tests/test_motion.cpp`, "A four-bar driven
  through its dead point reports the branch jump").
- **What it cannot find**: a linkage passing a singular pose where the two
  branches cross with the same rates (a Grashof change point whose knee
  goes straight through) changes branch without a change of rate. A
  generator that cares which branch it is on still checks it, as the
  Strandbeest checks its knees' signs.

### Tracing a point

What a linkage is for is usually a PATH: a walker's foot, a coupler point,
a follower's tip. `run_motion_study {study, trace: [{name, occurrence,
point_mm}]}` follows each named point, given in its occurrence's own (part)
coordinates, through every sample, from the poses the run already solved
(`motion::TracePoints`); inside a flexible placement the point's occurrence
is `path: [placement, occurrence]`, a moving one. The reply's `trace` gives
each its `points_mm` (world mm, one per sample), `min_mm`, `max_mm` and
`extent_mm` over the solved samples, and `unsolved_samples`. A sample that
did not solve has `null` there, never the last good point. `motion_status
{study, trace}` traces a stored run, which is how an `async` run is traced
(`trace` with `async` is refused: there are no samples yet).

The trace is asked for, not stored: it costs no format and no rerun, since
a stored run with the same inputs answers it. It is checked before the run
(an unknown occurrence, an empty or repeated name, a path into a placement
that is not flexible, each refused naming what there is), and an inner
occurrence that does not move inside its placement, which the run records
no pose for, is refused after the run and before `leave_at` moves
anything. The hinge box, the carburettor and the Strandbeest read their
paths this way (`tests/test_motion.cpp`, `tests/motion_smoke.py`). The
Motion window does not draw a path: a trace is a request parameter with no
definition in the document for the window to show.

### Motion video

`export_motion_video {study, path, format?, width?, height?, fps?, frames?,
quality?, async?}` films a study: the file to put in a README, send to
someone or attach to an issue. `export_assembly_video` films an assembly
sequence the same way ([playing and filming a sequence](#playing-and-filming-a-sequence)):
the capture, the encoder and the job are one, and only the poses differ
(`VideoPoses`: a study's run, or a sequence's timeline).

- **Formats.** `mp4` (H.264), `webm` (VP9) and `gif` (one palette for the
  whole animation, from ffmpeg's palette pass) are encoded by **ffmpeg**, run
  as an external program found on PATH (`ffmpeg.exe` on Windows); `png`
  writes `frame_0000.png`… into a FOLDER and needs nothing. Omitted, the
  format is the path's extension.
- **Refused, never substituted**: an unknown format or quality, a path whose
  extension disagrees with `format`, `png` given a file rather than a folder,
  an odd width or height for mp4/webm (4:2:0 colour), fewer than two frames,
  a study that cannot pose (`why_not`) or whose run of its poses does not
  solve, no renderer (`--headless`), and a
  missing ffmpeg — "export_motion_video format mp4 needs ffmpeg on PATH;
  install it, or use format png". `ping`'s `features.ffmpeg` says whether it
  is there, so a test or an agent can gate on it. When ffmpeg itself fails,
  the refusal carries the last lines it wrote on stderr.
- **Poses: what Play shows.** The video plays the study as the Motion
  window's Play does: the run's samples at 24 a second
  (`kPlaybackSamplesPerSecond`), each frame the playback's own blend between
  the two samples either side (positions lerped, orientations slerped), so a
  49-sample study is a two-second video. The poses are the stored run when
  playback could show it (fresh, solved, not cancelled); without one the
  export makes its own — the study run without collision checks, on a
  worker, and NOT stored as the study's result (`PosesToPlay`). It needs no
  run, and the document is never posed: the frames are shown through the
  playback's pinned poses (`ShowMotionPoses`), which is display-only, so the
  assembly is where it was afterwards and an edit made during an async
  export survives it. Playback is stopped first; the viewport shows the film
  going by and returns to the document's placements at the end.
- **Frames: what the viewport draws.** Each frame is the viewport's own
  render of the model (`AppState::beginFrameCapture`): the mesh, field and
  outline renderers at the viewport's settings — MSAA, shadow map, SSAO,
  outline widths — and none of the overlays (sketches, datums, gizmos,
  handles, selection or hover tints, panels), from the current camera at
  `width` x `height`. It is NOT supersampled: a 2x capture averaged down drew
  the outlines lighter and the ambient occlusion at four times the pixels,
  which is the picture the app does not show. Offscreen, a frame on a sample
  is the screenshot of the assembly posed there, pixel for pixel
  (`motion_video_smoke`); windowed, it is the viewport re-rendered as the
  frame loop draws it, apart from the SSAO noise tiling from another origin
  (`assembly/motion_video_frames_are_the_view`, `captureViewport`).
- **Streaming.** A job of kind `motion-video` in `background_jobs`. Showing a
  pose and capturing it need the renderers, so they are the job's main-thread
  slice (its `finish`): each slice aims the renderers at the video's size,
  streams frames for up to 100 ms and aims them back, so the windowed app
  keeps drawing its progress. The readback is PIPELINED
  (`render::FrameStream`): one target made once, three slots each with its
  own command buffer, fence and host-cached readback buffer, and the slot is
  the frame index the renderers record with — frame N+1 is posed and
  recorded while N renders, and N is read back only when its slot is needed
  again. An encoder thread turns each frame into RGB and writes it — into
  ffmpeg's stdin, or a PNG — behind a queue six frames deep (counting those
  on the GPU). No temporary frames are written for a video. Cancel stops
  after the frames in hand and deletes the partial file. Synchronous by
  default (the answer is the file's `bytes`, `frames`, `fps`, size and
  `seconds`); `async: true` returns at once and `motion_video_status` reads
  the outcome.
- **Encoding.** mp4 is x264 (`-preset medium -profile:v high`, `+faststart`),
  webm VP9 (`-b:v 0 -row-mt 1`), both 4:2:0 converted with the BT.709 matrix
  and tagged BT.709, limited range — what a player assumes of an HD video
  (ffmpeg's own default converts with BT.601 and tags nothing). `quality`
  `low|medium|high` is x264 CRF 23/18/14, VP9 CRF 36/30/24, a GIF palette of
  64/128/256 colours.
- **Defaults** come from what is exported: the size is the viewport's in the
  pixels it is drawn with (its size times the framebuffer scale, rounded
  down to even for mp4/webm), `fps` 60, `frames` as many as Play takes at
  that rate (`(samples - 1) / 24` seconds, both ends included), `quality`
  medium. `frames` given spreads the study over that many frames.
- **Speed.** On the OM615 (332 occurrences, 49 samples, 1280 x 720) the
  export is about 3.3 s for 121 frames, of which about 2.4 s is the
  collision-free run of its poses; the frames themselves stream at over a
  hundred a second. It was 49 frames in about 21 s, each a scrub solve and a
  one-shot capture.
- **GUI.** "Export animation…" in the Motion window, under "Export GLB…",
  opens a save dialog whose file type picks the format (a `.png` pick writes
  the frames into a folder named after the file); while it runs the button is
  its progress bar and Cancel.

Code: `src/ui/MotionVideo.cpp`, `src/render/FrameStream.{h,cpp}`, the
capture in `src/app/main.cpp` (`ViewportFrameCapture`),
`src/common/ChildProcess.{h,cpp}` (PATH lookup and a child fed on stdin:
`posix_spawn`, or `_popen` on Windows).

## Assembly sequences

How the parts go together, in order: a document object on the assembly
(`set_assembly_sequence`, `get_assembly_sequence`; saved in the index from
v187, groups and poses from v188, one undo step). Each step is one of:

- `occurrence`: the top-level occurrence it places, along its **path**
  (below);
- `occurrences`: a **group** of two or more that moves as one, rigidly,
  along one path, checked against everything placed before it and never
  against itself. Two parts that block each other one at a time go in
  together this way -- the clock's stage-2 third and escape arbors with
  their wheels meshed, the engine's piston, rod and wrist pin down a bore,
  the jar's lid with its gasket in it. A group goes in as it is assembled
  (no turn about the path is tried). Its default path is its members'
  **common seat axis**: each member's own cylinder at its mate to
  something placed before it (members seated only on each other give
  none), refused by name when those axes are not parallel and no
  `direction` is given;
- `pose`: [{`mate`, `rotation_deg` | `offset_mm`}], a step that places
  nothing. Those mate values (a revolute's or cylindrical's angle, a
  slider's or cylindrical's travel: the mate's own value, as `set_mate`
  sets it) are held and the assembly solved there by the mate solver,
  walked in strides of 10° or 2 mm so a linkage stays on its branch; every
  later step is checked at that pose (`at_pose` in its row; the pose
  step's row says what it `moved`). Poses accumulate. A mate without that
  freedom is refused when the sequence is set; a pose that does not
  solve, or that the solve moves off (a relation or a closed loop drives
  the mate), is refused naming the mate when it is checked. A mate has no
  limits in the model, so there is none to refuse a value past. The
  engine's cap pins for cylinders 1 and 4 go in with the crank turned half
  a turn (at rest they are forced 26 mm through the block's bulkheads),
  the Strandbeest's back pivot cap with the crank at 15°.

A placing step's path:

- `direction`: the way it moves going home, in the assembly's frame. A part
  given one goes on as it is placed (no turn about the path is tried).
  Without one the path is the axis of the part's **own cylinder** that its
  mate seats -- the cylinder whose axis passes nearest the mate frame's
  origin on its side (within 2 % of its size, half a millimetre at least)
  -- not the mate frame's Z, which a fastened mate need not put along the
  part (the engine's wrist pins and pulleys were swept the wrong way by
  it); both ways along it are tried, and turns about it as
  `check_insertion` tries them. The mate is `mate`, else the one mate
  joining the occurrence to one placed before it.
- `origin_mm`: a point on the path's line (default the cylinder's axis, or
  the part's centre with a direction); `distance_mm`: from its seat to
  where it starts (default: clear of everything placed before it);
  `turn_deg`: turned about the path over that distance, a bayonet, read
  step by step at the turn each step has; `thread`: it goes on by its
  thread. A **modelled** thread (`modeled` on its Thread feature) turns by
  its helix as it goes -- a turn a pitch of its offset from the seat, from
  whichever side it comes (right hand: clockwise seen from behind it) --
  and the sections judge its flanks along it, read at a 24th of a turn;
  the thread it goes into must have its pitch and hand. A cosmetic thread
  meeting one placed before it, coaxial with the path, is judged by the
  pair's own fit (one external, one internal, the same major diameter and
  pitch), not read as interference. With neither the step fails naming
  why.

`check_assembly_sequence` slides each step's occurrence (or group) home
along its path against **every occurrence placed before it**, not only
its mate partner, at the pose the steps before it set, by the insertion
check's sections with one exact boolean confirming each step
([insertion](../inspect/README.md#insertion)). The first placing step is
the base. It runs as an `assembly_sequence` job, off the frame. A step
that cannot be checked -- its occurrence gone or placing no matter, no
mate to a placed part and no direction, no cylinder at its mate, a group
with no common seat axis, a pose that does not solve -- is refused naming
it; an occurrence with matter that no step places is listed in `missing`
and fails the sequence. A hidden occurrence places none: the
Strandbeest's leg pair put together hides its whole frame and crankshaft,
which still carry the crank and the pivots, and places the pieces they
are pressed from, each fastened where it sits. The answer: `pass`,
`seconds`, `sections`, and per step `against` (what it was checked
against), `worst_against` (the occurrence it meets worst), its `path`
(`from`, `moves`, `origin_mm`, `either_way`), the insertion's reading
(`press_mm`, `interfering_mm`, `excess_mm`, `deeper_mm`, `confirm`, any
`threads`), `solids` and `seconds`.

**Sections are cut once a run** and moved between steps: a placed part's
bands are kept per path direction (and its reverse) and slice thickness,
and carried to each later step's axis and reach -- and with a pose, when
it only turned about the path -- rather than cut again; a step that sweeps
a short stretch of a long part cuts only that stretch
([insertion](../inspect/README.md#insertion) says how; `sections`
{`reused`, `cut`, `evicted`} solids in the answer, a step's `solids`).
The requirement kind `assembly_sequence` states it checked
([requirements](../requirements/README.md#measures)).

### A check answered again

The check's answer is kept under everything it reads
(`remote::SequenceInputKey`, the same key the `assembly_sequence`
requirement's staleness reads): the assembly's occurrences, mates,
relations and configurations and every sub-assembly's, every placed
part's bodies -- shapes, thread marks, face names, materials -- with the
tolerances and print process a fit is judged by, and the sequence as
saved, its steps and allowance. `check_assembly_sequence`, the
requirement and Play all run the check through `remote::RunSequenceCheck`,
which answers a check whose key matches one of the last eight answered in
the session with that answer, `reused: true` added (its `seconds` are the
run's it kept). The clock's generator checks each stage and then states it
as a requirement and checks that: the second check was another 80 s at
stage 1 and 165 s at stage 2, and is now a fraction of a second -- reading
the check and hashing the assembly. A cancelled check keeps nothing.

What `assembly_sequence_smoke` holds it to: a pin through a lug and a fork
passes last, and put in before the fork the fork fails, blocked by the
pin's head -- an earlier part, not its mate partner; a bearing cap slid
along its bore is stopped by the shaft's collars, and lowered along its
bolts goes home; a missed occurrence fails the sequence by name. Two
wheels whose disks overlap in plan at two heights each block the other
one at a time and go home as a group along their common axis, the plate's
sections reused from the first wheel's step; a group with a member seated
along another axis is refused; a cap on a crank's pin under a bar fails
with the crank at rest and goes home after a pose step turns it; a pose
of a mate with no such freedom is refused; groups and poses survive
saving and opening again. A second check of the same is reused and answers
the same, the requirement after it too; an occurrence added, the order
changed, or the pin's head cut away each checks again -- the last now
passing -- and the occurrence removed again is the assembly answered
before.

### Playing and filming a sequence

A sequence is shown going together: in the Motion window (**Play**, a
**Time** scrub, **Stop**, under the studies) and as a video,
`export_assembly_video` -- `export_motion_video`'s request, job (kind
`motion-video`, `motion_video_status`) and answer, with the sequence's
timeline as what is filmed.

- **What it shows comes from the check.** Play and the export run
  `check_assembly_sequence` (on a worker; the panel shows it as a bar with
  Cancel, the export inside its job) and lay the timeline out from its
  answer: each step's line (`path.origin_mm`), its way home
  (`path.moves`, the side the check found it goes on from) and its
  distance from clear (`travel_mm`) -- so the film and the check cannot
  disagree. A sequence that does not pass is **refused**, naming its first
  failing step and what it was forced against, or what no step places.
  The timeline is kept while the assembly and its parts are unchanged
  (their versions), so Play again or an export after Play does not check
  again; an edit ends playback. A Play after an edit undone, or after
  `check_assembly_sequence`, is answered from the check's kept answer
  ([a check answered again](#a-check-answered-again)).
- **The timeline** (`core/SequenceTimeline.h`). Each step's occurrences
  appear at its turn, stand where they start for 0.35 s, travel home in a
  straight line along the path and stay there; the base is put in place.
  A part not yet put in is hidden: its mesh, its outlines and its section
  caps (and so its shadow and its ambient occlusion, which draw the
  visible set). A step's
  `turn_deg` is turned over the path, right hand about the way it moves,
  from where it starts to its seat; a step going on by its `thread` turns
  a turn a pitch (its modelled thread's pitch and hand, else any thread
  its part carries) -- the check's own rule for a modelled thread. One
  speed for the whole film, from the assembly's size: its bounds'
  diagonal in 3 s, a move at least 0.4 s so a short press is still seen,
  a thread at most 3 turns a second; 1.5 s home at the end. A turned try
  the check found (`rotation_deg`) is not shown: the part goes on as
  placed. A **group** moves as one, rigidly. A **pose** step is solved
  on a copy of the assembly the way the check solves it (the same 10° /
  2 mm strides, poses accumulating), every placement kept at each stride,
  and played by blending between them (positions lerped, orientations
  slerped); it takes as long as its largest turn of a part already shown
  at 120° a second, or its largest slide at the one speed, and no time
  when it moves nothing shown yet. Every later step goes home to where the
  poses so far put its seat, as the check checked it there.
- **Display-only**, as motion playback is: `DisplayMatrix` and the
  renderer sync read the pose (`SequencePlaybackWorld`,
  `SequencePlaybackHides`), the solved placements never move, Stop puts
  the display back, and the two playbacks stop each other. Play stops at
  the end, put together (an assembly does not go round as a mechanism
  does); Play again starts from the base.
- **In section.** Section caps are cut at a resync and baked in world
  space, and a playback moves parts without one, so after each pose both
  playbacks call `FollowPlaybackCaps`: a leaf's caps move with it when the
  move keeps every section plane (a piston down a bore the cut runs
  along), are cut again where it now stands when it does not (the crank's
  half turn across the cut; the cap cache makes a repeated pose cheap), and
  a part not yet put in has none.
- **The moving part highlighted.** From appearing to seated, a step's
  occurrences are drawn in the selection's amber (every mesh and cap of
  them), and in their own colour once home; a pose step lights what its
  solve moves of what is shown (`sequence::MovingAt`). Part of the
  selection projection while Play shows it, and of each frame of the film,
  whose capture otherwise draws no tint. On by default: the Motion
  window's "Highlight the moving part", `highlight: false` on the export.
- **Pace.** A speed, a ratio of the timeline's own (the Motion window's
  Speed, 0.25x to 8x; `speed` on the export, which then takes that many
  times fewer frames). The timeline's rule -- one speed from the
  assembly's size -- is unchanged; the pace only plays it faster or
  slower. Play runs on the interface's clock: the wall clock, except
  while a demo scene records, when it is 1/60 s a frame.
- **The film** is the motion video's capture, encoder and job
  ([motion video](#motion-video)) with a different pose source; its length
  is the timeline's at the fps and pace asked, known once the check has run (an
  async answer says `frames: null` until then).

What holds it: `test_sequence_timeline.cpp` (hidden before its step and
home after, half way at its midpoint, a group moved rigidly, a thread's
turn a turn a pitch, the speed scaling with the size, a pose reaching its
value with a later part going home to the posed seat, a pose of nothing
shown taking no time, what is moving at each moment);
`assembly_video_smoke.py` (a pin films appearing after the lug alone,
held at its start, travelling down the picture and staying home; the
assembly unmoved; a sequence that does not check refused naming the
fork; a crank put in turned a quarter, turned on to a half by a pose and
capped, whose film ends pixel for pixel where the film of the assembly
solved at the half turn ends; the moving pin redder than the same film
with `highlight: false` and home in its own colour, `speed: 2` half the
frames; in section, the first frame the lug alone, as a film of the lug
alone has it, the pin held at its start the screenshot of it put there,
and the last frame the screenshot of the assembly); the GUI test
`assembly/a_sequence_plays_and_stops` (lit on its way, played on at the
panel's speed it stops at the end, put together). The demo's `assembly`
scene plays the printed engine's (tools/demo/README.md#the-assembly-scene). The clock films both stages with
`--video` ([clock](../../generators/clock/README.md)).

## Exploded views

Named, persistent explode states: one world-space offset per occurrence,
scaled by a transient factor (0..1). **Pure view state** — applied to the
rendered instance transform only, never to occurrence transforms, mates or the
solve, so a saved exploded view cannot corrupt placements. A top-level
offset moves its whole subtree; a moving part of a flexible placement may have
a step of its own, by path (v130,
[flexible sub-assemblies](#flexible-sub-assemblies)).

With a view active the gizmo shows translate arrows at the displayed position
and writes the step's offset (a fixed occurrence explodes too). Auto-explode
seeds offsets radially from the assembly centroid, scaled by its size, spacing
sub-assemblies by their subtree bounds. The active view and factor are
session state (`set_explode`), not saved.

## Bill of materials

Derived every time, never stored. `ComputeBom` (`src/ui/Bom.cpp`) groups the
flattened leaf parts by part, configuration and material -- a pawn and a
rook of one piece part are two lines, named `piece (pawn)` -- so a
sub-assembly placed twice doubles its parts' quantities; hidden or suppressed occurrences (at any level) are left
out. A row carries `configuration` (its name, `""` for the part's active
one) and `configuration_id`, quantity, material, unit and total mass (null
when a body has no density), and for a generated standard part its
`standard` and `designation`; the CSV has a `configuration` column after
`part`. The per-unit BOM of a sub-assembly is the same function on its
own tab. `bom` / `export_bom {path}` (CSV); in the GUI a floating window from
the assembly toolbar. `assembly::MassProperties` aggregates mass, volume and
world centre of mass by the same visibility rule.

## In-context editing

Double-clicking an occurrence (or "Edit in context", or `edit_in_context`)
switches to the part's tab with the rest of the assembly drawn around it as
ghosts. The part stays at identity and each ghost is placed at
`OccMatrix(edited)⁻¹ · OccMatrix(other)`, so no part tooling (sketch planes,
picking, previews, sections) has to know about the assembly. Ghosts come from
the flattened tree, are alpha-blended, unpickable and outline-free, and the
context re-solves after each part regeneration so mated ghosts track live.
Activating any other tab exits; the edits were ordinary part edits with
ordinary undo. Referencing assembly geometry from the part is not supported.

## Fasteners

`add_fastener` / `fit_fastener` place standard hardware as ordinary
occurrences of generated parts (one part per distinct spec, named by its BOM
designation). The screw, nut and washers ride on the parts they sit on
(attachments above), following the holes they sit in. `fit_fastener` also
adds a fastened mate across every interface the bolt spans, once per bolt, anchored at that bolt's hole, with
the DOFs read off where the parts already are so the solve moves nothing; the
extra mates of a bolt pattern are redundant-but-consistent loop mates
([solver.md](solver.md#closing-loops)). `check_fasteners` re-derives every
placed fastener against its current stack. The standards and sizing are in
[../parts/README.md](../parts/README.md).

## Clearance assertions

A declared clearance between two top-level occurrences is a project
requirement ([requirements](../requirements/README.md#clearance-assertions)):
`add_assertion {name, a, b, min_mm, mode?, why?}` states one on the active
assembly, `verify_assertions` measures them (in mesh mode the tessellation
tolerance is subtracted from the measured gap), `list_assertions` reports
without measuring (`stale` when either occurrence, or what it places, has
changed since), and an assembly saved with `assertions/` loads them as
requirements.

## Clash sweeps: what a check costs

- `Measure::ContactBetween(..., wantDistance)` runs an exact boolean (overlap
  volume) and, only when asked, an exact distance pass. A motion study passes
  `wantDistance = false`; the reply then carries `distanceKnown = false`
  instead of a 0.0 that would read as contact.
- A distance test cannot replace the boolean: one solid entirely inside
  another has its boundaries far apart.
- Parallelism is across pairs: `motion::Run` uses `parallel::For`; `RunSweep`
  (`RemoteInspection.cpp`) runs its own pool sized by `parallel::Workers()`.
  Both run exact checks on `cad::BooleanWorkers` threads: half the machine
  where the kernel queues its booleans on one lock
  ([performance](../performance/README.md#the-kernels-error-handler-lock)).
  A motion run solves every sample first (in order: each solve starts from
  the last) and then checks EVERY sample's candidate pairs in one batch, so
  a pair that costs seconds at every pose -- a worm against its wheel --
  runs on all the cores at once instead of one sample's check waiting for
  the last one's. The pan/tilt head's exact pan study (48 samples, 1294
  pair checks) went from 144 s to 54 s by that alone. A cancel during the
  batch keeps the leading samples whose every check landed; one during the
  solves keeps none (a sample is finished when it is checked), except on a
  kinematics-only run, whose sample is its solve.
- A pair at a relative pose an earlier sample of the same run already found
  CLEAR (the same two bodies, B in A's frame equal to within 1e-9 per matrix
  entry: a dwell, a full turn come round) takes that answer instead of a
  second boolean (`pairs_repeated`); a clash is asked again, since where it
  is is in world space.
  Shapes are warmed single-threaded first, and every query boolean is
  non-destructive ([inspect](../inspect/README.md#query-booleans-are-non-destructive)).
- Pairs are box-pruned before the exact check, and pairs rigid to each other
  across the motion are checked once, at the first sample, then skipped
  (`pairsRigidChecked`, `pairsRigid`).
- Exact queries on spline-heavy bought parts (imported STEP motors, drivers)
  dominate sweep time.
- SDK: `Asm.check_clashes()` (`python/lobole/sdk.py`) is the whole-assembly,
  exact, volume-only gate (`interference`; refuses mesh mode or a named-pair
  restriction). `Asm.check()` is the contact, connectivity and near-miss report
  (`contact_report`, `near_tol` default 1.0). `interference` takes
  `mode: exact|mesh`; the motion RPCs call it `clash_mode`.

## Exact contact reuse across motion runs

`collide::ExactContactCache` (`src/core/CollisionCache.h`) is owned by
`AppState::motionContacts` and passed to the motion runner. It holds up to
50,000 exact pair results, FIFO eviction, keyed on both geometry identities,
both placement ids and the exact bits of both world matrices — no pose
rounding. Only successful answers are cached, clashes included; an unknown or
failed boolean is not clearance. A CLEAR answer is also found by the pair's
relative pose, to within 1e-9 per matrix entry (OpenCASCADE's confusion
distance is 1e-7 mm), so two bodies carried together, or a `recompute` whose
solve landed a hair away from the last run's, is a hit; a clash is matched
only at its exact world poses. Motion replies separate `pairs_cached` and
`pairs_repeated` from `pairs_executed`. The cache is not serialized.

## Background motion copies without regenerating

A background motion run copies every document under the assembly
(`ui::CopySubtree`) so the worker shares nothing with the tab -- each (part,
configuration) placed once (`assembly::SubtreePartConfigurations`), however
many occurrences place it. The copy is
`Document::CopyWithGeometry`, not the plain copy constructor, which ends in
`Regenerate()`: it hands the copy the source's bodies as baked geometry, and
the tip-hash comparison an opened project uses decides whether they still
describe the feature list. It returns `std::unique_ptr<Document>` on purpose —
`Document` declares a copy constructor, so it has no implicit move, and
`make_unique<Document>(CopyWithGeometry(...))` would run the full-regenerating
copy on top. Identities survive the copy, so background runs can hit
`ExactContactCache`. `tests/test_document_copy.cpp` asserts identical saved
bytes and `LastRegenStats().executed == 0`. The GUI panel shows the run's
fraction and a Cancel (`OccurrencePanel.cpp`); `motion_status` reports the
running job's id and fraction.

## Snap points while hovering

Hovering the mate tool resolves a face's snap points with one call,
`CADShape::FaceSnapPoints`, which walks the body's edges once; three separate
per-edge lookups would cost O(face edges × body edges) every frame.
`FaceSnapPoints stays linear on a drilled plate` gates it.

## Sections during motion

With a section on, scrubbing a motion study re-caps only the parts that moved:
`Measure::SectionCapCache` keys on `PlacementId` plus the planes. A part that
moves still pays an OCC boolean per step.

## Deriving a mate from the placement

A fastened mate between two frames has eight outcomes (which side, which
quarter turn), and none is predictable from the call, so without help a
caller can only try them and test the result. `add_mate {derive}` (fastened
mates only) reads the numbers off the parts' current placement instead
(`assembly::DeriveMateDofs`, `ui::DeriveMateFromPlacement`), so a mate added
to parts already where they belong keeps them there: `"twist"` reads
`flipped` and `rotation` (anchors coincident), `"pose"` all five DOFs, holding
the parts exactly where they are. It refuses frames that are not square to
one axis, and a refused derive leaves no mate behind.

## Undo

An assembly tab has its own snapshot undo (`AssemblyHistory`, full copies of
the `AssemblyDocument`): `SnapshotAssembly` before a mutation, `undo`/`redo`
and Ctrl+Z route to it on an assembly tab. Everything above — mates,
relations (including a freewheel's `lead`), views, studies — lives in the
document and is snapshotted with it. A gizmo drag or a panel drag is one undo
step.

## GUI

- **Assembly toolbar** (`src/ui/Toolbar.cpp`, commands in
  `src/ui/Commands.cpp`): Insert (part or sub-assembly), one button per mate
  type (`###tb_mate_*`), Fastener, Exploded view, Motion study, Simulation,
  BOM — the viewport modes open a floating tool window, one at a time.
- **Mate tool** (`src/ui/MateActions.cpp`): pick a face on each occurrence;
  hovering shows the snap candidate a click would take (face centre, hole
  centre, edge midpoint, corner — nearest wins) and the toolbar names it. After
  the second pick the mate previews live; Accept, Flip, Rotate (all but ball)
  or Cancel. An unnamed face cannot be mated.
- **Occurrence panel** (`src/ui/OccurrencePanel.cpp`): occurrences, mates
  (status, Flip, Rotate 90°, DOF fields, re-pick connector), relations (ratio,
  phase, a freewheel's `lead` read-only; created over RPC only, since it needs
  two mates picked), right-click a row to open what it references.
- **Gizmo** (`src/ui/GizmoInput.cpp`): on a selected, non-fixed, non-riding
  occurrence. A free occurrence gets world translate arrows and rotate arcs; a
  mated one only its mate's free DOF, and the drag writes the mate's DOF and
  re-solves (drag-within-mate). Snapshot at grab, cheap per-frame instance
  transform updates, solve at release. Rotation snaps to 45° within 5° unless
  Ctrl is held — the absolute mate angle for a mate, the turn for a free
  occurrence or a ball mate; translation does not snap.

## RPCs

In `src/remote/RemoteAssembly.cpp` and `src/remote/RemoteMotion.cpp`; the
`describe` rows there are the reference for parameters.

- Tabs: `add_assembly`, `add_part`, `list_parts`, `set_active_part`,
  `delete_part`.
- Occurrences: `add_occurrence`, `remove_occurrence`, `list_occurrences`
  (`bounds: true` adds world bboxes), `set_occurrence_transform` (orientation
  as a quaternion or `rotate {axis, angle_deg}`), `set_occurrence_visible`,
  `set_occurrence_fixed`, `rename_occurrence`,
  `set_occurrence_configuration`, `set_occurrence_material`,
  `set_occurrence_flexible`, `select_occurrence`. `add_occurrence` takes
  `configuration` and `material` too.
- Mates: `add_mate` (`derive: twist|pose` reads a fastened mate's numbers
  off the current placement), `set_mate`, `remove_mate`, `list_mates`,
  `select_mate`, `solve_assembly`. On the wire a mate's `type` is its kind
  (`revolute`, `slider`, …), the word `add_mate` takes; the file stores the
  same kind as `mate_type` ([format](../versioning/format.md)).
- Relations: `add_relation`, `set_relation`, `remove_relation`,
  `list_relations`.
- Motion: `add_motion_study`, `set_motion_study`, `remove_motion_study`,
  `list_motion_studies`, `run_motion_study`, `motion_status`,
  `set_motion_pose`, `export_motion`, `export_motion_video`,
  `motion_video_status`.
- Exploded views: `add_exploded_view`, `remove_exploded_view`,
  `list_exploded_views`, `set_explode_step`, `auto_explode`, `set_explode`.
- Sequences: `set_assembly_sequence`, `get_assembly_sequence`;
  `check_assembly_sequence` is in `src/remote/RemoteInspection.cpp`;
  `export_assembly_video` in `src/remote/RemoteMotion.cpp`.
- BOM and hardware: `bom`, `export_bom`, `add_fastener`, `fit_fastener`,
  `check_fasteners`.
- Clearances: `add_assertion`, `list_assertions`, `remove_assertion`,
  `verify_assertions` (requirements, in
  `src/remote/RemoteRequirements.cpp`).
- `edit_in_context`.

The Python SDK (`python/lobole/sdk.py`) wraps these on `Asm`: `gear`,
`chain`, `rack`, `screw`, `cam`, `freewheel`, `click`, `motion`, `check`
(connectivity, declared clearances and clashes, with an `allow` map of exact
pairs and volume ceilings for intended contact). `Occurrence.frame(name)`
reads where a part's named frame sits in the world; `Part.frame(...)` creates
one.

## Code

| path | what |
|---|---|
| `src/core/Occurrence.{h,cpp}` | the model: occurrences, connectors, mates, relations, views, studies, the clearance assertions an old file carries; `CamLift` |
| `src/core/AssemblySolve.{h,cpp}` | connector frames, relation propagation, the placement pass, attachments, `Flatten`, `ResolvePath`, `WouldCycle`, `SubtreeVersions`, `MassProperties` |
| `src/core/AssemblyFlexible.cpp` | flexible sub-assemblies: `ExpandFlexible`, `CollapseFlexible`, `FlexibleRefusal`, mates named `{path, mate}` |
| `src/core/AssemblyLoopSolve.{h,cpp}` | the loop solve; `MateAdjustFor`, `DeriveMateDofs` |
| `src/core/MotionStudy.{h,cpp}` | running a motion study |
| `src/core/SequenceTimeline.{h,cpp}` | an assembly sequence's timeline: each step's parts in time along their path |
| `src/core/io/SerializeAssembly.cpp`, `Serialization.cpp` | JSON and the directory layout |
| `src/ui/AssemblyActions.{h,cpp}` | solve/undo/selection glue, staleness, in-context entry, explode |
| `src/ui/MateActions.cpp`, `GizmoInput.cpp`, `OccurrencePanel.cpp`, `Bom.cpp`, `MotionActions.cpp`, `MotionVideo.cpp`, `FastenerActions.cpp` | GUI tools |
| `src/ui/SequencePlayback.{h,cpp}` | a sequence's timeline resolved from its check, played (the Motion window's section) and filmed |
| `src/ui/FlexibleActions.{h,cpp}` | flexible placements from the panel and the RPC alike: Flexible, the configuration, inner mates as choices, a moving part as a selection |
| `src/ui/RendererSync.cpp` | instanced assembly render sync, in-context ghosts |
| `src/remote/RemoteAssembly.cpp`, `RemoteMotion.cpp` | RPCs |

## Tests

- Unit (`tools/ctest -L assembly`, `-L motion`): `test_assembly.cpp` (model,
  serialization), `test_assembly_solve.cpp` (mate types, offset sign, the
  part-edit-moves-the-mate payoff, dangling refs), `test_assembly_loops.cpp`
  (four-bar DOF, branch keeping, redundant bolts, conflicting loops,
  partitioning, driven DOF held, which tilted connector keeps a loop open —
  `-L diagnose`), `test_assembly_relations.cpp` (every
  relation kind, propagation order, freewheel/reverser/click, loop rows),
  `test_assembly_flatten.cpp`, `test_motion.cpp`,
  `test_flexible_subassembly.cpp` (a parent relation into an inner mate,
  two placements posed apart, a configuration per placement, clashes per
  leaf, the refusals). Goldens in
  `tests/golden/assembly/` (`v135-part-configurations.json`: part
  placements in a configuration and a material).
- Headless smokes: `assembly_smoke.py`, `assembly_mates_smoke.py`,
  `assembly_loops_smoke.py`, `assembly_relations_smoke.py`,
  `assembly_propagation_smoke.py`, `nested_assembly_smoke.py`,
  `bom_smoke.py`, `exploded_smoke.py`, `motion_smoke.py`,
  `study_jobs_smoke.py` (job ids, a run cancelled with its client, reuse,
  `leave_at` in one solve, and a later `leave_at` reused),
  `fastener_stack_smoke.py`, `drawing_assembly_smoke.py`,
  `flexible_subassembly_smoke.py`, `part_configuration_smoke.py` (one part
  placed in two configurations and two materials: BOM lines, mass, STEP
  products, a job part in a configuration, the refusals).
  `motion_video_smoke.py` (renderer, label
  `pygui`): every video format read back by ffprobe, the refusals including
  a PATH without ffmpeg, the pose put back, an async cancel;
  `assembly_video_smoke.py` (the same label): a sequence filmed.
- GUI (`src/app/guitests/GuiTestsAssembly.cpp`, category `assembly`): mate
  hover marker and snaps, re-pick connector, mate tool rotate, sub-assembly
  insert/open/drag, relation and mate rows, BOM through a sub-assembly, motion
  scrub, a sequence played and stopped, and `motion_video_frames_are_the_view` (a filmed frame is the
  viewport, not the window). `GuiTestsFlexible.cpp` (category `flexible`):
  tick Flexible (and the refusal for a unit with nothing fixed), pick a
  configuration, a relation side and a study driver inside the unit and run
  the study, click and drag the crank within its mate, explode it on its
  own; each undone with Ctrl+Z.

## Not built

- Assembly-level features (a cut through several parts at assembly time):
  it would need per-occurrence geometry, which instancing and the
  feature-less assembly document do not have.
- References from a part to assembly geometry while editing in context.
- Mate limits (min/max on a DOF).
- Editing into a sub-assembly from its parent.
- A per-occurrence appearance override (a placement's `material` exists).
- A gear relation's phase derived from the gears' geometry; creating the
  kinds other than gear, internal gear, linear, epicyclic and a Geneva carry
  from the GUI.
- Contact dynamics: gears and cams are kinematic relations, checked for
  clash statically and over motion studies.
- A version pin on an occurrence's part (`part_version`).
- More than one level of flexibility; a GUI drag of a flexible unit's
  planar or ball mates (`set_mate` with a path poses a turn and a slide
  only).
