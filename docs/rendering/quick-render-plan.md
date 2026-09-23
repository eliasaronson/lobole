# Quick render: a still of what the viewport draws

## The problem

The Render dialog and `render_still` only path-trace. The viewport's own
raster look — materials, three lights, shadow map, SSAO, outlines — often
reads better than a path-traced still at anything below the top settings,
and it takes a fraction of a second where a clean trace takes minutes. There
is no way to save it at a chosen size: a screenshot is the window, panels and
all, at the window's resolution, with every overlay (sketch geometry, datum
gizmos, selection tints, handles) in it.

## The shape of the fix

A **quick** mode beside the path tracer, in the Render dialog and as
`render_still {mode: "quick"}`:

1. **Renders the scene into an image of the requested size.** `main.cpp`
   installs `AppState::captureRaster` (the shape `renderBurst` already has: a
   capability only main.cpp can provide, reachable from `ui/` and `remote/`),
   in both the windowed and the `--offscreen` loop. It points every scene
   renderer's viewport and the SSAO targets at W x H, projects the camera at
   W/H, records the prepasses (shadow map, SSAO) and the draw into
   `CaptureScreenshot`'s own attachments, then restores every size it changed
   — the viewport rect, the SSAO targets, the line widths — so the next frame
   is the frame it would have been.
2. **Only the model.** It records the mesh, the FEM field and the outlines,
   and none of the overlay renderers (sketch, underlay, CAM stock), and it
   clears the selection and hover tints first. What the app draws, minus
   what the app draws ON it.
3. **Supersampled.** `supersample` (1-4, default 2) renders at k x W by
   k x H and averages down in linear light (`DownsampleToPng`,
   render/Screenshot.cpp), on top of the pipelines' own MSAA; line widths are
   scaled by k so they come out as the viewport draws them. k is lowered, and
   the response says so, if k x W passes the device's image limit.
4. **Honest about its inputs.** Path-tracer-only parameters (`samples`,
   `seed`, `ground`, `backdrop`, `outlines`) are refused in quick mode rather
   than ignored; a picture that is one colour is saved with a warning, as a
   traced one is. Quick mode needs a renderer, not ray tracing, so the Render
   dialog and `view.render` open on a GPU without RT and offer quick only.

## Files

`src/ui/AppState.h` (`RasterStill`, `captureRaster`), `src/app/main.cpp`,
`src/render/Screenshot.{h,cpp}` (`DownsampleToPng`), `src/ui/Toolbar.cpp` (the
dialog), `src/ui/Commands.cpp` (the gate), `src/remote/RemoteDispatch.cpp`
(`render_still`), `tools/designer_mcp.py`, docs/rendering/r4-offline-render.md.

## Verification

- A smoke on an `--offscreen` instance: `render_still {mode: quick}` writes a
  PNG of exactly the requested size, not one colour, in seconds rather than
  a trace's minutes; a path-tracer parameter in quick mode is refused naming
  it; the viewport's own screenshot is byte-identical before and after
  (nothing left changed). `tests/render_quick_smoke.py`.
- The GUI test for the Render dialog drives quick mode through the dialog
  and finds the file.
- `render_pixel_smoke` unchanged; the full suites.
