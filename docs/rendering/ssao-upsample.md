# SSAO joint-bilateral upsample — near-full AO at the half tier's cost

Status: **SHIPPED 2026-08-26.** As-built, measured on the RTX rig
(offscreen, the AO smoke's plate+slot scene):

- **The upsample is live and exactly local**: new-half vs old-half differ by
  mean 0.0012/255, and the amplified difference map lights ONLY silhouette
  and contact edges — on equal-depth surfaces the bilateral weights collapse
  to exact bilinear, as designed.
- **Half vs native full: mean 0.14/255**; strong occlusion (>=8/255) is
  2.9k px in both and lands identically.
- **A finding the first A/B run surfaced**: the half tier carries a faint
  4-7/255 wash on open surfaces (~19k px) that full does not — coarse-depth
  self-occlusion bias in the half-res AO COMPUTATION. Measured identical
  before and after the upsample (18904 vs 18908 px), so it predates this
  work and had simply never been measured (the smoke only ever tested
  `full`). The smoke's half-tier locality invariant therefore runs at the
  >=8 threshold where displaced-occlusion detection is meaningful.
- **RTX perf cannot resolve the win** (~2-3 ms buckets wandering under DVFS
  in a vsync frame — the frame-timing doc's laptop caveat, observed again).
  The Intel claim is arithmetic (AO+blur at a quarter of the pixels + one
  5-tap full-res pass) and is held to the owner's Arc HUD after rebuild:
  predicted everything-max **~40 -> ~31 ms** running `half`.

Planned 2026-08-26, built the same day. The Intel-GPU follow-on the
GPU timestamps were built to justify (or kill): the field matrix from the
owner's Arc (MTL) session, GPU-bound in every config, reads

| config | frame | fps |
|---|---|---|
| everything max | 40.2 ms | 24.9 |
| shadows off | 38.4 ms | 26.0 |
| SSAO off | 28.6 ms | 35.0 |
| both off | 23.7 ms | 42.2 |

Full-res SSAO costs **~12–15 ms of the frame** (from the frame-time deltas —
the honest cross-config metric; the per-pass split wobbles across configs
even saturated, from inter-pass overlap attribution). The 23.7 ms floor is
geometry + 4x MSAA and belongs to a future MSAA setting, not to this item.

## The shape: upgrade `half`, keep `full` untouched

The `off | half | full` vocabulary does not change, and no mode is added:

- **`half`** (the default, and what the Arc runs): AO is computed at half
  res exactly as today — the change is that the plain hardware-bilinear
  stretch mesh.frag effectively does today is replaced by a **joint-bilateral
  upsample** pass into a full-res R8 target, guided by the full-res prepass
  depth. Bilinear bleeding AO across depth edges is the visible defect of
  today's half tier (silhouette halos, contact shadow smear); this is its
  fix, at the cost of one cheap fullscreen pass (5 taps/pixel).
- **`full`**: native full-res AO, byte-for-byte today's path. Kept
  deliberately: it is the permanent ground truth every tuning session and
  regression check diffs against, and the escape hatch if a scene ever shows
  an upsample artifact. "Upsampled full" and "improved half" compute the
  same thing — there is no fourth mode to invent.
- **`off`**: unchanged.

Predicted effect on the Arc: everything-max ~40 → **~31 ms (~32 fps)** by
running `half` that now looks near-`full`. NVIDIA stays on `full`, identical
pixels to today (vsync-bound in every config there; nothing to win).

## Implementation (all inside SSAORenderer)

One new Target `m_aoUpsample` (R8, **full** res) + framebuffer — created
unconditionally (one R8 image of memory keeps the descriptor set legal
across SetFullResolution toggles), recorded only when `!m_fullRes`; one
pipeline on the existing `m_r8RenderPass` (it is
format-only, resolution-agnostic, and carries the write→sample subpass
dependencies the other passes already rely on); one descriptor set (blurred
AO + prepass depth — the blur set's shape). `RecordSSAOPasses` appends the
pass after the blur in half mode. `AOView()` returns the upsampled target in
half mode, the native blur target in full mode — mesh.frag is untouched (it
samples by normalized UV, so texture size is invisible to it), and the
existing `Resize`/`SetFullResolution` rebind flow already re-points it.

The shader reuses the blur's exact weighting convention (raw [0,1] depth
difference, `exp` falloff, push-constant sharpness): each full-res pixel
blends the 4 surrounding half-res AO texels by bilinear-proximity ×
depth-similarity. A pixel whose whole 2x2 neighborhood lies across a depth
edge (thin silhouette against far background) takes the depth-nearest texel
instead of a 0/0 blend — the classic nearest-depth fallback, and the case
plain bilinear gets maximally wrong today.

GPU cost lands inside the timestamps' `ssao` bucket automatically.

## What can look worse, and the honest bound

AO features narrower than ~2 full-res pixels cannot be resolved from
half-res depth samples — a sub-pixel gap's contact darkening softens. That
bound holds for `half` today too (which additionally bleeds); `full` remains
available for anyone who cares about it. Mistuned depth sharpness shows as
edge halos — a dev-watch screenshot loop item, and it fails visibly.

## Verification

- **A/B pixel bound, permanent** (`render_ao_smoke.py` gains a phase): the
  same scene rendered at `half` (upsampled) and `full` (native) must agree
  within a mean-absolute-difference bound on the viewport; and upsampled
  `half` must sit CLOSER to native `full` than a deliberately mis-guided
  upsample would — so the test fails if the depth guide stops working, not
  only if the image changes wholesale. Thresholds set from measured values,
  recorded here as-built.
- **Perf**: GPU-timestamp `ssao` bucket at half vs full on the RTX offscreen
  (direction + magnitude); the real claim is settled by the owner's Arc HUD
  after rebuild — predicted everything-max ~31 ms.
- **Mode cycling**: the settings path off→half→full→half already exercises
  target recreation + rebind; it now also covers creating/destroying the
  upsample target (the existing GUI settings coverage runs it).
- **Visuals**: before/after screenshots on real parts, sent for the owner's
  judgement — "looks right" is theirs to call.
