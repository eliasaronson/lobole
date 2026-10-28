# Frame pacing and frame timing

How the windowed loop paces frames, how per-frame GPU resources are indexed,
and how to measure where a frame's time goes.

## Frames in flight

`main.cpp` hands every renderer the **swapchain image index**
(`wd->FrameIndex`, `0 .. ImageCount-1`) as its frame index, because that is
the granularity the app's fences protect: before recording, it waits on the
*acquired image's* fence, which says nothing about any other image's
submission. Per-frame resources — a renderer's UBO, its descriptor sets,
dynamic vertex/index buffers — therefore need **one slot per swapchain
image**. Two images sharing a slot let the CPU overwrite a buffer the GPU is
still reading for the other frame: the picture draws with the previous
frame's camera, and a moving view visibly shakes, by an amount that grows
with the window size.

`render/FramesInFlight.h` states the rule once:

- `render::kMaxFramesInFlight = 8` is a **capacity**, not a count — sized with
  margin over any real swapchain (2-4, occasionally 5-6 with MAILBOX), so no
  resize path has to re-initialize renderers when the image count changes.
- `render::FrameSlot(imageIndex, who)` returns the index directly. Past the
  capacity it warns once and wraps — degraded rather than an out-of-bounds
  write.

Every scene renderer — `MeshRenderer`, `OutlineRenderer`, `SSAORenderer`,
`SketchRenderer`, `StockRenderer`, `FieldRenderer`, `UnderlayRenderer` — and
`GpuTimer` sizes its per-frame resources by `kMaxFramesInFlight` and indexes
them through `FrameSlot`. `OutlineRenderer` creates its per-mesh edge buffers
per slot on demand, so capacity costs nothing for slots a swapchain never
uses. A renderer that wrapped the index into fewer slots would alias images
0 and 2 on the 3-image swapchain the app requests, and draw with another
frame's camera or geometry while the view moves.

Synchronization validation cannot see this class of bug: the UBO slots are
persistently mapped, and sync validation does not track CPU writes through a
mapping. It is prevented by construction (the slot rule above), not caught.

## Swapchain and present

- **Present mode**: FIFO (vsync). `--gui-tests` instead prefers MAILBOX, then
  IMMEDIATE, then FIFO — FIFO throttles an occluded window to about 1 fps,
  which would stall the suite — and caps the loop at 60 fps so the fast test
  engine cannot drive the GPU into a timeout device-lost.
- **Image count**: `g_MinImageCount = 3`. With two images and FIFO the CPU
  cannot begin a frame until the presentation engine releases the image it
  is scanning out, so a frame that overruns the refresh period by a
  microsecond waits a whole extra period; the GPU, idle for most of it,
  clocks down, and the next frame overruns too. One spare image keeps the
  queue fed. More than three adds latency and does not help.
- The count is a **request**: the driver clamps it into the surface's
  `[minImageCount, maxImageCount]`. The startup log prints what was asked,
  what the surface allows and what was granted (`Swapchain: N images (asked
  3, surface allows a..b), 8 per-frame slots`), and `frame_stats.swapchain`
  reports `{images, present_mode}`. A surface whose maximum is 2 cannot be
  helped, and shows itself there.
- `remote_smoke` asserts the windowed app got at least three images.

## The frame-time instrument

The loop always measures itself (a few `glfwGetTime()` calls a frame) and
shows nothing unless asked. Three ways in, none of them in the menus or
Preferences, because this is session debugging state, not a preference:

- `lobole --perf-hud` — from launch;
- **Ctrl+Shift+F** — the hidden `view.perf_hud` command;
- **`frame_stats`** over the remote/MCP — the numbers, and `{hud: true|false}`
  toggles the overlay. This is the door for agents and tests.

### Phases

`render/FrameTimer.h` (header-only; it takes timestamps as arguments, so the
statistics are unit-tested against synthetic frames in
`tests/test_frame_timer.cpp`) keeps a 240-frame ring and reports avg / p50 /
p95 / max — the tail matters, because a drag smooth except for one hitch in
twenty has a fine average — and a phase split:

| phase | covers | slow means |
|---|---|---|
| `update` | events, remote requests, document → renderer sync | a rebuild that should have been cached |
| `ui` | ImGui `NewFrame` → `Render`: panels, picking, overlays | something walking the whole model each frame |
| `wait` | `vkAcquireNextImage` + the in-flight fence | GPU-bound, or just vsync |
| `record` | command-buffer recording and submit | too many draws, descriptor churn |
| `present` | `vkQueuePresentKHR` | GPU-bound, or just vsync |

**Read `wait` and `present` together** — which of the two holds the vsync
idle is a driver and present-mode detail. So the HUD and `frame_stats` lead
with `cpu_ms` (update + ui + record) versus `display_ms` (wait + present).
With vsync, a large `display` share is healthy: the app is idling until the
display is ready. It indicts the GPU only when the frame time is also above
the refresh period. When it is, check the swapchain line: a `present` that is
a multiple of the refresh period on a two-image swapchain is the FIFO cliff
above.

