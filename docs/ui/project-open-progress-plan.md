# Opening a project off the frame, with progress

## The problem

Opening `scratch/bike/bike.dsgn` (21 parts, 19 MB) takes 20 s, and the
window is frozen for all of it: no bar, no redraw, nothing to say it has not
hung. The baked geometry that makes an open a load rather than a rebuild
(docs/versioning/baked-geometry.md) is ignored for every part, because it was
written in geometry format 1 and this build reads 2. So every part is rebuilt
from its recipe, one after another, on the UI thread:

```
Frame 3 s · Front wheel 7 s · Rear wheel 6 s · Rear rack 3 s · the other 17 ~1 s
```

The same happens whenever the blob does not match: a geometry-format bump, a
project whose `geometry/` is git-ignored, a clone, or a recipe edited outside
the app. It is the expected path for a project somebody else built, not a
corner case. And it is exactly what a STEP import already survives, with the
shared `DrawJobProgress` bar on a background job (ui/ImportJob.h).

## The shape

**Split `OpenProject` into a read and an adopt.**

- `ReadProject(path, progress) -> LoadedProject` is pure. It parses
  `project.json`, loads every document (`DocumentIO::Load`, `LoadAssembly`,
  `LoadDrawing`, `LoadJob`) and the catalogs, sections and parameters into a
  plain struct, and touches no `AppState`. This is where the 20 s go.
- `AdoptProject(state, LoadedProject&&)` is the rest of today's function, on
  the main thread: swap the slots in, activate, sync the project scope, solve
  an assembly, frame, stamp. It is milliseconds.
- `OpenProject` stays, as `AdoptProject(ReadProject(...))`, so its 19
  callers (RPC `open_project`, restore, merge, branch switch, sync, the tests)
  keep a synchronous answer and change nothing.

**The opens a person starts go through a background job.** File > Open and
the recent-files menu call `StartOpenProject`. It runs `ReadProject` on the
registry's "open" `BackgroundJob` (ui/BackgroundJobs.h), the pattern import
and sync already use, and a modal draws the shared bar:

```
 Opening bike ─────────────────────────────
  Rebuilding parts (the saved geometry is from an older build)
  [██████████░░░░░░░░░░░░]  12 / 21 parts
  Front wheel, Rear wheel
                                   [Cancel]
```

- The fraction is parts done over parts to load. The line under the bar names
  the parts being built, because one wheel is a third of the wait.
- The status says WHY it is slow when a blob was refused (format, stale hash,
  none on disk), from the reason `LoadGeometry` already logs. A project whose
  blob loads opens in well under a frame's worth of the bar, and the modal
  only appears after 0.3 s so a fast open does not flash one.
- **Cancel** discards the half-read project. The project that was open stays
  open, because nothing in `AppState` is touched until the adopt. That is a
  better cancel than import's, and it comes free from the split.
- The modal blocks input while it is up, so the project that was open
  cannot be edited under a read that is about to replace it.

**Build the parts in parallel.** Parts are independent documents with their
own shapes, so `ReadProject` loads them with `parallel::For`, one part per
worker. The bike's floor is then the slowest part (the front wheel, 7 s),
not the 20 s sum. This is the one step with a thread-safety question
(core/Parallel.h's caveat is about shared shapes, and separate documents
share none, but regeneration may touch process-wide caches). So it is gated,
not assumed:

- a test loads a multi-part fixture with `DESIGNER_SWEEP_THREADS=1` and with
  all threads and requires identical chain hashes, volumes and element names
  per body;
- one `tools/ci-local.sh --tsan` run over that test;
- if a shared cache does race, the fix is to warm it or lock it at its own
  definition, not to fall back to serial silently.

**Say what to do about it afterwards.** When any part was rebuilt because
its blob was refused, the status bar says so once the project is open:
"Rebuilt 21 parts: the saved geometry was from an older build. Saving
re-bakes it, and the next open is a load." A save already re-bakes. Nothing
tells the person that today, so they pay the 20 s on every open.

