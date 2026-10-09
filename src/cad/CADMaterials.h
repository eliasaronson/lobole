// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace CADMaterials {

enum class MaterialType {
  // Metals
  ALUMINUM,
  STEEL,
  STAINLESS_STEEL,
  BRASS,
  BRONZE,
  COPPER,
  CHROME,
  GOLD,
  SILVER,
  TITANIUM,
  // Aluminium alloys by grade (EN 573 / AA designation and temper): their
  // own rows because a grade is what a drawing orders and what a study's
  // yield is -- 6061-T6's 276 MPa is nearly three times the generic row's
  // 95. One appearance with ALUMINUM; CAM cuts them as aluminium
  // (FamilySlug).
  ALUMINUM_6061_T6,
  ALUMINUM_6082_T6,

  // Non-metals
  PLASTIC_WHITE,
  PLASTIC_BLACK,
  // PETG as an FDM printer lays it down: the printed parts of a pan/tilt
  // head. Its own row because 1270 kg/m3 against generic plastic's 1040 reads
  // a printed part a fifth light, and a study wants its own modulus. The
  // process allowance a printed part is MODELLED with is not a material
  // property (docs/tolerances/README.md#printed-parts).
  PETG,
  // PLA as an FDM printer lays it down: what most printed parts are. Its own
  // row for the filament's density and modulus, as PETG has.
  PLA,
  // 3K twill carbon plate: the material a frame, an arm or a drone's whole
  // structure is cut from, and 1550 kg/m3 against black plastic's 1040 —
  // modelling one as the other reads a third light, which is a third of an
  // airframe's structural mass. Rendered with SurfacePattern::Weave, which is
  // what tells it from PLASTIC_BLACK on sight — the two differ only in gloss
  // otherwise.
  CARBON_FIBRE,
  RUBBER,
  GLASS,
  // Synthetic corundum (Al2O3): watch jewels, bearings, windows. Its own row
  // because its density is half a steel's — a jewel modelled as steel with a
  // red appearance weighs twice what it does. Blued steel is NOT a row: the
  // blue is an oxide film a few hundred nanometres thick, so steel with a
  // blue appearance is the honest model of it.
  RUBY,
  CONCRETE,
  // Cured casting epoxy, the fill of an inlay (a chess board's dark
  // squares). Its own row for its density and stiffness; the colour is the
  // pigmented black an inlay is poured in -- a clear pour is this with a
  // glass appearance.
  EPOXY,

  // Building materials (v165): what a house is made of, so its mass and its
  // parts list mean something and a finish is not an override on concrete.
  BRICK,        // fired clay brick
  ROOF_TILE,    // concrete roof tile
  PLASTER,      // gypsum plaster
  ROOFING_FELT, // bitumen roofing felt
  RENDER,       // cement render
  SOIL,         // the ground a building stands on

  // Wood & engineered wood. Appearance is a procedural surface pattern shaded
  // from local object-space position (no textures / UVs); see SurfacePattern.
  WOOD_OAK,
  WOOD_PINE,
  WOOD_WALNUT,
  WOOD_MAPLE, // hard maple: the pale wood of a turned set or a board
  WOOD_BEECH,
  PLYWOOD_BIRCH,
  PLYWOOD_PINE,
  MDF,

  // Default
  DEFAULT
};

// A procedural surface figure, evaluated in object space by
// src/shaders/grain.glsl and shared verbatim by the raster (mesh.frag) and
// ray-traced (closesthit.rchit) paths. There are no textures and no UVs
// anywhere in the appearance system.
//
// Patterns are FUNCTIONS and materials are PARAMETERS to them — carbon,
// fibreglass and Kevlar are one Weave at three scales, not three patterns.
// That asymmetry is what keeps this scalable as the library grows: material
// count and pattern count grow at very different rates (6061/7075/2024 are
// three rows and one appearance). So never branch on a MaterialType in a
// shader.
//
// Values are mirrored as GRAIN_PATTERN_* in src/shaders/grain.glsl and reach
// the GPU in the x component of a single vec4 (see PatternAxis for w). The
// raster push-constant block is at the 128-byte Vulkan floor, so a new pattern
// must fit inside that vec4 rather than add another.
enum class SurfacePattern : int {
  Flat = 0,    // every metal, plastic, ceramic — and MDF, which really is flat
  Wood = 1,    // solid-wood rings + turbulence
  Plywood = 2, // face veneer, ply stripes on the cut edges
  Weave = 3,   // 2/2 twill, for woven composites
};

// Which object-local axis the pattern runs along, given the part's local
// extent. Solid wood runs along the LONGEST extent (a board's length); sheet
// patterns (plywood, weave) run across the THINNEST (the sheet normal).
//
// This lives here rather than in a renderer because it is a property of the
// pattern, not of a draw call: the raster and ray-traced paths each carried
// their own copy of the rule, and the copies disagreed — the ray tracer
// measured the extent AFTER the occurrence transform was baked into the
// vertices, so a rotated part's grain ran one way in the viewport and another
// in a still.
int PatternAxis(SurfacePattern pattern, const glm::vec3& localExtent);

