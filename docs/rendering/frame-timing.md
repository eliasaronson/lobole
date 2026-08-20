# Frame timing — measuring the loop without showing it

> Status: **built 2026-08-20.** From "add a way to measure frame-time, but it
> should not be shown by default to users."

## Why a number, and why a *split*

Two performance bugs this year were diagnosed by guessing and then measuring
by hand: the section-drag lag on assemblies (a per-frame rebuild of geometry
that only changes on edit) and the camera wobble on the Intel GPU (per-frame
resources sized to the wrong swapchain image count). In both cases the
question was the same and there was no way to ask it: *is the frame slow
because of our CPU work, or because we are waiting on the GPU?*

An fps readout cannot answer that — with vsync on, a healthy frame and a frame
that spends 15 ms blocked in `vkAcquireNextImage` both read 60. So the thing
being added is not a counter, it is a **phase split**:

| phase | what it covers | slow means |
|---|---|---|
| `update` | events, remote requests, document → renderer sync | a rebuild that should have been cached |
| `ui` | `NewFrame` → `Render`: panels, picking, overlays | a panel walking the whole model each frame |
| `wait` | `vkAcquireNextImage` + the in-flight fence | GPU-bound, or just vsync — see below |
| `record` | command-buffer recording and submit | too many draws / descriptor churn |
| `present` | `vkQueuePresentKHR` | GPU-bound, or just vsync — see below |

**Read `wait` and `present` together, as one "display" number.** The first
run of the HUD found this the hard way: on an RTX 4060 under Xvfb the entire
vsync idle sat in `present` (97% of the frame) and `wait` was 0.00 ms — which
of the two blocks is a driver and present-mode detail, so a doc that had
promised "`wait` is the vsync idle" would have sent the next reader looking
for a bug in the wrong half. So the HUD and the RPC both report the
conclusion — `cpu` (update + ui + record) versus `display` (wait + present) —
above the per-phase rows, and the rows carry tooltips saying which is which.

And with vsync on, a large `display` share is **normal and healthy**: it is
the app idling until the display is ready. It only indicts the GPU when the
total frame time is also above the refresh period, or when vsync is off
(`--gui-tests` runs uncapped).

## Not shown by default

The measurement always runs — five `glfwGetTime()` calls per frame, which is
nothing — but nothing about it is visible unless asked for. Three doors, all
closed by default and none of them in the menus or Preferences:

- `designer --perf-hud` — for a dev-watch session that wants it from launch.
- **Ctrl+Shift+F** at runtime, deliberately undocumented in the UI.
- `frame_stats` over the remote/MCP interface — returns the numbers (including
  `cpu_ms`/`display_ms`), and `{hud: true}` toggles the overlay. This is the
  door that matters for agents and for CI: a HUD nobody is looking at proves
  nothing, a number in a test does.

It is session state, not a preference: it belongs to the moment you are
debugging, and `docs/ui/settings.md`'s own test ("would the user be annoyed to
set it again tomorrow?") says no. Making it a preference would also mean
either showing it in the Preferences window — which is exactly what was asked
against — or carrying a hidden key the window cannot edit.

## Statistics, not a spot reading

A single frame's time is noise. The timer keeps a 240-frame ring (~4 s at 60
fps) and reports `avg / p50 / p95 / max`, because the interesting failures are
the *tail*: a drag that is smooth except for a 60 ms hitch every twentieth
frame has a fine average and an unusable feel. p95 and max are what caught the
section-drag lag when it was described as "super slow and laggy"; the average
had barely moved.

`FrameTimer` is header-only and takes its timestamps as arguments (no clock
inside), so the statistics are unit-tested against a synthetic frame sequence
with known percentiles rather than by staring at a HUD.

## Deferred, with reasons

- **GPU timestamp queries** (`vkCmdWriteTimestamp` around the prepasses) would
  attribute time *inside* the GPU — shadows vs SSAO vs mesh. Real value, but
  it needs a per-frame query pool, resolve-a-frame-late bookkeeping, and
  `timestampPeriod` handling; the CPU split above already separates "our work"
  from "the GPU's", which is the question that was actually being asked twice.
- **Recording to a file** for offline comparison. `frame_stats` returns JSON,
  so a caller that wants a time series can poll and keep it; building a
  recorder before anything consumes one is speculative.
- **A per-panel breakdown** of the `ui` phase. Worth it the first time `ui` is
  the phase that is slow; today it never has been.

## Verification

- `tests/test_frame_timer.cpp` (headless): a synthetic 100-frame sequence with
  known times — avg, p50, p95 and max are exact; the phase averages sum to the
  total; the ring drops the oldest frame past capacity and the window
  parameter reads only the newest N.
- `frametime/frame_stats_measure_hud_stays_hidden` (GUI): the app has run
  frames, so the snapshot has a positive fps and phases that sum to the frame;
  the HUD window does not exist until Ctrl+Shift+F opens it, and closes again
  on the second press — the "hidden by default" half of the request, asserted
  rather than assumed. The category is `frametime` and not `perf` because the
  test engine routes a category of exactly `perf` into its Perfs group, which
  this suite never queues: the test listed but never ran.
