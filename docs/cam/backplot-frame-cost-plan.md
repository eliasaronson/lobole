# The backplot costs nothing on an idle frame

## The problem

With the helical rolling pin's job open, an idle frame takes 63 ms on the
user's machine and 30 ms on a 1920x1080 Xvfb run: 15 fps with the GPU at
19 ms. The frame HUD puts 93% of it in `ui`.

The run was profiled with `tools/profile.sh`, using a GUI-engine case that
opens the job and turns 300 idle frames. The idle stretch is **93%
`memmove`**, all of it the backplot:

| where | share of idle samples |
|---|---|
| `AppendCamToolpathOverlay` → `prims` (the cache copied into a fresh vector) | ~69% |
| `SketchRenderer::UploadRaw` (the same vector copied into the mapped buffer) | ~24% |

The backplot is 678,816 vertices, which is 113k segments at 56 bytes per
vertex, about 38 MB. Its vertices are cached (`g_backplot`, keyed on
`BackplotKey`), but a cache that is then copied twice per frame is not
cached where the cost is paid:
- **Once into `prims`.** A new 38 MB allocation every frame, page faults
  included, and the previous frame's copy is freed.
- **Once into the per-frame vertex buffer.**

The waveguide fix (the comment above `g_backplot`) stopped the rebuild, and
the copy was left standing.

## The fix

The backplot gets **its own channel in `SketchRenderer`**: a shared, immutable
vertex list plus a version.

- `SetCachedPrimitives(std::shared_ptr<const std::vector<PrimVertex>>,
  uint64_t version)` stores the pointer. Nothing is copied.
- Each frame-in-flight slot keeps the version it last uploaded, and
  `Render` copies only into a slot that is stale. After a change there are
  `kFramesInFlight` uploads, and after that none.
- It is drawn with the primitive pipeline, before the other primitives,
  depth-tested like them. The picture does not change.
- `CachedPrimitiveCount()` and `CachedUploads()` are exposed for tests. The
  existing `backplot_reaches_the_renderer_on_a_job_tab` asks the renderer
  what it holds, and it asks the new channel.

**CamPanel splits what it hands out.**
- `CamBackplot(state)` returns the cached vertices as a `shared_ptr` with
  their key. It returns null when the path is hidden, when there is no
  toolpath, or while a simulation is playing its own path.
- `g_backplot` becomes a `shared_ptr`, and a rebuild makes a NEW vector.
  The renderer may still be holding the old one, so the old one is never
  mutated.
- `AppendCamToolpathExtras` appends the rest: the tool glyphs, or the
  played path.
- `AppendCamToolpathOverlay` stays as the two joined together, for the GUI
  tests that read the picture.
- `UI.cpp` calls the split form, and on every tab without a backplot it
  sets the channel to null.

**The Generate window's total.** "N mm of cutting" walks every move of
every toolpath on every frame the window is open. It is cached on the same
key, now exposed as `CamToolpathsKey`.

Not changed: `AppendCamVisualisation`'s framed bounds and the simulation's
per-frame path. Neither appears in the pin's profile, and the framed-bounds
cost was already answered by `JobPlacedBodies` / `WarmTightBounds`.

## Files

- `src/render/SketchRenderer.{h,cpp}`: the cached channel.
- `src/ui/CamPanel.{h,cpp}`: `CamBackplot`, `AppendCamToolpathExtras`,
  `CamToolpathsKey`.
- `src/ui/UI.cpp`: the call site.
- `src/ui/CamGenerate.cpp`: the cached total.
- `src/app/guitests/GuiTestsCam.cpp`: the gates.

## Verification

- **GUI test `cam/backplot_is_not_copied_per_frame`:**
  - After a generate, the renderer holds the backplot, and the pointer is
    the one `CamBackplot` returns (shared, not copied).
  - Ten idle frames later, `CachedUploads()` has grown by at most
    `kFramesInFlight`, and the plain primitive list does not contain the
    backplot.
- **`backplot_reaches_the_renderer_on_a_job_tab`,** re-pointed at the
  channel. It is filled when shown and empty when hidden.
- **The pin, measured the same way as the problem** (the profiling case,
  not committed): `ui` per idle frame, before and after.
- **`tests/render_pixel_smoke.py`,** because this touches a renderer.
- **Suites:** the GUI suite, and `tools/test-fast.sh`.
