# Assemblies

An assembly is a document that places parts (and other assemblies) in space
and constrains them to each other. It sits in the same project as the parts it
places, as another tab.

Two topic pages carry the detail:

- [solver.md](solver.md) — connector frames, the mate types and their
  maths, the placement pass, and the numeric pass that closes mate loops.
- [relations.md](relations.md) — couplings between two mates' degrees of
  freedom: gear, chain, rack, screw, linear, cam, freewheel, click.

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
| relations | couplings between two mates' DOFs ([relations.md](relations.md)) |
| exploded views | named display offsets |
| motion studies | one mate DOF driven over a range, clash-checked |
| clearance assertions | declared minimum gaps, re-checked on demand |
| configurations | occurrence/mate/relation suppression and held mate DOFs ([../configurations/README.md](../configurations/README.md)) |
| FEM studies | studies on the assembly ([../simulation/README.md](../simulation/README.md)) |

An assembly is its own document kind, not a feature list: a `Feature`
regenerates from earlier features of the *same* document, and an occurrence
references another document. So an assembly regenerates by a placement pass
(the solve), not by executing features.

On disk it shares the one `designer-document` format and `kVersion` with parts
(discriminated by `kind: "assembly"`), one file per item under
`occurrences/`, `mates/`, `exploded/`, `relations/`, `assertions/`, `motion/`
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
`attach_offset` / `attach_rotation` in the parent's frame). This is how placed
fasteners follow their plate. It is a composition, not a mate: no solver sees
it, `ApplyAttachments` recomputes the rider's `position`/`orientation` after
the solve, it goes one level deep only, and the gizmo will not drag a rider
(its pose is derived). Deleting an occurrence deletes what rides on it.

Rendering is instanced: every occurrence of a part draws the part's one
tessellation with its own transform.

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
  one assembly can differ. Per-occurrence *part* configurations are not
  supported (a part configuration changes geometry).
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
live playback moves each one. A clearance assertion's key hashes the inner
poses. An exploded view moves the unit by its own step, and a moving part by
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

Units on the wire: a mate's `rotation` is **radians**; `rotation_deg` is the
same field in degrees (giving both is refused). `offset`, `dx`, `dy` are mm.

**Re-solve triggers**, all through `ui::SolveActiveAssembly`: any mate or
occurrence edit, opening a project, activating an assembly tab whose parts
changed (`AssemblyPartsStale` compares per-document version stamps taken at
the last solve, so this works headless too), and every part regeneration
while editing in context. A mated, non-fixed occurrence's own transform is
overwritten by the next solve.

## Relations

