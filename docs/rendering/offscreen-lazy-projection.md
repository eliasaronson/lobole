# `--offscreen` projects the document only when something looks

## The measurement

`--offscreen` used to run the windowed loop's per-frame sync on its own 2 ms
tick. That sync is version-gated, so it fired once per document edit rather
than once per tick — but it is O(every body), and it removes and re-adds all
of them. On a 200-body part, measured on this machine:

| | edit latency (RPC round trip) |
|---|---|
| `--headless` (no renderer) | 66 ms |
| `--offscreen`, eager | **1778 ms** |
| `--offscreen`, lazy | **141 ms** |

Each of those eager edits spent 2–5 s of renderer work on a picture nobody
had asked for. The tessellation cache does not save you here: a
single-parameter edit re-runs the whole feature tree, so every body comes
back as a NEW shape and the cache — keyed on shape identity — misses all 200.

## The rule

The tick projects nothing. Everything that reads the projection pulls it
first, through `ui::SyncRenderersToDocument`:

- a capture (`screenshot`, and each `render_sheet` tile) — `SyncOffscreenScene`
  in main.cpp, which also does the render-target maintenance that only a frame
  cares about (shadow/SSAO recreate + rebind, clip planes, deform scale);
- `select_face` and `orient_to_face`, which resolve an element name through
  `documentMeshes`;
- `rt_build` and `render_still`, which read the raster tessellation cache
  rather than re-meshing every body.

`TickCamPlayback` stays on the tick: playback is wall-clock and has to advance
whether or not anyone is looking. The windowed loop is unchanged — it draws
every frame, so it projects every frame.

## What a stale projection may not do

Skipping the pull does not fail loudly. `documentMeshes` still has the right
LENGTH after an edit, so a body index reads back a plausible mesh id belonging
to superseded geometry, and `select_face` then picks a real face that is not
the one you named. Ask `ui::RenderMeshesCurrent()` instead of assuming.

`get_state` is the one reader that deliberately does NOT pull: it is the hot
poll, and forcing the projection there would hand back everything measured
above. It reports `bodies[].mesh` only while the projection is current and
omits it otherwise — an absent key is a loud failure, a wrong id is a silent
one. Callers wanting a face after an edit should use the stable element name
(`list_elements` → `select_face(element=…)`), which pulls for itself; the
`{mesh, face}` pair has always been the legacy route, and its ids have always
changed on regeneration.

## The gate

`tests/offscreen_smoke.py` counts the app's own `Document synced` log lines:
five edits with nothing looking must produce zero, the next capture exactly
one, and that one must be of the current document. A second phase pins the
omission of `bodies[].mesh` while the projection is behind. Restoring the
eager call to the tick fails the first phase with "6 syncs" — checked, not
assumed.
