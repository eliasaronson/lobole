# CAM

Milling toolpaths and G-code, inside the CAD. A **job** is a project document
that references parts, lays them out in stock and says how they are cut. It
owns no geometry: its operations name faces of the parts by stable element
reference (toponaming), so a model edit reaches the toolpath instead of
orphaning it. That is the reason to have CAM in the CAD rather than downstream
of a STEP export.

Scope: 2.5D milling (profile, pocket, adaptive clearing, face, drill, edge
break), 3-axis surface finishing and roughing, an indexed 4th axis, and
continuous 4-axis finishing on a rotary axis. Three post dialects: `mach3`,
`linuxcnc`, `grbl`. The posted file is verified by simulating it through a
height map (or, for a turning part, a radius map), and the same simulation is
what the viewport plays back.

Topic pages:

| page | what |
|---|---|
| [setup.md](setup.md) | machine profiles, setups, parts and placements, the fixturing frame, stock, work origin, safe Z, the work area, fixtures, cutting order, undo |
| [operations.md](operations.md) | operation types, how the verbs derive them, the 2.5D strategies, rough-then-finish, 3-axis surfacing, the capability and field tables |
| [rotary.md](rotary.md) | the indexed and the continuous 4th axis: patterns, feed compensation, A winding, the rotary verifier and picture |
| [post.md](post.md) | dialects, what the file says, work origin in the file, arcs, tool changes, clipping to the machine, file names, what to check at the machine |
| [simulation.md](simulation.md) | the verifier, the verdict, holder/fixture/travel checks, cutter findings, and the cutting simulation |
| [gui.md](gui.md) | the job tab: toolbar verbs, verb windows, the CAM panel, what the viewport draws, Generate, Simulate, gizmos, Preferences › Machines |

## The model

```
JobDocument                        core/cam/JobDocument.h
  parts[]    JobPart{id, sourceDocId, name}      a reference, never a copy
  setups[]   cam::Setup                          core/cam/CamJob.h
    machine          embedded copy of a Machine profile (post, arcs, rotary, travel, library)
    stock, originAt, origin (derived), reachOffset
    frame            how the part is clamped (setup -> model rotation)
    rotationA        the machine's rotary index (needs machine.hasRotaryAxis)
    topFace          the face Z zero was picked from (a Target)
    safeZ, rapidFeed
    placements[]     where each part sits in the stock: pos, rotZdeg, enabled, optional frame
    fixtures[]       clamps / vises / keep-out boxes in BED space, optional seat
    order, splitOutput
    tools[]          copied from the machine's library
    operations[]     cam::Operation{type, toolNumber, targets[], depths, per-type fields}
```

- A **job part** names a project part document by its stable id. A job part's
  source that is deleted is reported (`missing`), never substituted.
- A **target** is `cam::Target{jobPartId, ElementRef}`. Two parts in one job
  can own the same feature id and element name, so a bare `ElementRef` is not
  an answer; an operation names the PART, and is cut wherever that part is
  placed. A target that does not say which part, when the name is ambiguous, is
  refused rather than resolved against the first match.
- A **setup** is one clamping: one stock, one touch-off, one machine. A second
  side of a part (a flip) is a second setup.
- The job holds one id counter for parts, setups, operations, placements and
  fixtures.

## The pipeline

```
JobBodies (model space, every part)                    ui/JobActions
  -> BodiesInSetupFrame (per part: FixtureFrame, R_x(A), placement)   core/cam/CamSetup
  -> Generate / GenerateAll  (section -> Clipper2 offset -> link)      core/cam/CamStrategies,
                              drop cutter + patterns for surfacing       CamSurfaceStrategies
  -> ClipToMachine (when the machine states a work area)               core/cam/CamTravel
  -> PostProcess (dialect, origin subtracted, arcs fitted)             core/cam/CamPost
  -> ParseGcode -> Inspect / InspectRotary (the verdict)                core/cam/CamGcode, CamVerify, CamRotary
                -> StockSim / RotaryStockSim (the picture)              core/cam/CamSim
```

Toolpaths are generated **in setup space**: the bodies are transformed so the
setup's "up" is +Z, and the whole 2.5D pipeline works unchanged on a part that
is not modelled face-up. Element names survive because only the shape is
transformed. A job tab draws its parts laid out in setup space too, so the
backplot, the tool glyph, the stock and the simulation are drawn in setup
space with no transform back.

## Invariants

- **A setup never changes the model.** It is a document-level object outside
  the regeneration chain; describing how to cut a part does not rebuild it.