A relation couples two mates' degrees of freedom — `dof_b = ratio · dof_a +
phase` for gear, chain, rack, screw and linear; a lift table for a cam; a
one-way drive for a freewheel; a pawl for a click. In a mate tree it is
enforced by propagation before placement; inside a loop it is a residual row
in the loop solve. Dragging or `set_mate` on either mate moves the other. See
[relations.md](relations.md).

## Motion studies

A `MotionStudy` drives ONE mate DOF from `from` to `to` in `samples` steps
(default 48), solves at each sample, and checks every occurrence pair for a
clash (`clash_mode` `mesh`, the default, or `exact`). The definition is saved;
samples and the collision table are results, not document state. A study may
name the configuration it runs under (`configuration`: an id, 0 = base,
omitted = whichever is active).

- The range is stored in the DOF's own unit (radians / mm); the RPC takes
  degrees for a rotation and converts once.
- Samples are walked in order, each solve starting from the last, so a closed
  loop stays on the branch it is on — and the first sample is WALKED to from
  wherever the document has the driver, at the study's own spacing, as the
  scrub walks: a jump from a crank left at 180 degrees to 0 landed a
  slider-crank on its mirrored branch, and the whole run followed it there.
  A sample that does not solve is reported unsolved with its residual, never
  drawn in the last good pose.
- Pairs rigid to each other over the whole study — fastened, or carried by
  the same moving part — cannot be made to collide by the motion, so they are
  checked once, at the first sample, and skipped at every later one. A
  kinematics-only pass over the samples decides which pairs move (the
  relative transform of two top-level occurrences changes at some sample).
  The rigid check is always EXACT, whatever `clash_mode` says: touching is
  what a bolted joint does, and a mesh check calls touching a clash. A clash
  found there is a standing clash: reported at sample 0 with `rigid: true`,
  listed again in `rigid_clashes`, left out of `seated_contacts` and
  `appeared`, and the study is not clean. The result says where the rigid
  pairs were checked (`rigid_checked_at_sample`), how many body pairs that
  cost (`pairs_rigid_checked`) and how many later checks were skipped
  (`pairs_rigid`); the table's last line says the same.
- Every clash says where it is, in the names `interference` uses: an exact
  check gives the overlap's `volume`, `centroid_mm` and
  `bbox_min_mm`/`bbox_max_mm`; a mesh check knows only that two bodies
  overlap and gives `within_min_mm`/`within_max_mm`, the region their boxes
  share.
- `run_motion_study` runs on a copy of the assembly, so the document is left
  where it was; `leave_at` (`"start"`, `"last"` or a sample index) scrubs the
  live assembly to a sample afterwards so the far end of the travel can be
  measured. `leave_at_deg` (a rotation study) or `leave_at_mm` (an offset
  study) leaves it at a driver VALUE between samples instead: it walks to the
  nearest sample as the scrub does and takes one step from there, so a
  slider-crank lands on the branch the run found. A value outside the
  study's range, the other unit, or `leave_at` beside it is refused.
  `set_motion_pose {t}` scrubs (no undo step). `include_poses`
  returns every occurrence's placement and every moving mate's value per
  sample. `async: true` runs it as a background job.
- `export_motion` writes an animated GLB (metres, Y-up, 24 fps) and refuses
  when any sample is unsolved.

Code: `src/core/MotionStudy.{h,cpp}`, `src/remote/RemoteMotion.cpp`,
`src/ui/MotionActions.cpp`.

### Motion video

`export_motion_video {study, path, format?, width?, height?, fps?, frames?,
quality?, async?}` films a study: the file to put in a README, send to
someone or attach to an issue.

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
flattened leaf parts by part id, so a sub-assembly placed twice doubles its
parts' quantities; hidden or suppressed occurrences (at any level) are left
out. A row carries quantity, material, unit and total mass (null when a body
has no density), and for a generated standard part its `standard` and
`designation`. The per-unit BOM of a sub-assembly is the same function on its
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
(attachments above). `fit_fastener` also adds a fastened mate across every
interface the bolt spans, once per bolt, anchored at that bolt's hole, with
the DOFs read off where the parts already are so the solve moves nothing; the
extra mates of a bolt pattern are redundant-but-consistent loop mates
([solver.md](solver.md#closing-loops)). `check_fasteners` re-derives every
placed fastener against its current stack. The standards and sizing are in
[../parts/README.md](../parts/README.md).

## Clearance assertions

`add_assertion {name, a, b, min_mm, mode?, why?}` declares a minimum gap
between two top-level occurrences (by id), held in the document.
`verify_assertions` re-checks and records the verdict (`holds`, `violated`,
`unverifiable` when an occurrence no longer resolves); in mesh mode the
tessellation tolerance is subtracted from the measured gap. `list_assertions`
reports without re-checking, and reads `stale` when the placed geometry of the
two subtrees has changed since (a content hash, not the assembly's version
counter, so an unrelated edit or a reload does not make every assertion
stale).

## Clash sweeps: what a check costs

- `Measure::ContactBetween(..., wantDistance)` runs an exact boolean (overlap
  volume) and, only when asked, an exact distance pass. A motion study passes
  `wantDistance = false`; the reply then carries `distanceKnown = false`
  instead of a 0.0 that would read as contact.
- A distance test cannot replace the boolean: one solid entirely inside
  another has its boundaries far apart.
- Parallelism is across pairs: `motion::Run` uses `parallel::For`; `RunSweep`
  (`RemoteInspection.cpp`) runs its own pool sized by `parallel::Workers()`.
  Shapes are warmed single-threaded first, and every query boolean is
  non-destructive ([inspect](../inspect/README.md#query-booleans-are-non-destructive)).
- Pairs are box-pruned before the exact check, and pairs rigid to each other
  across the motion are checked once, at the first sample, then skipped
  (`pairsRigidChecked`, `pairsRigid`).
- Exact queries on spline-heavy bought parts (imported STEP motors, drivers)
  dominate sweep time.
- SDK: `Asm.check_clashes()` (`tools/designer_sdk.py`) is the whole-assembly,
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
failed boolean is not clearance. Motion replies separate `pairs_cached` from
`pairs_executed`. The cache is not serialized.

## Background motion copies without regenerating

A background motion run copies every document under the assembly
(`ui::CopySubtree`) so the worker shares nothing with the tab. The copy is
`Document::CopyWithGeometry`, not the plain copy constructor, which ends in
`Regenerate()`: it hands the copy the source's bodies as baked geometry, and
the tip-hash comparison an opened project uses decides whether they still
describe the feature list. It returns `std::unique_ptr<Document>` on purpose —
`Document` declares a copy constructor, so it has no implicit move, and
`make_unique<Document>(CopyWithGeometry(...))` would run the full-regenerating
copy on top. Identities survive the copy, so background runs can hit
`ExactContactCache`. `tests/test_document_copy.cpp` asserts identical saved
bytes and `LastRegenStats().executed == 0`. The GUI panel shows the run's
fraction and a Cancel (`OccurrencePanel.cpp`); `motion_status` reports no
fraction.

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
  `set_occurrence_configuration`, `set_occurrence_flexible`,
  `select_occurrence`.
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
- BOM and hardware: `bom`, `export_bom`, `add_fastener`, `fit_fastener`,
  `check_fasteners`.
- Clearances: `add_assertion`, `list_assertions`, `remove_assertion`,
  `verify_assertions`.
- `edit_in_context`.

The Python SDK (`tools/designer_sdk.py`) wraps these on `Asm`: `gear`,
`chain`, `rack`, `screw`, `cam`, `freewheel`, `click`, `motion`, `check`
(connectivity, declared clearances and clashes, with an `allow` map of exact
pairs and volume ceilings for intended contact). `Occurrence.frame(name)`
reads where a part's named frame sits in the world; `Part.frame(...)` creates
one.

## Code

| path | what |
|---|---|
| `src/core/Occurrence.{h,cpp}` | the model: occurrences, connectors, mates, relations, views, studies, assertions; `CamLift` |
| `src/core/AssemblySolve.{h,cpp}` | connector frames, relation propagation, the placement pass, attachments, `Flatten`, `ResolvePath`, `WouldCycle`, `SubtreeVersions`, `MassProperties` |
| `src/core/AssemblyFlexible.cpp` | flexible sub-assemblies: `ExpandFlexible`, `CollapseFlexible`, `FlexibleRefusal`, mates named `{path, mate}` |
| `src/core/AssemblyLoopSolve.{h,cpp}` | the loop solve; `MateAdjustFor`, `DeriveMateDofs` |
| `src/core/MotionStudy.{h,cpp}` | running a motion study |
| `src/core/io/SerializeAssembly.cpp`, `Serialization.cpp` | JSON and the directory layout |
| `src/ui/AssemblyActions.{h,cpp}` | solve/undo/selection glue, staleness, in-context entry, explode |
| `src/ui/MateActions.cpp`, `GizmoInput.cpp`, `OccurrencePanel.cpp`, `Bom.cpp`, `MotionActions.cpp`, `MotionVideo.cpp`, `FastenerActions.cpp` | GUI tools |
| `src/ui/FlexibleActions.{h,cpp}` | flexible placements from the panel and the RPC alike: Flexible, the configuration, inner mates as choices, a moving part as a selection |
| `src/ui/RendererSync.cpp` | instanced assembly render sync, in-context ghosts |
| `src/remote/RemoteAssembly.cpp`, `RemoteMotion.cpp` | RPCs |

## Tests

- Unit (`ctest -L assembly`, `-L motion`): `test_assembly.cpp` (model,
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
  `tests/golden/assembly/`.
- Headless smokes: `assembly_smoke.py`, `assembly_mates_smoke.py`,
  `assembly_loops_smoke.py`, `assembly_relations_smoke.py`,
  `assembly_propagation_smoke.py`, `nested_assembly_smoke.py`,
  `bom_smoke.py`, `exploded_smoke.py`, `motion_smoke.py`,
  `fastener_stack_smoke.py`, `drawing_assembly_smoke.py`,
  `flexible_subassembly_smoke.py`. `motion_video_smoke.py` (renderer, label
  `pygui`): every video format read back by ffprobe, the refusals including
  a PATH without ffmpeg, the pose put back, an async cancel.
- GUI (`src/app/guitests/GuiTestsAssembly.cpp`, category `assembly`): mate
  hover marker and snaps, re-pick connector, mate tool rotate, sub-assembly
  insert/open/drag, relation and mate rows, BOM through a sub-assembly, motion
  scrub, and `motion_video_frames_are_the_view` (a filmed frame is the
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
- Per-occurrence part configurations; editing into a sub-assembly from its
  parent.
- Creating a relation from the GUI; a gear relation's phase derived from the
  gears' geometry.
- Contact dynamics: gears and cams are kinematic relations, checked for
  clash statically and over motion studies.
- A version pin on an occurrence's part (`part_version`).
- More than one level of flexibility; a GUI drag of a flexible unit's
  planar or ball mates (`set_mate` with a path poses a turn and a slide
  only).
