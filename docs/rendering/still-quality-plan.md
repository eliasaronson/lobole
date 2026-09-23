# Still quality: smooth traced outlines, fast readback, quick renders off the frame

## The problems

1. **A traced still's outlines are stair-stepped.** The still finds its edges
   as discontinuities in a primary-ray G-buffer (body id, face normal), and
   the edge mask is averaged down into coverage. That G-buffer is 2x2 per
   output pixel up to ~2 Mpx and 1x1 above — a memory cap, since it is 16
   bytes a ray and read back whole. At 2560 x 1600 every line is a binary
   1 px staircase.
2. **Reading a frame back is slow, and it is the whole cost of a quick
   render.** Measured on the FS chassis at 2560 x 1600: the GPU draws the 4x
   supersampled frame (10240 x 6400) in 0.3 s, and the loop that reads it
   out of the mapped buffer takes 27.3 s. The buffer is HOST_VISIBLE |
   HOST_COHERENT, which on a discrete GPU is uncached, write-combined memory,
   and the loop reads it a byte at a time. Every `screenshot` pays the same
   (1.7 s for a 4 Mpx frame), and so do the still's own two readbacks.
3. **A quick render freezes the app while it runs** and shows no progress.

## The shape of the fix

1. **Readback memory the CPU can read.** `vkutil::CreateReadbackBuffer`
   picks HOST_VISIBLE | HOST_CACHED (coherent if there is one, else it
   invalidates after the copy), falling back to coherent-only on a device
   without cached memory. Used by all three readbacks: `CaptureScreenshot`,
   the still's radiance readback and its outline G-buffer. A capture then
   settles the silhouette lines itself (`OutlineRenderer::SettleSilhouettes`):
   they are reclassified once the camera has been still for
   `silhouetteSettleMs` of WALL time, so a capture that follows a camera move
   sooner drew the static edges only — hidden until now because every
   capture spent a second in readback first.
2. **The outline G-buffer traced in strips, at 4x4.** The raygen shader
   takes a row offset and the full aux height, so one launch covers a strip
   of rows; the aux image is a strip of at most 4 M texels (64 MB), so
   memory no longer depends on the still's size and the scale is 4x4 at any
   resolution the device can address (lowered only if 4 x width passes its
   largest image). Each strip carries a halo of rows above and below — as
   many as the edge test and the line-width dilation reach — so a seam
   computes exactly what one whole-image pass would. The edge test,
   dilation and block average move out of `RayTracingRenderer` into a pure
   function (`render/StillOutlines.{h,cpp}`, in core) that takes a strip and
   returns coverage rows. The dilation radius is taken from the line width
   in aux pixels, so a 1 px line is about 1 px at any scale.
3. **Quick renders off the frame.** The capture — the only part that needs
   the renderers — stays on the main thread (a few hundred ms). The
   downsample and PNG encode run on a worker that reports progress and
   honours Cancel; the Render dialog shows a progress bar and the app keeps
   drawing. `AppState::renderRaster` becomes `captureRaster`, which returns
   the frame and writes nothing; `DownsampleToPng` takes an optional progress
   and cancel flag. The `render_still` RPC stays synchronous — it captures
   and encodes in line.

## Files

`src/render/VulkanCommon.{h,cpp}`, `src/render/Screenshot.{h,cpp}`,
`src/render/OutlineRenderer.h`,
`src/render/StillOutlines.{h,cpp}` (new), `src/render/RayTracingRenderer.{h,cpp}`,
`src/shaders/raygen.rgen`, `src/app/main.cpp`, `src/ui/AppState.h`,
`src/ui/RendererSync.cpp` (`PumpRenderJob`), `src/ui/Toolbar.cpp`,
`src/remote/RemoteDispatch.cpp`, docs/rendering/r4-offline-render.md.

## Verification

- `tests/test_still_outlines.cpp`: a diagonal boundary between two bodies at
  4x gives fractional coverage (more than two distinct values), a 1 px line
  covers about one pixel across, and strip-by-strip coverage is identical
  to one whole-image pass for strips that cut through the edge.
- Timings on the FS chassis before and after: quick render at 1x/2x/4x,
  `screenshot`, a traced still at 2560 x 1600.
- A crop of a traced still's outlines, looked at, against the quick render's.
- `render_quick_smoke`, `render_still_reach_smoke`, `render_glass_smoke`,
  `offscreen_smoke` (its deterministic traced PNG), `render_pixel_smoke`;
  the Render dialog GUI test drives a quick render to its progress bar and
  the file; the full suites.
