# Materials and appearance

How a body gets its look: the physical material it is made of, the
procedural figure some materials carry, an optional visual override, and
glass.

## Materials

A material is a slug on the body's creating feature (`Feature::m_material`),
stamped onto its bodies each regeneration (`Body::material`), and resolved
into `CADMaterials::MaterialProperties` (`src/cad/CADMaterials.{h,cpp}`):
base colour, metallic, roughness, specular, reflectance, plus the physics —
density, Young's modulus, Poisson's ratio, yield — that mass properties, the
BOM and FEM read. `DEFAULT` is the empty slug and carries no physics
(density 0, so mass reports none rather than a guess). The accepted slugs
are `MaterialLibrary::AllSlugs()` (`describe` reports them as
`material_slugs`). `petg` and `pla` are the printed parts' materials:
1270 and 1240 kg/m³ and their filament datasheets' moduli, where generic
plastic would read a printed part a fifth light; how a printed part is modelled to fit is its print process,
not its material ([../tolerances/README.md](../tolerances/README.md#printed-parts)).

### Grades

A shop orders an alloy and a temper, not "aluminium", so a grade is a row
of its own: `aluminum_6061_t6` and `aluminum_6082_t6`. Each carries its
specification's MINIMUM yield for plate (240 MPa: ASTM B209 for 6061-T6/T651
at 6.3-100 mm, EN 485-2 for 6082-T6/T651 at 12.5-100 mm), which is what a
design may count on, against the generic row's 95; its modulus (68.9 and
70 GPa); and the generic row's density and look. Every row has a `grade`,
what a drawing's parts list and title block print: a grade row its grade
(`6082-T6`), a generic row its name in capitals (`ALUMINUM`), which says
that no grade was chosen (`MaterialLibrary::GradeOf`,
[../drawings/README.md](../drawings/README.md#parts-lists-and-balloons)).

`MaterialLibrary::FamilySlug` reads a grade as its family (`aluminum`) for
a consumer that knows materials only that way: CAM's chipload classes
(`cam::CutClassOf`). Sheet metal's bend data and the tube rules are for one
alloy each, and refuse a grade they do not carry rather than bend 6061-T6
with a 5052 sheet's radii.

### Building materials

What a house is made of, so its mass and its parts list mean something and
a finish is not an appearance override on concrete: `brick` (fired clay,
1850 kg/m³), `roof_tile` (concrete tile, 2200), `plaster` (gypsum, 1150),
`roofing_felt` (bitumen, 1100), `render` (cement render, 1900) and `soil`
(1650). Each density is the middle of a handbook range for the material of
the body, not a wall's bulk with its mortar and voids; the ranges and their
sources are beside the values in `CADMaterials.cpp`. All are matte and
non-metallic: brick a muted red-brown, tile a darker one, plaster
off-white, felt near-black, render light grey, soil dark brown.

FEM moduli are given where a source gives one (brick masonry 10 GPa, a
concrete tile 25, gypsum plaster 2, render 12), with no yield, since all are
brittle: a study reports stress and no factor of safety. Felt (viscoelastic)
and soil (not a linear solid) carry none, as rubber carries none, so a study
on them refuses rather than answering with a made-up modulus. CAM's cutting
data reads them, with concrete, glass and ruby, as `NotCut`: a feed check on
such a body says the material is not one a router cuts, rather than that no
material was set (`cam::CutClassOf`).

The viewport shades metallic-roughness PBR in `mesh.frag`; the path tracer
reads the same values through `RTMaterial` (`RayTracingRenderer.h`), so the
two agree.

**An unknown slug is refused, never defaulted.** A default would render as a
plausible metal while reporting no mass.

- `CADMaterials::MaterialTypeFromString` throws, naming the accepted set
  (`SlugList()`); the empty slug stays legal and means default.
- `TryMaterialTypeFromString` is the `[[nodiscard]] bool` form.
- `GetMaterialOrDefault` is for draw-time consumers that must not throw —
  renderer sync, mass properties, mesh export, panel labels.
- **`Feature::SetMaterial` is the one funnel** and validates there; every
  ingress goes through it — the `set_material` RPC and `FeatureFromJson`,
  which is what `add_feature`, `modify_feature` and document load use.

`tests/test_conventions.cpp` scans string-to-enum parsers by the name
`*FromString`, so a new parser belongs under that name.

## Surface patterns

A figure — wood grain, plywood plies, a woven composite — is procedural.
There are no textures and no UVs anywhere in the appearance system. The
figure is evaluated in object (part-local) space, so it stays glued to the
part under any transform, and it lives in exactly one place,
`src/shaders/grain.glsl`, `#include`d by both `mesh.frag` and
`closesthit.rchit` so the viewport and the still cannot drift.

1. **The material names the figure**: `MaterialProperties::pattern` — a
   `CADMaterials::SurfacePattern`: `Flat`, `Wood`, `Plywood`, `Weave` — with
   `patternScale` (per mm) and `patternContrast` (0..1), set per material in
   the `pattern(...)` table in `CADMaterials.cpp` (oak, pine, walnut, maple,
   beech, birch and pine plywood, carbon fibre). Every metal, plastic and ceramic, and MDF,
   is `Flat`.
2. **One `vec4` carries it**: `(pattern, scale, contrast, axis)` — the
   `procMaterial` field of the mesh push constant on the raster path and
   `RTMaterial::procParams` on the traced one.
3. **`CADMaterials::PatternAxis`** derives the axis from the part's
   object-local extent: solid wood runs along the **longest** extent (a
   board's length), sheet patterns (plywood, weave) across the **thinnest**
   (the sheet normal). Both paths call it on the untransformed extent — the
   rule belongs to the pattern, not to a renderer — so a rotated occurrence's
   grain runs the same way in the viewport and in a still.
4. **`grain_apply()`** darkens albedo toward the figure, raises roughness,
   and returns an object-space normal perturbation (`dNormalObj`) for the
   caller to rotate into its own frame. It is a no-op for `Flat`.

**Patterns are functions and materials are parameters to them.** Never
branch on a `MaterialType` in a shader. A new material is a row; a new
pattern is a row plus one branch in `grain.glsl`. The raster push-constant
block is at the 128-byte Vulkan floor, so **a new pattern must fit inside the
existing `vec4`**; one needing more parameters needs an id-indexed material
buffer, the shape the path tracer already uses.

**The weave** is a 2/2 twill: cells are `floor(uv * scale)`, the warp floats
on top when `mod(ci.x - ci.y + 4, 4) < 2` (the one-cell shift per row is what
reads as twill's diagonal rather than a checkerboard), and the tow
cross-section is `sin(pi * f)`, so the gaps fall out as `1 - h`. The figure
is carried by **normal relief**, not tint: carbon's albedo is near black, and
the look of a laminate is light running along each tow, which bending the
shading normal across the tow produces from an isotropic BRDF. The weave
fades out on cut edges. Base colours and relief strengths are tuned against a
render, not derived.

## Appearance override

The look can be overridden without changing the physics — blue HDPE barrels,
painted steel, stained pine:

```cpp
struct Appearance {                    // src/core/Feature.h
  std::optional<glm::vec3> color;      // LINEAR base colour
  std::optional<float> roughness;      // 0..1
  std::optional<float> metallic;       // 0..1
};
```

- Persisted on the creating feature (`Feature::m_appearance`), stamped onto
  its bodies (`Body::appearance`) in a post-pass of `Document::Regenerate`,
  and **stripped from the regeneration chain hash**, so a recolour executes
  no features (`tests/appearance_smoke.py` asserts `executed == 0`).
- **Resolve order** (`ui::ResolveBodyLook`, `src/ui/BodyAppearance.cpp`):
  palette colour → import colour (`Body::displayColor`, from an XCAF STEP
  import) → material → appearance fields on top. A colour-only override on a
  body with no material resolves against a neutral dielectric (metallic 0,
  roughness 0.6), not `DEFAULT`'s metal. Overriding the colour of a patterned
  material keeps the pattern.
- The merge edits CPU-resolved values, so the push constant does not grow.
- Mass properties, FEM and the BOM read `Body::material` only (or, in an
  assembly, the placement's `material`, which overrides every body's); an
  appearance never reaches physics.
- The colour is **linear**, not sRGB: the renderer lights it as linear and
  STEP/GLB export is handed it as linear, so an sRGB 0.5 grey is about 0.2.
  The [tone curve](README.md#tone-mapping) shows a lit linear colour about as
  its sRGB encode, so a value converted from a photograph (c^2.2) renders
  near the photograph — no extra darkening.

`set_appearance {feature_id | body, color?, roughness?, metallic?}` sets it;
no optional field clears it; a colour given as 0..255 is refused with a hint.
`get_state` echoes it per body; the body context menu in the feature tree has
an **Appearance** submenu.

### A face of its own

One face of a body can wear its own appearance (v164) — a wall brick outside
and plaster inside, a painted end on a bare bar:

- **Held** on the body's creating feature beside the body override, as
  `Feature::m_faceAppearances` (`FaceAppearance {element, look}`), keyed by
  the face's **element name** ([toponaming](../modeling/toponaming.md)). A
  face a later feature made — a cut's floor or wall — is still a face of the
  body, named in its `ElementMap`, so it is painted on the body's creating
  feature too. Serialized as `face_appearance` [{`element`, `color`?,
  `roughness`?, `metallic`?}]; stripped from the chain hash like
  `appearance`.
- **Resolved** at every regeneration by the same stamp
  (`Document::Regenerate`): each name is looked up in the `ElementMap` of
  every body its feature created and lands in `Body::faceAppearances` (face
  index → override). A face a later feature modified or cut in pieces is
  renamed `…;M<tag>` / `…;M<tag>.<i>`; the paint follows it to every face
  so descended (a pocket in a painted top leaves the top painted). A name that resolves on none of them is **kept and
  reported** — `Feature::UnresolvedFaceLooks`, a warning carrying
  `face_appearance` in every regeneration summary (so the response to the
  edit that lost the face says so), and `face_appearance_unresolved` on the
  feature in `get_state`. A feature with no body this time (suppressed,
  rolled back, failed) reports nothing; its faces are absent, not stale.
- **Look:** `ui::ResolveFaceLook` lays the face's set fields over the BODY's
  resolved look, so an unset field inherits the body's appearance, then its
  material. On a body with no material a face override is the same neutral
  dielectric a body override is.
- **Viewport:** the body stays one tessellation and one instance.
  `ui::FaceLookRuns` cuts its triangles (`triangleFaces`; a face's triangles
  are contiguous) into runs that share a look, and `MeshRenderer` draws one
  `vkCmdDrawIndexed` per run with its own push constant (`MeshLookRun`).
  Highlight ranges are vertex ranges, which a sub-draw keeps, so selection
  and hover look as before. A body with no face override draws exactly as it
  did — the pixel baseline does not move. A Derived reference ghost ignores
  face looks (it is one translucent tint).
- **Path tracer:** the body stays ONE geometry with a material per look and
  a per-triangle material index (`RT_GEOM_PER_TRIANGLE_MATERIAL`, read by
  `closesthit.rchit` and `section.rahit`). One geometry per colour would
  have made the still's outline pass draw a body boundary along every colour
  change.
- **STEP:** an XCAF colour on the face's sub-shape label, over the body
  product's own colour (`StepBody::faceColors`). Import reads the product
  colour only, so a round trip through our own importer loses the face.
- **glTF/OBJ/PLY:** a colour and a PBR material on the face's sub-shape of
  the prototype shape, so the writer emits that face as its own primitive.

`set_appearance {feature_id | body, element, color?, roughness?, metallic?}`
paints one face; `element` is a face name from `list_elements` on that body
(an edge is refused, as is a name that names no face). With no optional field
it clears that face — even a stale one, which is how a stale override goes.
`get_state` lists a body's painted faces as `face_appearance` [{`element`,
`face`, …}], the face index being the one `list_elements` and `select_face`
use. There is no GUI for it yet.

Not supported: a per-occurrence appearance override (the same part coloured
differently in two places of an assembly), and image textures. A per-occurrence MATERIAL does exist (`set_occurrence_material`,
v135): it replaces every body's material for mass, the BOM, rendering and
STEP ([assemblies](../assemblies/README.md#configurations-per-placement)).

## Glass

`MaterialProperties::transmission` (0..1, 0 = opaque) and `ior`. `glass` is
1.0 and 1.52; every other material is 0. It is a material property, not an
appearance field.

### In the viewport

A transmissive body is drawn in the ghost pass (alpha-blended, depth writes
off, not a shadow caster, not in the SSAO prepass) at
`CADMaterials::ViewportAlpha` (a clear body keeps a tenth of its colour). It
is shaded **as glass**, not as a plain ghost: `MeshRenderer::SetInstanceGlass`
marks it with a negative alpha — the push constant has no room for a flag —
and `mesh.frag`'s glass branch composites the tinted body faintly, the
environment reflected with a Fresnel weight rising to a mirror at grazing
angles, and the lights' highlights at full strength. A plain ghost multiplies
everything by its alpha and reads as a grey veil.

It is see-through, not refractive. It samples the AO of the surface behind
it, and overlapping panes blend unsorted.

A section cap of a transmissive body is drawn as glass too.

### In the still

- **A path loop in `raygen.rgen`**, up to 8 segments. The payload carries
  `flags`, `throughput`, `nextOrigin`, `nextDir`; closest-hit hands the
  continuation back instead of tracing it, so a ray crossing many panes does
  not deepen recursion. The outline G-buffer is written from the first hit
  only.
- **A dielectric branch in `closesthit.rchit`**: entering or leaving is
  decided from the unflipped normal; reflect or refract is chosen at random
  in proportion to Schlick's Fresnel (total internal reflection when
  refraction fails); entering tints the path by the glass colour; the view
  vector comes from the ray, not the camera.
- **Shadow and AO rays pass through glass, tinted.** They use the camera
  rays' payload type so the hit group's any-hit (`section.rahit`) can
  multiply a shadow ray's transmittance and ignore the glass hit;
  `traceVisibility` returns a colour, not a bool. Shadow rays drop
  `OpaqueEXT` when the scene has glass or a section, and BLAS geometry is
  `NO_DUPLICATE_ANY_HIT` so a pane is never counted twice. Without this, a
  crystal would black out the dial under it through both the key-light shadow
  and the AO.
- **`miss.rmiss`** returns the lighting environment to a path that has
  reflected, and the backdrop otherwise.

## Tests

- `tools/ctest -L materials` (`tests/test_materials.cpp`) — the pattern partition,
  `PatternAxis` (a long board picks its length, a sheet its thickness, a
  cube is deterministic), slug refusal, that glass and only glass
  transmits with an IOR over 1, and that each building material has its
  density in its handbook range, a matte look, a modulus where one is
  sourced and the `NotCut` class.
- `tests/appearance_smoke.py` — override, clear, zero features executed.
- `tools/ctest -L appearance` (`tests/test_face_appearance.cpp`) — a face
  override resolves by name, survives an upstream edit, rebuilds nothing; a
  cut's floor painted on the box and reported when the cut is suppressed;
  the round trip; the face look's inheritance and the runs that tile a
  tessellation. `tests/test_step_export.cpp` checks the files: the colour on
  the STEP face, the face's own glTF primitive.
- `tests/face_appearance_smoke.py` — the RPC: paint, edit, a cut face, the
  stale-name warning in the response that lost it, clearing a stale name,
  undo, refusals, persistence.
- `tests/render_glass_smoke.py` — a red block behind a pane: it shows through
  `glass` and not through `steel` (skips without ray tracing).
- `tests/render_pixel_smoke.py` — its scene assigns no materials, so a
  pattern change must leave it byte-identical; if it moves, a shared helper
  in `grain.glsl` changed for everyone.
