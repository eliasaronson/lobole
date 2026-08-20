# Per-frame GPU resources must be sized to the swapchain, not to 2

> Status: **plan, then built 2026-08-20.** Written from a user report —
> "rotating the camera shakes/wobbles on the Intel GPU, and the size of the
> shaking scales with the window size".

## The bug

`main.cpp` hands every renderer the **swapchain image index**:

```cpp
g_MeshRenderer->Render(fd->CommandBuffer, wd->FrameIndex);  // 0 .. ImageCount-1
```

Each renderer sizes its per-frame resources — the camera/light UBO, its
descriptor sets, the outline renderer's per-mesh index buffers — to a
hard-coded `MAX_FRAMES_IN_FLIGHT = 2`, and wraps the caller's index to fit:

```cpp
const uint32_t frame = currentFrame % MAX_FRAMES_IN_FLIGHT;
```

That wrap was added for a real crash (an out-of-bounds `memcpy` through a
garbage pointer, caught by ASan **on the Intel Arc**). It stopped the
corruption and introduced aliasing: on a 3-image swapchain, images 2 and 0
share slot 0.

Sharing is not safe. The app waits on the *acquired image's* fence before
recording, which says nothing about the frame that used image 2 — that
submission can still be reading slot 0 while the CPU overwrites it for image
0. Some frames therefore draw with a view matrix from the wrong frame, and
during a drag the image snaps between two camera positions.

**Why only Intel.** Measured on this machine: the RTX 4060 reports
`ImageCount = 2`, so `% 2` is the identity and nothing ever aliases. Mesa
hands out 3 or more, so the wrap starts folding.

**Why the amplitude scales with the window.** The error is a fraction of one
camera step — an angular error, which is constant in NDC. Its size *in
pixels* is therefore proportional to the viewport, and it also grows with drag
speed and with a lower frame rate (both make the per-frame camera delta
bigger). That report was the clue that ruled out frame pacing, which would
scale with rotation speed alone.

Three renderers wrap (`MeshRenderer`, `OutlineRenderer`, `SSAORenderer`);
`FaceHighlighter` carries the same constant.

## A second bug of the same family, found while verifying the first

Forcing a 3-image swapchain on the NVIDIA and turning on synchronization
validation produced **10 `SYNC-HAZARD-WRITE-AFTER-WRITE` errors before the
camera was even touched** — and they are not the UBO:

```
vkCmdBeginRenderPass ... writes to resource, which was previously written at
the end of subpass 0 (vkCmdEndRenderPass) by the attachment storeOp.
No sufficient synchronization is present to ensure that a layout transition
does not conflict with a prior write (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_
WRITE_BIT) at VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT.
```

Every framebuffer shares one depth image (and one MSAA colour image), which is
fine — frames execute in order on one queue — **provided the render pass says
so**. It does not:

```cpp
dep.srcStageMask = COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS;
dep.dstStageMask = COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS;
dep.dstAccessMask = COLOR_ATTACHMENT_WRITE | DEPTH_STENCIL_ATTACHMENT_WRITE;
//   srcAccessMask is never set — it is 0
```

`srcAccessMask = 0` means the pass does not wait for the *previous* frame's
depth write at all, and the depth store completes at `LATE_FRAGMENT_TESTS`,
which `srcStageMask` does not name. So the fix is to complete the dependency,
not to allocate one depth buffer per image: three lines, no extra memory. (A
per-image depth buffer would also work and would cost ~32 MB per image at 4K —
the wrong trade for a missing bitmask.)

The two bugs are independent and both ship in this change. Only the depth one
is visible to validation: the UBO slots are host-mapped, and sync validation
does not track CPU writes through a persistent mapping — which is exactly why
this survived so long.

## The fix: capacity, not plumbing

Give the per-frame arrays a **capacity** of `render::kMaxFramesInFlight = 8`
and index them with the image index **directly**, with no modulo.

The obvious alternative — plumb `ImageCount` into each `Initialize()` and size
the vectors to it — is more precisely "correct" and was rejected:

- The image count can change when the swapchain is recreated (resize, present
  mode). Nothing in the app re-initializes renderers today, so exact sizing
  would need a new resize path — and a new resize path is a new way to be
  wrong about exactly this.
- The cost of over-allocating is nil *for the cheap arrays*. A UBO slot is a
  few hundred bytes; eight descriptor sets instead of two is noise.

The outline renderer was the exception, and this doc originally claimed
otherwise: its per-mesh edge index buffers were created and mapped for
**every** slot when a mesh was added, so raising the capacity multiplied them
per mesh — meaningful memory on an assembly with dozens of parts. They are
now created **on demand** (`EnsureEdgeBuffer`, from the upload path), so only
the 2-3 slots a real swapchain touches ever exist. That also made the GUI
suite slightly faster (68 s vs the 80-92 s baseline): a mesh that is added
and destroyed without rendering now allocates nothing.

8 is chosen with margin: swapchains are 2–4 in practice, occasionally 5–6
with MAILBOX. If a driver ever exceeds it the renderer logs once and falls
back to the modulo — today's behaviour, wobble and all, rather than a crash or
a silent out-of-bounds write.

## Verification

The mechanism is a **write-after-read hazard**, which is precisely what
Vulkan's synchronization validation reports. So the check does not need an
Intel GPU:

1. Force the aliasing regime on this machine: `g_MinImageCount = 3` (the
   NVIDIA then reports a 3-image swapchain).
2. Run with `DEV_WATCH_VALIDATE=1` (sync + GPU-assisted validation).
3. **Before the fix**: 10 `SYNC-HAZARD-WRITE-AFTER-WRITE` on the depth
   attachment, at startup. (The UBO aliasing is invisible here — see above —
   and is established by arithmetic instead: `2 % 2 == 0 % 2`.)
4. **After the fix**: none — verified through 40 scripted camera moves at
   `ImageCount = 3` — and both suites pass at the normal 2.

One measurement lesson, because it nearly caused a pointless refactor: the
first post-fix GUI run took **1125 s** instead of ~80, which read as a bad
regression from the extra slots. It was a laptop suspend mid-run (plus a
stray preview instance on the GPU). Re-running with nothing contending gave
92 s. Wall-clock on a laptop is not evidence until it is repeated.

Also worth having, because this class of bug is invisible without it:

- **Log the swapchain image count at startup.** Nobody could have guessed
  "your GPU gives 2 and theirs gives 3" from the app's output.
- **A guard that cannot be forgotten**: each renderer asserts the incoming
  index is inside its capacity, so the next renderer to be written cannot
  quietly reintroduce the wrap.
