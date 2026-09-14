# Two swapchain images cost 10-25 fps; three gives 60

Measured 2026-09-14 on the report "rendering is slow": 14 fps, 71.6 ms a
frame, with `present` at 94% of it — on a scene of eight wooden panels.

## The frame that was being read wrongly

The HUD's split said the obvious thing and it was misleading:

```
cpu 4.52 ms   display 67.08 ms
gpu shadow 1.07  ssao 8.47  main 5.64  = 15.17 ms
present 66.96 ms  94%
```

15 ms of GPU work and 67 ms of waiting reads as "we are waiting for the
display, which is normal with vsync" — the rule docs/rendering/frame-timing.md
states. But the total was four times the refresh period, and that is the
condition the rule excludes. Two numbers nobody could see explained it.

## What was actually happening

**The swapchain had two images**, and the present mode is FIFO.

With two images one is on screen and one is being drawn; the CPU cannot start
the next frame until the presentation engine releases the one it is scanning
out. A frame that overruns the refresh period by a microsecond therefore
waits a WHOLE extra period — 16.7 ms becomes 33.3 — and the pattern is
self-reinforcing on a laptop:

**the GPU, blocked and idle for most of that period, clocks down.** This
machine's RTX 4060 sat in P8 at 210 MHz against a 3105 MHz maximum, 3.4 W of
a 55 W limit, with `nvidia-smi` reporting "Idle" as the clock reason while
simultaneously reporting 41-45% utilization. Every pass then costs ~15x what
it should (full-res SSAO at 1.2 Mpix: 20 ms, where the card is capable of
~1.4), which overruns the next period, which idles the GPU again.

That is why the same scene measured anywhere between 10 and 25 fps run to
run: it is a feedback loop, and where it settles depends on where it starts.

## The measurement

Same binary, same project, same camera, one environment variable choosing the
image count. Real X display, i3, no compositor, NVIDIA rendering and the
Intel iGPU driving the panel (PRIME):

| images | fps | avg | p50 | p95 | present | gpu |
|---|---|---|---|---|---|---|
| 2 | 10.2 | 98.1 | 76.6 | 254.2 | 90.8 | 19.51 |
| 2 (again) | 24.7 | 40.5 | 31.7 | 109.1 | 37.6 | 8.90 |
| **3** | **47.5** | 21.0 | 17.1 | 50.9 | 18.7 | 5.58 |
| **3 (again)** | **48.7** | 20.5 | 18.2 | 45.3 | 18.2 | 5.69 |
| 4 | 37.2 | 26.9 | 19.8 | 67.6 | 24.3 | 10.98 |
| 5 | 38.6 | 25.9 | 21.2 | 58.7 | 23.4 | 6.90 |

Note the GPU column: the passes get *cheaper* as the queue stays fed. That is
the clock loop closing, not less work.

**More is not better.** Four and five are worse than three — a deeper queue
adds latency and hands the presentation engine more to hold. Three is one
spare image so the queue never runs dry, which is all that was needed.

With three images and the AO at half resolution the same scene runs
**59.1 fps, p50 16.8 ms** — the display's own period, which is where a
vsynced app should sit.

## What shipped

- `g_MinImageCount = 3` (main.cpp). The driver clamps it to what the surface
  allows, and the renderers already hold `kMaxFramesInFlight = 8` slots
  indexed by image, so a larger swapchain was always *correct* — see
  docs/rendering/frames-in-flight.md, which fixed that aliasing and verified
  three images with synchronization validation.
- **`frame_stats` and the HUD report the swapchain**: `{images, present_mode}`.
  This is the instrument that was missing. A present time that is a multiple
  of the refresh period, with two images, is this cliff — and no other number
  in the app could distinguish it from a slow GPU.

## Is three general, or is it a fix for one card?

The number we set is a **request**. The driver clamps it into the surface's
own `[minImageCount, maxImageCount]` (imgui_impl_vulkan does this before
creating the swapchain), so "ask for 3" means "at least three wherever the
driver permits, and never fewer than the driver's own floor". Both GPUs in
this machine, logged at startup since 2026-09-14:

```
NVIDIA RTX 4060 : 3 images (asked 3, surface allows 2..8)
Intel Arc (MTL) : 3 images (asked 3, surface allows 3..no limit)
```

**Mesa was never on the cliff and cannot be put on it.** Sweeping the request
on the Intel — one wall, 1280x720, half-res AO, shadows on — it answers 3
whether asked for 2, 3 or 4:

| request | got | fps | avg | p50 | present | gpu |
|---|---|---|---|---|---|---|
| 2 | 3 | 15.8 | 63.5 | 57.0 | 0.0 | 58.15 |
| 3 | 3 | 20.7 | 48.3 | 39.2 | 0.0 | 51.76 |
| 4 | 3 | 17.0 | 58.8 | 54.0 | 0.0 | 63.37 |

The spread there is GPU-bound noise, not the swapchain: the Arc spends 52-63
ms a frame on a scene the NVIDIA draws in 5.6, and `present` is 0.0 ms
because that GPU owns the display and nothing is copied. Its frame rate is a
separate question — our shaders are heavy for an iGPU — and no swapchain
depth changes it.

So the shape of the rule is: **a driver whose floor is already 3 is
unaffected; a driver whose floor is 2 gains the spare image.** NVIDIA's
proprietary driver reports 2 on Windows as well, so the same cliff was
reachable there (packaging/windows/) and the same fix covers it. AMD is
unmeasured here.

The one case the request cannot fix is a surface with `maxImageCount` of 2 —
then the clamp hands back 2 and the app is on the cliff with nothing to do
about it. That case is now **visible rather than mysterious**: the startup log
prints what was asked and what the surface allowed, `frame_stats.swapchain`
carries the count, and `remote_smoke` asserts the spare image so such a
machine reports itself instead of quietly running at a third of its frame
rate.

## What is still on the table

- **`present` is 14.6 ms of a 16.9 ms frame even at 60 fps**, because every
  frame is copied from the NVIDIA to the GPU that owns the display. Rendering
  on the Intel iGPU removes the copy entirely (`present` measured at 0.0 ms)
  but its own passes cost 77 ms for this scene, so it is not a trade worth
  making here. A machine whose panel is wired to the discrete GPU would not
  pay either cost.
- **The P8 clock floor is a system condition, not ours**: `NVreg_Dynamic\
PowerManagement=3` with the X server on the Intel `modesetting` driver. It
  amplifies the cliff but does not create it, and feeding the queue properly
  is what lifts the card out of it in practice.
- A **vsync setting** (mailbox/immediate) is not shipped. FIFO at three
  images reaches the refresh period on this machine, and an uncapped present
  mode buys nothing above it while costing power and tearing.
