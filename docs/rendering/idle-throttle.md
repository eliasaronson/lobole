# Idle throttle — stop re-rendering a scene nobody is changing

Status: **SHIPPED 2026-08-26**, as designed below. As-built notes:

- The wake callback's smoke check is conclusive, not marginal: with the
  callback deliberately disabled, **8 of 8** idle pings exceeded the 30 ms
  bound; with it, 0 do (`remote_smoke`, "Idle throttle wakes on RPC").
- `ProcessPending`'s activity count includes an async job still in flight
  (staged screenshot, sheet render, ui action) — those span frames and need
  the loop at full rate to make progress, even with an empty queue.
- The build surfaced an unrelated latent abort: the Study panel's gravity
  field passed `EnterReturnsTrue` to `InputFloat3`, which `InputScalar`
  ASSERTS on — invisible under this build dir's Release cache, fatal on a
  fresh clone (CMakeLists defaults empty build type to **Debug**), the
  moment gravity was switched on. Fixed with the panel's own
  deactivation-commit recipe. Found because a blank `-DCMAKE_BUILD_TYPE=`
  flag accidentally flipped this tree to Debug for two hours — the one
  useful thing that mistake did.

Follow-on to the Intel-GPU
performance assessment: the loop renders the full pipeline (shadow pass,
SSAO prepass+AO+blur, 4× MSAA main pass) at vsync rate while the app sits
idle, which a CAD app does most of the time. On a laptop iGPU that is fans,
battery, and — because CPU and GPU share one power budget on Meteor Lake —
lower turbo clocks left for the frames that matter. On the dGPU it is watts
for nothing.

## Design: pace the iterations, never skip inside one

Two modes, chosen once per loop iteration:

- **Active** — `glfwPollEvents()`, exactly today's loop, FIFO present pacing.
- **Idle** — `glfwWaitEventsTimeout(0.1)` at the top of the loop instead:
  the next iteration starts within 100 ms, or **immediately** on any OS
  event or a posted wake.

Every iteration that runs still renders **exactly as today** — same passes,
same present. There is no skipped-render bookkeeping, no partial
invalidation, no stale-swapchain state to reason about; the only variable is
*when* iterations happen. The iconified branch (sleep 10 ms, `continue`)
stays the one true "no render" path, unchanged. This is why the risk is low:
we extend the loop's control flow, not the Vulkan work inside a frame.

A 10 Hz floor rather than render-on-demand is deliberate: with a floor, a
missed activity signal costs at most 100 ms of staleness instead of a frozen
UI, async completions (solver jobs, git workers, autosave, the F8 disk poll)
are picked up within 100 ms with no plumbing, and the win is still ~85% of
the idle GPU work.

The wait sits **before** `frameTimer.BeginFrame`, so the sleep is not
measured as frame time: at idle, `frame_stats` reads ~10 fps with the same
per-frame ms as before, and cpu-vs-display keeps meaning what
[frame-timing.md](frame-timing.md) says it means.

## What counts as "active"

Signals are gathered at the END of an iteration (they are that iteration's
state) and decide the NEXT one. Any signal → active, plus a **1.0 s
cooldown** after the last one — longer than ImGui's tooltip delay, so a
still-mouse hover still produces its tooltip, and nav/fade animations finish.

| signal | why |
|---|---|
| mouse moved / any button down / wheel / key or char events | input; also covers every drag (button is down) |
| `io.WantTextInput` | text cursor blink |
| `Camera::IsAnimating()` | view tweens run dt-stepped |
| document version ≠ last seen | edits from clicks AND RPCs repaint |
| topo/study job, offline render job in flight | progress UI |
| a remote request was executed this iteration | agent-driven sessions stay fluid |
| pacer disabled (setting off, or `--gui-tests`) | see below |

**Remote wake.** Requests queue from socket reader threads and are executed
by the main-thread pump (`ProcessPending`), one drain per iteration — so an
idle loop would add up to 100 ms to EVERY RPC round trip, which multiplies
across a scripted session into real seconds and flaky smoke deadlines.
`RemoteServer` gains a wake callback invoked on enqueue; `main.cpp` points it
at `glfwPostEmptyEvent()` (documented thread-safe), so `WaitEventsTimeout`
returns the moment a request lands. This is the one integration point that
must be right, and it gets its own latency assertion (below).

**`--gui-tests` bypasses the pacer** — the test engine drives frames and its
mode already has its own pacing branch (MAILBOX + a 60 fps cap). A test that
wants to test the pacer itself sets `AppState::pacerForceEnable`.

**Accepted costs** (all bounded by the floor): terminal output produced while
the user only watches repaints at 10 Hz (typing is input → full rate); a git
fetch spinner steps at 10 Hz; window expose/damage repaints within 100 ms.

## The pieces

1. **`app/FramePacer.{h,cpp}`** — pure decision logic (`Inputs` in,
   active/idle out, owns the cooldown clock and last-seen version). In
   `designer_app`, so it is headless-unit-testable; `main.cpp` only gathers
   the inputs.
2. **`RemoteServer::SetWakeCallback`** — one call site at the reader-thread
   enqueue; `ProcessPending` returns how many requests it executed.
3. **Settings row** `idle_throttle` (Performance, bool, default **on**,
   Tier::Live) — the escape hatch a bug report needs.
4. **Visibility**: `FrameTimer` marks idle-paced frames; `frame_stats` gains
   `idle_frames`; the perf HUD shows the share. A throttle nobody can see is
   undiagnosable.

## Tests

- **Headless** (`tests/test_frame_pacer.cpp`): each signal forces active;
  cooldown expires by the clock, not by frame count; doc-version delta fires
  once; disabled/test-mode always active; force-enable overrides test mode.
  Sabotage-checked (a pacer that never idles must fail the idle case; one
  that ignores cooldown must fail the tooltip case).
- **GUI** (`shell` category — NOT `perf`, which never runs): with
  `pacerForceEnable`, no input for > cooldown ⇒ the idle counter rises; a
  mouse move snaps it back to active; without force-enable, test mode never
  idles.
- **Windowed smoke** (`remote_smoke`): N pings against an idle app complete
  in far less than N × 100 ms — the regression test for the wake callback.
  Without the wake this fails by an order of magnitude, not by noise.

## Non-goals

MSAA/present-mode changes (riskier, separate item), per-pass GPU timestamps
(separate item), render-on-demand below the 10 Hz floor, and any change to
what a frame draws.
