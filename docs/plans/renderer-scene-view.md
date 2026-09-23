# Pass the frame state to a scene renderer at `Render`

## The problem

Every scene renderer keeps the viewport rectangle, the camera matrices and the
clip planes as sticky state, set through `SetViewport`, `UpdateCamera` and
`SetClipPlanes` before `Render` is called (`src/render/SceneRenderer.h`). The
registry loops in `main.cpp` make those calls uniformly, and
`tests/test_conventions.cpp` checks every `SceneRenderer` is registered — so a
missing wire is *caught*. It is still *possible*: a renderer drawn with an
unset or stale viewport draws full-window at the wrong scale and offset, which
`--offscreen` cannot show because there the viewport is the whole
framebuffer.

## The fix

Pass the frame state with the call:

```cpp
struct SceneView {
  glm::ivec4 viewport;   // x, y, w, h — the 3D sub-region between the panels
  glm::mat4 view, proj;
  ClipPlaneArray clips;
};
virtual void Render(VkCommandBuffer, uint32_t frame, const SceneView&) = 0;
```

`SetViewport`, `UpdateCamera` and `SetClipPlanes` disappear from the interface
and from all six renderers, with their call sites. The three render blocks
(windowed frame, windowed screenshot, offscreen capture) already hold all of
it, so the per-frame contract becomes one loop:

```cpp
for (SceneRenderer* r : g_Scene)
  r->Render(cmd, frame, view);
```

The part that is real work: `SSAORenderer::RecordSSAOPasses` takes a
`MeshRenderer&` and reads the camera matrices and shadow casters out of it, on
purpose, so the prepasses match the main pass exactly. Removing the sticky
camera from `MeshRenderer` means handing the prepasses the same `SceneView`
(`RecordScenePrepasses` in `main.cpp`), in the AO and shadow path.

`SetViewportSize(w, h)` (framebuffer size, a different question) and the
typed `AppState` pointers the `ui/` layer uses for content stay as they are.
The registration and teardown lists and their gate stay: they are also what
catches a renderer missing from shutdown.

## Files

`src/render/SceneRenderer.h`; `src/render/{Mesh,Field,Stock,Outline,Underlay,Sketch}Renderer.{h,cpp}`;
`src/render/{SSAORenderer,ShadowRenderer}.{h,cpp}`; `src/app/main.cpp` (the
three render blocks, `RecordScenePrepasses`, `CaptureRasterStill`);
`src/ui/UI.cpp` (the viewport loop over `AppState::sceneRenderers`);
docs/rendering/README.md.

## Verification

- Headless suite and the GUI suite (which drives the windowed frame path and
  carries the CAM simulation's viewport-offset assertion).
- `tests/render_pixel_smoke.py` byte-identical, `tests/render_ao_smoke.py`
  (AO sampled at the right place in the viewport sub-rect),
  `tests/render_quick_smoke.py`, `tests/offscreen_smoke.py`.
- Windowed screenshots, looked at: a part with a section plane, a sketch in
  edit over a reference image, a FEM field, a CAM simulation — compared with
  the same four taken before the change.
