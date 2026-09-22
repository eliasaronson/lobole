# "Add operation" on a rotary job, and profiles off the frame

## The problem

Opening `scratch/rolling_pin_helical/pin_parallel.dsgn` and pressing the CAM
panel's **Add operation** froze the app for about two minutes. Two defects
combined to cause it:

1. **The button always adds a whole-part "Cut out" profile**
   (`CamPanel.cpp`, `cam_add_op`: `DeriveOperation(..., OpType::Profile, {})`).
   The pin's setup is on the trunnion, and both of its operations are
   `surface_rotary`. A 3-axis contour from the top of the dowel to the bottom
   of it, round its silhouette, means nothing on that setup. The button
   ignored the setup and substituted a plausible operation, which the "refuse,
   don't substitute" rule forbids.
2. **Profile generation runs on the frame.** Adaptive clearing (phase 1) and
   surfacing (phase 5 of docs/ui/long-operations.md) already run on a worker.
   A profile still calls `ProjectedOutlines` inline, and that function takes
   an OCC section of every body at every depth pass. For the pin that is
   about 20 sections of a 300 mm body cut by helical sweeps, and each one is
   seconds. Nothing ever said the profile was cheap; it was simply never
   measured on a part like this one.

Either fix alone leaves the other bug in place. A "Cut out" asked for through
the toolbar verb (`job.whole`) on the same pin freezes just the same, and a
rotary job still gets an operation it cannot use.

## The fix

### 1. Add operation follows the setup

A core helper, `cam::AddOperationType(setup)` in `CamDerive.cpp`:

- **SurfaceRotary** when the machine has a rotary axis and the setup already
  cuts on it, meaning it holds a `SurfaceRotary` operation. That is the same
  evidence `DeriveRest` already reads to pick its type.
- **Profile** otherwise, which is today's behaviour for every 3-axis job.

It reads what the setup already contains, not the machine alone. A 4th-axis
machine carrying a flat plate clamped to the bed is still a 3-axis job, and
giving that plate a rotary pass would be the same bug turned round.

The button derives that type. When `DeriveOperation` refuses (a V-bit on a
rotary pass, say), the refusal is shown, as it is today.

### 2. The projected outlines go to a worker

The expensive step in a profile is `ProjectedOutlines(bodies, topZ, passes,
deflection)`. It is a pure function of the geometry and three plain values.
So the deferral happens at that call, not around the whole operation:

- **The key is `OutlineKey(bodiesKey, topZ, passes, deflection)`.** Because
  it hashes every argument the function takes, it is complete by
  construction. There is no field list to drift, and so no convention gate is
  needed. This is the difference from `SurfaceKey`, which has to name every
  field a strategy reads, and a profile reads far more of them: side, tabs,
  leads, reliefs, stock, and more.
- **`OutlineRequest` / `OutlineResult` / `OutlineCache` / `OutlinePass`** sit
  next to `ProjectedOutlines` in `CamPicks.h` and `CamGeometry.cpp`, with the
  same shape as the surface ones.
- **`GenerateContext` gains `outlineCache` and `pendingOutline`.**
  `GenerateProfile` takes the context and routes both of its projections
  through one helper: a cache hit, or a request with `tp.pending = true`, or
  (with no context) inline as before.
- **`src/ui/CamJobs.cpp` gets a third queue**, `QueueOutlinePasses`, and it
  runs FIRST. An outline is the input to a profile and takes seconds, so the
  backplot fills early. `LandedAPass`'s idle test, `CancelCamPasses` and
  `GenerateCamToolpathsNow` all count the new queue.
- **`AppState` gains `camOutlineCache`, `camOutlineQueue` and
  `camOutlineInFlight`.**

It is thread-safe for the reason surfacing already is. `SectionLoops` goes
through `Measure::SectionCaps`, whose booleans are built with `BuildQuery`,
which is non-destructive (docs/performance/query-booleans.md). The worker also
works on a copy of the framed bodies.

Pocket's and Face's per-pass `SectionLoops`, and the section loops adaptive
reads on the frame, are left as they are. They are one section per pass
rather than a projection over all of them, and this plan does not claim them.

## Files

- `src/core/cam/CamPicks.h`, `src/core/cam/CamGeometry.cpp`: the outline
  request, key, cache and pass.
- `src/core/cam/CamToolpath.h`: the `GenerateContext` fields.
- `src/core/cam/CamStrategies.cpp`: `GenerateProfile` takes the context.
- `src/core/cam/CamDerive.cpp`, `CamToolpath.h`: `AddOperationType`.
- `src/ui/AppState.h`, `src/ui/CamJobs.cpp`, `src/ui/CamPanel.cpp`: the queue,
  the context, and the button.
- Docs: docs/ui/long-operations.md (a section for outlines), docs/cam/gui.md
  (what Add operation adds).

## Verification

- **`tests/test_cam.cpp`, "a profile's outlines are cached, and can be
  deferred to a worker":**
  - Inline, the result equals a cached generate.
  - Deferred, the toolpath is pending with no error and one request.
  - `OutlinePass` of that request, put into the cache, regenerates to exactly
    the inline moves.
- **`tests/test_cam.cpp`, "Add operation follows the setup":**
  - A 3-axis setup gets Profile.
  - A rotary machine with no rotary operation gets Profile.
  - A setup holding a rotary operation gets SurfaceRotary.
- **GUI test `cam/a_profile_is_cut_off_the_frame`,** the twin of
  `adaptive_clears_off_the_frame`:
  - `cam_add_op` returns with the toolpath pending.
  - Frames keep coming while it runs.
  - What lands equals a synchronous generate.
- **Manual:** the pin in an offscreen instance, with `add operation` timed.
- **Suites:** `tools/test-fast.sh`, then the CAM GUI tests (`-f cam/`) and
  the full headless suite before committing.
