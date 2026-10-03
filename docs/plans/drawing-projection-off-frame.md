# Drawing projection off the frame

## The problem

Every drawing projection runs on the main thread. Switching to a drawing tab
whose views have never been projected this session calls
`UpdateDrawingRenders(state)` with no budget from `ui::SetActivePart`
(`src/ui/DocumentSlots.cpp`), so every view's exact HLR, its section cut and
its decorations run inside one frame. On `scratch/pantilt/drawings.dsgn` the
switch from `print sheet` to `pantilt drawing` froze the user's app for
43.7 s (watchdog: `HLRBRep_InternalAlgo::HideSelected` under
`ProjectDrawingView` under `SetActivePart`).

What that tab costs, on an offscreen instance with nothing else running:

| view | kind | quality | hidden lines | outline | hide (self + pair) | pairs | dearest occupants |
|---|---|---|---|---|---|---|---|
| front | assembly, front | exact | off | 1.34 s | 10.6 s (5.1 + 5.5) | 4032 | pan worm 3.9 s, pan wheel 2.8 s, tilt wheel 2.2 s |
| C-C | assembly, top, section | exact | off | 0.46 s | 4.8 s (0.7 + 4.2) | 240 | pan wheel 4.6 s |
| B-B | assembly, right, section | exact | off | 0.47 s | 2.3 s (1.3 + 1.0) | 812 | tilt worm 1.3 s |