- **One transform seam.** `JobBodies` assembles the job's bodies, and
  `BodyToSetup` composes each part's placement and fixturing frame with the
  rotary index (`BodiesInSetupFrame` applies it, and the verifier maps
  another setup's cut through it) — `SetupToModel` is the one place
  `frame · R_x(A)` is composed and `FixtureFrame` the one place "how is this
  part clamped" is answered. A consumer that builds its own copy of the transform is a bug.
- **The picture and the file cannot disagree.** The backplot is drawn from the
  same `std::vector<Toolpath>` the post consumes; the verifier and the
  simulation both read the posted G-code; the simulation's last frame is
  bit-identical to the map the verdict is computed from (`CarveSweep` is the
  one carve).
- **Verification runs on the posted file**, not on the in-memory moves, so a
  bug in the post, in modal state or in linking is caught, and so is a rapid
  through material that leaves the part perfect.
- **Every default carrying a unit is derived** from the tool, the face or the
  stock: stepdown half the cutter, stepover 45 % (or from the cusp for a
  ball), safe Z above the stock and fixtures, origin at a stock corner, finish
  stock a tenth of the roughing cutter, peck one diameter. None is a literal.
- **Refuse, don't substitute.** Unknown enum strings (op type, side, tool
  type, corner relief, rotary pattern, origin corner, order, fixture side,
  stock edge, post) are refused naming what is accepted. A tool object with no
  `number` is refused rather than editing tool 1. A tool the setup does not
  have is refused rather than falling back. A field an operation type does not
  read is refused at the RPC. An inside profile with nothing to follow
  refuses rather than offsetting the part's own outline. A depth a tool
  cannot reach, deeper than its flutes on a shank wider than the cutter, is
  refused by the edit that asks for it ([setup.md](setup.md#reach)).
- **Feeds and speeds are the user's.** The app computes none it cannot
  justify; it checks chip load and reach and says what it found
  ([simulation.md](simulation.md#findings-about-the-cutter)). Reach is also
  said, or refused, when the op or its tool is edited
  ([setup.md](setup.md#reach)).
- **Nothing silently changes strategy.** When adaptive clearing declines a
  region and concentric clearing cuts it, the toolpath's `strategy` says so.
  An operation that produces nothing carries `error`; a target of a
  multi-target operation that could not be followed is named in `skipped`
  (and posted as a `SKIPPED` comment) without costing the others.
- **Stock is the tight bound.** Stock and placement bounds use OCC's tight
  bounding box (surfaces evaluated), not the conservative one that bounds a
  B-spline by its control points; stock is material a person buys and
  clamps. Shapes cache the tight bound, and `JobBodies`/`JobPlacedBodies` warm
  it once on the bodies that survive, because `BodiesInSetupFrame` returns
  copies and a bound warmed on a temporary is lost.
- **Physical tolerances, not nanometres.** Comparisons against float-derived
  geometry use a micron (`top_z` against the stock top, reach against flute
  length) and `kStockFit = 1e-3` mm for "does the part fit this sheet"; OCC's
  bounding boxes carry a gap of that order.

## Rapids, profile outlines and through cuts

A rapid never goes below the stock top (`RapidFloor`, `CamGeometry.cpp`):
above the stock there can be no material; below it the pass feeds in along its
ramp, because a pocket's entry sits mid-spiral where the previous pass was
still ramping. A profile never plunges: a non-centre-cutting end mill rubs.

Depth cut past the stock floor (a through cut into the spoilboard) is reported
as `belowStock` / `below_stock_mm`, not as a gouge.

## Where the code is

| area | files |
|---|---|
| data model | `src/core/cam/CamJob.h` (Tool, Machine, Operation, Setup, Placement, Fixture, Move, Toolpath), `JobDocument.{h,cpp}` |
| names and capability | `CamNames.{h,cpp}` (one row per enum value; `OpField` capability mask), `CamOpFields.{h,cpp}` (one row per operation field) |
| machine | `CamMachine.{h,cpp}` (`MatchMachine`, the store's file names) |
| framing, stock, origin, fixtures, seats | `CamSetup.cpp` |
| picks | `CamPicks.{h,cpp}`, `CamGeometry.cpp` (`PickedFaceRegion`, `PickedWallContour`, `PickedSurfaceRegion`, `ProjectedOutlines`), `CamSurfacePick.cpp` |
| derivation | `CamDerive.cpp` (`DeriveOperation`, `DeriveSafeZ`, `DeriveSurfaceRough`, `DeriveRest`, `DeriveRotaryRough`, `AddOperationType`, `CuttingMinutes`) |
| generation | `CamToolpath.h` (the engine's API), `CamToolpathInternal.h`, `CamStrategies.cpp` (2.5D), `CamAdaptive.{h,cpp}` (libarea bridge), `CamSurfaceStrategies.cpp` (3-axis and rotary surfacing), `CamSurface.{h,cpp}` (patterns), `CamDropCutter.{h,cpp}` (cutter-location surface) |
| layout, order, travel | `CamTravel.cpp` (`CheckPlacements`, `PlacementRefusal`, `CuttingOrder`, `MachineWarning`, `ClipToMachine`, `GenerateAll`) |
| post | `CamPost.{h,cpp}`, `CamRotaryFeed.{h,cpp}` |
| parse and verify | `CamGcode.{h,cpp}` (parser), `CamVerify.{h,cpp}` (height map, verdict, `Explain`, `MachineLines`), `CamRotary.{h,cpp}` (radius map), `CamFeeds.{h,cpp}` (chip load, reach) |
| simulation | `CamSim.{h,cpp}` (`Timeline`, `StockSim`, `RotaryStockSim`), `CamStockMesh.{h,cpp}` (stock and tool meshes), `src/render/StockRenderer.{h,cpp}` |
| serialization | `src/core/io/SerializeCam.cpp`, the job section of `Serialization.cpp` |
| RPC | `src/remote/RemoteCam.cpp` |
| UI | `src/ui/JobActions.{h,cpp}` (the job tab's bodies, fixtures, safe Z), `CamPanel.{h,cpp}`, `CamTool.{h,cpp}` (verbs and their windows, verification), `CamGenerate.{h,cpp}`, `CamPlayback.{h,cpp}`, `CamJobs.{h,cpp}` (background passes), `MachineActions.{h,cpp}`, `SettingsWindow.cpp` (Preferences › Machines), `GizmoInput.cpp` (placement and fixture gizmos), `Toolbar.cpp` (`DrawJobToolbar`), `Commands.cpp` / `core/io/Keymap.cpp` (`job.*`) |

## RPCs

All in `RemoteCam.cpp`, each with a `describe` row that is the reference for
its parameters. Every job verb takes `job` (name or id); without it the verb
acts on whichever job tab is in front, which is a trap in a script that
switches tabs — `cam_post` writes a file, and the wrong job's program under
the right name is scrap.

| group | methods |
|---|---|
| machines | `list_machines`, `save_machine`, `delete_machine` |
| the job | `machine_part` (new job holding a part), `add_job_part`, `remove_job_part`, `list_job_parts`, `check_layout`, `set_job_placement` |
| fixtures | `add_job_fixture`, `set_job_fixture`, `remove_job_fixture` |
| setups | `add_cam_setup`, `list_cam_setups`, `set_cam_setup` (also edits a tool: `{id, tool: {number, …}}`), `remove_cam_setup` |
| operations | `add_cam_op`, `set_cam_op`, `move_cam_op`, `remove_cam_op`, `cam_derive_op` (the dry run) |
| output | `cam_toolpath` (`async` hands slow passes to a `cam` background job), `cam_verify`, `cam_simulate`, `cam_post` |

`add_cam_op` without `top_z`/`bottom_z` derives them through the same
`cam::DeriveOperation` the GUI verbs use, so a script and a person get the
same operation from the same pick. No CAM method carries the `NeedsPart`
flag in its `MethodDoc` row: a job tab has no part document, and a gated
method would die there.

## GUI entry points

A job is made from the tab bar's `+` (New Job), set up in the CAM panel
(machine, stock, parts, Create setup), and machined from the job tab's own
toolbar of verbs (`job.*` commands, keymap context `job`). The CAM panel
exists only on job tabs. See [gui.md](gui.md).

## Background work

Editing one operation regenerates only that operation
(`RefreshCamToolpath`); `RefreshCamToolpaths` regenerates the whole setup and
is for setup-wide edits (the stock, a tool, the frame), since one adaptive
pocket can take seconds. A document edit ends playback rather than
re-cutting inside the frame.

The expensive steps of a generate run on a `cam` background job
(`src/ui/CamJobs.cpp`), keyed by content so a result that lands after its
inputs moved on is simply never read:

- a profile's projected outlines (`QueueOutlinePasses`, started first: every
  profile waits on its own);
- adaptive clearing passes;
- surfacing operations (one entry per operation).

A toolpath waiting on a pass is `pending`; post, verify and simulate refuse
while any is pending, saying why. `cam_toolpath {async: true}` returns at once
with `pending` and `queued_passes`; otherwise the RPC settles first. The
panel's verification of the posted file runs on its own `cam-verify`
background job (inline in the GUI suite, which turns deferral off). The worker is thread-safe
because the sectioning booleans are non-destructive (`BuildQuery`) and it
works on copies of the framed bodies.

## Building without CAM

`-DENABLE_CAM=OFF` removes toolpath generation: no Clipper2, no
`CamGeometry`/`CamDerive`/`CamStrategies`/`CamAdaptive`, no CAM RPCs in
`describe`, no CAM tests. The job document, its names and file format, the
fixtures and seats, placements, cutting order and travel arithmetic, and the
post (`CamPost`, `CamRotaryFeed`) are built either way, so a job tab still
opens and saves. `ping` reports which optional subsystems a build has.
`-DENABLE_CAM_ADAPTIVE=OFF` drops only libarea's adaptive clearing (linked as
the LGPL shared library `designer_adaptive`); pockets then clear
concentrically. `src/core/cam/` compiles with `-Werror=switch`, so an
`OpType` added without a case in every switch breaks the build.

## Tests

- Headless, `tests/test_cam*.cpp` (`ctest -L cam`), fixtures in
  `tests/cam_fixtures.h`: toolpath geometry against known shapes
  (`test_cam.cpp`), setups/frames/stock/origin (`test_cam_setup.cpp`),
  machine profiles, dialects and the frozen G-code goldens
  (`test_cam_machine.cpp`, `tests/golden/cam-{block,curved}-{mach3,linuxcnc,grbl}.nc`,
  regenerated with `DESIGNER_REFRESH_CAM_GOLDEN=1` and the diff read before
  committing), the verifier (`test_cam_verify.cpp`), the simulation
  (`test_cam_sim.cpp`), surfacing (`test_cam_surface.cpp`), rough and rest
  (`test_cam_rough_rest.cpp`), rotary (`test_cam_rotary.cpp`,
  `test_cam_spiral.cpp`, `test_cam_helical.cpp`), clipping to the machine
  (`test_cam_travel.cpp`), tools (`test_cam_tools.cpp`), workholding
  (`test_cam_workholding.cpp`), and the capability table enforced
  (`test_cam_capability.cpp`: every field a type does not declare is poked,
  and the moves must not change; every field round-trips through save and
  load).
- The job format: goldens in `tests/golden/job/` gated by `test_golden.cpp`.
- RPC: `tests/headless_smoke.py` (machines, fixtures, seats, the op-field
  round trip through `set_cam_op`/`list_cam_setups`), `rpc_gate_smoke`,
  `rpc_units_gate.py`, the params gate.
- GUI: `src/app/guitests/GuiTestsCam.cpp` (the `cam/*` cases), which run
  windowed — `--offscreen` has no panels, so something drawn at the wrong
  scale or offset looks right there.
- `-DENABLE_CAM=OFF` is built and tested by CI.

## What it does not do yet

- **Repeats.** Several placements of one part are held in the model, but the
  generator resolves a region to the first body that answers, so a part
  placed twice is refused (`PlacementRefusal`) rather than cut once.
- **Nesting / auto-arrange.** Placement is by hand (with snapping). A nest
  that spins parts needs a grain direction on `Stock` first, which does not
  exist.
- **Work offsets.** The file is single-offset (`G54`); repeating a program at
  several work offsets, or tiling a part longer than the travel across
  setups, is not built.
- **Stock kinds.** `StockMode` is `Relative` or `Fixed`, both boxes. No stock
  from a solid (an off-cut), no round stock (a rotary blank is derived from
  the part), no picked-vertex origin (`ElementMap` has no vertex kind).
- **Several setups in one file.** `PostProcess` posts one setup; a flip job is
  two files.
- **Helical arcs.** Only flat `G2`/`G3` are fitted.
- **Planning around fixtures.** Operations are told they hit a fixture, not
  steered away; nothing holds a part down on 3D operations (tabs are
  profile-only); fixtures are not checked on a rotary setup (the chuck and
  tailstock are); no spindle nose.
- **Stock carried between setups.** Leftover another setup removes is said
  as that setup's ([simulation.md](simulation.md#leftover-another-setup-removes)),
  but each setup's simulation, rapid, bite and collision checks start from its
  own fresh stock.
- **Waterline roughing** (clearing each level between the part's section and
  the stock); rotary roughing is layers, not constant engagement.
- **Paths that follow a feature** other than a twist (wrapped text, engraving
  round a cylinder).
- **Simulation extras**: colour by operation, a deviation map, a dirty-rect
  stock remesh.
- **A setup sheet.**
