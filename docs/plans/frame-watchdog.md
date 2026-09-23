# Frame watchdog

## The problem

When the app hangs, nothing says where. The crash handler
(`CrashHandler.cpp`) covers SIGSEGV and SIGABRT; a frame that never ends is
neither, so the log simply stops. A tester reported two freezes in sketch
mode with no crash, no stack and no error, and they could not be diagnosed.
The only question available afterwards is whether the process still answers
`ping` on its remote port (the UI thread is stuck while the app lives) or not
(the whole process is blocked).

## The fix

A monitor thread that logs when a frame has not finished within a few
seconds, naming the phase the main loop was last in. The main loop sets a
breadcrumb (an atomic enum plus a timestamp) around its phases — input and
UI, the sketch solve (`UpdateSketchStatus` runs a full solve every frame
while a sketch is edited), regeneration, the renderer resync, rendering and
present, a native file dialog — and the watchdog, waking once a second,
logs `frame stalled N s in <phase>` once per stall, and again when it ends
with the total. It changes nothing about how the frame runs; it turns "it
hung" into "it hung in X".

## Files

- `src/app/main.cpp`: the breadcrumbs around the windowed and `--offscreen`
  loop phases, and starting/stopping the watchdog.
- A new `src/app/FrameWatchdog.{h,cpp}`: the thread, the phase enum, the
  threshold.
- `src/ui/UI.cpp`: breadcrumbs inside `DrawUI` around the sketch solve and
  regeneration, if the loop-level phases prove too coarse.

## Verification

- A headless unit test drives the watchdog with a fake clock: a phase held
  past the threshold logs once with its name, a short one logs nothing, and
  the end of a stall is reported.
- Manually: a debug-only key that sleeps the main thread for 5 s produces
  one stall line naming the phase, and the app carries on.
- The GUI suite and the frame-timing tests are unchanged (the watchdog is
  idle unless a frame stalls).