## Files

- `src/ui/ProjectActions.{h,cpp}`: `LoadedProject`, `ReadProject`,
  `AdoptProject`; `OpenProject` becomes their composition.
- New `src/ui/OpenJob.{h,cpp}`: `StartOpenProject`, the job's progress
  atomics (parts done and total, the names in flight, the refusal reason) and
  the modal, drawn with `DrawJobProgress`.
- `src/ui/FileDialogs.cpp`, `src/ui/MainMenuBar.cpp`: the two user-started
  opens call `StartOpenProject`.
- `src/core/io/GeometryCache.{h,cpp}`: `LoadGeometry` returns why it refused a
  blob, instead of only logging it.
- `src/core/io/Serialization.cpp`: nothing. `Load` is already pure, which
  is what makes the split possible.
- Tests: the serial-vs-parallel load gate (headless); a GUI test that opens a
  project with a stale blob through the menu path, sees the modal and the
  bar, and cancels back to the project that was open; one that lets it finish
  and sees the "save re-bakes" line.

## Verified by

The three suites. The new load gate at one thread and all of them, plus one
TSan run. And the bike, timed before and after: the headless `open_project`
(20.4 s now, expected near the 7 s of its slowest part), and the windowed open
from the menu, which must keep drawing and show the bar throughout.

## As built

As planned. The bike opens in **8.1 s instead of 20.4 s** (headless
`open_project`, an idle machine), against the 7 s of its slowest part, and a
windowed open from the menu keeps drawing behind the bar.

- The rebuild reason comes from `DocumentIO::InspectGeometry`, which reads
  only the blob's index, and from the regeneration's own statistics (a baked
  open counts its features cached, a rebuild starts from an empty cache).
  `LoadGeometry` is unchanged. The notice is a menu-bar item beside
  "Changed on disk", "Geometry rebuilt", with the count and reason in its
  tooltip. `open_project` answers `rebuilt_parts` and `rebuild_reason`.
- A quick open is read inside `StartOpenProject` (it waits 300 ms) and
  adopted by the loop's poll at the end of that frame, so a baked open never
  shows the modal. The reap is not called from the menu:
  `test_conventions.cpp` keeps joins out of draw paths.
- The second regeneration was measured and dropped from the plan. `Load`
  regenerates without the project's variables and `SyncProjectScope` again
  with them, but on the bike the second pass is free (a cache hit), and the
  20 s was all in the loads.
- The parallel read needed three fixes, all found by reading the
  regeneration path for process-wide state and confirmed under TSan:
  - OpenCASCADE's STEP and XCAF exchange shares one unlocked application
    and lazily filled tables, so every STEP read or write and every XCAF
    document holds `DataExchangeMutex()` (cad/XcafDocument.h). With the lock
    taken out of the structured read, TSan reports races inside the reader
    on the gate's STEP-import parts.
  - The sketch-id generator is `thread_local`.
  - PlaneGCS called `Eigen::setNbThreads(1)` from every solver's
    constructor, a write to one static from every sketch solving at once. It
    now does that once per process.
- The gate is `tests/test_open_project.cpp`: every golden document that
  loads on its own plus eight STEP-import parts, read on one thread and on
  all of them. It requires the same bodies bit for bit, the same element
  names and the same feature errors. The chain hash is compared only where it
  is reproducible at all: a sketch that projects external geometry mints
  fresh random entity ids on every rebuild, so its hash differs between two
  serial loads too.
- There was no unsaved-changes prompt on File > Open to run first. The modal
  blocks input while the read runs instead.
- GUI test: `file/a_slow_open_shows_progress_and_cancels` makes the open slow
  on any machine with a per-document delay (`OpenJobOptions`, tests only),
  cancels back to the open project, lets a second open finish with its
  rebuild notice, saves the notice away, and opens a baked project with no
  modal.
