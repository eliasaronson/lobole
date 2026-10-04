# Architecture tier 0: what modelling a house ran into

## The problem

The first building modelled here (generators/house/, Garden shed) met
friction that is not about buildings as such. Most of it hits mechanical parts
too. In order of what it cost the round:

1. **No measure of the area a section encloses.** The round's plan scale was 7 %
   short, and nothing could have said so. The listing's 180 m² is the area
   inside the outer walls; the app measures section MATERIAL, not the floor
   the walls ring. A requirement on that area would have failed on the first
   run.
2. **One appearance per body.** A wall that is brick outside and plaster inside
   is two bodies. A machined housing painted outside with bare bores has the
   same problem.
3. **The material catalogue is a machine shop's.** No brick, roof tile,
   plaster, roofing felt or soil, so mass and the BOM of a building are
   meaningless and every finish is an override.
4. **Drawing views hide occurrences, not bodies.** Keeping the ground off a
   part's sheet took a configuration that suppresses it.
5. **The only environment is an indoor HDRI.** An `environment` still puts a
   house on a kitchen counter. There is no sky.
6. **Underlays cannot check a model.** `add_reference_image` takes no rotation
   (`set_reference_image` does). An underlay draws only while its sketch is
   being edited, never in a still, so a model cannot be looked at over the
   plan it was traced from.
7. **`render_still` on a drawing tab says "no geometry to render"**, about a
   model with 128 bodies.
8. **Appearance colours render lighter than picked.** Both renderers tone-map
   with the Narkowicz ACES fit without the 0.6 exposure it was fitted for, then
   apply a 2.2 power, then a ×1.1 lift (`mesh.frag`, `ReadbackStill`). A linear
   0.2 lands near byte 162, where an sRGB encode gives 124. Fixing it changes
   every picture the app makes, so it is built on a branch with before/after
   stills and merged only on the user's word.

## The shape of the fix

### Area enclosed by a section (1)

A requirement measure kind, `section_area`, with `of: material | enclosed`
and an optional `bodies` list (feature ids or body names) on a part. It
reuses the section cutter (`Measure::CrossSectionRegions`). `enclosed` fuses
the chosen bodies' section faces and sums the areas their inner wires bound:
the floor inside a ring of separate wall bodies, or a bore's area in a plate.
The `section` RPC gains the same `enclosed` answer. kVersion bump and a
golden, because an unknown measure kind refuses the file.

### Per-face appearance (2)

An appearance override on a FACE, keyed by its element name, held on the
body's creating feature beside the body-level one and resolved at
regeneration into face indices. It is left out of the regen hash, as body
appearance is. `set_appearance {face}` takes an element ref; a face whose
name no longer resolves is reported, not dropped.
- **Raster:** the body draws as sub-ranges by face, the way the selection
  highlight already uses `triangleFaces`' contiguous vertex ranges, one push
  constant per colour run.
- **Path tracer:** a per-primitive material index.
- **STEP:** XCAF colour on the face sub-shape.
- **GLB:** one primitive per colour.

kVersion bump and a golden.

### Building materials (3)

`brick`, `roof_tile` (concrete), `plaster`, `roofing_felt`, `render`,
`soil`. Each needs a density, a colour/roughness row, FEM elastic constants
where there is a source, a CAM cut class, and the MCP slug list and docs that
are gated on the catalogue.

### Hide bodies of a part in a drawing view (4)

The view's `hidden` also takes a part source's body names or feature ids,
resolved at parse time to feature ids, and `AppendBodies` skips them. No
format change: `hidden` is already a list of ids.

### An outdoor environment (5)

`render_still backdrop: sky`. It is a procedural sky (zenith/horizon
gradient and a sun disc) that lights the scene as well as backing it: an
`IBLPrecompute` entry that builds from an image, fed a generated
equirectangular sky. The studio HDRI stays the default.

### Underlays you can look at (6)

- `add_reference_image` takes `rotation_deg`.
- A reference image gains `show_always`: drawn when its sketch is not being
  edited, and in quick stills (`RecordModel`).
- kVersion bump and a golden.

### Honest refusals (7)

`render_still` names the active tab when it is not a part or assembly.

### Colour that matches the swatch (8), on a branch

Exposure 0.6 before the ACES fit, no ×1.1, the sRGB transfer curve, in
`mesh.frag` and `ReadbackStill` alike. The branch carries before/after stills
of four existing generator models. It is merged only after the user has seen
them, with a new pixel baseline (`tools/ci-local.sh --pixel-record`).

## Files

- **Area (1):** `src/core/Measure.{h,cpp}`, `src/core/Requirements.{h,cpp}`,
  `src/remote/RemoteRequirements.cpp`, `src/remote/RemoteInspection.cpp`.
- **Faces (2):** `src/core/Feature.h`, `src/core/Document.cpp`,
  `src/core/io/SerializeFeatures.cpp`, `src/ui/BodyAppearance.*`,
  `src/ui/RendererSync.cpp`, `src/render/MeshRenderer.*`,
  `src/render/RayTracingRenderer.*`, `src/shaders/{mesh.frag,closesthit.rchit}`,
  `src/ui/StepExport.cpp`, `src/cad/CADKernel.cpp`, `src/cad/MeshExport.cpp`,
  `src/remote/RemoteModeling.cpp`.
- **Materials (3):** `src/cad/CADMaterials.*`, `src/core/cam/CamFeeds.cpp`,
  `tools/designer_mcp.py`, `tests/test_materials.cpp`.
- **Drawings (4):** `src/remote/RemoteDrawings.cpp`, `src/ui/DrawingActions.cpp`.
- **Sky (5):** `src/render/IBL.*`, `src/shaders/miss.rmiss`,
  `src/ui/RendererSync.cpp`, `src/app/main.cpp`, `src/remote/RemoteDispatch.cpp`.
- **Underlays (6):** `src/core/features/SketchFeature.h`, serialization,
  `src/ui/UI.cpp`, `src/app/main.cpp`.
- **Refusal (7):** `src/ui/RendererSync.cpp`.
- **Colour (8):** `src/shaders/mesh.frag`, `src/render/RayTracingRenderer.cpp`.
- **Docs:** each area's README, docs/versioning/format.md, the RPC
  `describe` rows.

## Verification

- A test per item through the real pipeline:
  - a ring of four wall bodies reports the enclosed floor exactly;
  - a face recoloured survives a regeneration that renames nothing and reports
    one that it cannot resolve;
  - a STEP round trip keeps the face colour;
  - a part view with a hidden body projects without it;
  - every new material has a density and a cut class;
  - an underlay with `show_always` appears in a quick still.
- `tools/test-fast.sh`, the headless suite, the GUI suite, and
  `tests/render_pixel_smoke.py` byte-identical for everything but (8).
- generators/house rebuilt on the merged tree:
  - a `section_area` requirement on the floor inside the outer walls, against
    the listing's 180 m²;
  - brick/plaster by face instead of lining bodies;
  - the site hidden by body;
  - a sky still;
  - the plan underlay shown under the model.
