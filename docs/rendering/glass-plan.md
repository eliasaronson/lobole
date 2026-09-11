# Transparent glass

`glass` was a material slug that rendered as an opaque, near-white mirror:
`transmission` was declared in the ray tracer's material on both sides of the
upload and never read, and the raster viewport has no notion of it. A watch
crystal therefore hid the dial in every still, and a caseback window hid the
movement. This makes glass see-through — refractive in the path-traced still,
alpha-blended in the viewport.

## Data

`CADMaterials::MaterialProperties` gains `transmission` (0..1, 0 = opaque) and
`ior` (index of refraction). `GLASS` is 1.0 and 1.52; every other row stays 0.
This is a MATERIAL property, not an appearance override, so nothing new is
saved and the file format does not move. (A per-body transmission override in
`Appearance` would be a format change — v101, a golden, a history row — and is
left for when something needs a tinted-glass part that is not `glass`.)

## The still (ray tracer)

The still was one primary ray per sample, shaded in closest-hit with no bounce
loop, and its shadow and AO rays were opaque by flag. Four changes:

1. **A path loop in `raygen.rgen`** (up to 8 segments). The payload gains
   `flags`, `throughput`, `nextOrigin`, `nextDir`; closest-hit hands a
   continuation back instead of tracing it, so the pipeline's recursion depth
   stays at 2 however many panes a ray crosses. The outline G-buffer is still
   written from the first hit only.
2. **A dielectric branch in `closesthit.rchit`** for `transmission > 0`: enter
   or leave decided from the UNFLIPPED normal, reflect or refract chosen at
   random in proportion to Schlick's Fresnel (total internal reflection when
   refraction fails), entering tints the path by the glass colour, and the
   surface's own light is the key light's glint. `V` comes from the ray
   direction, not the camera, which was only right for camera rays.
3. **Shadow and AO rays pass through glass**, tinted. They now carry the same
   payload type as camera rays (so the one hit group's any-hit can read it);
   `section.rahit` multiplies a shadow ray's transmittance and ignores the
   hit when the surface is glass, and `traceVisibility` returns a colour, not
   a bool. Shadow rays drop `OpaqueEXT` when the scene has glass
   (`clipInfo.y`), and BLAS geometry is `NO_DUPLICATE_ANY_HIT` so a pane is
   never counted twice. Without this a crystal would black out the dial under
   it twice over: key-light shadow and AO.
4. **`miss.rmiss`** returns the lighting environment to a path that has
   reflected, and the backdrop otherwise — the backdrop is the picture's
   background, not what glass reflects.

Section caps of a cut glass body stay opaque.

## The viewport (raster)

A transmissive body is drawn in the existing ghost pass (alpha-blended, depth
write off, excluded from shadow casters and the SSAO prepass) at
`CADMaterials::ViewportAlpha` — see-through, not refractive. Known limits it
inherits from that pass: it samples the AO of the surface behind it, and two
overlapping panes blend unsorted.

## Files

`src/cad/CADMaterials.{h,cpp}`, `src/render/RayTracingRenderer.{h,cpp}`,
`src/ui/RendererSync.cpp`, `src/shaders/{raygen.rgen,closesthit.rchit,
miss.rmiss,shadow.rmiss,section.rahit}`; tests
`tests/render_glass_smoke.py` and `tests/test_materials.cpp`.

## Verification

- `test_materials.cpp`: glass, and only glass, transmits, with an IOR over 1.
- `render_glass_smoke.py`: a red block behind a pane, rendered as a still
  with the pane `glass` and again with it `steel` — the block must show red
  through the glass and not through the steel. Skips (77) without ray
  tracing, like the other still tests.
- The existing still tests (`remote_smoke`, `offscreen_smoke`,
  `render_still_reach_smoke`) and `render_pixel_smoke` (raster, unchanged).
- The watch's stills with the crystal and caseback window left in, looked at.
