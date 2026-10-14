# Debugging the GPU

Two different questions, and reaching for the wrong tool is most of the time
lost:

| question | tool |
|---|---|
| **Did we use Vulkan correctly?** A freed buffer still in flight, a missing barrier, an out-of-bounds read in a shader. | validation layers |
| **What did this frame actually draw?** The pass ran, the API was used legally, and the picture is still wrong. | a frame capture (RenderDoc) |

Neither measures time — that is the frame-time instrument
([frame-pacing.md](frame-pacing.md)) — and neither reports device memory,
which nothing in the app does yet.

## Validation layers — did we use the API correctly

```
DEV_WATCH_VALIDATE=1 tools/dev-watch.sh
```

Sets `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` with **synchronization
validation** and **GPU-assisted validation** enabled, so the problem is named
in the dev-watch log. Run it inside `nix-shell` so `VK_LAYER_PATH` is set
(`tools/dev-watch.sh` derives it from nixpkgs as a fallback). It is slow — a
debugging mode, not a default. What it finds:

- **use-after-free of in-flight resources** — a buffer destroyed while a
  submitted frame still reads it, which surfaces as `VK_ERROR_DEVICE_LOST`
  without validation;
- missing or wrong barriers, layout transitions, descriptor mismatches, and
  write-after-write hazards between frames (sync validation);
- out-of-bounds buffer or image access from a shader (the GPU-assisted half).

Sync validation does **not** track CPU writes through a persistently mapped
buffer, so a per-frame UBO shared between two frames in flight is invisible
to it; that class is prevented structurally
([frame-pacing.md#frames-in-flight](frame-pacing.md#frames-in-flight)).

CPU-side memory errors are a different tool: `tools/run-sanitized-app.sh`
runs the windowed app under ASan + UBSan (build `build-asan` with
`-DENABLE_SANITIZERS=ON` first). It sees host allocations and does **not**
see GPU memory.

## Frame capture — what the frame actually drew

Validation is silent when every call is legal and the result is still wrong:
a matrix built in the wrong space, a texture sampled at the wrong
coordinates, a pass reading last frame's attachment. For those, capture the
frame and look at each draw's inputs and outputs. Loböle is an ordinary
Vulkan target, so RenderDoc works with no changes to the app.

```
nix-shell -p renderdoc --run qrenderdoc
```

Launch the app from RenderDoc's **Launch Application** tab:

| field | value |
|---|---|
| Executable | `build/lobole` |
| Working directory | the repo root |
| Command line | `--remote <port>` (a scratch port — `tools/scratch_port.py` picks one) |

`F12` (or the capture button) grabs a frame.

- **Run it from inside `nix-shell`** (`nix-shell shell.nix --run qrenderdoc`
  with renderdoc also in scope), or the app will not find its Vulkan loader
  and OCC libraries.
- **Drive the capture from the remote**: set the camera and render options
  over `python/lobole/client.py` so the frame you capture is the frame you
  meant to, and the same one can be captured again after a fix.

What to look at, in order:

1. **The pass list.** Is the pass there at all, and in the expected order? A
   renderer missing from one of its registrations is the most repeated
   rendering bug class here ([README.md#scene-renderers](README.md#scene-renderers)).
2. **The inputs to the failing draw** — descriptor sets, the bound image, the
   push constants. The mesh push-constant block is exactly at the 128-byte
   Vulkan-guaranteed minimum, so a field added to it silently displaces
   another.
3. **The output attachment after each pass**, not just the final image. A
   correct AO buffer sampled with the wrong coordinates looks right in the
   intermediate and wrong in the composite — the split a capture shows and a
   screenshot cannot.
4. **Viewport and scissor**, whenever something is offset rather than wrong.
   The 3D viewport is a SUB-RECT of the window in the real app and the WHOLE
   framebuffer under `--offscreen`, so a full-window assumption is invisible
   offscreen and wrong in the app. Capture the *windowed* build for these.

A capture is a **single frame**. A bug that needs two frames to appear — a
stale attachment, two frames in flight sharing a resource — looks like "this
frame is fine"; the tool for it is sync validation plus reading the resource
lifetimes.
