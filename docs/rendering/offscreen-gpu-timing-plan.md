# Per-pass GPU timing where nothing can contaminate it

## The problem

The GPU timestamps answer the one question a "slow on my GPU" report needs —
which PASS the time goes to — and they exist only in the windowed frame loop,
which is the worst place to read them:

- **`main` absorbs the stall.** The submit waits at
  `COLOR_ATTACHMENT_OUTPUT`, so the GPU's wait for the swapchain image falls
  between the SSAO timestamp and the end of the main pass. Measured: the same
  box on the same GPU reports `main 3.97 ms` visible and `11.07 ms` with the
  window occluded — 15.21 ms when zoomed out to a few pixels
  (docs/performance/present-stall.md).
- **A window can be covered.** FIFO throttles an occluded window to ~1 fps,
  which silently voids a measurement taken while something lands on top.
- **The display path dominates on this machine** (a cross-GPU copy on the
  discrete card, a ~25 fps ceiling on the integrated one), so the numbers that
  are ours are the smallest part of what the instrument reports.

So "does the procedural wood shader cost anything", "what does the outline
pass cost", "is this iGPU slow at our shading" are all currently answered by
subtraction and hope.

`--offscreen` already renders with the real renderer — full materials,
shadows, SSAO — with no window, no surface and no display server. It is the
clean room. It just has no timer in it.

## What to build

### 1. The timer runs in `--offscreen`

`g_GpuTimer.Initialize` in `RunOffscreen` (the GUI path does it in
`SetupVulkanWindow`, which never runs here), and `Shutdown` with the rest of
the teardown.

The capture already brackets exactly the right spans through its two
callbacks — `recordPrepass` outside the render pass, `recordScene` inside it,
then `vkCmdEndRenderPass`, submit and `vkQueueWaitIdle` (render/Screenshot.cpp).
So no signature changes:

- prepass callback: `BeginFrame` (resets the slot, writes `Begin`), then
  `RecordScenePrepasses(cb, slot, &g_GpuTimer)` for `AfterShadow`/`AfterSSAO`.
- scene callback: the scene renderers, then `Write(AfterMain)`.
- after `CaptureScreenshot` returns — the queue is idle, so the results are
  there — `Harvest(slot)` and mirror into `AppState::gpuStats`.

One slot, index 0: there are no swapchain images here and no frame loop to
collide with. The windowed screenshot path stays untimed for the reason its
comment already gives — it would double-write the live frame's slot.

`main` offscreen is the scene alone, with no ImGui drawn over it. That is a
difference from the windowed number and an improvement for this purpose.

### 2. `frame_stats` answers offscreen instead of refusing

Today it throws ("no frame loop in --offscreen"), and
`tests/offscreen_smoke.py` pins that. It should return what exists — the `gpu`
block and how many renders it covers — and **no frame numbers at all**: there
is no frame loop, so fps/avg/phases are absent rather than zero, because a
zero would read as "infinitely fast" to anything that plots it. `--headless`
keeps refusing: no renderer, nothing to say.

Samples accumulate per capture, so `screenshot` N times is how a caller asks
for N samples; no new RPC.

## Files

- `src/app/main.cpp` — `Initialize`/`Shutdown` in `RunOffscreen`, the three
  write points through the capture callbacks, `Harvest` + mirror.
- `src/remote/RemoteDispatch.cpp` — `frame_stats` in offscreen mode.
- `tests/offscreen_smoke.py` — the refusal assertion becomes an assertion that
  a capture produces per-pass numbers.
- `docs/rendering/frame-timing.md` — where to read the passes from, and why
  not from a windowed frame that is display-bound.

## Two things the first measurements forced, both now built

- **`frame_stats {reset: true}`.** The ring holds 120 samples, which is right
  for watching a frame loop and wrong for measuring: six renders of a new
  configuration read as the average of those six and the 114 before them, so
  a sweep walked its own answer forward monotonically. Reset clears the ring
  (and the mirror at once, so a caller asking again in the same batch is not
  handed what it just discarded).
- **Medians alongside the means.** The first render after a state change
  carries first-use work. One 14 ms outlier in six renders turned a 0.05 ms
  pass into 2.47 — which read as "this material costs 2.4 ms" until the run
  was repeated with forty. `*_p50_ms` says 0.05 either way, and is what a
  measurement should read.

## And a third, once the first numbers were in: measure a BURST

One sample per saved capture is the wrong unit. The PNG encode between them
is CPU work that leaves the GPU idle, and an idle laptop GPU drops to its
lowest clock, so a measurement taken that way describes the idle floor: the
same assembly read **12.5 ms a frame over 30 saved captures and 1.4 ms over
300 back-to-back renders**, and took 13.8 s instead of 0.7.

`frame_stats {renders: N}` draws N frames into the offscreen target and saves
nothing — an empty `outputPath` skips the readback and the encode, so the
loop is render-submit-render. It warms up first (32 frames, unmeasured):
pipelines are built on first use and the clocks ramp over the first tenth of
a second, which had a bare box reading 0.237 ms against 0.029 measured warm.

It is installed as `AppState::renderBurst` by `RunOffscreen` only — the same
seam shape as `ensureRemotePort` — so the GUI and headless modes say they
cannot rather than pretending.

## Verification

- `offscreen_smoke`: after a screenshot, `frame_stats` reports
  `gpu.samples >= 1` and `total_ms > 0` on a device with timestamp support,
  and still carries no `fps`. On a device without timestamps it says
  `supported: false` rather than omitting the block.
- The measurement this exists for, run once and recorded in
  docs/performance/present-stall.md: the same scene on both GPUs of this
  machine, per pass, uncontaminated.
- Full headless suite + `./tools/gui-tests-xvfb.sh` (the windowed path must be
  untouched).
