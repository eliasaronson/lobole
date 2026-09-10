# Surface patterns

How a material gets a figure — wood grain, plywood plies, a woven composite —
and what keeps that scalable as the material library grows.

## The problem

`CARBON_FIBRE` had correct physics (1550 kg/m³, 55 GPa, 550 MPa) and no
appearance. It rendered near-identically to `PLASTIC_BLACK`, differing only in
gloss — which is the one confusion the material exists to prevent, since
modelling an airframe as black plastic reads a third light.

## The shape of it

There are no textures and no UVs anywhere in the appearance system. A figure is
procedural, evaluated in OBJECT (part-local) space so it stays glued to the part
under any transform, and lives in exactly one place: `src/shaders/grain.glsl`,
`#include`d by both the raster (`mesh.frag`) and the ray-traced
(`closesthit.rchit`) shaders so the two cannot drift.

Four pieces:

1. **The material says which figure.** `MaterialProperties::pattern` (a
   `CADMaterials::SurfacePattern`), plus `patternScale` and `patternContrast`,
   set one row per material in `CADMaterials.cpp`'s `pattern()` table.
2. **One `vec4` carries it**: `(pattern, scale, contrast, axis)` — as a push
   constant on the raster path (`MeshRenderer::PushConstants::procMaterial`) and
   as `RTMaterial::procParams` on the ray-traced one.
3. **`CADMaterials::PatternAxis`** derives the fourth component from the part's
   object-local extent. Solid wood runs along the LONGEST extent (a board's
   length); sheet patterns — plywood, weave — run across the THINNEST (the sheet
   normal).
4. **`grain_apply()`** darkens albedo toward the figure, adjusts roughness, and
   hands back an object-space normal perturbation for the caller to rotate into
   its own frame. It is a no-op for `Flat`, which is every metal, plastic and
   ceramic, and MDF.

## Why this scales

Material count and pattern count grow at very different rates. 6061, 7075 and
2024 are three rows and one appearance; carbon, fibreglass and Kevlar are three
parameter sets for one `Weave`. The whole engineering space needs roughly six
patterns against a library that can run to hundreds of materials.

So: **patterns are functions and materials are parameters to them.** Never
branch on a `MaterialType` in a shader. A new material is a row; a new pattern
is a row plus one branch in `grain.glsl`.

Two constraints follow from that and are worth knowing before adding a fifth
pattern:

- **The raster push-constant block is 128 bytes, exactly the Vulkan-guaranteed
  `maxPushConstantsSize` floor.** A new pattern must fit inside the existing
  `vec4`. A pattern needing two independent scales does not fit, and the answer
  then is an id-indexed material buffer — the shape the ray tracer already uses
  (`AddMaterial` returns a `matId`) — which is also what user-defined materials
  would need. Do that when a forcing function arrives, not before.
- **The axis rule belongs to the pattern, not to a renderer.** It used to be
  duplicated in `MeshRenderer.cpp` and `RendererSync.cpp`, and the copies
  disagreed: the ray tracer measured the extent AFTER baking the occurrence
  transform into its vertices, so a rotated occurrence's grain ran one way in
  the viewport and another in a still. Both call `PatternAxis` now.

## The weave

`Weave` is a 2/2 twill. Cells are `floor(uv * scale)`; the warp floats on top
when `mod(ci.x - ci.y, 4) < 2`, and shifting that by one cell per row is what
reads as twill's diagonal rather than a plain checkerboard. The tow
cross-section is `sin(pi * f)`, so the gaps between tows fall out as `1 - h`.

The figure is carried by **normal relief, not tint**. Carbon's albedo is ~0.03 —
there is nothing to darken — and the whole look of a real laminate is the
anisotropic sheen of light running ALONG each tow. Bending the shading normal
only ACROSS a tow produces exactly that out of an isotropic BRDF, which is why
`grain_apply` has an `out vec3 dNormalObj` at all. `facing` fades the weave out
on cut edges, where a real laminate shows a resin stack.

Both the base colour and the relief strength are tuned against a render, not
derived: the scene's rig is bright enough that carbon at its nominal 0.07 albedo
came out lighter than black plastic and faintly lavender. The woods carry the
same note for the same reason.

## Refusing an unknown material

A material is stored on `Feature`/`Body` as a raw slug string and resolved
lazily by ~15 consumers. `MaterialLibrary::FromSlug` used to return `DEFAULT`
for an unrecognised one — an aluminium appearance carrying density 0 — so a
document naming a material this build lacks looked like machined aluminium while
honestly reporting no mass. Every material added made one more slug for an older
build to swallow.

The split now:

- `MaterialTypeFromString` throws, naming the accepted set (`SlugList()`). The
  empty slug stays legal and means default.
- `TryMaterialTypeFromString` is the `[[nodiscard]] bool` form.
- `GetMaterialOrDefault` is for draw-time consumers that must not throw —
  renderer sync mid-frame, mass properties, mesh export, panel labels.
- **`Feature::SetMaterial` is the one funnel** and validates there. Every
  ingress runs through it: the `set_material` RPC, and `FeatureFromJson`, which
  is what `add_feature`, `modify_feature` and document load all use. Before
  this, `add_feature(feature={..., "material":"unobtanium"})` was accepted and
  the bad string sat on the feature until something tripped over it.

`FromSlug` escaped `test_conventions.cpp`'s silent-parser scan for one reason:
the scan keys off the name `*FromString`. A new string-to-enum parser goes in
under that name.

## Verification

- `ctest -L materials` — the four-way pattern partition, the slug refusal, and
  `PatternAxis` (a long board picks its length, a sheet its thickness, a cube is
  deterministic).
- `tests/render_pixel_smoke.py` — its scene assigns no materials, so a new
  pattern must leave it byte-identical. If it moves, a shared helper in
  `grain.glsl` changed for everyone.
- **Look at it.** A carbon plate beside a black-plastic plate beside an oak
  board, rendered through BOTH paths (`screenshot` and `render_still`): the
  twill must read at arm's length, the two paths must agree, and carbon must be
  darker than the plastic. Then the same in an assembly with one occurrence
  rotated 45°, which is the case the duplicated axis rule got wrong.
