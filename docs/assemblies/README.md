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

An occurrence may place another assembly. Sub-assemblies are **rigid**: a
placed assembly is one solid unit at whatever placements its own document
stores, and the parent's solve never recurses into it. Mates, exploded views
and the drag gizmo operate between top-level occurrences.

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
- A mate may anchor on a face *inside* a placed sub-assembly: the connector's
  `inner_path` names the occurrence chain down to the part. The mate still
  positions the top-level occurrence.
- Paths are read-only. `assembly::ResolvePath` takes an id chain (`"12/47"`)
  or an unambiguous name path; `interference`, `measure` and `frame` accept
  one, so a question can cross the boundary. Editing a sub-assembly's contents
  happens on its own tab.
- `add_occurrence` refuses a reference that would close a cycle
  (`WouldCycle`); `delete_part` refuses while any assembly places the
  document.
- Staleness is transitive: `SubtreeVersions` stamps every document version in
  the subtree, so an edit inside a sub-assembly re-solves and resyncs the top.

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
  loop stays on the branch it is on. A sample that does not solve is reported
  unsolved with its residual, never drawn in the last good pose.
- `run_motion_study` runs on a copy of the assembly, so the document is left
  where it was; `leave_at` (`"start"`, `"last"` or an index) scrubs the live
  assembly to a sample afterwards so the far end of the travel can be
  measured. `set_motion_pose {t}` scrubs (no undo step). `include_poses`
  returns every occurrence's placement and every moving mate's value per
  sample. `async: true` runs it as a background job.
- `export_motion` writes an animated GLB (metres, Y-up, 24 fps) and refuses
  when any sample is unsolved.

Code: `src/core/MotionStudy.{h,cpp}`, `src/remote/RemoteMotion.cpp`,
`src/ui/MotionActions.cpp`.

## Exploded views

Named, persistent explode states: one world-space offset per occurrence,
scaled by a transient factor (0..1). **Pure view state** — applied to the
rendered instance transform only, never to occurrence transforms, mates or the
solve, so a saved exploded view cannot corrupt placements. A top-level
offset moves its whole subtree.

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
  across the motion are skipped (`pairsRigid`).
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
  `set_occurrence_configuration`, `select_occurrence`.
- Mates: `add_mate` (`derive: twist|pose` reads a fastened mate's numbers
  off the current placement), `set_mate`, `remove_mate`, `list_mates`,
  `select_mate`, `solve_assembly`.
- Relations: `add_relation`, `set_relation`, `remove_relation`,
  `list_relations`.
- Motion: `add_motion_study`, `set_motion_study`, `remove_motion_study`,
  `list_motion_studies`, `run_motion_study`, `motion_status`,
  `set_motion_pose`, `export_motion`.
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
| `src/core/AssemblyLoopSolve.{h,cpp}` | the loop solve; `MateAdjustFor`, `DeriveMateDofs` |
| `src/core/MotionStudy.{h,cpp}` | running a motion study |
| `src/core/io/SerializeAssembly.cpp`, `Serialization.cpp` | JSON and the directory layout |
| `src/ui/AssemblyActions.{h,cpp}` | solve/undo/selection glue, staleness, in-context entry, explode |
| `src/ui/MateActions.cpp`, `GizmoInput.cpp`, `OccurrencePanel.cpp`, `Bom.cpp`, `MotionActions.cpp`, `FastenerActions.cpp` | GUI tools |
| `src/ui/RendererSync.cpp` | instanced assembly render sync, in-context ghosts |
| `src/remote/RemoteAssembly.cpp`, `RemoteMotion.cpp` | RPCs |

## Tests

- Unit (`ctest -L assembly`, `-L motion`): `test_assembly.cpp` (model,
  serialization), `test_assembly_solve.cpp` (mate types, offset sign, the
  part-edit-moves-the-mate payoff, dangling refs), `test_assembly_loops.cpp`
  (four-bar DOF, branch keeping, redundant bolts, conflicting loops,
  partitioning, driven DOF held), `test_assembly_relations.cpp` (every
  relation kind, propagation order, freewheel/reverser/click, loop rows),
  `test_assembly_flatten.cpp`, `test_motion.cpp`. Goldens in
  `tests/golden/assembly/`.
- Headless smokes: `assembly_smoke.py`, `assembly_mates_smoke.py`,
  `assembly_loops_smoke.py`, `assembly_relations_smoke.py`,
  `assembly_propagation_smoke.py`, `nested_assembly_smoke.py`,
  `bom_smoke.py`, `exploded_smoke.py`, `motion_smoke.py`,
  `fastener_stack_smoke.py`, `drawing_assembly_smoke.py`.
- GUI (`src/app/guitests/GuiTestsAssembly.cpp`, category `assembly`): mate
  hover marker and snaps, re-pick connector, mate tool rotate, sub-assembly
  insert/open/drag, relation and mate rows, BOM through a sub-assembly, motion
  scrub.

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
