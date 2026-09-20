# Long operations off the frame — plan (A3)

> Status: **step 0 SHIPPED 2026-09-01** (the two bugs, and the single poll
> entry point they needed); **phase 1 SHIPPED 2026-09-02** (the registry,
> `background_jobs`/`cancel_background_job`, and the CAM adaptive pocket off
> the frame — see [As built](#as-built-phase-1-2026-09-02)); **phases 2-4
> SHIPPED the same day** (the sync job, the FEM solves and `mesh_study`, the
> STEP import — see [As built, phases 2-4](#as-built-phases-2-4-2026-09-02)).
> **COMPLETE**: every long operation but the offline path tracer (not a
> thread; see below) is a registry entry. [Read against the tree](#read-against-the-tree-2026-09-02) records
> what the code grew between the plan and the build. Written after reading all five
> existing implementations, which is what changed the plan: the review
> (capability-review-2026-08-31.md, A3) said there were four cases with three
> hand-rolled answers. There are **five**, and reading them turned up two live
> bugs that have nothing to do with tidiness.

## What existed (2026-09-01, before the migration)

Kept as the record of why. Today every row below is a registry entry
(see [As built](#as-built-phase-1-2026-09-02)); only `RenderJob` keeps its
shape, by design.

Five long operations, five shapes. The rule they all follow is already
articulated, in `ImportJob.h`:

> "the worker touches ONLY pure data, and every Document mutation stays on the
> main thread."

That rule is right and is not in question here. What is unshared is everything
around it.

| job | progress | cancel | polled by | completion |
|---|---|---|---|---|
| `TopoJob` | 4 atomics (iteration, compliance, maxChange, volumeFraction) | yes, keeps best | `StudyPanel`, `RemoteSimulation`, `RemoteServer` | `optional<TopoResult>` after join |
| `StudyJob` | none | **no** — the linear solver has no interruption points | same three | `StudyRuntime` after join |
| `ImportJob` | phase enum + fraction + partsDone/Total | yes | the **frame loop** (main.cpp, twice) | plan + model, then a *sliced* main-thread materialize |
| `SyncJob` | none | no | **`HistoryPanel`, inside a `TreeNode`** | ok/error after join |
| `RenderJob` | plain bools, main-thread sliced | yes | inline in main.cpp | path/error |

Four different poll drivers, three progress vocabularies, two of five
cancellable, and one that is not a thread at all.

## The two bugs this turned up

Both are the same defect, and neither needs the mechanism below to fix.

**A collapsed section stops a push from completing.** `PollSync` is called at
`HistoryPanel.cpp:686`, inside `if (ImGui::TreeNode("Sharing"))`. Collapse that
section — or close the History panel — during a fetch, push or clone, and
nothing ever joins the worker or reports its result. The operation finishes and
the app never finds out.

**Leaving Simulation mode strands a solve.** `PollTopologyRun` and
`PollStudyRun` are called from `DrawTopoResults`, which `UI.cpp:801` draws only
while `state.document && state.simulation.IsActive()`. Switch to an assembly
tab or leave Simulation mode mid-solve and the result is not stored until you
come back. An RPC client still reaps it (`RemoteSimulation` polls too), which
is why this has survived: it is invisible to every scripted test and only bites
a person clicking.

**Fix these first, and separately.** Moving a poll out of a `TreeNode` is one
line, and holding a one-line bug fix hostage to a refactor is how both get
delayed. They also make the case for the rest of this document better than any
argument about duplication does.

### Shipped 2026-09-01

`ui::PollBackgroundJobs` (`src/ui/BackgroundJobs.h`) joins whatever finished,
applies it, and advances whatever is sliced. The app's **three** loops call it
once each — the windowed frame loop, the `--offscreen` frame loop, and the
`--headless` pump — and nothing under `src/ui` calls the individual polls any
more. The headless pump did not poll at all before, so a job there could only
finish when a client happened to ask; the RPC handlers keep their own polls,
deliberately, because a handler must apply a finished result before it answers
a question about it.

Gated by `a background job is polled by the loop, never by a panel`
(`test_conventions.cpp`), which scans `src/ui` for calls to any of the five
joins. This is checked rather than remembered because the mistake is invisible
in review — a poll call looks equally at home in a draw function. Verified by
sabotage: putting `PollSync` back into the History panel fails it with
"src/ui/HistoryPanel.cpp calls PollSync".

**One case this does NOT fix**, named rather than left to be rediscovered: the
FEM polls need a `Document`, so a solve started on a part tab is still not
reaped while an assembly tab is active. That needs the job to stop being
addressed by "the active document", which is phase 3's business.

## The mechanism

One registry, owned by `AppState`, polled from **`PollBackgroundJobs` and
nowhere else** — which step 0 made the three loops (windowed, `--offscreen`,
`--headless`) call once each, with the RPC handlers keeping their own polls
for the reason given above. Where a job is polled stops being a property of
which panel is open.

```cpp
struct Job {
  std::string kind;   // "import" | "topo" | "study" | "sync" | "cam" | …
  std::string label;  // "Clearing pocket 2 of 5" — shown as-is
  std::atomic<double> fraction{-1.0};  // <0 = indeterminate, no bar
  std::atomic<bool> cancel{false};     // main sets; a job may ignore it
  std::atomic<bool> done{false};       // worker sets last
  std::string error;                   // read after done + join
  std::thread thread;
  // Runs on the MAIN thread after the join. Returns false to be called again
  // next frame — which is how ImportJob's sliced materialize fits without a
  // second mechanism.
  std::function<bool(AppState&)> finish;
  ~Job() { cancel.store(true); if (thread.joinable()) thread.join(); }
};
```

Three things this deliberately does **not** try to unify:

- **The progress vocabulary.** `fraction` plus a `label` the domain writes is
  the common denominator; `TopoJob`'s compliance and `ImportJob`'s
  partsDone/Total stay where they are, because a shared struct with every
  field any job might want is how the abstraction rots.
- **Cancellability.** `StudyJob` cannot be interrupted and saying so is
  honest. `cancel` is a request; a job that ignores it reports that it does.
- **The result type.** It lives in the domain's own struct and is consumed by
  that domain's `finish`. The registry never sees it.

### What it buys immediately

An RPC surface that works for every job rather than one: `background_jobs`
(list what is running, with kind, label, fraction) and
`cancel_background_job {kind}` — not `jobs`, because on this API a "job" is
already the CAM job document (`add_job_part`, `list_job_parts`), and an agent
reading `jobs` would expect those. Today
`study_status` is bespoke, and `mesh_study` has no async path at all —
[known-bugs.md](../known-bugs.md) records a modal study outliving the client's
60 s socket timeout, which "LOOKS like a hang followed by a mystery". With the
registry, `mesh_study` gets polling for free rather than growing a second
bespoke status verb.

## Order

Deliberately not a big-bang rewrite of five working things — the refactoring
policy is "split what the next feature extends", and four of these five work.

All done, in this order, 2026-09-01/02:

0. **The two bugs above**, now, independently of everything else —
   `65a4159c`.
1. **The registry, one poll site, and `background_jobs`/
   `cancel_background_job`** — with the CAM adaptive pocket as the only
   adopter. Nothing existing changes, and the new job proves the shape
   before anything is migrated onto it — `a2587a27`.
2. **`SyncJob`** — the worst-affected and the simplest (no progress, no
   cancel). If the shape cannot carry it, that is cheap to discover here —
   `1ef01cbf`.
3. **`TopoJob` + `StudyJob`** together: they share a panel and a poll site, and
   splitting them would leave the panel polling two ways — `777f1e0d`.
4. **`ImportJob`** last — the sliced materialize and the modal make it the
   hardest, and by then the registry has been proven three times —
   `1a65d0a9`.

`RenderJob` is not a thread; whether it belongs here at all is a question for
when the rest is done, not now.

## Phase 1 in detail — the CAM adaptive pocket

The first adopter, and the reason A3 is worth doing now rather than later:
generating an adaptive pocket takes **~7 s on the UI thread**
([known-bugs.md](../known-bugs.md)), on the operation a person adds and then
adjusts. It is the one big cost in CAM that is not incremental.

It is also already diagnosed, which removes the usual first day of work.
`tools/profile.sh --test 'cam: clearing the outline passes over holes'`
attributes **25% of self time to `Adaptive2d::CalcCutArea`** and about 30% more
to Clipper's scanbeam underneath it — vendored library, inside
`Adaptive2d::Execute`, with no interruption points of its own.

That last part shapes the work:

- **Cancel is coarse.** libarea cannot be interrupted mid-`Execute`. The job
  can refuse to *start* the next operation, and can drop a finished result the
  user has already cancelled, but a single adaptive op runs to completion.
  Report that rather than offering a Cancel that does nothing.
- **The backplot shows the PREVIOUS toolpath while a new one is generating.**
  Blanking it would be worse than the freeze: an empty backplot reads as "this
  operation produces nothing", which is a real state the panel already has to
  express.
- **The operation row owns the progress**, not a modal. A modal is right for
  import (the document is being built and nothing else is safe) and wrong here
  (the rest of the job is still editable).
- **The cheap half first, and possibly instead.** Caching by operation
  parameters + document version would make a re-open or an unrelated edit never
  re-run libarea at all. If that removes most of the observed cost, the worker
  becomes a smaller change against a smaller problem — measure before building
  both.

## Two things this is NOT, and the escape hatch it needs

Asked directly whether a job registry is the right shape at all, or whether the
render thread should be separated instead. Both worth answering here, because
the answers bound what this plan is.

### A render thread would not fix any of this

The five jobs are TASK parallelism; a render thread is frame-loop structure.
None of the measured stalls — 7 s adaptive pocket, 5.9 s resync, 259 s open —
is present-bound, so separating render fixes none of them.

With an immediate-mode UI it would be actively worse. ImGui rebuilds the
interface every frame from `AppState` on the main thread, so during a 7 s
generate a render thread keeps presenting the LAST frame: a smooth,
responsive-looking window that ignores every click. An honest freeze is better
than that. And the responsiveness people reach for a render thread to get is
precisely what moving the work off the main thread gives — for real, not
cosmetically.

Four more reasons specific to this tree: the renderers read `AppState` and the
`Document` directly, so threading them means snapshotting their whole input
surface per frame; the idle throttle already rests at ~10 Hz, which moots most
of the benefit; Vulkan resource lifetime gets much harder in a codebase that
has already shipped a device-lost from freeing an in-use buffer; and frame
timing already says the GPU is not where the time goes.

**One future candidate, and it is not the viewport.** `RenderJob`, the offline
path tracer, has no thread at all — it slices on the MAIN thread. That is real
render work stealing main-thread time, and it is the one place a background
render thread would earn itself. Not now.

**What others do**, for calibration: Blender's `WM_jobs_*` is almost exactly
the shape below — named job types, progress, cancel, one of a kind at a time —
with `BLI_task` pools for parallelism inside a job and the UI on one thread.
FreeCAD is the counterexample: ad-hoc `QThread`/`QtConcurrent` per feature and
a lot of modal dialogs. Games separate render because they are present-bound
against a fixed budget; an app that rests at 10 Hz is not.

### Data parallelism is a different mechanism, and is also duplicated

A job is a user-visible operation. How one job uses the cores is a separate
question, and it already has two hand-rolled answers:
`AssemblyActions.cpp:1121` (parallel tessellation — half the cores capped at 8,
with a hard-won note about oversubscribing a mesher that is already internally
parallel, and about OCC's per-thread signal handler) and `IBL.cpp:114`. Each
builds and joins its own `std::vector<std::thread>`.

That wants a shared pool eventually. It is **not** this plan, and conflating
the two is how a job registry turns into a scheduler nobody can reason about.
Named here so the next person does not solve it by widening `Job`.

### The escape hatch for uninterruptible work

This plan accepts coarse cancel because libarea's `Execute` has no
interruption points. If that proves unacceptable, the answer is the one IDEs
use for language servers: run it in a **separate process** and kill it. That is
genuine cancellation of third-party code that offers none, at the cost of
serialising the input and output across a pipe. Written down now so it is a
considered fallback rather than a rediscovery.

## Read against the tree (2026-09-02)

The plan was written 2026-09-01. The router round (R1–R7,
[../cam/router-round.md](../cam/router-round.md)) shipped the next day, and
three of its seven items touch phase 1. Read against the code before building,
this is what the plan had not said — in order of how much it changes the work.

### The seam is narrower than "the worker touches only pure data"

`cam::AdaptiveClear(setup, op, tool, region, z, fromZ)` reads exactly seven
scalars — `setup.safeZ`, `op.stepover`, `op.stockToLeave`, `op.side`,
`tool.diameter`, `tool.feedPlunge`, `tool.feedCut` — plus the loops. The OCC
sectioning that produces those loops runs on the line before the call, in
`GenerateAdaptive`. So the worker runs **libarea and nothing else**, on copies
of plain structs; no OCC shape crosses a thread, and the per-thread OCC signal
handler that parallel tessellation had to install (`AssemblyActions.cpp`)
does not apply. None of the 17 edit sites change: every one of them already
goes through `RefreshCamToolpath(s)`, and that is where the split goes.

### Content keys, not versions — and that answers three items at once

Rather than a job that "belongs to operation 7 of setup 2 of tab 4", the
result is keyed by a **hash of exactly those inputs**: a `cam::AdaptiveCache`
on `AppState`, and `Generate` takes a `GenerateContext` that either fills the
cache synchronously or, when `defer` is set, records the requests it skipped
and marks the toolpath `pending`. The worker consumes that queue one pass at
a time and its `finish` puts the result in the cache and re-runs the (now
cheap) generate. Three of the findings below fall out of this:

- **Document affinity (phase 1, not phase 3).** `state.camToolpaths` and
  `ActiveCamSetup` are per ACTIVE tab, so a result addressed to "the active
  job" lands in the wrong tab after a switch. A content-keyed cache has no
  tab: whichever job tab is active when a pass lands regenerates from its
  own document and picks up whatever is cached. The FEM caveat in step 0
  stands for phase 3; it never applies here.
- **The stale-result race.** The motivating case is "adds and then adjusts",
  so the second edit arrives while the first pass runs. A result for the old
  parameters is simply a cache entry nobody reads; the regenerate after it
  lands sees the new parameters, misses, and queues them. One in-flight pass
  at a time; a key already queued or in flight is not queued twice; a drag
  therefore costs one pass per distinct value that survived to a frame, not
  one per frame.
- **Undo and re-open (R7).** `JobUndoGuard` restores the job document and
  `RestoreJob` regenerates everything, so undoing an adaptive edit used to
  cost the 7 s again. With the cache it is a hit. The undo guard's own
  fingerprint is whole-job and stays that way — it answers "did anything
  change", not "what is this pass worth".

The key is computed in `CamAdaptive.cpp` next to the reads it covers, and
`test_conventions` checks that every `op.`/`tool.`/`setup.` field that file
reads appears in the key function — the drift this design is exposed to is a
new read that the key does not know about, and that is invisible in review.

### R2 and R6: a full pass is now the common case

A finish-tool operation regenerates with a FULL pass (`RefreshCamToolpath`
falls through to `RefreshCamToolpaths` because tool order sorts across the
two toolpaths), and the posted file is only right once every toolpath has
landed. So the unit of "pending" is the toolpath, the backplot keeps the
previous moves of a pending toolpath rather than blanking them, and
everything that consumes the list — post, verify, simulate, and the RPCs
that wrap them — asks `CamToolpathsPendingWhy` first and refuses in words.

### RPC semantics: synchronous by default, `async` opt-in

`cam_toolpath`, `cam_post`, `cam_verify` and `cam_simulate` are synchronous
today and `headless_smoke` plus every benchmark run depends on that. They
stay so: a reader RPC **settles** first — joins the in-flight pass, computes
the rest of the queue inline, regenerates — and answers with complete
toolpaths. `cam_toolpath {async: true}` defers instead and answers with what
is pending, for a client that would rather poll `background_jobs`. Mutating
RPCs (`set_cam_op`, `set_job_placement`, …) defer like the panel does, so an
agent editing a job on the human's windowed instance no longer freezes it.

### Cancel, quit and close

Cancel is coarse, as planned: it empties the queue and lets the pass now
cutting finish (its result is cached; it was paid for). The toolpaths that
were waiting stop being pending and say they were cancelled — the next edit
or Generate queues them again. The registry entry joins its thread on
destruction, so closing the app or the tab mid-pass blocks for the remainder
of ONE pass, not the whole operation — accepted, and said here rather than
discovered.

### The gate needs a small adaptive operation

"Generating an adaptive operation does not block a frame" is measured in the
GUI suite on an operation sized for a test, not the 7 s baffle pocket: the
assertion is about frame time while a pass is in flight, and the pass has to
be long enough to span several frames and short enough to keep the suite's
budget.

## As built (phase 1, 2026-09-02)

What is in the tree, and where the plan bent:

- **`ui/BackgroundJob.h`** is the struct from "The mechanism" plus a
  `cancellable` flag and a `reaped` marker; **`ui/BackgroundJobs.h`** grew
  the registry verbs (`StartBackgroundJob`, `BackgroundJobRunning`,
  `CancelBackgroundJobs`, `JoinBackgroundJobs`, `ListBackgroundJobs`) and
  `PollBackgroundJobs` reaps the registry (after `PumpRenderJob`, the one
  legacy poll left once phases 2-4 landed). A
  `finish` may start a successor — that is how a queue of passes advances —
  so the reap walks by index over the entries that existed when it began.
- **`cam::GenerateContext`** (`CamToolpath.h`) is how a generate says what
  it would have computed: with a cache, passes are looked up; with `defer`
  as well, misses are appended to `pending` and the toolpath comes back
  `Toolpath::pending`. Every depth pass of an operation is queued in one go,
  top first, and nothing after the queueing runs on a partial result. No
  signature outside the generate family changed.
- **`cam::AdaptiveRequest` / `AdaptiveKey` / `AdaptiveCache`**
  (`CamAdaptive.h`) live beside the seven reads they cover, and
  `CamAdaptive.cpp` now compiles in every CAM build (it used to be dropped
  without libarea, which would have left `AdaptiveAvailable` unresolved).
  The cache is bounded at 256 entries and two million moves, oldest out.
- **`ui/CamJobs.cpp`** is the worker glue: `QueueAdaptivePasses` (dedupe
  against cache, queue and in-flight), one `cam` job per pass with the label
  "Clearing 'Pocket', pass 2 of 4", a `finish` that caches the result and
  regenerates whichever job tab is active, `SettleCamToolpaths` for the
  synchronous RPC path, `CancelAdaptivePasses`, and `CamToolpathsPendingWhy`
  which post, verify, simulate and Save G-code all ask first.
- **Verification waits**: `RunCamVerification` on a pending list records
  "Not verified yet: …" and sets `camVerifyWhenSettled`; the last pass's
  `finish` runs it.
- **The GUI suite generates inline** (`ResetAppForTest` sets
  `camDeferAdaptive = false`): thirty tests read `camToolpaths` right after a
  pick, and the deferred path is the subject of exactly one of them.
  `cam/adaptive_clears_off_the_frame` turns it back on and measured, on a
  60 x 40 plate with a 30 x 20 x 8 pocket: **the pick returns in 0.5 ms; 193
  frames run while the passes cut; the worst frame is 31 ms.** The headless
  twin (`test_actions`, "an adaptive pocket is cleared off the frame and
  lands complete") pumps the registry the way `HeadlessMain` does and checks
  landing, cache hits, keep-previous, settle and cancel against a
  synchronous generate.
- **RPC**: `cam_toolpath`, `cam_post` and `cam_verify` generate through
  `GenerateCamToolpathsNow` (join the pass in flight, then the cache, then
  inline); `cam_simulate` settles the active list first; `cam_toolpath
  {async: true}` defers and answers `{pending, queued_passes, job}`.
  `background_jobs` and `cancel_background_job` are server-level and in
  `kNoPartDoc`.

Not done here, on purpose: phases 2-4 (SyncJob, TopoJob + StudyJob with
`mesh_study` as the free adopter, ImportJob) — each a migration onto a shape
now proven once. And the perf HUD does not yet show the registry; the panel
row does.

## As built, phases 2-4 (2026-09-02)

- **Phase 2, SyncJob** (`1ef01cbf`): the struct is plain data — the
  worker's whole world, copied in and read back after the join the registry
  does. Its thread and completion are a "sync" entry whose `finish` applies
  what `PollSync` used to (divergence, the merge half of a pull, opening a
  clone). `SyncRunning` asks the registry; the `syncJob` slot is gone. The
  shape carried it without change, which was the point of doing it second.
- **Phase 3, TopoJob + StudyJob**, and the affinity fix step 0 could not
  make: each payload records the **part slot id** it was started from and
  its `finish` resolves that slot (`PartDocById`), so a solve started on a
  part tab lands in that part whichever tab is active — or is dropped with a
  warning if the tab was closed. `state.topoJob`/`studyJob` stay as
  `shared_ptr` payloads the panel's status reads while the job runs; the
  registry owns the thread. The RPC pump and the simulation handlers call
  `ReapBackgroundJobs` where they used to poll two functions with "the
  active document". `cancel_study` sets the "topo" job's cancel.
- **Meshing left the frame too.** `PrepareStudy` was already split at the
  solve, but it MESHED on the main thread, so an async `run_study` still
  held a frame for the length of a Netgen mesh. It is now `PlanStudy`
  (expressions, refs, materials, and a **deep copy** of each shape) plus
  `MeshPlannedStudy` (mesh, fixed nodes, tractions — plain data, on the
  worker). The copy is not optional: Netgen attaches triangulation to the
  faces it meshes, which on the document's own shape is a write under the
  renderer's reads. `MeshStudy` got the same split (`PlanStudyMesh` +
  `MeshPlanned`), and `test_fem` holds both pairs to their inline originals
  — same nodes, tets, supports and tractions, and `!IsSame` on the shape.
- **`mesh_study {async: true}`** is a "mesh" job. `study_status` says
  `{running, meshing: true}` until it lands and then carries `mesh: {bodies,
  seconds, error?}` from a `MeshReport` keyed by (part slot, study). This
  closes the known-bugs entry about `mesh_study` outliving the client's
  socket timeout: the synchronous form still exists for a caller who wants
  it, and the describe row now says which to use.
- **Phase 4, ImportJob**: the payload keeps its phase/fraction/parts atomics
  for the modal; the worker's `cancel` is the registry's, and the sliced
  materialize is the job's `finish` — called once after the join to read the
  worker's verdict (a feature import, a failure, a cancel, or "materialize"),
  then once per poll, returning false until the last part. The moment the
  materialize starts it flips the entry's `cancellable` off, which is what
  the modal and `background_jobs` both read. `PollImport` is gone; the
  convention gate's join list is now `PumpRenderJob` and `ReapBackgroundJobs`
  (the latter is the registry's own reap, which the remote layer may call).
  Gated headless in `test_actions` (a 3-product STEP lands as three part
  tabs and an assembly, a slice a poll) and by the existing GUI case
  `assembly/a_step_import_runs_in_background` (the modal stays up across the
  part→assembly tab switch).
- Not done, and not owed by this plan: `PrepareTopology` still meshes on
  the main thread (the same split would apply; no one has hit it); the perf
  HUD does not list the registry; `RenderJob` stays a main-thread slicer
  (see "Two things this is NOT").

### Adopters since (2026-09-10)

The registry grew two entries this plan did not write. Recorded here because
this document is where someone looks for "what are the app's long operations",
and `ui/BackgroundJobs.h` is where someone looks for the list.

- **`"mesh"`** — the FEM mesh, alongside `"study"` and `"topo"`.
- **`"motion"`** (G2) — a motion study's sampled solve and exact clash sweep.
  `cancellable = true`, a fraction per sample, and it is the only adopter
  besides the import whose progress a person can see: an in-panel bar in the
  assembly Model panel rather than a modal, because unlike an import you want
  to keep looking at the part while it runs. Cancelling keeps the samples
  already checked and the result says so.

  Motion is also the case that showed what the registry does NOT cover. The
  job was correct and off the frame from the day it landed, and pressing
  Validate still froze the app for seconds — because `StartMotionRun`
  snapshots every document under the assembly BEFORE the worker exists, and a
  Document copy used to re-execute its whole feature list. A registry entry
  makes the *work* asynchronous; the setup a job needs to be handed pure data
  is main-thread by construction, and it is not free by construction
  (docs/performance/motion-prep-copy.md).

## Out of scope

**Regeneration itself.** `Document::Regenerate` is synchronous on the calling
thread and stays that way here. Making it async is not a job-registry question
— it is a question about the document model's single-threaded mutation
assumption, which checkpoints, undo, the golden corpus and every RPC handler
rest on. That is A2's neighbourhood, not this one.

## Gates

- The two bugs get a test each: a sync job completes with the History panel
  closed, and a study job is reaped after leaving Simulation mode. Both are
  headless-testable through the actions layer (`tests/test_actions.cpp`).
  **Done, one step further**: "a fetch runs on the registry and reports
  through the loop" (no panel at all) and "a study solved on the registry
  lands in its own part, not the active tab" (not merely a mode left — a
  different tab active when it lands).
- `background_jobs` and `cancel_background_job` join `rpc_params_gate` like
  every other RPC. **Done.**
- The CAM adaptive job asserts what the freeze cost: generating an adaptive
  operation does not block a frame. The GUI suite can measure that directly —
  it already asserts frame behaviour in `shell/idle_throttle_rests_and_input_wakes`.
  **Done**: `cam/adaptive_clears_off_the_frame` (pick 0.5 ms, worst frame
  31 ms while the passes cut).

## Phase 5 — surfacing off the frame (2026-09-20)

`GenerateContext`'s own comment says it decides "how a generate treats **the
one expensive step**, adaptive clearing". That stopped being true at v118 and
is now emphatically false: a 3-axis finishing pass and a continuous 4-axis one
both run INLINE, on the frame, and the rotary rolling pin takes **seventeen
minutes**. The frame HUD on that job shows single `ui` frames of 10 s and
72 s — the app is not slow, it is stopped.

Everything phase 1 built is reusable and none of it is adaptive-specific in
principle: one `BackgroundJob` of kind `cam`, a queue with in-flight
deduplication, `Toolpath::pending` so post/verify/simulate refuse rather than
act on a list that is not yet the job, a done/total for the progress row, and
cancel. What is adaptive-specific is only the *payload*.

**The unit differs and that is fine.** Adaptive defers a PASS — one Z level of
one operation, because that is what costs seconds. Surfacing defers the whole
OPERATION, because a finishing pass is one computation from the cutter-location
surface outward and has no natural interior seam. Both are "a keyed
computation whose result a later regenerate reads out of a cache", which is
the only shape the queue actually needs.

So the queue becomes generic over that shape rather than gaining a second
copy of itself — two queues drift, and this one already has the subtle parts
(deduplicate against cached AND in-flight, land on whichever tab regenerates
next, keep a cancelled pass's result because it was paid for).

### What makes surfacing safe to move

It was written for this without the move being taken. The patterns are "plain
values in, polylines out" with no `Setup`, no `Document` and no OpenCASCADE
(`CamSurface.h` says so and gives the rotary reason), and every model sample
goes through `GenerateMeshDetached`, which deep-copies before meshing
precisely because a triangulation rides on the shared `TShape`. A request can
therefore carry the bodies it needs.

### The key

Content, as everywhere here. The core cannot fingerprint geometry cheaply, so
the CALLER supplies one: `GenerateContext::bodiesKey`, which the UI fills from
`JobRenderVersion` — the mix it already keeps of the job, its sources, the
active setup, the fixturing and every placement. The core hashes that with
exactly the `setup.`/`op.`/`tool.` fields the surfacing strategies read, and
the same gate the adaptive key has applies: a field read but not hashed is a
stale result nobody can see.

### Not in scope

Cancel mid-pass. The drop cutter has no interruption points either, so
cancelling abandons the *queue* and lets the pass in flight finish, exactly as
adaptive does today.

### As built (phase 5, 2026-09-20)

Measured by `cam/surfacing_runs_off_the_frame`, the twin of phase 1's own
case and deliberately so — the properties that matter are the same three, and
a second mechanism watched a different way would be a second mechanism:

```
generate returned in 0.1 ms
7 frames while surfacing, worst 18.2 ms
```

**One worker, two payloads.** `StartNextCamPass` drains the clearing queue
first and then the surfacing one. Not arbitrary: a finishing pass is planned
against the PART, not against what roughing left, so neither waits on the
other — but clearing is seconds and surfacing is minutes, and filling the
cheap ones first means the backplot grows steadily instead of sitting empty
through the long one.

**The names stopped lying.** `camDeferAdaptive` → `camDeferPasses`,
`camAdaptiveBatch*` → `camPassBatch*`, `AdaptiveProgressOf` →
`CamPassProgressOf`, `CancelAdaptivePasses` → `CancelCamPasses`. The queue
was never adaptive-specific in anything but its payload, and a name that
describes one of two kinds is the kind of thing that grows a second copy of
the machinery beside it.

**The key gate has a twin too** (`test_conventions.cpp`, "the surface key
covers every field the pass reads"), read against the two surfacing
generators rather than the whole of `CamStrategies.cpp` — hashing the fields
a profile reads would be a key that invalidates on things surfacing never
looks at. Checked by deleting `hash.D(op.stepover)`: it names the field.