Beside the wall-clock phases each frame keeps its **work CPU**
(`Stats::workCpuMs`): the main thread's CPU time over `update` + `ui`, read
from the thread CPU clock. A phase is wall time, and on a loaded machine a
frame that computed nothing reads tens of ms of `ui` waiting for a core; a
bound that must hold under load reads this instead (see Tests below).

The worst frame in the window also keeps its own phase split
(`worst_phases_ms`, shown as `worst … · <phase> …` in the HUD), because
averages dilute a one-off hitch 240:1. A swapchain-rebuild frame skips its
phase marks, so its time lands in the next closed phase.

### `frame_stats`

Parameters: `window` (frames, default 240), `hud`, `reset`, `renders`.
Windowed replies carry `frames`, `total_frames`, `fps`, `avg_ms`, `p50_ms`,
`p95_ms`, `max_ms`, `phases_ms`, `cpu_ms`, `display_ms`, `idle_frames`,
`wake`, `wake_age_s`, `worst_phases_ms`, `worst_idle`,
`swapchain{images, present_mode}`, `gpu{…}` and `hud`. `--headless` has no
frame loop and refuses.

### GPU timestamps

`render::GpuTimer` writes four timestamps per frame — command-buffer start,
after the shadow pass, after the SSAO chain, after the main pass — into one
query slot per frame slot. A slot is harvested right after the loop's fence
wait for that image, when its previous submission has completed, with the
availability flag and never a wait. A per-slot "used" flag keeps a slot from
being read before its first write (reading an unwritten query is invalid). A
queue with `timestampValidBits == 0` reports `supported: false` rather than
omitting the block.

`frame_stats.gpu` is `{supported, samples, shadow_ms, ssao_ms, main_ms,
total_ms}`; the parts are contiguous, so they sum to the total. The windowed
screenshot path records its prepasses with no timer, so it cannot
double-write the live frame's slot.

Reading them:

- **Pass cost is not feature cost.** The shadow and SSAO buckets time their
  own passes; the *consumption* — PCF sampling, the AO texture read — runs in
  `mesh.frag`, inside `main`. A feature's cost is its pass plus the change in
  `main` with it toggled.
- **A laptop GPU's clocks follow its load**, so comparing one pass across
  configurations in a vsync-paced frame is unreliable; compare total GPU time
  and p95.
- **`main` absorbs the display stall.** The submit waits at
  `COLOR_ATTACHMENT_OUTPUT`, so the GPU's wait for the swapchain image falls
  inside the main-pass interval. In a display-bound (or occluded) window,
  `main` reports time it did not spend shading. Measure passes offscreen.

### Measuring a pass offscreen

`--offscreen` has no swapchain, no window to cover and no UI over the scene,
so it is where per-pass numbers are clean. `GpuTimer` runs there on one slot
(index 0), bracketing each capture through the capture's prepass and scene
callbacks. `frame_stats` offscreen returns `{mode, renders, gpu{…}, note}` —
the `gpu` block plus `*_p50_ms` medians over a 120-sample ring — and **no**
fps or phase numbers, since there is no frame loop and a zero would read as
infinitely fast.

- `{reset: true}` clears the samples so the next renders are measured alone.
- **`{renders: N}`** (1-10000) draws N frames back to back into the
  offscreen target, saving nothing, after an unmeasured 32-frame warm-up
  (pipelines build on first use, and clocks ramp). This is the way to measure
  a pass: saving a PNG between captures leaves the GPU idle long enough to
  drop its clocks, and the numbers then describe the idle floor. The burst is
  `AppState::renderBurst`, installed only by `RunOffscreen`; the other modes
  say they cannot.
- Read `*_p50_ms`: the first render after a state change carries first-use
  work.

### Tests

`tests/test_frame_timer.cpp` (exact percentiles, phases summing to the
total, the ring and window, work CPU per frame). The GUI test
`frametime/frame_stats_measure_hud_stays_hidden` asserts positive fps, phases
summing to the frame, GPU parts summing exactly to the total, and that the
HUD window does not exist until Ctrl+Shift+F opens it (the category is
`frametime`, because the test engine routes a category of exactly `perf`
into a group this suite never queues). `tests/offscreen_smoke.py` asserts a
capture produces per-pass numbers and no `fps`.

