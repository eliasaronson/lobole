# The release demo video

A 60-90 second video for the first release: what Loböle does, shown in the
real app, re-recordable whenever the interface changes before (or after) the
release.

## The problem

A hand-driven screen recording costs a retake per mistake and goes stale with
every UI change. The app can already be driven by script, and the GUI test
engine it bundles has a Cinematic speed (pauses between actions, a software
mouse cursor drawn into the frame) and a video recorder that pipes frames to
ffmpeg, now fed with frames by the app. What is missing is the scenes, the
script that records them, and the footage around them.

## The shape of it

1. **Full-window capture for the engine** is built: the app re-renders
   each frame the engine asks for, and the engine encodes the clip
   (docs/ui/testing.md#recording-a-scene, `capture/records_a_clip`).
2. **Demo scenes as GUI tests**, category `demo`, in
   `src/app/guitests/GuiTestsDemo.cpp`. Each scene drives the real widgets
   at Cinematic speed and wraps itself in `CaptureBeginVideo` /
   `CaptureEndVideo`. They run in the GUI suite like any other test, at the
   suite's fast speed and with nothing recorded (no `--capture-dir`), so a
   UI change that breaks a scene fails the suite the day it lands rather
   than at the next release. A recording run passes `--gui-test-speed
   cinematic` and `--capture-dir`.
3. **`tools/demo/record.sh`**: one Xvfb display at 1920x1080, a scratch
   config and state directory (the scenes must not touch the user's), every
   scene recorded to `scratch/demo/<scene>.mp4`. A GUI test run opens its
   window at 1280x720 on purpose -- the layout tests assert what fits at
   that size -- so the video's 1920x1080 needs a `--window-size WxH` that
   only a run asking for it gets, and the suite never does.
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

- `src/app/guitests/GuiTestsDemo.cpp`: the scenes, beside
  `capture/records_a_clip`; each records through `BeginRecording` /
  `EndRecording` when `GuiTestCaptureDir()` is set.
- `tools/demo/record.sh` (new).
- `docs/ui/testing.md`: the `demo` category; a short `tools/demo/README.md`
  on recording and the shot list.

## Verification

- The demo scenes pass in the GUI suite at fast speed, recording nothing.
- Each scene recorded by `tools/demo/record.sh` is watched end to end
  before it goes into the edit.
