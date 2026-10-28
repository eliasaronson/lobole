# Long operations off the frame

Anything that takes seconds runs on a worker thread as an entry in one
registry, and the frame keeps drawing. The rule every job follows:

> The worker touches only pure data, and every Document mutation stays on the
> main thread.

## The registry

`ui::BackgroundJob` (`src/ui/BackgroundJob.h`) is one running operation:

| field | meaning |
|---|---|
| `id` | handed out by `StartBackgroundJob` from a session counter, never reused: the handle a study job is asked about and cancelled by |
| `kind` | `"cam"`, `"cam-verify"`, `"cam-simulate"`, `"sync"`, `"topo"`, `"study"`, `"check"`, `"mesh"`, `"requirements"`, `"import"`, `"motion"`, `"motion-video"`, `"open"`, `"job-warm"`, `"drawing"`, `"contact"`, `"interference"`, `"insertion"`, `"assembly_sequence"`, `"design_sweep"`, `"feature"` |
| `label` | shown as-is ("Clearing 'Pocket', pass 2 of 4") |
| `fraction` | atomic; `< 0` is indeterminate (no bar) |
| `cancel` | atomic request set by the main thread; a job may ignore it |
| `cancellable` | whether cancel does anything; the UI and `background_jobs` read it |
| `owner` | the RPC connection a STUDY job dies with (0: none -- the GUI started it, or `outlive_client`); CAM passes have none |
| `studyId`, `cancelReason` | which study a study job runs; why `cancel` was set, for the record kept once it is reaped |
| `done` | set by the worker last |
| `error`, `thread`, `started`, `reaped` | |
| `finish` | runs on the **main** thread after the join; returns `false` to be called again next frame, which is how a sliced main-thread step (the import's materialize, the motion video's pose-and-capture) fits without a second mechanism |

The destructor sets `cancel` and joins. The verbs are in
`src/ui/BackgroundJobs.h`: `StartBackgroundJob`, `BackgroundJobRunning`,
`FindBackgroundJob`, `CancelBackgroundJobs`, `JoinBackgroundJobs`,
`ListBackgroundJobs`, `ReapBackgroundJobs`, and by id
`FindBackgroundJobById`, `CancelBackgroundJobById`, `CancelJobsOwnedBy`
and `FinishedJobById` (the reap records how each job ended -- done,
cancelled or failed -- and keeps the last 64 in `AppState::finishedJobs`).

