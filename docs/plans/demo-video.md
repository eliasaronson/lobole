# The release demo video

A 60-90 second video for the first release: what Loböle does, shown in the
real app, re-recordable whenever the interface changes before (or after) the
release.

## The problem

A hand-driven screen recording costs a retake per mistake and goes stale with
every UI change. The app can already be driven by script, and the GUI test
engine it bundles has a Cinematic speed (pauses between actions, a software
mouse cursor drawn into the frame) and a video recorder that pipes frames to
ffmpeg. What is missing is the frames: the engine asks the application for
each frame's pixels (`ImGuiTestEngineIO::ScreenCaptureFunc`), and the app
does not provide them.

Recording the display instead does not work. A windowed instance on Xvfb
renders at about 110 fps at 1920x1080 (`frame_stats`), but the driver
presents the swapchain without going through Xvfb's framebuffer: x11grab and
`xwd -root` both read a black screen. The pixels have to come from the app.

## The shape of it

1. **Full-window capture for the engine.** While a capture is running, the
   frame loop copies the finished swapchain image (the scene AND the
   interface, after ImGui has drawn) into a host-visible buffer before
   presenting it, and `ScreenCaptureFunc` hands the requested rect of the
   last such frame to the engine; `ImGuiTestEngine_PostSwap` is called after
   each present. Nothing is copied while no capture runs. The copy needs the
   swapchain created with `TRANSFER_SRC` usage, and an ordinary launch does
   not ask for it: some drivers drop their internal image compression for a
   swapchain with extra usage, a cost every frame would pay for a feature
   almost never on. Only `--capture-dir` asks, checked against the surface's
   capabilities and refused with a message if the surface cannot.
   Recording runs on the fixed 1/60 s clock the GUI suite already uses, so
   every frame is one video frame at 60 fps however long it took to render.
2. **Demo scenes as GUI tests**, category `demo`, in
   `src/app/guitests/GuiTestsDemo.cpp`. Each scene drives the real widgets
   at Cinematic speed and wraps itself in `CaptureBeginVideo` /
   `CaptureEndVideo`. They run in the GUI suite like any other test, at the
   suite's fast speed and with nothing recorded (no `--capture-dir`), so a
   UI change that breaks a scene fails the suite the day it lands rather
   than at the next release. A new `--gui-test-speed fast|normal|cinematic`
   (default fast) picks the run speed, and `--capture-dir` turns the
   recording on and says where the clips land.
3. **`tools/demo/record.sh`**: one Xvfb display at 1920x1080, a scratch
   config and state directory (the scenes must not touch the user's), every
   scene recorded to `scratch/demo/<scene>.mp4`.
4. **Rendered footage** from what exists: `export_motion_video` for the
   OM615 in section and the planetary gearbox, `render_still` for still
   shots, and the branded log end (`generators/logo/`) for the close.
5. **The agent segment is recorded live**, not scripted: a real Claude Code
   session building a part over MCP beside the app, sped up. A scripted
   one would not show what it claims.
6. **The edit** (Kdenlive or Resolve) is by hand: captions burned in, no
   voice, music only with a licence that allows redistribution.

## The shot list

| # | scene | length | source |
|---|---|---|---|
| 1 | the OM615 turning over in section | 5 s | `export_motion_video` |
| 2 | a sketch, constrained, extruded; one variable changed and the part follows | 15 s | demo scene `parametric` |
| 3 | the history panel: a checkpoint, a rollback, the diff of the text format | 10 s | demo scene `history` |
| 4 | an agent builds a part from a prompt | 20 s | live recording |
| 5 | an FEM study's stress plot; a CAM toolpath simulating its cut | 15 s | demo scenes `fem`, `cam` |
| 6 | `check_print` on a print-in-place part; the printed part | 10 s | demo scene `print` + a photo |
| 7 | the log end, the lockup, the URL | 5 s | `generators/logo/`, `assets/brand/` |

## Files

- `src/app/main.cpp`: the capture copy in the frame loop, `PostSwap`, the
  two new arguments; `ScreenCaptureFunc` and the engine's video settings
  (ffmpeg from `PATH`, `IMGUI_CAPTURE_DEFAULT_VIDEO_PARAMS_FOR_FFMPEG`).
- `src/app/guitests/GuiTestsDemo.cpp` (new) and its registration; the suite
  runner's filter and count (`tools/gui-tests-xvfb.sh`).
- `tools/demo/record.sh` (new).
- `docs/ui/testing.md`: the `demo` category, the speed and capture flags;
  a short `tools/demo/README.md` on recording and the shot list.

## Verification

- A GUI test records a two-second clip of a scene that orbits the camera
  and opens a panel, and checks the file: 120 frames at 1920x1080, not
  black, the panel's pixels present, consecutive frames differing.
- `tests/render_pixel_smoke.py` passes unchanged (no capture, no copy).
- The demo scenes pass in the GUI suite at fast speed, recording nothing;
  a launch without `--capture-dir` creates its swapchain exactly as before.
- Each scene recorded by `tools/demo/record.sh` is watched end to end
  before it goes into the edit.
