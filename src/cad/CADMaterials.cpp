// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cad/CADMaterials.h"
#include <cctype>
#include <stdexcept>

namespace CADMaterials {

std::unordered_map<MaterialType, MaterialProperties> MaterialLibrary::materials;

void MaterialLibrary::InitializeMaterials() {
  if (!materials.empty())
    return;

  // Metals - High metallic, low roughness, realistic colors
  // Aluminium is graded rows only: its conductivity runs from 120 to 237
  // W/(m K) across alloys and its yield from under 100 MPa to over 500, so
  // no generic row could carry either honestly. The two grades share one
  // appearance.
  materials[MaterialType::ALUMINUM_6061_T6] = {
      glm::vec3(0.91f, 0.92f, 0.92f), // Light silver-white
      1.0f,                           // Fully metallic
      0.1f,                           // Very smooth
      0.9f,                           // High specular
      0.04f,                          // Standard metal reflectance
      "Aluminium 6061-T6"};

  materials[MaterialType::STEEL] = {
      glm::vec3(0.65f, 0.67f, 0.68f), // Dark metallic gray
      1.0f,                           // Fully metallic
      0.2f,                           // Slightly rougher than aluminum
      0.8f,                           // High specular
      0.04f,                          // Standard metal reflectance
      "Steel"};

  materials[MaterialType::STAINLESS_STEEL] = {
      glm::vec3(0.75f, 0.77f, 0.78f), // Brighter than regular steel
      1.0f,                           // Fully metallic
      0.15f,                          // Smoother than regular steel
      0.85f,                          // High specular
      0.04f,                          // Standard metal reflectance
      "Stainless Steel"};

  materials[MaterialType::BRASS] = {
      glm::vec3(0.91f, 0.78f, 0.35f), // Golden yellow
      1.0f,                           // Fully metallic
      0.15f,                          // Smooth
      0.85f,                          // High specular
      0.04f,                          // Standard metal reflectance
      "Brass"};

  // A thermostatic bimetal's two NiCr steels, roll-bonded: the bright face
  // of a steel strip.
  materials[MaterialType::BIMETAL_KANTHAL_94S] = {
      glm::vec3(0.70f, 0.71f, 0.72f), // Steel strip, as rolled
      1.0f,                           // Fully metallic
      0.25f,                          // Rolled, not polished
      0.8f,                           // High specular
      0.04f,                          // Standard metal reflectance
      "Kanthal 94S bimetal"};

  materials[MaterialType::BRONZE] = {
      glm::vec3(0.80f, 0.50f, 0.20f), // Reddish-brown
      1.0f,                           // Fully metallic
      0.2f,                           // Slightly rough
      0.8f,                           // High specular
      0.04f,                          // Standard metal reflectance
      "Bronze"};

  materials[MaterialType::COPPER] = {
      glm::vec3(0.95f, 0.64f, 0.54f), // Reddish orange
      1.0f,                           // Fully metallic
      0.1f,                           // Smooth
      0.9f,                           // Very high specular
      0.04f,                          // Standard metal reflectance
      "Copper"};

  materials[MaterialType::CHROME] = {
      glm::vec3(0.55f, 0.56f, 0.59f), // Cool metallic gray
      1.0f,                           // Fully metallic
      0.05f,                          // Very smooth (mirror-like)
      0.95f,                          // Very high specular
      0.04f,                          // Standard metal reflectance
      "Chrome"};

  materials[MaterialType::GOLD] = {
      glm::vec3(1.0f, 0.84f, 0.0f), // Pure gold color
      1.0f,                         // Fully metallic
      0.1f,                         // Smooth
      0.9f,                         // High specular
      0.04f,                        // Standard metal reflectance
      "Gold"};

  materials[MaterialType::SILVER] = {
      glm::vec3(0.97f, 0.96f, 0.91f), // Bright silver
      1.0f,                           // Fully metallic
      0.05f,                          // Very smooth
      0.95f,                          // Very high specular
      0.04f,                          // Standard metal reflectance
      "Silver"};

  materials[MaterialType::TITANIUM] = {
      glm::vec3(0.54f, 0.50f, 0.50f), // Dark metallic
      1.0f,                           // Fully metallic
      0.25f,                          // Slightly rougher
      0.75f,                          // Good specular
      0.04f,                          // Standard metal reflectance
      "Titanium"};

  materials[MaterialType::ALUMINUM_6082_T6] =
      materials[MaterialType::ALUMINUM_6061_T6];
  materials[MaterialType::ALUMINUM_6082_T6].name = "Aluminium 6082-T6";

  // Non-metals - Low metallic, varied roughness
  materials[MaterialType::PLASTIC_WHITE] = {
      glm::vec3(0.95f, 0.95f, 0.95f), // Pure white
      0.0f,                           // Non-metallic
      0.5f,                           // Medium roughness
      0.5f,                           // Medium specular
      0.04f,                          // Plastic reflectance
      "White Plastic"};

  materials[MaterialType::PLASTIC_BLACK] = {
      glm::vec3(0.05f, 0.05f, 0.05f), // Dark black
      0.0f,                           // Non-metallic
      0.6f,                           // Slightly rough
      0.4f,                           // Lower specular
      0.04f,                          // Plastic reflectance
      "Black Plastic"};

  materials[MaterialType::PETG] = {
      // A printed orange. At 0.8/0.32 it rendered yellow under the old tone
      // curve; this darker value reads orange under the corrected one too.
      glm::vec3(0.42f, 0.07f, 0.01f),
      0.0f,
      0.35f, // a printed surface: glossy filament, layer lines
      0.5f,
      0.05f, // ((n-1)/(n+1))^2 at n = 1.57
      "PETG"};

  materials[MaterialType::PLA] = {
      // A printed blue, so a PLA part and a PETG one tell apart on sight.
      glm::vec3(0.02f, 0.10f, 0.32f),
      0.0f,
      0.4f, // a printed surface: matte-ish filament, layer lines
      0.5f,
      0.045f, // ((n-1)/(n+1))^2 at n = 1.53
      "PLA"};

  materials[MaterialType::TPU] = {
      // A printed translucent-ish white-grey, so a TPU gasket in a blue PLA
      // lid reads as the other material on sight.
      glm::vec3(0.55f, 0.55f, 0.52f),
      0.0f,
      0.7f, // a soft, satin printed surface
      0.3f,
      0.045f, // ((n-1)/(n+1))^2 at n = 1.5
      "TPU"};

  // Darker than its nominal albedo, chosen for the scene's bright rig: at
  // the nominal 0.07 this rendered LIGHTER than black plastic, and blue,
  // which is the one comparison the material exists to win. The weave's relief
  // scatters normals, so it catches more environment than a flat surface of
  // the same albedo — the tow highlights carry the read now, not the base.
  materials[MaterialType::CARBON_FIBRE] = {
      glm::vec3(0.032f, 0.032f, 0.035f), // Near-black, barely cool
      0.0f,                              // Non-metallic
      0.30f,                             // Glossy epoxy surface
      0.5f,                              // ... and quite specular
      0.05f,                             // Epoxy reflectance
      "Carbon Fibre"};

  materials[MaterialType::RUBBER] = {glm::vec3(0.1f, 0.1f, 0.1f), // Dark rubber
                                     0.0f,  // Non-metallic
                                     0.9f,  // Very rough
                                     0.1f,  // Low specular
                                     0.04f, // Low reflectance
                                     "Rubber"};

  materials[MaterialType::GLASS] = {
      glm::vec3(0.95f, 0.98f, 1.0f), // Clear glass tint
      0.0f,                          // Non-metallic
      0.0f,                          // Perfectly smooth
      1.0f,                          // Maximum specular
      0.08f,                         // Glass reflectance
      "Glass"};

  materials[MaterialType::RUBY] = {
      glm::vec3(0.62f, 0.02f, 0.07f), // Synthetic ruby red
      0.0f,                           // Non-metallic
      0.05f,                          // Polished
      1.0f,                           // High specular
      0.077f,                         // ((n-1)/(n+1))^2 at n = 1.77
      "Ruby"};

  materials[MaterialType::CONCRETE] = {
      glm::vec3(0.7f, 0.7f, 0.65f), // Gray concrete
      0.0f,                         // Non-metallic
      0.8f,                         // Rough surface
      0.2f,                         // Low specular
      0.04f,                        // Low reflectance
      "Concrete"};

  materials[MaterialType::EPOXY] = {
      glm::vec3(0.03f, 0.03f, 0.03f), // Pigmented black
      0.0f,
      0.15f, // cast and sanded flush, then finished: near gloss
      0.6f,
      0.045f, // ((n-1)/(n+1))^2 at n = 1.55
      "Epoxy"};

  // Building materials. Linear base colours, tuned by eye under the corrected
  // tone curve, and matte: none of these has a gloss to speak of, and a felt
  // roof at plastic's 0.6 read wet.
  materials[MaterialType::BRICK] = {
      glm::vec3(0.30f, 0.11f, 0.06f), // muted red-brown fired clay
      0.0f,
      0.85f,
      0.2f,
      0.04f,
      "Brick"};
  materials[MaterialType::ROOF_TILE] = {
      glm::vec3(0.17f, 0.06f, 0.04f), // dark red-brown concrete tile
      0.0f,
      0.7f, // a sealed face weathers to a low sheen
      0.25f,
      0.04f,
      "Roof Tile"};
  materials[MaterialType::PLASTER] = {
      glm::vec3(0.74f, 0.72f, 0.67f), // off-white
      0.0f,
      0.9f,
      0.15f,
      0.04f,
      "Plaster"};
  materials[MaterialType::ROOFING_FELT] = {
      glm::vec3(0.025f, 0.025f, 0.024f), // near-black bitumen
      0.0f,
      0.8f, // mineral-surfaced
      0.2f,
      0.04f,
      "Roofing Felt"};
  materials[MaterialType::RENDER] = {
      glm::vec3(0.55f, 0.55f, 0.53f), // light grey cement render
      0.0f,
      0.9f,
      0.15f,
      0.04f,
      "Render"};
  materials[MaterialType::SOIL] = {
      glm::vec3(0.10f, 0.07f, 0.045f), // dark brown topsoil
      0.0f,
      0.95f,
      0.05f,
      0.04f,
      "Soil"};

  // Wood & engineered wood. Base tone only here; the grain figure is applied
  // procedurally in the shader (see the "procedural grain" block below).
  // Colors are the light early-wood tone — the shader darkens toward the
  // grain lines using patternContrast, so these read as the overall hue.
  // Each is a finished sample's typical sRGB tone, converted to linear
  // (c^2.2; the sRGB triple is in the comment). The tone curve shows a lit
  // linear colour about as its sRGB encode
  // (docs/rendering/README.md#tone-mapping), so these read as the wood does.
  // They were once pre-darkened ~1.4x for a curve that washed albedos out.
  materials[MaterialType::WOOD_OAK] = {
      glm::vec3(0.55f, 0.35f, 0.16f), // Warm honey brown (196,160,110)
      0.0f,
      0.55f,
      0.4f,
      0.04f,
      "Oak"};
  materials[MaterialType::WOOD_PINE] = {
      glm::vec3(0.73f, 0.51f, 0.26f), // Pale yellow (222,190,140)
      0.0f,
      0.6f,
      0.35f,
      0.04f,
      "Pine"};
  materials[MaterialType::WOOD_WALNUT] = {
      glm::vec3(0.11f, 0.048f, 0.025f), // Dark chocolate brown (92,62,44)
      0.0f,
      0.5f,
      0.45f,
      0.04f,
      "Walnut"};
  materials[MaterialType::WOOD_MAPLE] = {
      glm::vec3(0.79f, 0.61f, 0.38f), // Pale cream, less yellow (230,205,165)
      0.0f,
      0.5f,
      0.4f,
      0.04f,
      "Maple"};
  materials[MaterialType::WOOD_BEECH] = {
      glm::vec3(0.68f, 0.40f, 0.22f), // Pinkish tan (215,170,130)
      0.0f,
      0.55f,
      0.4f,
      0.04f,
      "Beech"};
  materials[MaterialType::PLYWOOD_BIRCH] = {
      glm::vec3(0.77f, 0.61f, 0.38f), // Pale cream birch face (228,205,165)
      0.0f,
      0.55f,
      0.4f,
      0.04f,
      "Birch Plywood"};
  materials[MaterialType::PLYWOOD_PINE] = {
      glm::vec3(0.68f, 0.46f, 0.22f), // Softwood ply face (215,180,130)
      0.0f,
      0.6f,
      0.35f,
      0.04f,
      "Pine Plywood"};
  materials[MaterialType::MDF] = {
      glm::vec3(0.46f, 0.26f, 0.13f), // Uniform tan-brown, matte (180,140,100)
      0.0f,
      0.75f,
      0.25f,
      0.04f,
      "MDF"};

  // Default material
  materials[MaterialType::DEFAULT] = materials[MaterialType::ALUMINUM_6061_T6];
  materials[MaterialType::DEFAULT].name = "Default";

  // Densities (kg/m^3), for mass properties (inspection I2). Set after the
  // DEFAULT copy above so DEFAULT keeps density 0 = unknown: an unassigned
  // body reports no mass instead of a silently-guessed aluminum one.
  materials[MaterialType::ALUMINUM_6061_T6].density = 2700.0f;
  materials[MaterialType::ALUMINUM_6082_T6].density = 2700.0f;
  materials[MaterialType::STEEL].density = 7850.0f;
  // Stainless 1.4301: SCI P413, Design Manual for Structural Stainless
  // Steel, 4th ed. (2017), Table 2.7: 7 900 kg/m^3.
  materials[MaterialType::STAINLESS_STEEL].density = 7900.0f;
  // Brass is C26000 cartridge brass: Wieland Concast, "C26000" datasheet,
  // physical properties provided by CDA: 8.53 g/cm^3.
  materials[MaterialType::BRASS].density = 8530.0f;
  // KANTHAL 94S: Kanthal, Thermostatic Bimetal Handbook (catalogue
  // 3-A-1-3), the KANTHAL 94S page (p. 40): 8.1 g/cm^3.
  materials[MaterialType::BIMETAL_KANTHAL_94S].density = 8100.0f;
  materials[MaterialType::BRONZE].density = 8800.0f;
  materials[MaterialType::COPPER].density = 8960.0f;
  materials[MaterialType::CHROME].density = 7190.0f;
  materials[MaterialType::GOLD].density = 19300.0f;
  materials[MaterialType::SILVER].density = 10490.0f;
  materials[MaterialType::TITANIUM].density = 4506.0f;
  materials[MaterialType::PLASTIC_WHITE].density = 1040.0f;
  materials[MaterialType::PLASTIC_BLACK].density = 1040.0f;
  materials[MaterialType::PETG].density = 1270.0f;
  materials[MaterialType::PLA].density = 1240.0f;
  // Bambu Lab TPU 95A HF's datasheet: 1.22 g/cm3 (its TPU 95A, 1.20).
  materials[MaterialType::TPU].density = 1220.0f;
  materials[MaterialType::CARBON_FIBRE].density = 1550.0f;
  materials[MaterialType::RUBBER].density = 1200.0f;
  materials[MaterialType::GLASS].density = 2500.0f;
  materials[MaterialType::RUBY].density = 3980.0f;
  // Glass transmits (docs/rendering/materials.md#glass); soda-lime's index.
  materials[MaterialType::GLASS].transmission = 1.0f;
  materials[MaterialType::GLASS].ior = 1.52f;
  materials[MaterialType::CONCRETE].density = 2400.0f;
  materials[MaterialType::EPOXY].density = 1150.0f;
  // Building materials, as the material of the body (not a wall's bulk with
  // its mortar and voids, which a model draws as separate bodies or not at
  // all). Handbook ranges, the value used in the middle of each:
  //   fired clay brick, solid      1800-1900  (Engineering ToolBox; EN 771-1
  //                                            gross densities of solid
  //                                            units)
  //   concrete roof tile           2100-2400  (bulk of the tile, EN 490)
  //   gypsum plaster, set          1000-1300  (EN 13279 / EN 12524 tables)
  //   bitumen roofing felt         ~1100      (EN 12524: bitumen 1050)
  //   cement render / mortar       1800-2000  (EN 12524: cement mortar 1800,
  //                                            render 1800-2000)
  //   soil, moist topsoil to clay  1500-1800  (EN ISO 10456 / geotechnical
  //                                            handbooks)
  materials[MaterialType::BRICK].density = 1850.0f;
  materials[MaterialType::ROOF_TILE].density = 2200.0f;
  materials[MaterialType::PLASTER].density = 1150.0f;
  materials[MaterialType::ROOFING_FELT].density = 1100.0f;
  materials[MaterialType::RENDER].density = 1900.0f;
  materials[MaterialType::SOIL].density = 1650.0f;
  // Wood densities are moisture-dependent; these are typical air-dry (~12% MC)
  // values, well-defined enough for mass properties.
  materials[MaterialType::WOOD_OAK].density = 700.0f;
  materials[MaterialType::WOOD_PINE].density = 500.0f;
  materials[MaterialType::WOOD_WALNUT].density = 610.0f;
  materials[MaterialType::WOOD_MAPLE].density = 705.0f;
  materials[MaterialType::WOOD_BEECH].density = 720.0f;
  materials[MaterialType::PLYWOOD_BIRCH].density = 680.0f;
  materials[MaterialType::PLYWOOD_PINE].density = 500.0f;
  materials[MaterialType::MDF].density = 750.0f;

  // Linear-elastic properties for FEM (Young's modulus GPa, Poisson ratio,
  // yield strength MPa). Representative handbook values; like density they
  // are left 0 on DEFAULT so a study on an unassigned body fails loudly.
  // Yield is 0 where it isn't a meaningful single number (brittle/soft
  // materials) — the study then reports stress without a factor of safety.
  auto elastic = [&](MaterialType t, double e, double nu, double yield) {
    materials[t].youngGPa = e;
    materials[t].poisson = nu;
    materials[t].yieldMPa = yield;
  };
  // A grade's yield is its specification's MINIMUM for plate, which is what
  // a design may count on (a datasheet's typical 276 MPa for 6061-T6 is not):
  // ASTM B209 6061-T6/T651 plate 6.3-100 mm, 35 ksi (240 MPa); EN 485-2
  // 6082-T6/T651 plate 12.5-100 mm, Rp0.2 240 MPa (260 below 6 mm).
  // Moduli: the Aluminum Association's 68.9 GPa, EN 755-2's 70 GPa.
  elastic(MaterialType::ALUMINUM_6061_T6, 68.9f, 0.33f, 240.0f);
  elastic(MaterialType::ALUMINUM_6082_T6, 70.0f, 0.33f, 240.0f);
  elastic(MaterialType::STEEL, 200.0f, 0.30f, 250.0f);
  // Stainless 1.4301: SCI P413, Design Manual for Structural Stainless
  // Steel, 4th ed. (2017), §2.3.1: E = 200 GPa, nu = 0.3 (G = 76.9 GPa);
  // Table 2.2: f_y = 230 MPa for 1.4301 cold-rolled strip, t <= 8 mm (f_u
  // 540 MPa). The thinnest product form's, which is what a clip or a spring
  // is cut from.
  elastic(MaterialType::STAINLESS_STEEL, 200.0, 0.30, 230.0);
  // Brass is C26000 cartridge brass: Wieland Concast, "C26000" datasheet,
  // physical properties provided by CDA: E = 16 000 ksi (110.3 GPa), G =
  // 6 000 ksi (41.37 GPa), so nu = E / 2G - 1 = 0.333. No yield: Concast
  // gives one for ASTM B927 ROD only (H01 205 to H04 345 MPa under 1/2"),
  // which is a temper and a product form, not the material -- so a factor
  // of safety on brass is refused rather than read off one of them.
  elastic(MaterialType::BRASS, 110.3, 16000.0 / (2.0 * 6000.0) - 1.0, 0.0f);
  // KANTHAL 94S: Kanthal, Thermostatic Bimetal Handbook (catalogue
  // 3-A-1-3), p. 40: E = 190 GPa at 20 C. Kanthal publishes no Poisson's
  // ratio (nor either layer's data), so none: a structural study of a body
  // of it is refused (ElasticDataRefusal), and its motion is read from its
  // measured constants below, not from a layer model. No yield: the
  // handbook's permissible stress is a graph per grade with no values in
  // its text.
  elastic(MaterialType::BIMETAL_KANTHAL_94S, 190.0, 0.0, 0.0);
  elastic(MaterialType::BRONZE, 110.0f, 0.34f, 125.0f);
  elastic(MaterialType::COPPER, 117.0f, 0.34f, 70.0f);
  elastic(MaterialType::CHROME, 279.0f, 0.21f, 0.0f);
  elastic(MaterialType::GOLD, 79.0f, 0.44f, 0.0f);
  elastic(MaterialType::SILVER, 83.0f, 0.37f, 0.0f);
  elastic(MaterialType::TITANIUM, 116.0f, 0.32f, 880.0f);
  elastic(MaterialType::PLASTIC_WHITE, 2.5f, 0.35f, 50.0f);
  elastic(MaterialType::PLASTIC_BLACK, 2.5f, 0.35f, 50.0f);
  // Filament datasheets (tensile bars printed flat): E 2.0-2.2 GPa, yield
  // 45-53 MPa. A printed part is weaker ACROSS its layers (the pan/tilt
  // sources read 30-50 MPa there), which an isotropic study cannot see.
  elastic(MaterialType::PETG, 2.1f, 0.38f, 50.0f);
  // Prusament PETG's datasheet (v1.1): interlayer adhesion 18 +/- 4 MPa.
  materials[MaterialType::PETG].interlayerMPa = 18.0;
  // Prusament PLA's datasheet, printed specimens: E 2.3 GPa and yield
  // 51 +/- 3 MPa printed flat (2.4 and 59 upright); density 1.24 g/cm3.
  elastic(MaterialType::PLA, 2.3f, 0.36f, 48.0f);
  // The same datasheet (v1.1): interlayer adhesion 17 +/- 3 MPa, a third of
  // the in-layer yield -- what an upright print's bending breaks on
  // (docs/simulation/README.md#printed-layers).
  materials[MaterialType::PLA].interlayerMPa = 17.0;
  // Bambu Lab's TPU 95A datasheet, printed specimens: Young's modulus 7.8
  // MPa, tensile strength 27-30 MPa at over 650 % elongation. An elastomer
  // has no yield, so none: a study reports its stress without a factor of
  // safety. Nearly incompressible, as rubbers are. A linear study is only
  // honest for its small strains; a gasket's 25 % squeeze is not one.
  elastic(MaterialType::TPU, 0.0078f, 0.48f, 0.0f);
  // Quasi-isotropic 3K/epoxy laminate, in-plane: this is a plate material
  // and the numbers are the plate's, not a unidirectional ply's.
  elastic(MaterialType::CARBON_FIBRE, 55.0f, 0.30f, 550.0f);
  elastic(MaterialType::GLASS, 70.0f, 0.22f, 0.0f);
  elastic(MaterialType::RUBY, 345.0f, 0.29f, 0.0f);
  elastic(MaterialType::CONCRETE, 30.0f, 0.20f, 0.0f);
  elastic(MaterialType::EPOXY, 3.0f, 0.35f, 60.0f);
  // Building materials: brittle, so no yield (a study reports stress and no
  // factor of safety). Brick masonry 5-15 GPa (EN 1996-1-1 takes E = 1000
  // f_k); a concrete tile is a dense concrete's ~25 GPa; set gypsum plaster
  // ~2 GPa; cement render, a mortar, 10-15 GPa. Bitumen felt (viscoelastic)
  // and soil (not a linear solid at all) are left 0, as rubber is: a study
  // on them refuses rather than answering with a made-up modulus.
  elastic(MaterialType::BRICK, 10.0f, 0.20f, 0.0f);
  elastic(MaterialType::ROOF_TILE, 25.0f, 0.20f, 0.0f);
  elastic(MaterialType::PLASTER, 2.0f, 0.25f, 0.0f);
  elastic(MaterialType::RENDER, 12.0f, 0.20f, 0.0f);
  // Wood is strongly anisotropic (along-grain stiffness is ~15-20x the
  // transverse), but our FEM is linear-isotropic. These are representative
  // ALONG-GRAIN values (modulus, and modulus-of-rupture as a yield proxy);
  // a study treats the part as isotropic, so results are approximate and
  // conservative only when the load happens to run along the grain. Plywood
  // is cross-laminated so its in-plane response is closer to isotropic.
  elastic(MaterialType::WOOD_OAK, 11.0f, 0.37f, 95.0f);
  elastic(MaterialType::WOOD_PINE, 9.0f, 0.35f, 80.0f);
  elastic(MaterialType::WOOD_WALNUT, 11.5f, 0.38f, 100.0f);
  elastic(MaterialType::WOOD_MAPLE, 12.6f, 0.37f, 109.0f);
  elastic(MaterialType::WOOD_BEECH, 14.3f, 0.36f, 110.0f);
  elastic(MaterialType::PLYWOOD_BIRCH, 10.0f, 0.30f, 55.0f);
  elastic(MaterialType::PLYWOOD_PINE, 8.0f, 0.30f, 45.0f);
  elastic(MaterialType::MDF, 3.6f, 0.25f, 18.0f);

  // Hardening curves, for a plastic study (docs/simulation/README.md
  // #plasticity): (von Mises stress MPa, equivalent plastic strain) from
  // (yield, 0). Only where a source is cited here; every other material has
  // none, and a plastic study on it is refused rather than run perfectly
  // plastic or on a guessed slope.
  //
  // Structural steel: EN 1993-1-5:2006 Annex C.6(2), Figure C.2 c),
  // "elastic-plastic with linear strain hardening": past f_y the
  // stress-strain line has the slope E/100. In plastic strain that is the
  // hardening modulus H = E (E/100) / (E - E/100) = E/99. The curve stops at
  // 5 % plastic strain, the principal strain limit C.8(1) Note 1
  // recommends; past its end CalculiX holds the stress at the last point,
  // which a result warns of. The library's steel is the generic structural
  // grade (f_y 250 MPa), to which the model applies.
  auto linearHardening = [&](MaterialType t, double tangentOverE,
                             double lastStrain) {
    MaterialProperties& m = materials[t];
    const double e = 1000.0 * m.youngGPa;
    const double et = tangentOverE * e;
    const double h = e * et / (e - et);
    m.hardening = {{m.yieldMPa, 0.0},
                   {m.yieldMPa + h * lastStrain, lastStrain}};
  };
  linearHardening(MaterialType::STEEL, 1.0 / 100.0, 0.05);
  // Every other metal is left without one: stainless steel (EN 1993-1-4's
  // Ramberg-Osgood model), the aluminium alloys (EN 1999-1-1 Annex E) and
  // the rest have published curves, but none is cited here yet. The printed
  // plastics' datasheets give a yield and a break, not a curve between them.

  // Thermal numbers (docs/simulation/README.md#thermal): a conductivity at
  // room temperature, W/(m K), and a linear expansion coefficient, 1/K. Only
  // where a source is cited here; every other material has none, and a
  // thermal study or thermal load on it is refused rather than guessed.
  auto thermal = [&](MaterialType t, double conductivity, double expansion) {
    materials[t].conductivityWmK = conductivity;
    materials[t].expansionPerK = expansion;
  };
  // Structural steel: EN 1993-1-2:2005 §3.4.1.3(1), eq. (3.3a), lambda_a =
  // 54 - 3.33e-2 theta_a W/(m K) for 20 <= theta_a < 800 C, at 20 C 53.33;
  // EN 1993-1-1:2005 §3.2.6(1), alpha = 12e-6 per K for T <= 100 C.
  thermal(MaterialType::STEEL, 54.0 - 3.33e-2 * 20.0, 12e-6);
  // Stainless 1.4301 (the library's stainless): SCI P413, Design Manual for
  // Structural Stainless Steel, 4th ed. (2017), §2.4 Table 2.7, room
  // temperature, annealed: 15 W/(m C), and 16e-6 per C over 20-100 C.
  thermal(MaterialType::STAINLESS_STEEL, 15.0, 16e-6);
  // Brass, C26000: Wieland Concast, "C26000" datasheet (CDA's physical
  // properties): conductivity 121.2 W/(m K) at 20 C, expansion 19.2e-6 per
  // C over 20-300 C.
  thermal(MaterialType::BRASS, 121.2, 19.2e-6);
  // KANTHAL 94S: Kanthal, Thermostatic Bimetal Handbook (catalogue
  // 3-A-1-3), p. 40: conductivity 12 W/(m K). No expansion coefficient:
  // a bimetal is two, and its motion is its specific curvature's (below),
  // so a thermal load on it is refused.
  thermal(MaterialType::BIMETAL_KANTHAL_94S, 12.0, 0.0);
  // Its maker's constants, the same page and the type table (pp. 17-18):
  // specific curvature k = 17.8e-6 per K +/- 4 %, specific deflection a =
  // 9.5e-6 per K, linear from 0 to +200 C; components 38/7 NiCr steel and
  // 19/7 NiCr steel. "Both components consist of NiCr steels. KANTHAL 94S
  // is suitable for use in water and steam" (handbook §4).
  {
    MaterialProperties& b = materials[MaterialType::BIMETAL_KANTHAL_94S];
    b.specificCurvaturePerK = 17.8e-6;
    b.specificCurvatureTolerance = 0.04;
    b.specificDeflectionPerK = 9.5e-6;
    b.linearFromC = 0.0;
    b.linearToC = 200.0;
  }
  // The 6xxx alloys: EN 1999-1-2:2007 §3.3.1.3(1), lambda = 0.07 theta + 190
  // W/(m C) for 3xxx and 6xxx alloys, at 20 C 191.4 (read in the English
  // column of DSTU-N B EN 1999-1-2:2010, its Ukrainian adoption; the 2023
  // edition was not checked); EN 1999-1-1 §3.2.5, alpha = 23e-6 per K for
  // T < 100 C (as SCIA's EN 1999-1-1 training manual, 2015, p. 12 quotes
  // it).
  thermal(MaterialType::ALUMINUM_6061_T6, 0.07 * 20.0 + 190.0, 23e-6);
  thermal(MaterialType::ALUMINUM_6082_T6, 0.07 * 20.0 + 190.0, 23e-6);
  // PLA, BULK: NatureWorks, "Engineering Properties of Ingeo Biopolymer"
  // (NWPD001_080311), page 1: 0.130 W/(m K) at 25 C for amorphous sheet (a
  // printed part is amorphous). No expansion coefficient: NatureWorks
  // publishes no measured one (its Moldflow report MAT2238's 8.5e-5 is
  // marked a generic estimate), so a thermal load on PLA is refused.
  thermal(MaterialType::PLA, 0.130, 0.0);
  materials[MaterialType::PLA].bulkConductivity = true;
  // PETG, BULK: Eastman, "Eastar copolyester 6763 technical information"
  // (MBS-80Q 2/12), Table 2: thermal conductivity 0.21 W/(m K) (ASTM C177),
  // linear expansion 5.1e-5 per C (ASTM D696, -30 to 40 C).
  thermal(MaterialType::PETG, 0.21, 5.1e-5);
  materials[MaterialType::PETG].bulkConductivity = true;
  // Service temperature: a printed plastic's heat deflection temperature at
  // 0.45 MPa (ISO 75, method B's stress), from the datasheets its
  // interlayer strength cites. Metals get none: their reduction factors
  // start far above where a printed part fails (EN 1993-1-2's yield factor
  // is 1.0 to 400 C), and no number is added without a source.
  // Prusament PLA TDS v1.1 (27-07-2022), "Heat Deflection Temperature (0.45
  // MPa) 55 C, ISO 75".
  materials[MaterialType::PLA].serviceMaxC = 55.0;
  materials[MaterialType::PLA].serviceBasis =
      "heat deflection temperature, 0.45 MPa, ISO 75 (Prusament PLA "
      "datasheet v1.1)";
  // Prusament PETG TDS v1.1 (16-02-2022), "Heat Deflection Temperature
  // (0.45 MPa) 68 C, ISO 75".
  materials[MaterialType::PETG].serviceMaxC = 68.0;
  materials[MaterialType::PETG].serviceBasis =
      "heat deflection temperature, 0.45 MPa, ISO 75 (Prusament PETG "
      "datasheet v1.1)";

  // Procedural surface figure: which pattern, its scale per local unit, and
  // its contrast (0..1). Shaded from local object-space position by
  // src/shaders/grain.glsl; MDF stays Flat, and so does every material not
  // listed. Scale is per-mm; tune against a render.
  auto pattern = [&](MaterialType t, SurfacePattern p, float scale,
                     float contrast) {
    materials[t].pattern = p;
    materials[t].patternScale = scale;
    materials[t].patternContrast = contrast;
  };
  using SP = SurfacePattern;
  pattern(MaterialType::WOOD_OAK, SP::Wood, 0.15f, 0.35f);
  pattern(MaterialType::WOOD_PINE, SP::Wood, 0.09f, 0.5f);   // wide, strong
  pattern(MaterialType::WOOD_WALNUT, SP::Wood, 0.22f, 0.3f); // fine, figured
  pattern(MaterialType::WOOD_MAPLE, SP::Wood, 0.2f, 0.25f);  // faint, fine
  pattern(MaterialType::WOOD_BEECH, SP::Wood, 0.25f, 0.22f); // fine, even
  pattern(MaterialType::PLYWOOD_BIRCH, SP::Plywood, 0.7f, 0.3f); // plies/mm
  pattern(MaterialType::PLYWOOD_PINE, SP::Plywood, 0.5f, 0.35f);
  // 3K twill is roughly a 2 mm repeat, so 0.5 tows/mm. The contrast here is
  // the tow-gap darkening only: on a 0.07 albedo there is almost nothing to
  // darken, and what actually makes the weave read is the normal relief
  // grain.glsl derives from the same params.
  pattern(MaterialType::CARBON_FIBRE, SP::Weave, 0.5f, 0.45f);
  // MDF: Flat — a wood product, but genuinely near-uniform.

  // RUBBER, ROOFING_FELT, SOIL and DEFAULT intentionally left at 0
  // (near-incompressible / unassigned): a linear-static study on them errors
  // rather than guessing.

  // What a drawing writes: a grade row its grade, a generic row its name
  // in capitals (which says that no grade was chosen).
  for (auto& [type, m] : materials) {
    m.grade = m.name;
    for (char& c : m.grade)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  materials[MaterialType::ALUMINUM_6061_T6].grade = "6061-T6";
  materials[MaterialType::ALUMINUM_6082_T6].grade = "6082-T6";
  materials[MaterialType::BIMETAL_KANTHAL_94S].grade = "KANTHAL 94S";
  materials[MaterialType::DEFAULT].grade.clear();
}

const MaterialProperties& MaterialLibrary::GetMaterial(MaterialType type) {
  InitializeMaterials();
  auto it = materials.find(type);
  if (it != materials.end()) {
    return it->second;
  }
  return materials[MaterialType::DEFAULT];
}

std::string ElasticDataRefusal(const MaterialProperties& m) {
  if (!(m.youngGPa > 0.0))
    return "has no stiffness data";
  if (!(m.poisson > 0.0))
    return "has no Poisson's ratio in the library (its maker publishes "
           "none, and none is entered without a source), so a structural "
           "study cannot solve it";
  return {};
}

float ViewportAlpha(const MaterialProperties& m) {
  // A tenth of a clear body's colour: in the viewport glass is shaded as
  // glass (mesh.frag), and its reflection, its highlights and its outline
  // are what give a pane presence — at a fifth it read as a grey veil.
  return m.transmission > 0.0f
             ? glm::clamp(1.0f - 0.9f * m.transmission, 0.1f, 1.0f)
             : 1.0f;
}

namespace {
// Single source of truth for slug <-> type and UI ordering. Grouped metals
// first, then non-metals; DEFAULT is intentionally excluded from AllTypes() (it
// is the empty-slug fallback, not a user-picked material).
const std::vector<std::pair<MaterialType, const char*>>& SlugTable() {
  static const std::vector<std::pair<MaterialType, const char*>> table = {
      {MaterialType::STEEL, "steel"},
      {MaterialType::STAINLESS_STEEL, "stainless_steel"},
      {MaterialType::BRASS, "brass"},
      {MaterialType::BRONZE, "bronze"},
      {MaterialType::COPPER, "copper"},
      {MaterialType::CHROME, "chrome"},
      {MaterialType::GOLD, "gold"},
      {MaterialType::SILVER, "silver"},
      {MaterialType::TITANIUM, "titanium"},
      {MaterialType::BIMETAL_KANTHAL_94S, "bimetal_kanthal_94s"},
      {MaterialType::ALUMINUM_6061_T6, "aluminum_6061_t6"},
      {MaterialType::ALUMINUM_6082_T6, "aluminum_6082_t6"},
      {MaterialType::PLASTIC_WHITE, "plastic_white"},
      {MaterialType::PLASTIC_BLACK, "plastic_black"},
      {MaterialType::PETG, "petg"},
      {MaterialType::PLA, "pla"},
      {MaterialType::TPU, "tpu"},
      {MaterialType::CARBON_FIBRE, "carbon_fibre"},
      {MaterialType::RUBBER, "rubber"},
      {MaterialType::GLASS, "glass"},
      {MaterialType::RUBY, "ruby"},
      {MaterialType::CONCRETE, "concrete"},
      {MaterialType::EPOXY, "epoxy"},
      {MaterialType::BRICK, "brick"},
      {MaterialType::ROOF_TILE, "roof_tile"},
      {MaterialType::PLASTER, "plaster"},
      {MaterialType::ROOFING_FELT, "roofing_felt"},
      {MaterialType::RENDER, "render"},
      {MaterialType::SOIL, "soil"},
      {MaterialType::WOOD_OAK, "wood_oak"},
      {MaterialType::WOOD_PINE, "wood_pine"},
      {MaterialType::WOOD_WALNUT, "wood_walnut"},
      {MaterialType::WOOD_MAPLE, "wood_maple"},
      {MaterialType::WOOD_BEECH, "wood_beech"},
      {MaterialType::PLYWOOD_BIRCH, "plywood_birch"},
      {MaterialType::PLYWOOD_PINE, "plywood_pine"},
      {MaterialType::MDF, "mdf"},
  };
  return table;
}
} // namespace

std::string MaterialLibrary::Slug(MaterialType type) {
  for (const auto& [t, slug] : SlugTable()) {
    if (t == type)
      return slug;
  }
  return ""; // DEFAULT (and anything unmapped) serializes as empty
}

bool MaterialLibrary::TryMaterialTypeFromString(const std::string& slug,
                                                MaterialType& out) {
  if (slug.empty()) {
    out = MaterialType::DEFAULT;
    return true;
  }
  for (const auto& [t, s] : SlugTable()) {
    if (slug == s) {
      out = t;
      return true;
    }
  }
  return false;
}

MaterialType MaterialLibrary::MaterialTypeFromString(const std::string& slug) {
  MaterialType t{};
  if (TryMaterialTypeFromString(slug, t))
    return t;
  // Refuse, don't substitute: this used to return DEFAULT, which is an
  // aluminium appearance carrying density 0 — so a document naming a material
  // this build does not have looked like machined aluminium and reported no
  // mass, and every material added made one more slug for an older build to
  // swallow.
  throw std::runtime_error("unknown material slug '" + slug +
                           "' (expected one of: " + SlugList() + ")");
}

const MaterialProperties&
MaterialLibrary::GetMaterialOrDefault(const std::string& slug) {
  MaterialType t = MaterialType::DEFAULT;
  (void)TryMaterialTypeFromString(slug, t); // unresolved stays DEFAULT
  return GetMaterial(t);
}

std::string MaterialLibrary::FamilySlug(const std::string& slug) {
  if (slug == "aluminum_6061_t6" || slug == "aluminum_6082_t6")
    return "aluminum";
  return slug;
}

std::string MaterialLibrary::GradeOf(const std::string& slug) {
  if (slug.empty())
    return {};
  MaterialType t{};
  if (!TryMaterialTypeFromString(slug, t)) {
    std::string out = slug;
    for (char& c : out)
      c = c == '_' ? ' ' : static_cast<char>(std::toupper(c));
    return out;
  }
  return GetMaterial(t).grade;
}

const std::string& MaterialLibrary::HardeningSlugList() {
  static const std::string list = [] {
    std::string s;
    for (const MaterialType t : AllTypes())
      if (!GetMaterial(t).hardening.empty())
        s += (s.empty() ? "" : ", ") + Slug(t);
    return s;
  }();
  return list;
}

const std::string& MaterialLibrary::ConductivitySlugList() {
  static const std::string list = [] {
    std::string s;
    for (const MaterialType t : AllTypes())
      if (GetMaterial(t).conductivityWmK > 0.0)
        s += (s.empty() ? "" : ", ") + Slug(t);
    return s;
  }();
  return list;
}

const std::string& MaterialLibrary::ExpansionSlugList() {
  static const std::string list = [] {
    std::string s;
    for (const MaterialType t : AllTypes())
      if (GetMaterial(t).expansionPerK > 0.0)
        s += (s.empty() ? "" : ", ") + Slug(t);
    return s;
  }();
  return list;
}

const std::string& MaterialLibrary::SlugList() {
  static const std::string list = [] {
    std::string s;
    for (const auto& slug : AllSlugs())
      s += (s.empty() ? "" : ", ") + slug;
    return s;
  }();
  return list;
}

int PatternAxis(SurfacePattern pattern, const glm::vec3& localExtent) {
  // Sheet patterns lie IN the sheet, so their axis is its normal — the
  // thinnest extent. Solid wood runs along a board's length, the longest.
  const bool sheet =
      pattern == SurfacePattern::Plywood || pattern == SurfacePattern::Weave;
  int axis = 0;
  for (int a = 1; a < 3; ++a) {
    if (sheet ? (localExtent[a] < localExtent[axis])
              : (localExtent[a] > localExtent[axis]))
      axis = a;
  }
  return axis;
}

const std::vector<std::string>& MaterialLibrary::AllSlugs() {
  static const std::vector<std::string> slugs = [] {
    std::vector<std::string> v;
    for (const auto& [t, slug] : SlugTable())
      v.push_back(slug);
    return v;
  }();
  return slugs;
}

const std::vector<MaterialType>& MaterialLibrary::AllTypes() {
  static const std::vector<MaterialType> types = [] {
    std::vector<MaterialType> v;
    for (const auto& [t, slug] : SlugTable())
      v.push_back(t);
    return v;
  }();
  return types;
}

} // namespace CADMaterials