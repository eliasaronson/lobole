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

**That P8 is a consequence, not a cause** — corrected 2026-09-14 after
asserting the opposite twice. The governor works: a sustained load (glmark2
off-screen through PRIME offload) takes the card to **P0, 1890 MHz, 90%
utilization** within seconds. And with the spare swapchain image, our own app
at 1878x1200 with FULL-res AO on the speaker assembly oscillates **P0 to P5**
and holds **60.0 fps, 16.7 ms**. The card drops to P8 when the demand does,
which for a vsync-paced viewport is most of the time — and at P8 there is one
fifteenth of the throughput left, so a frame that grows tips over the period
and the two-image cliff turns that into a collapse. Feeding the queue is what
lifts it out.

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

**Mesa was never on the cliff and cannot be put on it**, because its floor is
3 — the surface will not hand out a 2-image swapchain at all. It does honour
larger requests, and they do not help; a sweep on the Intel (one box filling
1280x720, AO and shadows off, so the GPU costs ~4 ms) moves nothing that is
not noise:

| request | got | fps | avg | p50 | gpu | wait | present |
|---|---|---|---|---|---|---|---|
| 3 | 3 | 25.6 | 39.0 | 33.3 | 4.17 | 35.83 | 0.03 |
| 4 | 4 | 23.5 | 42.6 | 34.4 | 4.36 | 37.54 | 0.03 |
| 5 | 5 | 34.8 | 28.8 | 24.9 | 4.09 | 24.80 | 0.03 |

(An earlier version of this file claimed Mesa answers 3 whatever it is asked.
It does not: that sweep ran after the environment override had been removed,
so all three arms were the same build. The floor of 3 is the real finding and
it is what the startup log shows.)

Note where the idle sits on Mesa: `wait`, not `present` — the frame-timing
doc's warning that which of the two holds it is a driver detail, in the flesh.

## The iGPU is not slow at our shaders; the display path caps it

The same measurement answers "should we worry about the Intel?". On the Arc,
one box filling 1280x720 with AO and shadows off:

| what is on the box | main pass |
|---|---|
| palette colour, no material | 3.97 ms |
| aluminum | 4.49 ms |
| plastic_white | 4.27 ms |
| wood_oak / wood_pine / wood_walnut | 4.38 / 4.36 / 4.28 ms |
| plywood_birch | 4.24 ms |

The **procedural wood grain is free** — about 0.4 ms over a flat colour on an
iGPU, which retires the obvious suspicion. And the Arc's ceiling is not ours
at all: `vkcube`, an independent Vulkan program drawing one spinning cube,
runs at **25.6 fps on that GPU on this display** — the same number our app
gets. (On the NVIDIA, vkcube manages 50.1: that path pays the cross-GPU
copy.) Whatever caps the iGPU here is in the X11 present path, and no amount
of work removed from our frame will lift it.

## A trap in our own instrument: `main` absorbs the stall

`main_ms` is the interval between the SSAO timestamp and the end of the main
render pass — and the GPU's wait for the swapchain image lands *inside* it,
because the submit waits at `COLOR_ATTACHMENT_OUTPUT`. So when the app is
display-bound, the main pass reports time it did not spend shading.

Measured by accident, which is the best kind of proof here: the same box on
the same GPU reported `main 3.97 ms` with the window visible and
`main 11.07 ms` once the window was occluded (FIFO throttles a covered window
to ~1 fps — `wait` went to 853 ms). Zoomed out to a handful of pixels it
reported **15.21 ms**, more than when it filled the screen.

So: **read the per-pass numbers only when the frame is NOT display-bound.**
A pass number taken from a stalled frame is what made the Arc look ten times
slower than it is — the 41-57 ms "main" in the first cross-GPU table on this
page is that artefact, not a shader cost.

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

## What the clean instrument says (2026-09-14)

With per-pass timing available in `--offscreen` — no swapchain, no window, no
occlusion, 1280x720, scene only — the passes can be read directly
(docs/rendering/offscreen-gpu-timing-plan.md). Medians of 300 renders drawn
**back to back**, after a warm-up, which takes 2.9 s for the whole table:

