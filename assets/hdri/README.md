# HDRI environments

Studio environment maps used for image-based lighting (see
[docs/rendering/showcase-v1.md](../../docs/rendering/showcase-v1.md), phase R3,
and [r4-offline-render.md](../../docs/rendering/r4-offline-render.md) for the
path-traced still).

The filename lives in ONE place per language — `RuntimePaths::
kEnvironmentHdriFile` and CMake's `DESIGNER_HDRI` — because it used to be
written out at six sites, which is how a resolution nobody revisits gets six
places to be wrong in.

## brown_photostudio_02_2k.hdr

- Source: [Poly Haven](https://polyhaven.com/a/brown_photostudio_02)
- License: **CC0** (public domain — no attribution required, but credited here
  as a courtesy)
- A neutral two-softbox photo studio: even, soft key lighting with enough fill
  that shadowed faces don't crush to black, and clean reflections for metals.

### Why 2K, when 1K used to be "intentional"

The 1K file this replaced carried the note *"1K equirectangular is intentional
— it is downsampled to a small irradiance map + prefiltered specular mips at
load, so higher resolution would be wasted."* That was true when it was
written, and stopped being true at R4: the full-resolution source is now kept
through the precompute and uploaded as its own texture (binding 8), which the
miss shader and near-mirror reflections (roughness < 0.15) sample directly.
So the resolution IS visible, in two places — the background when a still asks
for `backdrop: environment`, and chrome.

That stale sentence is the reason nobody revisited the resolution while the
render backgrounds looked like a smear (2026-08-31).

2K rather than 4K: 6 MB against 24 MB, for a difference that only shows in a
reflection already blurred by roughness. If the background sharpness of an
`environment` still ever matters more than the download, the 4K is the same
asset at the same URL and the only change is this filename in two places.