**One poll site.** `ui::PollBackgroundJobs` (`PumpRenderJob`, then
`ReapBackgroundJobs`, then `ui::RefreshRequirementsInPlace`, which measures
the [expression requirements](../requirements/README.md#expression) whose
values moved -- not a job, and here for the same reason) is called once per iteration by each of the app's three
loops: the windowed frame loop and the `--offscreen` loop in
`src/app/main.cpp`, and the `--headless` pump in `src/app/HeadlessMain.cpp`.
Where a job is reaped is therefore not a property of which panel is open: a
push completes with the History panel closed, and a solve lands after leaving
Simulation mode. A `finish` may start a successor (that is how a queue of
passes advances), so the reap walks only the entries that existed when it
began. The RPC handlers may also call `ReapBackgroundJobs`, because a handler
must apply a finished result before it answers a question about it.

`tests/test_conventions.cpp`, "a background job is polled by the loop, never
by a panel", fails if any file under `src/ui` other than `BackgroundJobs.cpp`
calls `PumpRenderJob` or `ReapBackgroundJobs`. It is checked rather than
remembered because a poll call looks equally at home in a draw function.

**Deliberately not unified:** the progress vocabulary (a job's domain keeps
its own counters — the import's parts done, the topology run's compliance —
and writes `label` and `fraction` for everyone else), cancellability (a job
that cannot be interrupted says so), and the result type (it lives in the
domain's own struct and is consumed by that domain's `finish`).

**Progress UI.** One widget, `DrawJobProgress` (`src/ui/Widgets.h`), used by
the open and import modals, the render-still progress, the Generate window,
the Study panel, the motion window, the requirements panel and the drawing
panel's view list. `test_conventions.cpp` ("every long operation draws the
one progress bar") fails on an `ImGui::ProgressBar` anywhere but
`Widgets.cpp`, so a new long operation cannot grow a bar of its own. A modal is used only where nothing else is safe while
the job runs (an import or an open is building the document); elsewhere the
bar sits where the work was asked for and the rest of the UI stays live.

**RPC.** `background_jobs` lists what is running (id, kind, label, fraction,
cancellable), its row naming every kind a job is given (`test_conventions.cpp`,
"every background job kind is in background_jobs' row"), and
`cancel_background_job {kind}` requests a cancel; a study
job is also asked about and cancelled by its id, `study_status {job}` and
`cancel_study {job}`
([studies as jobs](../simulation/README.md#studies-as-jobs)); for `cam`
it first empties the CAM queues (`CancelCamPasses`). Both work without a part
tab. They are not called `jobs`, because on this API a job is the CAM job
document.

## The cancel rule

Every cancellable kind follows one rule:

> **A cancel never applies a half-finished change to the model; results
> already paid for are kept as results.**

"Paid for" means whole: a pass, a view, a sample, an iterate, a measured
point is kept; a part of one answer is not, because a partial report reads
as a complete one (a contact report missing pairs reads as fewer contacts).
A kind with nothing to interrupt says `cancellable: false`, and a cancel
asked of it changes nothing it does; such a job is recorded as `done`, not
`cancelled`, when it runs to its end. `tests/test_conventions.cpp`, "every
job kind states its cancel and names a test of it", fails on a kind with no
row here, a row with no outcome, or a test named below that does not exist.

| kind | what a cancel keeps | the test of it |
|---|---|---|
| `cam` | not cancellable mid-pass: the pass in flight finishes and is cached; the queues are emptied, the pending toolpaths say so, a waiting post or verify is refused | `an adaptive pocket is cleared off the frame and lands complete`, `cam_parked_smoke.py::main` |
| `cam-verify` | not cancellable: the carve runs to its end; a waiting `cam_verify` is refused, and its verdict is not cached (the next call carves again) | `cam_parked_smoke.py::cancel_cam_verify` |
| `cam-simulate` | not cancellable: the read runs to its end and the playback opens | `cam_parked_smoke.py::cancel_cam_simulate` |
| `sync` | the transfer, which git cannot interrupt, finishes and what it fetched or cloned is kept; the step that would change the open project after it -- a pull's merge, a clone's open -- does not run | `a cancelled clone keeps what it cloned and does not open it` |
| `topo` | its best iterate, stored as the study's result marked `cancelled` (it replaces the earlier result, and is never reused as a cached answer) | `topo_smoke.py::main` |
| `study` | nothing: a solve cancelled is an answer nobody wants, even one a direct factorization finished after the cancel | `fem_async_smoke.py::main` |
| `check` | the studies solved whole before the cancel; the one in flight is dropped | `topo_build_smoke.py::main` |
| `mesh` | nothing: the report is a question about the mesh, dropped with it | `a study solved on the registry lands in its own part, not the active tab` |
| `requirements` | the evidence each requirement already measured; the measure in flight is not landed | `requirements/a_slow_check_keeps_the_frame`, `requirements_smoke.py::off_frame` |
| `design_sweep` | the points it measured, the answer marked cancelled; the model is never touched (it sweeps copies) | `requirements/a_sweep_by_clicking`, `design_sweep_smoke.py::main` |
| `import` | nothing: the part is unchanged and the read dropped; once it materializes it is not cancellable and lands whole | `import/a_mesh_import_runs_in_background`, `import_job_smoke.py::main` |
| `motion` | the leading samples whose every check landed, the result marked cancelled | `study_jobs_smoke.py::main` |
| `motion-video` | the pose restored, no video file; the frames written stay in their folder | `motion_video_smoke.py::main` |
| `open` | the open project, untouched: nothing in `AppState` changes until the adopt | `file/a_slow_open_shows_progress_and_cancels` |
| `job-warm` | not cancellable: it is a warm cache, nothing a person asked for, and the open it follows is whole either way | `cam_parked_smoke.py::cancel_job_warm` |
| `drawing` | the views that landed, with their saved projections; the rest say "projection cancelled" and resume on the next update | `a cancelled drawing projection says so and resumes`, `drawing/a_cancelled_projection_resumes` |
| `contact` | nothing: a report of some pairs would read as the report of all of them | `contact_smoke.py::main` |
| `interference` | not cancellable (one boolean): the waiting call answers | `contact_smoke.py::cancel_interference` |
| `insertion` | nothing: the waiting `check_insertion` is refused | `insertion_smoke.py::cancel_insertion` |
| `assembly_sequence` | nothing: the waiting `check_assembly_sequence` is refused | `assembly_sequence_smoke.py::cancel_assembly_sequence` |
| `feature` | an edit's build: the edit, rolled back with no undo step; an existing feature's: the feature, marked not built (cancelled), until it is asked for ([heavy features](#heavy-features)) | `direct/cancelling_a_heavy_delete_undoes_it`, `direct/cancelling_a_rebuild_leaves_it_not_built`, `heavy_feature_smoke.py::main`, `A build stops on its cancel and keeps nothing` |

## Cancel, quit and close

Cancel is a request. A job whose inner work has no interruption points
(libarea's adaptive clearing, the drop cutter, OCC sectioning, the linear
solver) cannot stop mid-pass: cancelling abandons whatever is queued and lets
the pass in flight finish, and a finished pass's result is kept because it was
paid for. The registry entry joins its thread on destruction, so closing the
app or a tab mid-pass blocks for the remainder of one pass, not the whole
operation. A `SIGTERM` to `--headless` or `--offscreen` does not wait on it
past a few seconds ([stopping](../architecture.md#stopping)).

## CAM: toolpath passes

Three kinds of expensive CAM computation run on the registry, each a keyed
computation whose result a later regenerate reads out of a cache:

| queue | unit | key |
|---|---|---|
| outline (`camOutlineQueue`) | one `ProjectedOutlines` projection (a profile's OCC sections of every body at each pass depth) | `OutlineKey` (`core/cam/CamPicks.h`): `bodiesKey`, how the setup frames the part (`FramingKey`), the top, the pass heights and the deflection |
| adaptive | one Z level of one adaptive-clearing operation (libarea) | `AdaptiveKey` (`core/cam/CamAdaptive.h`) over the seven setup/op/tool scalars `AdaptiveClear` reads, plus the loops |
| surface | one whole 3-axis or rotary surfacing operation | `SurfaceKey` (`core/cam/CamSurface.h`): `bodiesKey` and `FramingKey` plus the fields the surfacing strategies read, the stock its entries come down to, and whether a rest pass's finish is cut before it (`FinishedBefore`) |

**Content keys, not versions.** A result is keyed by a hash of exactly its
inputs, so it has no tab, no operation index and no document version:

- a result for parameters the user has since changed is a cache entry nobody
  reads, and the regenerate after it lands sees the new parameters, misses and
  queues them — a drag costs one pass per distinct value that survived to a
  frame;
- a pass that lands after a tab switch regenerates whichever job tab is
  active from its own document;
- undo and re-open are cache hits.

The core cannot fingerprint geometry cheaply, so the caller supplies
`GenerateContext::bodiesKey`, which the UI fills from `JobPassesKey` (the
job, its sources, the fixturing and every placement). That
fingerprint is one per job, while an operation cut at several indices, or a
second setup, frames the same bodies differently, so the outline and surface
keys also hash `FramingKey` (the fixture frame, the index and every
placement of the setup the pass is cut in,
[rotary.md](../cam/rotary.md#one-operation-at-several-indices)).

**Which setup the tab shows is not an input.** `JobRenderVersion`, which the
renderer resyncs on, is `JobPassesKey` plus the shown setup; the pass keys
leave it out. With it in, a parked `cam_toolpath` queued its passes the
moment it selected the job, while the tab showed no setup, and the tab then
showed one: every pass was owed again under a second key and cut twice. The
chess knights' `cam_toolpath` took 286 s for 131 s of passes (`a pass is
keyed on the job, not on which setup the tab shows`).

**The keys must cover every field the pass reads.** A field read but not
hashed is a stale result nobody can see, so `test_conventions.cpp` has "the
adaptive key covers every field the pass reads" and "the surface key covers
every field the pass reads", each scanning the generator's reads against its
key function. `OutlineKey` needs no such gate: `ProjectedOutlines` is a pure
function of exactly what it hashes.

**Shape in code.**

- `cam::GenerateContext` (`core/cam/CamToolpath.h`) tells a generate how to
  treat the expensive steps: look them up in the caches, and with `defer`
  set, append misses to `pending` / `pendingSurface` / `pendingOutline` and
  return the toolpath marked `Toolpath::pending`. Every depth pass of an
  operation is queued at once, top first, and nothing after the queueing runs
  on a partial result.
- `cam::AdaptiveCache` is bounded (256 entries, two million moves, oldest
  out).
- `src/ui/CamJobs.{h,cpp}` is the worker glue: `QueueAdaptivePasses`,
  `QueueSurfacePasses`, `QueueOutlinePasses` (each deduplicates against the
  cache, the queue and the pass in flight), `CamToolpathsPendingWhy`,
  `SettleCamToolpaths`, `GenerateCamToolpathsNow`, `CancelCamPasses`,
  `CamPassProgressOf`. Each queue has its own in-flight slot and starts its
  own `cam` job, drained outline first (every profile waits on its own
  outline), then adaptive, then surfacing (seconds before minutes, so the
  backplot grows steadily). A `finish` caches the result and regenerates
  whichever job tab is active.
- The worker's inputs are its own: a deferred outline request deep-copies the
  bodies (`BRepBuilderAPI_Copy`) on the main thread when it is made, and
  surfacing samples the model through `GenerateMeshDetached`, because a
  triangulation is written onto a shared `TShape` and the frame may mesh the
  same shapes while a worker reads them.

**A pending toolpath is not the job yet.** The unit of "pending" is the
toolpath: an edit to one operation can regenerate every toolpath (tool order
sorts across them), and a posted file is right only once all of them have
landed. The backplot keeps showing the
previous moves of a pending toolpath rather than blanking (an empty backplot
reads as "this operation produces nothing"). Post, verify, simulate and Save
G-code all ask `CamToolpathsPendingWhy` first and refuse in words.
`RunCamVerification` on a pending list records "Not verified yet" and sets
`camVerifyWhenSettled`, and the last pass's `finish` runs it. Verification
itself is a `cam-verify` job. It reads the job's other setups from the same
caches (`CamToolpathsQueued`) and never cuts them on the frame: a setup whose
passes are still queued is named as not yet compared, and the verification
runs again when they land
([simulation.md](../cam/simulation.md#leftover-another-setup-removes)).

**RPC semantics: synchronous, off the frame.** `cam_toolpath`, `cam_post`,
`cam_verify`, `cam_simulate` and `export_setup_sheet` answer with complete toolpaths, but the
server parks them while the passes they read cut on the worker (and
`cam_verify`'s simulation on a `cam-verify` job), answering when those land
([cam/README.md](../cam/README.md#requests-that-wait)). The handlers
themselves still generate through `GenerateCamToolpathsNow` (join the pass in
flight, then the cache, then compute the rest inline), which is what a
`batch` and the parked request's fallback run. `cam_toolpath
{async: true}` answers at once with `{pending, queued_passes, job}`,
where `queued_passes` counts all three queues, for a client that would rather
poll `background_jobs`. Mutating RPCs (`set_cam_op`, `set_job_placement`, …)
defer as the panel does, so an agent editing a job on a windowed instance does
not freeze it.

**The GUI suite generates inline.** `ResetAppForTest` sets
`camDeferPasses = false`, because many tests read `camToolpaths` right after a
pick; `cam/adaptive_clears_off_the_frame`, `cam/surfacing_runs_off_the_frame`
and `cam/a_profile_is_cut_off_the_frame` turn it back on and assert the
generate returns at once and no frame is held while the passes run. Headless
twins in `tests/test_actions.cpp` and `tests/test_cam.cpp` pump the registry
as `HeadlessMain` does and check landing, cache hits, keep-previous, settle
and cancel against a synchronous generate.

Not deferred: Pocket's and Face's per-pass `SectionLoops` and the section
loops adaptive takes before queueing — one section per pass each; the outline
cache is the natural home if one ever holds a frame.

### Passes kept on disk

A surfacing pass is also kept on DISK, under `$XDG_CACHE_HOME/designer/cam/`
(`cam::SurfaceStore`, `core/cam/CamSurfaceStore.h`), so a restart or a
rerun of an unchanged job reads it instead of cutting it again: the knights
blank's three turning passes, a minute cold, come back in half a second.
The in-memory cache stays in front; whoever would cut a pass it misses --
the worker, or the inline path -- reads the store first
(`SurfacePassStored`), and writes what it cuts.

- **Keyed on the geometry itself.** The store's key is `SurfaceKey` with
  `BodiesContentKey` for the geometry: the bodies' exact B-rep (no
  triangulation) and the names a pick resolves through. The session's
  `bodiesKey` is made of version counters, which say "unchanged since" inside
  one run and nothing across two.
- **Salted by the build.** The key covers a pass's inputs, not the code that
  cut it, so every file also carries a hash of the executable that wrote it,
  and another build's file is a miss. The hash is taken on first use, not
  at startup.
- **Bounded, least recently read out.** 2 GiB (a turning is ~50 MB); a read
  touches its file, and after a write the oldest untouched ones go. Writes
  land whole (a temporary, then a rename); a torn or foreign file is a miss
  and is removed.
- **Opened by the app, not by the core.** `ui::OpenCamSurfaceStore` at
  startup, windowed, offscreen and headless alike -- not under the GUI suite,
  which runs on defaults. An offscreen or headless instance does not isolate
  `XDG_CACHE_HOME`, so a test that reaches the store builds its own on a
  scratch directory (`tests/test_cam_surface_store.cpp`).

## FEM: meshing and solving

A solve's payload records the **part slot id** it was started from, and its
`finish` resolves that slot (`PartDocById`), so a solve started on one part
lands in that part whichever tab is active — or is dropped with a warning if
the tab was closed. `state.topoJob` / `studyJob` are `shared_ptr` payloads the
panel reads while the job runs; the registry owns the thread
(`src/ui/TopoJob.h`, `src/ui/SimulationActions.cpp`).

Meshing runs on the worker too. `PlanStudy` (expressions, refs, materials and
a **deep copy** of each shape) runs on the main thread, and `MeshPlannedStudy`
(mesh, fixed nodes, tractions — plain data) on the worker; `MeshStudy` splits
the same way into `PlanStudyMesh` + `MeshPlanned`. The copy is required:
Netgen attaches triangulation to the faces it meshes, which on the document's
own shape would be a write under the renderer's reads. `tests/test_fem.cpp`
holds both pairs to their inline originals (same nodes, tets, supports and
tractions, and a shape that is not the document's).

- `study` (static/modal) is cancellable at its boundaries: after the
  meshing, and within a few iterations of an iterative solve; a direct
  factorization has no interruption points, so a cancel during one lands
  when it ends. The answer is dropped either way.
- `topo` is cancellable and keeps its best iterate; `cancel_study` cancels
  it.
- `check` (`check_topology`) solves the shape's and the solid's static
  studies one after the other, each planned on the main thread; a cancel
  lands between them, inside an iterative solve or when a factorization
  ends, and keeps what was
  solved whole ([the topopt page](../topopt/README.md#a-static-study-of-the-optimised-shape)).
- `mesh_study {async: true}` is a `mesh` job (a cancel drops its report); `study_status` answers
  `{running, meshing: true}` until it lands and then carries
  `mesh: {bodies, seconds, error?}`. The synchronous form remains.

`PrepareTopology` still meshes on the main thread.

## Import

Every import that is not a sketch's is one `import` job
(`ui::StartImport(state, ImportRequest)`, `src/ui/ImportJob.{h,cpp}`):
File › Import of any STEP, IGES, STL, OBJ or BREP, the `import_step` RPC,
and `--file` at startup in all three modes. An `ImportRequest` carries the
path, the unit, `keep_origin`, the up axis and turn, the mode (`Auto`: a STEP
with several instances is an assembly; `Feature`; `Assembly`, which is
`as_assembly`), `allowLargeMesh` and the RPC connection that owns it (a
disconnect cancels it). Everything that can refuse is checked before the job
exists: another import running, no part, the feature limit, a mesh past the
[triangle limit](../interchange/README.md#what-a-mesh-costs).

The worker reads (`ImportFeature::ReadModel`, the read the feature's own
regeneration does) and, for a feature, measures each product's volume; for an
assembly it plans and writes the sidecars. Its `finish` is first called once
after the join to read the verdict (a feature import, a failure, a cancel, or
"materialize"), then once per poll, materializing a slice of parts per frame
and returning `false` until the last. A feature lands in the part tab the
import was started from, seeded with what the worker read, so the main
thread's share is the placement, the box and the names: 1.2 s of a
61 s import of 100k triangles. When the materialize starts it turns
`cancellable` off, which the modal and `background_jobs` both show. The
finish records how it ended (`ImportJob::outcome`: landed, failed, cancelled)
for whoever waits on it. The modal ("Importing", `###import_modal`) stays up
across the part → assembly tab switch
(`assembly/a_step_import_runs_in_background`); a cancelled import takes it
down at once, because the step a mesh read spends most of its time in (the
sewing's quilt) cannot be interrupted, and its result is dropped whenever it
ends. A mesh says its triangles and what it may take in the label and the
status line; past the limit, File › Import asks first (`AppState::importAsk`,
`###import_ask`).

`import_step` waits off the frame, as the other long RPCs do
(`remote::ParkImportRequest`, [requests that
wait](../architecture.md#requests-that-wait)): the handler runs once with a
slot set, checks every parameter, starts the job and leaves it in the slot;
the call answers when the finish has run, with the reply it always gave, and
answers a cancel the moment one is asked. `--file` starts the job before the
loop, so a client that connects meanwhile sees it in `background_jobs` and
can cancel it; a `--file` project still opens as before. GUI
`import/a_mesh_import_runs_in_background`, `import/a_large_mesh_asks_first`;
`tests/import_job_smoke.py`.

## Opening a project

`OpenProject` (`src/ui/ProjectActions.{h,cpp}`) is two steps:

- `ReadProject(path, progress) -> LoadedProject` is pure: it parses
  `project.json` and loads every document, the catalogs, sections and
  parameters into a plain struct, touching no `AppState`. Parts are
  independent documents and load in parallel (`parallel::For`). The project
  variables are evaluated first and handed to every part's load
  (`DocumentIO::ProjectScope`), so a part that uses one regenerates right the
  first time and keeps its baked geometry; loaded without them it failed,
  refused its bake, and rebuilt on every open. The read then clears that
  pointer: the scope is its own, and the adopt points the part at the
  project's.
- `AdoptProject(state, LoadedProject&&)` swaps the slots in, activates,
  syncs the project scope, solves an assembly and frames — milliseconds, on
  the main thread, except that framing a job tab builds the bodies it cuts
  (a configured part is a regeneration), which its first frame would
  otherwise do: 2 s for the chess set's undersides job, sixteen
  configurations of one piece.

`OpenProject` is their composition and stays synchronous for the RPC
(`open_project`), restore, merge, branch switch and sync. The opens a person
starts — File > Open, Open Recent, and the recovery prompt's "Open saved
file" — call `StartOpenProject` (`src/ui/OpenJob.{h,cpp}`), which runs
`ReadProject` as an `open` job under a modal: parts done over parts to load,
the names of the parts being built, and why they are being rebuilt. Cancel
discards the half-read project and leaves the open one untouched, since
nothing in `AppState` changes until the adopt. `StartOpenProject` waits up to
300 ms (`OpenJobOptions::quietWaitMs`) before showing the modal, so a baked
open never flashes it. The modal blocks input while it is up.

When parts were rebuilt because their saved geometry was refused (an older
geometry format, a stale hash, none on disk), a menu-bar item
`Geometry rebuilt` says how many and why (`DocumentIO::InspectGeometry` reads
only the blob's index), and saving re-bakes. `open_project` answers
`rebuilt_parts` and `rebuild_reason`. A blob that is there but damaged
(unreadable, a link out of the project, not matching its `content_hash`)
also rebuilds, and is reported as a derived drop
([a lossy load](../versioning/README.md#a-lossy-load)).

**The parallel read is safe because three process-wide states were made so:**
OpenCASCADE's STEP/XCAF exchange shares one unlocked application and lazily
filled tables, so every STEP read or write and every XCAF document holds
`DataExchangeMutex()` (`cad/XcafDocument.h`); the sketch-id generator is
`thread_local`; and PlaneGCS's `Eigen::setNbThreads(1)` runs once per process
rather than from every solver's constructor. `tests/test_open_project.cpp`
reads every golden document that loads on its own plus eight STEP-import parts
on one thread and on all of them, and requires the same bodies bit for bit,
the same element names and the same feature errors (the chain hash only where
it is reproducible: a sketch that projects external geometry mints fresh
entity ids on every rebuild). GUI: `file/a_slow_open_shows_progress_and_cancels`
(slowed by `OpenJobOptions::delayEachMs`, tests only).

Once adopted, a project with job tabs starts a `job-warm` job
(`ui::StartJobBoundsWarm`): the exact boxes of every other job tab's framed
parts, on deep copies (`CADShape::DetachedUnlocated`) the frame never meshes,
handed to the shape memo in its `finish`, so the first visit to a job tab
reads them instead of stalling (docs/cam/gui.md#job-tab-frame-cost). On the
chess set: 10 bodies, 17 ms on the frame to prepare, 4 s on the worker, and a
first visit to a turning job down from 0.6-0.9 s to under a millisecond.

## Drawings: projection off the frame

A drawing view's hidden-line removal is a `drawing` job, one job per view,
the next started from the last one's `finish` so views land one at a time
([drawings](../drawings/README.md#projection-off-the-frame)). The main
thread gathers each view's bodies and hands the worker deep copies
(`CADShape::DetachedUnlocated`, memoized per shape so the projection cache
keeps hitting); the worker hashes the inputs, installs a projection saved
with the drawing when the hash matches, else cuts and projects; the
`finish` assembles the view on its tab (by slot id, so a run that lands
after a tab switch writes the stashed tab). The run takes the projection
and cut caches for its life and gives them back when it ends. Cancel keeps
the views that landed. On the pan/tilt head's sheet, switching to the tab
held the frame for 43.7 s (23 s offscreen, the reopen another 23 s); on an
offscreen instance the switch now answers in 40 ms, the main thread is
never held more than 37 ms while the three views project for 23 s on the
worker, and a reopen installs the saved projections with no HLR, landed
0.5 s after the open. `UpdateDrawingRendersNow` is the waiting form,
for an RPC in a `batch` or on `--headless`; elsewhere the drawing RPCs are
parked until the job lands
([requests that wait](../drawings/README.md#requests-that-wait)).

## Motion studies

A motion study's sampled solve and exact clash sweep is a cancellable `motion`
job: the solves come first and report the first tenth of the fraction, the
checks of every sample in one batch the rest; cancelling keeps the leading
samples whose every check landed, and the result says so. Its bar is in the Motion tool window rather than a
modal, because you keep looking at the model while it runs.

A job makes the work asynchronous; the setup that hands the worker pure data
is main-thread by construction. `StartMotionRun` snapshots every document
under the assembly before the worker exists, so the cost of copying a
Document is paid on the frame.

## Contact reports

`contact_report {async: true}` is a cancellable `contact` job, one at a
time ([inspect](../inspect/README.md#clashes-contact-and-floating-parts)).
The handler gathers on the main thread (the bodies placed in world space,
their boxes, the mates) and hands the worker DEEP COPIES
(`CADShape::DetachedUnlocated`), since the frame may mesh the document's
shapes while the worker reads them. The worker runs the same sweep the
synchronous call does, writes the share of pairs checked to `fraction`, and
stops dispatching pairs on a cancel; its `finish` keeps the report in
`AppState::contactReports` (the last eight) for `contact_status {job}`, and
a cancelled run keeps none. It is owned by the RPC connection that started it
unless `outlive_client: true`.

## Interference of a named pair

An exact `interference {a, b}` is a REQUEST THAT WAITS
([architecture](../architecture.md#requests-that-wait)), not a job the
caller polls: `remote::ParkInspectionRequest` runs the handler on the main
thread with a slot set, the handler resolves the two sides and deep-copies
their bodies (`ContactBody`, as a contact report does), and the boolean and
the distance run on an `interference` job; the call answers when it lands.
One boolean has nothing to stop between, so it is not cancellable. Not
parked: the sweep (no pair; bounded by `deadline_s`), mesh mode, a call
inside `batch` or carrying `expect_revision`, and a call whose keys the
handler did not all read, which the server refuses before any job starts
([inspect](../inspect/README.md#threads)).

## A design sweep

`design_sweep` ([requirements](../requirements/README.md#design-sweep)) is
a cancellable `design_sweep` job, one at a time. The main thread checks the
request and writes each part the sweep rebuilds to its saved form in a
temporary directory, with a copy of the project variables and tables; the
sweep runs on a thread of the run's own (`SweepRun`), which reads them back
on up to `parallel::Workers()` workers (`cad::BooleanWorkers` of them:
half the machine on a kernel that queues booleans on one lock,
[performance](../performance/README.md#the-kernels-error-handler-lock)),
as an open reads a project, so it
shares no shape with the document. Like the requirements check, the job has
no registry thread: its `finish` is called every frame, carries the run's
progress (`label` "cam_d = 27.5 (7 of 15)", `fraction`) to the registry,
hands a cancel on to the workers (read between points), and once the run
has ended joins it and keeps its answer (the last eight, for
`design_sweep_result`). A cancelled sweep keeps the points it measured. It
is owned by the RPC connection that started it unless `outlive_client`; a
synchronous call parks on it.

## Heavy features

Regeneration runs on the main thread on every path, and stays there (below).
Only a feature that may cost seconds builds as a `feature` job: Delete Face
of many faces today (`BRepAlgoAPI_Defeaturing` on one thread: the Voron fan
grill's 916 faces, 14 s), and the organic meshes plan's Cradle next.

**Which.** A feature declares that it may (`Feature::MayBuildOffFrame`) and
estimates its own cost on the bodies it would act on (`BuildCostS`); it
goes to a job when the estimate reaches `heavy::kOffFrameS`, 0.25 s.
Delete Face estimates 15 ms a face, the grill's measured rate, so a hole's
bore and floor build inline and the grill's four holes do not. No list of
feature types exists.

**How.** Only a part the app holds builds off the frame
(`Document::SetBuildsOffFrame`, set on every part by the poll and before
each RPC; `Document::BuildOffFrameScope` for the parts an open or a restore
loads). Its regeneration, reaching a heavy feature that misses its regen
cache, first asks the process-wide store of finished builds
(`heavy::Find`, `core/HeavyBuild.h`), keyed by the feature's chain hash --
everything up to and including it, so a content key across documents. A
hit is installed like a regen-cache hit. A miss STOPS the regeneration:
the feature is `building`, everything after it `waiting`, the bodies are
those before it, the tip hash is withheld (nothing bakes it or hands it to
a copy), and `Document::PendingBuild` holds a request: a clone of the
feature and of every feature it references, its input bodies, the symbols,
configuration, sheet-metal and print context. Any other document -- a
copy, a trial, a test, a sweep's worker -- builds it inline as before, and
files what it built in the store.

`ui::PollFeatureBuilds` (`src/ui/FeatureBuildJob.h`, part of
`PollBackgroundJobs`) starts one `feature` job per part with a request. On
the main thread it deep-copies the input bodies (`heavy::Detach`:
OpenCASCADE writes to a builder's arguments and the frame meshes the
document's shapes); the worker runs the feature's own Execute on them
(`heavy::Run`), its cancel and progress reachable through the direct-edit
builders' deadline, which polls `heavy::Cancelled` at every progress point.
The `finish` files the result in the store and the part's regen cache
(`AdoptBuilt`) and regenerates the part, which hits it and builds what
follows; untouched bodies keep the document's own shapes. A second heavy
feature downstream is the next job.

**An edit made while one builds** restarts it if it changes what the
feature builds from, and otherwise waits. The request is a content key: an
edit upstream of the feature, or to it, or an undo past it, moves its chain
hash, and the running build could only land as an entry nobody reads, so it
is cancelled as superseded (not a user cancel) and the new key builds. An
edit downstream leaves the key; the job runs on and the edit is built when
it lands. What a superseded job finished is kept in the store, so a redo
back to it is instant.

**Undo.** The edit is on the undo stack from the moment it is made; undo
order is the order of edits, whatever is building.

**The RPC.** A call that leaves its part with a feature building is parked
(`BuildWait`, `RemoteUiActions.cpp`) and answers when the build lands, its
`regen` and `digest` recomputed against the part as built: the regen-honesty
rule, off the frame, as `import_step` waits on its job. A heavy direct edit
is not tried on a copy first (the trial would be the build, on the frame);
an `add_feature` or `modify_feature` whose own build fails is refused then,
the call rolled back. While the active part builds, every request that
reads its geometry -- a `NeedsPart` method that is not `Deferrable` --
waits (`BuildGate`), so no answer describes the bodies before the feature;
`get_state` answers at once, with each feature's `build` (`building`,
`waiting`, `cancelled`) and `regen.unbuilt`, which also makes `regen.ok`
false. `build_feature {feature_id}` asks for a cancelled one.

**Cancel** ([the rule](#the-cancel-rule)). The build belongs to an edit
when that edit's execution asked for it (`Command::CausedBuild`, the
request's serial; a batch's one step, a preview accepted while it builds).
Cancelling it while that edit is still the top of its part's undo stack
rolls the edit back and discards it (`CommandStack::RollbackSince`): the
features and the saved bytes as before, no undo step, and the call that
made it, parked, is refused saying so. The part reads unsaved afterwards,
as after any undo. Otherwise -- an open that rebuilt it, an undo, a build
asked for, an edit no longer last, a project variable's edit (which is on
no part's stack) -- the feature is left not built
(cancelled) and everything after it waiting, in the tree (dimmed, saying
why; right-click **Build**) and in `regen`. The mark is on the evaluation,
so an edit that changes its inputs builds it again. The doc side of a
cancel is applied by the poll, however the cancel was asked (the Model
panel's bar, the tree's **Cancel build**, `cancel_background_job {kind:
"feature"}`), so every path has the same outcome. A build outlives the RPC
client that made it: it is the document's.

**Saving** while one builds saves the recipe and no baked geometry for that
part; a reopen in the session hits the store, one after a restart rebuilds
it off the frame.

On the fan grill (`scratch/imported-edits`' steps on the Voron exhaust
grill STL, a 2937-face refined body), the `add_feature` of the 916-face
Delete Face answers in 15 s with the part as built, and the longest the main
thread was held meanwhile -- a second connection pinging every 20 ms -- was
26 ms. GUI: `direct/a_heavy_delete_face_builds_off_the_frame` (frames keep
coming, none held), `direct/cancelling_a_heavy_delete_undoes_it`,
`direct/cancelling_a_rebuild_leaves_it_not_built`; core:
`tests/test_heavy_build.cpp`; RPC: `tests/heavy_feature_smoke.py`. The test
knob `LOBOLE_TEST_HEAVY_FORCE` / `LOBOLE_TEST_HEAVY_DELAY_MS`
(`heavy::SetTestOptions`) sends every heavy-capable feature to a job and
holds its worker first.

## What is not on the registry

- **`RenderJob`**, the offline path tracer, is not a thread: it slices on the
  main thread (`PumpRenderJob`). It is the one place a background render
  thread would earn itself.
- **Regeneration.** `Document::Regenerate` is synchronous on the calling
  thread. Making it asynchronous is a question about the document model's
  single-threaded mutation assumption, which checkpoints, undo and every RPC
  handler rest on. A heavy feature's build is the exception, and only the
  build: the regeneration around it stays where it is
  ([heavy features](#heavy-features)).
- **Parallelism inside one operation** (parallel tessellation in
  `TessellationCache.cpp`, IBL prefiltering, `parallel::For` elsewhere) is a
  separate mechanism. A job is a user-visible operation; how it uses the
  cores is not the registry's business, and widening `BackgroundJob` into a
  scheduler is the wrong fix for it.
- A render thread would fix none of this: the stalls are task work, not
  presentation, and with an immediate-mode UI a separate render thread would
  keep presenting the last frame of a window that ignores every click.
- The perf HUD does not list the registry.
- **The disk poll** (`ui/DiskPoller.h`) has its own worker: it is nothing
  the user started or can cancel, and as a job it would flash a progress bar
  every few seconds and keep the idle throttle awake while it ran
  (docs/versioning/README.md#the-poll-runs-off-the-frame).

If coarse cancel ever proves unacceptable for uninterruptible third-party
code, the remedy is a separate process that can be killed, at the cost of
serialising the input and output.