| scene | shadow | ssao | main | total |
|---|---|---|---|---|
| one box, no AO, no shadows | 0.000 | 0.000 | **0.029** | 0.029 |
| + AO half | 0.000 | **0.343** | 0.033 | 0.376 |
| + AO full | 0.000 | **0.603** | 0.033 | 0.635 |
| + shadows (no AO) | **0.227** | 0.000 | 0.049 | 0.276 |
| AO half + shadows | 0.229 | 0.362 | 0.033 | 0.624 |
| + procedural wood material | 0.229 | 0.362 | 0.033 | **0.623** |
| side_wall part | 0.210 | 0.391 | 0.075 | 0.676 |
| front_wall part | 0.163 | 0.414 | 0.139 | 0.716 |
| speaker assembly | 0.455 | 0.489 | 0.557 | **1.502** |

**Our whole GPU frame for a real assembly is 1.5 ms.** The 93 ms frame this
page opens with was display path, all of it.

How the burst changes the answer, and why it is the right way to ask: saving
a PNG between renders is CPU work that leaves the GPU idle, and an idle
laptop GPU drops to its lowest clock. The same assembly measured **12.5 ms a
frame over 30 saved captures and 1.4 ms over 300 back-to-back renders** — a
9x error, and 200x slower to collect. The burst also warms up first: pipelines
are built on first use and the clocks ramp over the first tenth of a second,
which had a bare box reading 0.237 ms instead of 0.029.

The same table on the Intel Arc, uncontended rows: AO half 0.615, AO full
0.932, shadows 0.30, wood free, a bare box 0.042. So the discrete card is
about 1.5-1.8x faster per pass — a believable gap at this resolution, and
nothing like the ten times the windowed instrument suggested.

Two things it settles:

- **The scene is nearly free; the effects are the frame.** A box costs 0.029
  ms to shade and 0.62 ms with AO and shadows on — twenty times its own cost.
  The procedural wood material costs **0.001 ms** (0.623 against 0.624), so
  the grain shader was never why anything was slow.
- **AO is the single biggest pass**, half-res or not, and shadows cost about
  two thirds of AO at half. That is the same conclusion the field report
  reached from the other end, now from the pass itself.

**What survives a loaded machine, and what does not.** Part of this round ran
while another session was compiling and Chrome was busy; the tables were
re-measured afterwards and the discrete GPU's came back unchanged. Nothing
else uses that card, so its timestamps do not care what the CPU is doing.

The **Intel** figures do care, because that GPU draws the desktop. Simple
scenes repeat (a bare box 0.042/0.044 ms, AO half 0.657/0.733, shadows
0.322/0.381 across runs); complex ones do not — the same assembly gave
`main` 62.9 ms under contention and 3.5 ms quiet. The per-frame TOTAL median
is what gives it away: with the component medians at 0.3-1.2 ms the total
median was 16.2, which only happens when a *different* pass is interrupted
each frame.

Quiet, the Arc costs **0.66 ms** for half-res AO against the P8 NVIDIA's
3.06, and 0.32 ms for shadows against 2.04. That is not an argument for
rendering on the Arc, and the reason is the answer to "why is it not faster
in practice":

**the Arc is never quiet.** It draws the desktop — two displays, 3840x2160
and 3200x2000, plus a browser with 34 renderer processes — and every X11
present on this machine is its work too. Our frame's GPU time is interleaved
with all of it, and the timestamps show exactly that: the same scene, minutes
apart, cost **3.83 ms offscreen and 54.88 ms in a window** on that GPU. The
discrete card has the opposite deal — it does nothing but our rendering, pays
a cross-GPU copy to hand the frame over (`present` 14.2 ms of a 16.7 ms
frame), and still lands 60 fps.

A true cross-GPU comparison needs a machine where each GPU can be measured
idle. This one has neither: one is clock-locked, the other draws the desktop.

## What is still on the table

- **`present` is 14.6 ms of a 16.9 ms frame even at 60 fps**, because every
  frame is copied from the NVIDIA to the GPU that owns the display. Rendering
  on the Intel iGPU removes the copy entirely (`present` measured at 0.0 ms)
  but its own passes cost 77 ms for this scene, so it is not a trade worth
  making here. A machine whose panel is wired to the discrete GPU would not
  pay either cost.
- **The P8 state is demand-driven, not a system fault.** The card ramps to P0
  under load and sits at P8 when a vsync-paced frame leaves it idle 90% of
  the time. It is worth knowing because the per-pass numbers on this page
  were taken there — a burst of captures separated by PNG encoding has the
  same low duty cycle — so they are idle-clock figures and their RATIOS are
  what to read. `nvidia-powerd` is not running and persistence mode is off;
  neither prevents the ramp.
- A **vsync setting** (mailbox/immediate) is not shipped. FIFO at three
  images reaches the refresh period on this machine, and an uncapped present
  mode buys nothing above it while costing power and tearing.
