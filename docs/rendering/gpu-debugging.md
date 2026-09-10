# Debugging the GPU

> The two halves of "why does this frame look wrong", and which tool answers
> which. Written 2026-08-31 after a review found the second half undocumented
> (docs/capability-review-2026-08-31.md, P4).

There are two different questions, and reaching for the wrong tool is most of
the time lost:

| question | tool |
|---|---|
| **Did we use Vulkan correctly?** A freed buffer still in flight, a missing barrier, an out-of-bounds read in a shader. | validation layers |
| **What did this frame actually draw?** The pass ran, the API was used legally, and the picture is still wrong. | a frame capture |

The first is well covered here and has caught real bugs. The second had no
recipe at all, which is why the SSAO sub-rect bug shipped and was found later
by a hand-written invariant test.

---

## Validation layers — did we use the API correctly

```
DEV_WATCH_VALIDATE=1 tools/dev-watch.sh
```

Turns on `VK_LAYER_KHRONOS_validation` with **synchronization validation** and
**GPU-assisted validation**, and names the problem in the dev-watch log. Run it
inside `nix-shell` so `VK_LAYER_PATH` is set (the script derives it from
nixpkgs as a fallback).

It is slow — this is a debugging mode, not a default. What it is good at:

- **use-after-free / in-flight resources.** The two-extruded-parts device-lost
  was `OutlineRenderer` freeing a buffer the GPU was still reading; validation
  named it directly (see the memory note, and `known-bugs.md` history).
- missing or wrong barriers, layout transitions, descriptor mismatches;
- out-of-bounds buffer/image access from a shader (that is the GPU-assisted
  half — it costs a lot of performance, so it is bundled here rather than on
  by default).

CPU-side memory errors are a different tool again: `tools/run-sanitized-app.sh`
(ASan + UBSan) sees the host allocations, and explicitly does **not** see GPU
memory.

---

## Frame capture — what the frame actually drew

Validation is silent when every call is legal and the result is still wrong: a
matrix built in the wrong space, a texture sampled at the wrong coordinates, a
pass reading last frame's attachment. For those, capture the frame and look at
each draw's inputs and outputs.

Designer is an ordinary Vulkan target, so RenderDoc works with no changes to
the app.

```
nix-shell -p renderdoc --run qrenderdoc
```

Launch the app from RenderDoc's **Launch Application** tab:

| field | value |
|---|---|
| Executable | `build/designer` |
| Working directory | the repo root |
| Command line | `--remote 8901` (a scratch port — `tools/scratch_port.py` picks one) |

Then `F12` (or the capture button) grabs a frame. Two notes specific to this
app:

- **Run it from inside `nix-shell`**, or the app will not find its Vulkan
  loader and OCC libraries. `nix-shell shell.nix --run 'qrenderdoc'` with
  renderdoc also in scope is the simplest arrangement.
- **Drive the capture from the remote** rather than by hand: set the camera and
  the render options over `tools/designer_remote.py` so the frame you capture
  is the frame you meant to capture, and so the same one can be captured again
  after a fix. A frame captured by orbiting until it looks wrong cannot be
  compared with anything.

What to look at, in the order that has paid off here:

1. **The pass list.** Is the pass you are debugging there at all, and in the
   order you expect? A renderer that shipped with a wire missing is this
   project's most repeated rendering bug — see
   [renderer-wiring.md](renderer-wiring.md), where every one of the last three
   renderers arrived with one of its two registrations absent.
2. **The inputs to the failing draw** — descriptor sets, the actual bound
   image, the push constants. The push-constant block is at the 128-byte
   Vulkan floor here (the surface-pattern shader), so an added field silently
   displaces another.
3. **The output attachment after each pass**, not just the final image. The
   SSAO bug was a correct AO buffer sampled with the wrong coordinates: the
   intermediate looked right and the composite did not, which is exactly the
   split a capture shows and a screenshot cannot.
4. **Viewport and scissor**, whenever anything is offset rather than wrong. The
   3D viewport is a SUB-RECT of the window in the real app and the WHOLE
   framebuffer under `--offscreen` (CLAUDE.md), so a full-window assumption is
   invisible offscreen and wrong in the app. That asymmetry has produced two
   bugs; a capture from the *windowed* build is the one that shows it.

## What each tool cannot tell you

- **Neither** reports device-memory usage or leaks. Nothing does yet — see
  P2 in [../capability-review-2026-08-31.md](../capability-review-2026-08-31.md):
  every buffer takes its own `vkAllocateMemory`, a 299-part assembly builds
  1196 of them, and no allocation count or heap budget is reported anywhere.
- **Neither** measures time. That is `--perf-hud` / `Ctrl+Shift+F` /
  `frame_stats`, which carry per-pass GPU timestamps and worst-frame
  attribution ([frame-timing.md](frame-timing.md)).
- A capture is a **single frame**. A bug that needs two frames to appear — a
  stale attachment, a frame-in-flight aliasing bug like the Intel camera
  wobble ([frames-in-flight.md](frames-in-flight.md)) — shows up as "this
  frame looks fine", and the tool for it is the validation layer's
  synchronization mode plus reading the resource lifetimes.