`frametime/job_tab_idle_and_switch` holds a frame's CPU work to a bound, on the case
that once ran at one frame a second: a job tab asking OCC for the exact box of
a turned part's toroidal faces every frame, and a job-tab switch that paid
3-7 s (docs/modeling/README.md#bounding-boxes-are-memoized-on-the-shape). It turns a stepped shaft with
six rounded rims, holds it in the rotary at A = 30 (no quarter turn reaches
it, so every framed copy is a new shape, and only the memos -- `CachedStockView`
in `ui/CamTool.cpp`, and the exact box remembered by shape identity and
location, docs/cam/gui.md#job-tab-frame-cost -- keep the frame cheap), and
asserts the median idle frame's work over 30 frames. It then adds jobs at
A = 90 and 180, which carry their source's box through the quarter turn, and
switches among the three tabs three times, bounding the worst of the six
frames after each switch.

"Work" is the main thread's CPU time over the `update` and `ui` phases
(`Stats::workCpuMs`, which main.cpp feeds from the thread CPU clock through
`FrameTimer::SetWorkCpuMs`), not the phases' wall time: a loaded machine
stretches every wall phase with waits for a core, and drawing (llvmpipe's,
or the driver's) is left out because it is not what these regressions cost.
Measured on the GUI runner:

| what | healthy | bound | broken |
|---|---|---|---|
| idle frame's work, median | 0.3-0.8 ms | 15 ms | 75 ms (memos off) |
| switch to a quarter-turned job | 12-22 ms | 120 ms | 240 ms (box not carried) |
| switch to the A = 30 job | 13-18 ms | 120 ms | 160-173 ms (box not remembered) |

Under `tools/with-cpu-load.sh -n 22 --contend` the healthy numbers were 0.5,
30 and 30 ms, and the runner's `--slow-frames 160` moves none of them.
The broken columns of the first and last rows are negative controls, run
under `--slow-frames 160`: the same test with `CachedStockView`'s key check
and the shared box memo's lookup both switched off (first row), and with the
shared lookup alone (last row). The A = 30 job used
to pay its exact boxes again on every visit and was held only to a second;
revisiting it now costs what a quarter-turned one does, and it is held to the
same bound.

## Idle throttle

A CAD app sits idle most of the time, and re-rendering the full pipeline at
vsync rate for an unchanged scene costs power — and on an integrated GPU,
turbo headroom shared with the CPU. The loop therefore paces *iterations*;
it never skips work inside one.

- **Active**: `glfwPollEvents()`, FIFO-paced — the normal loop.
- **Idle**: `glfwWaitEventsTimeout(0.1)` at the top of the loop: the next
  iteration starts within 100 ms, or immediately on any OS event or posted
  wake.

Every iteration that runs renders exactly as an active one does; there is no
partial invalidation to get wrong. The 10 Hz floor means a missed activity
signal costs at most 100 ms of staleness rather than a frozen UI, and
background completions (jobs, git workers, autosave, the disk poll) are
picked up without plumbing. A minimised window renders nothing but serves
requests, background jobs and the autosave each iteration, waiting on events
with a 50 ms timeout between them
([crashes and stalls](../architecture.md#crashes-and-stalls)).

`src/app/FramePacer.{h,cpp}` is the pure decision (`PacerInputs` in,
active/idle out), headless-tested in `tests/test_frame_pacer.cpp`. Signals
are gathered at the end of an iteration and decide the next; any signal means
active, plus a **1.0 s cooldown** after the last one (longer than ImGui's
tooltip delay, so a still hover still gets its tooltip):

- mouse moved, any button down, wheel, any key or char input;
- `io.WantTextInput` (the text cursor blinks);
- the camera animating;
- the document version changed (edits from clicks and RPCs alike);
- a background job or render job in flight;
- CAM or motion playback running;
- a remote request executed this iteration;
- the pacer disabled (`idle_throttle` off, or `--gui-tests`, unless
  `AppState::pacerForceEnable` is set by a test).

**Remote wake.** Requests queue on socket reader threads and run on the main
thread (`ProcessPending`), so an idle loop would add up to 100 ms to every
RPC. `RemoteServer::SetWakeCallback` is pointed at `glfwPostEmptyEvent()`
(thread-safe) and fires on enqueue, so the wait returns the moment a request
lands. `ProcessPending`'s count includes an async job still in flight
(staged screenshot, sheet render, UI action), which needs full rate to
progress. `remote_smoke` ("Idle throttle wakes on RPC") asserts N pings
against an idle app finish in far less than N x 100 ms.

`--headless` and `--offscreen` have no window to post an event to. Their
loops wait in `RemoteServer::WaitForRequest(2 ms)` between passes, which
returns as soon as a request has arrived since the last wait, so background
jobs and autosave keep a 2 ms cadence and an RPC waits for none of it. A
plain 2 ms sleep there put its whole length on every round trip: 2.1 ms a
ping where it is now 0.08 ms, and `refused_calls_smoke`'s ~13 000 calls
took 29 s instead of 5. The wait is edge-triggered (a count of arrivals,
not "the queue is non-empty"), because a request held behind a capture
stays queued and the loop must not spin on it.

The wait sits **before** `FrameTimer::BeginFrame`, so the sleep is not frame
time: an idle app's `frame_stats` shows normal per-frame ms, and
`idle_frames` is the share of paced iterations. The pacer records which
signal last held full rate — `wake` / `wake_age_s` in `frame_stats`, "idle N% · wake
<signal> Ns ago" in the HUD — because "never goes idle" is otherwise
undiagnosable. A resting app reads ~10 fps by design.

The `idle_throttle` preference (default on, live) is the escape hatch. The
GUI test `shell/idle_throttle_rests_and_input_wakes` forces the pacer on and
checks that it rests without input and wakes on a mouse move.
