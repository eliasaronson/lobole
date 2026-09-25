# The motion video looks like the app

## The problem

A motion study played in the app is smooth and looks good. The same study
exported with `export_motion_video` is choppy, looks clearly worse, and is
slow to make, so a screen recording beats it. Three likely causes, each to
be confirmed before it is fixed:

- **What is captured.** Frames come from the quick render's raster path
  (`captureRaster`, model renderers only), not from the viewport's own frame.
  So they probably lack the viewport's outlines, SSAO, shadows, grid and
  lighting, or render them at other settings.
- **How many frames.** One frame per study sample: 49 frames at about
  12 fps for the OM615. The app's playback interpolates between samples at
  the display's rate.
- **How it is encoded.** Default H.264 settings, possibly without
  `yuv420p`, CRF or a bitrate chosen for quality.

## The shape of the fix

- **Capture the viewport's own frame.** Each frame is the viewport's full
  render at the requested size: the renderer set, the settings and the
  effects the user sees, minus the UI overlays. Offscreen mode must match
  windowed (CLAUDE.md: the offscreen framebuffer has no panels).
- **Play it the way the app plays it.** Frames are interpolated between
  samples with the same pose interpolation the in-app playback uses. The
  frame rate defaults to the display's usual 30 or 60, and the duration to
  what the in-app playback takes.
- **Encode for quality.** H.264 at CRF about 18, `yuv420p` for players,
  and `+faststart`. VP9 at comparable quality. GIF keeps its palette pass.
  The `quality` parameter maps to these.
- **Fast.** Readback is pipelined (see the quick render's uncached-readback
  fix in the memory note on still quality) and frames stream to ffmpeg
  without per-frame stalls. Measure frames per second before and after.

## Files

- `src/ui/MotionVideo.cpp`, `src/render/Screenshot.{h,cpp}`, the viewport
  render path it hooks, and the playback interpolation it reuses.
- `docs/assemblies/README.md#motion-video`.

## Verification

- A side-by-side: an in-app screenshot and the matching video frame (same
  pose, same camera, same size) are compared by eye and by a pixel diff. They
  must be the same picture, apart from the codec.
- The OM615 two revolutions exported at 1280×720, 60 fps. Report the
  frames, the export time, the file size, and a frame to look at.
- `motion_video_smoke`, the GUI test, and `render_pixel_smoke`.
