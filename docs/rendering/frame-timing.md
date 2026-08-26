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

## GPU timestamps — the deferral below, built (2026-08-26)

What un-deferred it: the Intel-GPU performance assessment needed to know
which PASS eats that GPU's frame, and there was no way to ask — the offscreen
harness cannot even run on that GPU (it cannot present under Xvfb; the app
falls back to the NVIDIA), so the numbers must come from the user's real
session, which means the instrument has to live in the app.

`render::GpuTimer` writes four timestamps per frame — command-buffer start,
after the shadow pass, after the SSAO chain, after the main pass — into one
query-pool slot per swapchain image (the FramesInFlight convention). The
bookkeeping the deferral predicted leans on synchronization the loop already
has: a slot is harvested right after the frame loop fence-waits that image,
at which point its previous submission has COMPLETED — so results are read
with the availability flag and never a wait. Cost: ~4 counter writes on the
GPU, one non-blocking readback on the CPU, zero stalls.

Surfaced as a `gpu` object in `frame_stats` (`supported`, `samples`,
`shadow_ms`, `ssao_ms`, `main_ms`, `total_ms` — the deltas are contiguous,
so the parts sum to the total by construction) and a line in the HUD. A
queue with `timestampValidBits == 0` reports `supported: false` rather than
omitting the key. The screenshot path re-records the prepasses into its own
buffer and deliberately writes no timestamps there — they would double-write
the frame's slot.

**Reading the numbers** — two caveats the first field screenshots (2026-08-26,
RTX 4060) demonstrated within the hour:

- **Pass cost is not feature cost.** The shadow/ssao items time their own
  render passes; the CONSUMPTION side — PCF shadow sampling, the AO texture
  read — executes in `mesh.frag`, inside `main`. Measured: the shadow pass
  read 0.79 ms, but turning shadows off dropped `main` by ~4.3 ms. A feature's
  full cost is its pass plus main's delta with it toggled.
- **Laptop clocks confound cross-config comparisons.** Turning SSAO off made
  `main` read SLOWER (4.70 → 10.10 ms): with less total load inside a vsync
  frame, the GPU downclocks and the same work stretches in wall time. Compare
  configurations by total GPU ms and p95/fps, not by one pass across configs
  — and trust the split most when the GPU is saturated (the Intel readings,
  pegged at 100%, are clock-stable; the idle NVIDIA ones are not).

Two things the build taught:

- **The availability flag does not cover uninitialized queries.** Reading a
  slot before its first reset+write is invalid
  (`VUID-vkGetQueryPoolResults-None-09401`) — the validation layer caught it
  on first-use slots, and a per-slot used flag now guards the harvest. This
  is exactly what the pre-landing validation run exists for.
- First real read (RTX 4060, dock assembly, 1600x1000): shadow 0.44 ms +
  SSAO 0.44 ms + main 0.98 ms = **1.85 ms of GPU work inside a ~30 ms vsync
  frame** — the CPU split reported all ~30 ms as `display` and could not say
  the GPU was 94% idle. That is the gap this closes.

## Worst-frame attribution (2026-08-26)

`max` alone says "something spiked" and nothing else — the phase averages
dilute a one-off 50 ms hitch 240:1. `Snapshot` now also carries the worst
frame's OWN phase split (`worstPhaseMs`, plus an idle-paced flag), shown as
a `worst 52.79 ms · ui 48.11` line in the HUD and as `worst_phases_ms` in
`frame_stats`. Free: the ring already held every frame's split; this is read
out in the walk Snapshot already does. First real reading attributed a
733 ms open-project frame to `update` (document load + regen + renderer
sync) — correct, on the first try. Caveats: a swapchain-rebuild frame skips
phase marks (its time lands in the next closed phase), and the idle
throttle's event wait sits OUTSIDE the frame, so idle iterations cannot fake
a spike — the flag exists for the rare case one genuinely is the worst.

## Deferred, with reasons
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
  GPU timestamps are supported on the rig, sampling, with a nonzero main pass
  and parts summing exactly to the total (a gap would mean a write point
  drifted off its pass boundary); the HUD window does not exist until
  Ctrl+Shift+F opens it, and closes again on the second press — the "hidden by default" half of the request, asserted
  rather than assumed. The category is `frametime` and not `perf` because the
  test engine routes a category of exactly `perf` into its Perfs group, which
  this suite never queues: the test listed but never ran.