All three hide occurrence 14. The tab switch took 24.6 s, of which HLR is
20.0 s; the rest is gathering the 64 bodies, the two section cuts (an OCC
boolean per body) and assembling the sheet. The other two drawings are cheap:
`print sheet` (seven `fast` iso views of single parts, 5-99 ms each, 0.33 s
with the open) and `base_plate drawing` (three exact views and a detail of
one part, 27 ms; the detail reuses its parent's projection).

What is already cached, and what is not:

- **A second visit is free** (3 ms): the tab's `drawingRenders` are stashed
  with its slot, and an `update_drawing` with nothing changed is all
  projection-cache hits (4 ms).
- **A reopen pays it all again.** `ProjectionCache` is session-local and the
  drawing file holds only the views' definitions, so reopening the saved
  project (whose active tab was now the pantilt drawing) took 23.0 s inside
  `open_project`, every self and pair computed afresh. The GUI's open is
  off the frame only until its job lands: `AdoptProject`
  (`src/ui/ProjectActions.cpp`) runs `UpdateDrawingRenders` on the main
  thread when the adopted tab is a drawing, so File > Open of that project
  shows its progress modal, closes it, and then freezes for the same 23 s.

Nothing runs a projection off the main thread today. `ProjectDrawingView` was
split into the HLR (`drawing::ProjectView`, pure in its input shapes) and
`AssembleSheetView` (needs the live documents) precisely so it could, and
`tests/test_drawing_hlr.cpp` shows deep copies project to the same curves and
names, but no caller hands the HLR to a worker
(docs/drawings/README.md#projection-hlr). Three things stand in the way:

- the projection reads the documents' own shapes: `fast` HLR meshes them, and
  the section cut is an OCC boolean, which writes p-curves onto its
  arguments; a worker doing either while the frame meshes or picks is a heap
  corruption;
- `ProjectionCache` is single-threaded and keyed by shape identity, and the
  section cuts live in a function-static cache in `CachedCut`;
- eleven call sites invoke the synchronous update directly.

## The fix

### 1. A `drawing` background job

Projection becomes a job on the registry (docs/ui/long-operations.md), kind
`"drawing"`, one at a time. `ui::RequestDrawingUpdate(state, slotId)` is the
entry point every GUI path uses:

- **On the main thread, before the worker exists:** gather each view's
  bodies (`GatherSourceBodies`, `assembly::Flatten`) and hand the worker
  **detached copies** (`CADShape::DetachedUnlocated`, as `job-warm` does).
  Copies are memoized per source shape and document revision in a
  `DrawingShapeMemo` owned by `AppState`, so the next update hands the
  worker the *same* copies and the projection cache keeps hitting; a
  revision that moved drops its copy. If copying a large assembly turns
  out to cost more than a frame, it is sliced across frames through
  `finish` returning `false`, as the import's materialize is.
- **On the worker:** for each view in turn, the section cut (on the copies)
  and `drawing::ProjectView`, with the job's `cancel` wired to
  `ProjectionOptions::cancelled`. The job takes the `ProjectionCache` and the
  cut cache (moved out of `CachedCut`'s static into the same object) for its
  lifetime and gives them back in `finish`, so the main thread never touches
  them while a worker does. `ProjectView` grows a progress callback beside
  `cancelled`, reporting pairs done over pairs owed; the job's `label` is
  "Projecting 'front' — pair 1200 of 4032" and `fraction` is views done plus
  the current view's share.
- **In `finish`, on the main thread:** resolve the drawing by its slot id
  (`PartDocById`'s pattern, so a job that lands after a tab switch writes
  the stashed slot's renders, and is dropped if the tab was closed), run
  `AssembleSheetView` and the decorations per landed view, and record each
  view's source version as today. A view whose source changed while it
  projected lands **stale**, as any last-good view does; projection stays
  explicit.

Views land one at a time: the worker publishes each finished
`ProjectionResult` and `finish` returns `false` until the last, so the front
view appears after 12 s rather than all three after 20.

**What the sheet shows meanwhile.** A view with previous geometry keeps it
(the existing last-good rule) under a "projecting" badge; a view with none
draws a placeholder: its frame at the bounds of its source's bounding box
projected along the view direction at its scale, captioned with the view
name. The progress bar is the one every other long operation draws
(`DrawJobProgress` in `src/ui/Widgets.cpp`: the same bar, overlay text and
Cancel as opening a project, generating a toolpath or a still render). It
sits on the drawing panel's view list, not in a modal: the sheet stays
usable.

**One progress bar everywhere.** The requirements panel draws its own
`ImGui::ProgressBar` with no Cancel; it moves onto `DrawJobProgress`, its
job's cancel wired to the button. `test_conventions.cpp` then fails on any
`ImGui::ProgressBar` outside `Widgets.cpp`, so a new long operation cannot
grow a bar of its own. A
dimension or note whose view is still projecting is **pending**, not
dangling: drawn in the pending style, never reported by `check_drawing` as
broken, never dropped.

**Cancel** keeps every view that landed, leaves the rest at last-good or
placeholder with the error "projection cancelled — Update views to project",
and keeps the finished self and pair entries in the cache (already true of a
cancelled projection). `background_jobs` lists the job and
`cancel_background_job {kind: "drawing"}` cancels it.

### 2. Projections saved with the drawing

A reopened unchanged project should draw at once. The drawing's directory
form gains, per view, its last `ProjectionResult` — model-space curves with
their index-space `SourceRef`s, the conic data, and for a section view each
body's cut loops and edge origins — beside the view's definition:

```
drawings/<name>/views/<id>-<slug>.projection.json   input_hash, bin_bytes, array refs
drawings/<name>/views/<id>-<slug>.projection.bin    f32 points, u16/u32 indices
```

written through the `ResultArrayWriter` / `ResultArrayReader` pair the
study results use (docs/simulation/README.md), float because it is derived
bulk data. What is stored is the projection, not the `SheetView`: placement,
scale, rotation, folding and element names are re-derived by
`AssembleSheetView` from the live documents, which is cheap and is the half
that must stay current anyway.

**Keyed by the inputs' hash.** `input_hash` is `inputhash::Hasher` over
exactly what `ProjectView` and the cut read: per input body in gather order,
its exact B-Rep (`Hasher::Shape`, memoized per shape identity), its
placement and its group id; the view direction and up; the mode; the
deflection; hidden lines; the section point and plane; and the OCC version
plus `drawing::kProjectionRevision`, a constant in `DrawingProjection.h`. The
revision is held honest by a test that projects a fixed fixture and compares
a digest of the result with a recorded pair (revision, digest): changing what
`ProjectView` returns without bumping the revision fails it. The B-Rep hash
is what guarantees the stored `SourceRef` edge indices still name the same
edges; a version counter or the recipe's chain hash does not survive a
rebuild under another OCC.

On open, the stored projections are read into the drawing slot as candidates.
The first projection job checks each candidate's hash on the worker, over
the same detached copies, and a match is installed with no HLR; a mismatch
is discarded and that view projects. The hash is not checked on the frame.
A save writes a view's files only when its bytes differ, writes none from an
autosave, and keeps them out of the project history, as study results are
(docs/versioning/README.md#saved-results-stay-out-of-the-history).

**The format moves.** This is new saved content in the document's directory
form, as study results were at v153, so it bumps `kVersion`, adds a row to
docs/versioning/format.md and a drawing golden at that version carrying a
stored projection, whose expectation checks it loads as a candidate and that
a projection with a foreign `input_hash` is ignored, not refused. The single
file form carries none.

### 3. Every caller of the synchronous update

The synchronous function stays, renamed `UpdateDrawingRendersNow`, for the
callers that may wait. It first joins a `drawing` job in flight and takes its
caches back. A new `test_conventions.cpp` rule fails if any file under
`src/ui` other than `DrawingActions.cpp` calls it: the frame never waits for
HLR.

| caller | today | becomes |
|---|---|---|
| `SetActivePart` (`DocumentSlots.cpp`), first visit to a drawing tab | full projection on the frame | `RequestDrawingUpdate` |
| `AdoptProject` (`ProjectActions.cpp`), opened with a drawing tab active | full projection on the frame | `RequestDrawingUpdate`; installs the saved projections whose hashes match |
| `drawing.update` (`Commands.cpp`), the toolbar's Update views | full projection | `RequestDrawingUpdate` |
| `DrawingPanel.cpp`, a view's Scale or Rotate | full update, all cache hits | `ReassembleDrawingViews`: `AssembleSheetView` from each view's kept `ProjectionResult`, no HLR; a view with none is requested |
| `DrawingPanel.cpp`, Delete view | full update | erase that view's render; nothing projects |
| `DrawingAnnotate.cpp`, accepting a detail | full update | reassemble: a detail reuses its parent's projection; requested only if the parent has none |
| `AddDrawingViewPlaced` from the Add view dialog (`DrawingPanel.cpp`) | a probe projection then a full update, both on the frame | the view is added at a provisional position and requested; the auto-placement runs in that view's `finish`, once its bounds are known, as one undo step with the add |
| `LayOutBalloons` (`DrawingAnnotate.cpp`, `Commands.cpp`) | full update | reads the landed geometry; while a named view is still projecting the action is disabled and says why |
| `ExportActiveDrawing` from File > Export (`FileDialogs.cpp`) | full update | requests the update and writes the file in its `finish`, the progress shown; an export never writes a stale or half-projected sheet |
| `update_drawing`, `check_drawing` (update on), `export_drawing`, `set_drawing_view`, `remove_drawing_view`, `add_drawing_view` placed, `layout_balloons` (`RemoteDrawings.cpp`) | synchronous on the frame | still answer with complete results, but are **parked** while the projection runs on the worker, as the CAM requests are (`ParkCamRequest`, docs/cam/README.md#requests-that-wait): the frame keeps drawing and other connections are answered. `budget_s` keeps bounding the update; `update_drawing {async: true}` answers at once with the job id for a client that would rather poll `background_jobs`. Inside a `batch`, and on `--headless`, they call `UpdateDrawingRendersNow`. `set_drawing_view` and `remove_drawing_view` reassemble, so they rarely park at all |

## Files

- `src/core/drawing/DrawingProjection.{h,cpp}`: progress callback,
  `kProjectionRevision`, `ProjectionResult` serialisation helpers.
- `src/ui/DrawingActions.{h,cpp}`: `RequestDrawingUpdate`,
  `UpdateDrawingRendersNow`, `ReassembleDrawingViews`, the cut cache out of
  `CachedCut`'s static, the shape memo, export through the job.
- new `src/ui/DrawingJob.{h,cpp}`: the job, its payload and `finish`.
- `src/ui/AppState.h`: the shape memo, the per-view kept `ProjectionResult`,
  the pending state.
- `src/ui/DocumentSlots.cpp`, `src/ui/ProjectActions.cpp`,
  `src/ui/Commands.cpp`, `src/ui/DrawingPanel.cpp` (placeholder, badge,
  progress, cancel), `src/ui/DrawingAnnotate.cpp`, `src/ui/FileDialogs.cpp`,
  `src/ui/RequirementsPanel.cpp` (the shared bar).
- `src/remote/RemoteDrawings.cpp`: parking, `async`, the `describe` rows.
- `src/core/io/Serialization.cpp`, `SerializationInternal.h`: the projection
  files, `kVersion`.
- `tests/test_conventions.cpp`, `tests/test_drawing_hlr.cpp`,
  `tests/test_drawing.cpp`, `tests/test_actions.cpp`,
  `tests/golden/drawing/v<N>-stored-projection.json` + `.expect.json`,
  `src/app/guitests/GuiTestsDrawing.cpp`, `tests/drawing_sheet_smoke.py`.
- Docs once built: docs/drawings/README.md (projection, staleness, the
  cache), docs/ui/long-operations.md (the `drawing` kind and its section),
  docs/versioning/format.md, docs/versioning/README.md.

## Verification

**The GUI test that fails today:**
`drawing/a_slow_drawing_tab_does_not_stall_the_frame`. A part carrying a
helical spring (a helix and a pipe) and a box, an exact front view of it on a
drawing tab, the tab never visited. The test switches to the drawing tab by
clicking it and then, frame by frame while the `drawing` job runs, records
each frame's main-thread CPU time (`MainThreadCpuMs`, as
`cam/adaptive_clears_off_the_frame` does). It asserts that the projection's
own reported cost is at least one second, so it cannot pass by being fast;
that no frame costs 250 ms or more; that frames kept coming; that the
placeholder was drawn while it ran; and that the view lands with curves.
Today the click runs the whole projection inside one frame, so the worst
frame is the projection itself and the 250 ms check fails. A second case
cancels mid-projection and asserts the placeholder stays with the cancelled
error and a later Update views lands.

**Headless twins** in `tests/test_actions.cpp`, pumping the registry as
`HeadlessMain` does: the job's sheet equals `UpdateDrawingRendersNow`'s curve
for curve; a job landing after a tab switch writes the stashed slot; a
source edit mid-job lands stale; cancel keeps last-good; a scale change
reassembles with zero HLR calls.

**Persistence:** `tests/test_drawing.cpp` saves a projected drawing, reopens
it and installs every view with zero self and pair calls; a moved occurrence
or an edited body misses only the views it reaches; the golden at the new
version loads, and the revision digest test guards `kProjectionRevision`.
`drawing_sheet_smoke.py`'s reopen asserts its views come back projected
without HLR.

**On the real case:** a copy of `scratch/pantilt/drawings.dsgn` on an
offscreen instance, switching from `print sheet` to `pantilt drawing`: no
frame over 250 ms, the views landing in about the 20 s they cost now, and
after a save and a reopen, the three views drawn with zero HLR calls.

Then the headless suite, `tools/test-fast.sh`, the GUI suite, and
`tests/render_pixel_smoke.py` for the drawing panel's new drawing.
