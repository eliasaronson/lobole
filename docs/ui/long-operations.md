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
| `kind` | `"cam"`, `"cam-verify"`, `"cam-simulate"`, `"sync"`, `"topo"`, `"study"`, `"check"`, `"mesh"`, `"requirements"`, `"import"`, `"motion"`, `"motion-video"`, `"open"`, `"job-warm"`, `"drawing"`, `"contact"` |
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
`ReapBackgroundJobs`) is called once per iteration by each of the app's three
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
cancellable) and `cancel_background_job {kind}` requests a cancel; a study
job is also asked about and cancelled by its id, `study_status {job}` and
`cancel_study {job}`
([studies as jobs](../simulation/README.md#studies-as-jobs)); for `cam`
it first empties the CAM queues (`CancelCamPasses`). Both work without a part
tab. They are not called `jobs`, because on this API a job is the CAM job
document.

## Cancel, quit and close

Cancel is a request. A job whose inner work has no interruption points
(libarea's adaptive clearing, the drop cutter, OCC sectioning, the linear
solver) cannot stop mid-pass: cancelling abandons whatever is queued and lets
the pass in flight finish, and a finished pass's result is kept because it was
paid for. The registry entry joins its thread on destruction, so closing the
app or a tab mid-pass blocks for the remainder of one pass, not the whole
operation.

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

A STEP import's worker reads and plans; its `finish` is first called once
after the join to read the verdict (a feature import, a failure, a cancel, or
"materialize"), then once per poll, materializing a slice of parts per frame
and returning `false` until the last. When the materialize starts it turns
`cancellable` off, which the modal and `background_jobs` both show. The modal
stays up across the part → assembly tab switch
(`assembly/a_step_import_runs_in_background`).

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
`rebuilt_parts` and `rebuild_reason`.

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

## What is not on the registry

- **`RenderJob`**, the offline path tracer, is not a thread: it slices on the
  main thread (`PumpRenderJob`). It is the one place a background render
  thread would earn itself.
- **Regeneration.** `Document::Regenerate` is synchronous on the calling
  thread. Making it asynchronous is a question about the document model's
  single-threaded mutation assumption, which checkpoints, undo and every RPC
  handler rest on.
- **Parallelism inside one operation** (parallel tessellation in
  `TessellationCache.cpp`, IBL prefiltering, `parallel::For` elsewhere) is a
  separate mechanism. A job is a user-visible operation; how it uses the
  cores is not the registry's business, and widening `BackgroundJob` into a
  scheduler is the wrong fix for it.
- A render thread would fix none of this: the stalls are task work, not
  presentation, and with an immediate-mode UI a separate render thread would
  keep presenting the last frame of a window that ignores every click.
- The perf HUD does not list the registry.

If coarse cancel ever proves unacceptable for uninterruptible third-party
code, the remedy is a separate process that can be killed, at the cost of
serialising the input and output.
