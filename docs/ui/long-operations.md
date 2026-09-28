# Long operations off the frame

Anything that takes seconds runs on a worker thread as an entry in one
registry, and the frame keeps drawing. The rule every job follows:

> The worker touches only pure data, and every Document mutation stays on the
> main thread.

## The registry

`ui::BackgroundJob` (`src/ui/BackgroundJob.h`) is one running operation:

| field | meaning |
|---|---|
| `kind` | `"cam"`, `"cam-verify"`, `"sync"`, `"topo"`, `"study"`, `"mesh"`, `"import"`, `"motion"`, `"motion-video"`, `"open"` |
| `label` | shown as-is ("Clearing 'Pocket', pass 2 of 4") |
| `fraction` | atomic; `< 0` is indeterminate (no bar) |
| `cancel` | atomic request set by the main thread; a job may ignore it |
| `cancellable` | whether cancel does anything; the UI and `background_jobs` read it |
| `done` | set by the worker last |
| `error`, `thread`, `started`, `reaped` | |
| `finish` | runs on the **main** thread after the join; returns `false` to be called again next frame, which is how a sliced main-thread step (the import's materialize, the motion video's pose-and-capture) fits without a second mechanism |

The destructor sets `cancel` and joins. The verbs are in
`src/ui/BackgroundJobs.h`: `StartBackgroundJob`, `BackgroundJobRunning`,
`FindBackgroundJob`, `CancelBackgroundJobs`, `JoinBackgroundJobs`,
`ListBackgroundJobs`, `ReapBackgroundJobs`.

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
the Study panel and the motion window. A modal is used only where nothing else is safe while
the job runs (an import or an open is building the document); elsewhere the
bar sits where the work was asked for and the rest of the UI stays live.

**RPC.** `background_jobs` lists what is running (kind, label, fraction,
cancellable) and `cancel_background_job {kind}` requests a cancel; for `cam`
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
| outline (`camOutlineQueue`) | one `ProjectedOutlines` projection (a profile's OCC sections of every body at each pass depth) | `OutlineKey` (`core/cam/CamPicks.h`): `bodiesKey`, the top, the pass heights and the deflection |
| adaptive | one Z level of one adaptive-clearing operation (libarea) | `AdaptiveKey` (`core/cam/CamAdaptive.h`) over the seven setup/op/tool scalars `AdaptiveClear` reads, plus the loops |
| surface | one whole 3-axis or rotary surfacing operation | `SurfaceKey` (`core/cam/CamSurface.h`): `bodiesKey` plus the fields the surfacing strategies read, the stock its entries come down to, and whether a rest pass's finish is cut before it (`FinishedBefore`) |

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
`GenerateContext::bodiesKey`, which the UI fills from `JobRenderVersion` (the
job, its sources, the active setup, the fixturing and every placement).

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

**RPC semantics: synchronous by default.** `cam_toolpath`, `cam_post` and
`cam_verify` generate through `GenerateCamToolpathsNow` (join the pass in
flight, then the cache, then compute the rest inline; `cam_verify` does so
for every setup of the job), and `cam_simulate`
settles first, so a script gets complete toolpaths. `cam_toolpath
{async: true}` defers instead and answers `{pending, queued_passes, job}`,
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

- `study` (static/modal) is not cancellable: the linear solver has no
  interruption points. `cancel_study` refuses while one runs.
- `topo` is cancellable and keeps its best iterate; `cancel_study` cancels
  it.
- `mesh_study {async: true}` is a `mesh` job; `study_status` answers
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
  independent documents and load in parallel (`parallel::For`).
- `AdoptProject(state, LoadedProject&&)` swaps the slots in, activates,
  syncs the project scope, solves an assembly and frames — milliseconds, on
  the main thread.

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

## Motion studies

A motion study's sampled solve and exact clash sweep is a cancellable `motion`
job with a fraction per sample; cancelling keeps the samples already checked,
and the result says so. Its bar is in the Motion tool window rather than a
modal, because you keep looking at the model while it runs.

A job makes the work asynchronous; the setup that hands the worker pure data
is main-thread by construction. `StartMotionRun` snapshots every document
under the assembly before the worker exists, so the cost of copying a
Document is paid on the frame.

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
