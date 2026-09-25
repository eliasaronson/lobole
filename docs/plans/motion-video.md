# Motion study to video

## The problem

A motion study can be scrubbed in the app and exported as a GLB animation,
but there is no way to get a video or GIF of it: the thing to put in a README,
send to someone, or attach to an issue. Today that takes a script that poses
each sample, screenshots it, and runs ffmpeg by hand.

## The shape of the fix

**One RPC, `export_motion_video {study, path, format, fps, width, height,
quality?}`**, run as a background job with progress and cancel, like the quick
render (`background_jobs`).

- **Formats.** `mp4` (H.264), `webm` (VP9) and `gif` (a palette pass for
  that animation) are encoded by **ffmpeg** as an external program. `png`
  writes numbered frames into a folder and needs nothing.
- **Missing ffmpeg is refused by name**, never replaced by another format:
  "export_motion_video format mp4 needs ffmpeg on PATH; install it, or use
  format png". `ping` reports whether ffmpeg was found (`features`), so a test
  or an agent can gate on it.
- **Frames.** Frame k poses the study at its k-th step, the way
  `set_motion_pose` does, stepping sample to sample so slider-cranks stay on
  their branch. The frame count is the study's samples unless `frames` is
  given. Each frame is captured with the same path as `screenshot`, at the
  requested size, from the current camera. The viewport's overlays (handles,
  selection) are off for the capture. Frames are streamed to ffmpeg's stdin as
  raw RGBA, so no temp files are written for video formats.
- **Defaults** come from what is being exported: the size from the viewport,
  `fps` so the study plays in about four seconds, and GIF colours from ffmpeg's
  palette pass. None are literals.
- **After export**, the assembly is left at the pose it had before, like a
  scrub.
- **GUI.** An "Export animation…" button in the Motions panel beside "Export
  GLB" opens a save dialog whose file type picks the format, and runs the
  job with the panel's progress bar.
- **SDK.** `app.export_motion_video(...)` and the MCP tool description.

## Files

- `src/remote/RemoteMotion.cpp` (the RPC and its describe row).
- `src/ui/MotionActions.{h,cpp}` (frame loop, job, ffmpeg process).
- The Motions panel UI file.
- `src/remote/RemoteDispatch.cpp` (`ping` features).
- `tools/designer_sdk.py`, `tools/designer_mcp.py`.
- `shell.nix` (ffmpeg, so tests always have it).
- `docs/assemblies/README.md` (motion studies).

## Verification

- A smoke exports a small slider-crank study to each format:
  - **png:** the frame count and size.
  - **gif:** decoded, checking the frame count and size.
  - **mp4 / webm:** `ffprobe` reports the frame count, size and codec.
- The smoke also checks the refusal with ffmpeg hidden from PATH, and that
  the assembly's pose is unchanged afterwards.
- A GUI test clicks "Export animation…" through a mocked save dialog, as the
  GLB export test does.
- Headless and GUI suites. Render the OM615's two revolutions to an MP4 and a
  GIF and look at both.