struct MaterialProperties {
  glm::vec3 baseColor;
  float metallic;
  float roughness;
  float specular;
  float reflectance;
  std::string name;
  // kg/m^3; 0 = unknown, so mass_properties reports no mass rather than a
  // guessed one (DEFAULT deliberately stays 0).
  double density = 0.0;
  // Linear-elastic properties for FEM (same 0 = unknown convention as
  // density; DEFAULT stays 0 so a study on an unassigned body fails with
  // "material has no stiffness data" rather than guessing).
  double youngGPa = 0.0; // Young's modulus, GPa
  double poisson = 0.0;  // Poisson's ratio (dimensionless)
  double yieldMPa = 0.0; // yield strength, MPa (0 = no factor-of-safety)

  // Procedural surface figure. Both fields are pattern-relative, and are 0
  // exactly when pattern is Flat:
  //   Wood     scale = rings per local unit, contrast = late-wood darkening
  //   Plywood  scale = plies per local unit, contrast = glue-line darkening
  //   Weave    scale = tows per local unit,  contrast = tow-gap darkening
  // Contrast is 0..1. Scale is per-mm — tune it against a render.
  SurfacePattern pattern = SurfacePattern::Flat;
  float patternScale = 0.0f;
  float patternContrast = 0.0f;

  // Light passing THROUGH the body: 0 = opaque (every row but glass), 1 =
  // clear. The still refracts through it at `ior`; the viewport draws it
  // see-through at ViewportAlpha. docs/rendering/materials.md#glass.
  float transmission = 0.0f;
  float ior = 0.0f; // index of refraction; meaningful only with transmission

  // What a drawing's parts list and title block write for it (v151): the
  // grade a shop orders ("6061-T6", "PETG"), or the material's name in
  // capitals for a generic row ("ALUMINUM") -- which then says, truthfully,
  // that no grade was chosen.
  std::string grade;
};

// How opaque the viewport draws a body (1 = opaque). A transmissive one goes
// through the ghost pass — alpha-blended, no depth write, no shadow — which
// is see-through but not refractive; a fully clear body keeps a tenth of its
// colour, and its reflection and highlights (mesh.frag's glass branch) give it
// the rest of its presence.
float ViewportAlpha(const MaterialProperties& m);

class MaterialLibrary {
public:
  static const MaterialProperties& GetMaterial(MaterialType type);

  // Stable serialization token (e.g. "stainless_steel"), independent of the
  // human-facing display name in MaterialProperties::name.
  static std::string Slug(MaterialType type);
  // Reverse of Slug. The empty slug is legal and means DEFAULT — that is what
  // set_material material="" writes and what Slug(DEFAULT) returns — but any
  // other unrecognised token is REFUSED, naming what IS accepted. It used to
  // resolve to DEFAULT, which is an aluminium appearance carrying density 0,
  // so a document written by a newer build looked like machined aluminium
  // while honestly reporting no mass. Named *FromString so the silent-parser
  // scan in tests/test_conventions.cpp covers it.
  static MaterialType MaterialTypeFromString(const std::string& slug);
  // Non-throwing form, for consumers that resolve a slug lazily somewhere that
  // must not throw (renderer sync mid-frame, mass properties, mesh export).
  // False for an unrecognised non-empty slug; `out` is then untouched.
  [[nodiscard]] static bool TryMaterialTypeFromString(const std::string& slug,
                                                      MaterialType& out);
  // GetMaterial(MaterialTypeFromString(slug)) for the lazy consumers that
  // resolve a slug somewhere that MUST NOT throw — renderer sync mid-frame,
  // mass properties, mesh export, panel labels. Falls back to DEFAULT.
  //
  // This is not a hole in the refuse-don't-substitute rule: every ingress
  // (Feature::SetMaterial, and so set_material / add_feature / modify_feature
  // / document load) refuses an unknown slug, so nothing that reaches a draw
  // call can carry one. The fallback exists so that if one ever did, it costs
  // a wrong-looking body rather than a dead frame.
  static const MaterialProperties&
  GetMaterialOrDefault(const std::string& slug);
  // All assignable materials in display order (for UI enumeration).
  static const std::vector<MaterialType>& AllTypes();
  // All assignable slugs in display order (RPC/describe enumeration).
  static const std::vector<std::string>& AllSlugs();
  // AllSlugs() joined with ", ", for refusal messages (cf.
  // cam::OpTypeNameList()).
  static const std::string& SlugList();
  // The generic material a grade row is one of ("aluminum_6061_t6" ->
  // "aluminum"), and any other slug itself: what a consumer that knows
  // materials only by family (CAM's cutting data) looks up. Not used where
  // the alloy decides the answer -- sheet metal's bend radii and a tube's
  // rules are for one alloy, and refuse a grade they do not carry.
  static std::string FamilySlug(const std::string& slug);
  // The grade a drawing writes for a slug (MaterialProperties::grade);
  // empty for the empty slug, the slug itself in capitals for one this
  // build does not know.
  static std::string GradeOf(const std::string& slug);

private:
  static std::unordered_map<MaterialType, MaterialProperties> materials;
  static void InitializeMaterials();
};

} // namespace CADMaterials
