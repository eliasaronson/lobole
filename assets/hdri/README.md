# HDRI environments

Studio environment maps used for image-based lighting (see
[docs/rendering/showcase-v1.md](../../docs/rendering/showcase-v1.md), phase R3).

## brown_photostudio_02_1k.hdr

- Source: [Poly Haven](https://polyhaven.com/a/brown_photostudio_02)
- License: **CC0** (public domain — no attribution required, but credited here
  as a courtesy)
- A neutral two-softbox photo studio: even, soft key lighting with enough fill
  that shadowed faces don't crush to black, and clean reflections for metals.
- 1K equirectangular is intentional — it is downsampled to a small irradiance
  map + prefiltered specular mips at load, so higher resolution would be wasted.
