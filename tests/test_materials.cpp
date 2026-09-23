#include <catch2/catch_test_macros.hpp>

#include "cad/CADMaterials.h"

#include <algorithm>
#include <set>
#include <string>

using CADMaterials::MaterialLibrary;
using CADMaterials::MaterialProperties;
using CADMaterials::MaterialType;
using CADMaterials::PatternAxis;
using CADMaterials::SurfacePattern;

// Which materials carry which procedural figure. Everything not named here is
// Flat — every metal, plastic and ceramic, and MDF, which is a wood product
// but genuinely near-uniform.
static bool IsGrainWood(MaterialType t) {
  return t == MaterialType::WOOD_OAK || t == MaterialType::WOOD_PINE ||
         t == MaterialType::WOOD_WALNUT;
}
static bool IsPlywood(MaterialType t) {
  return t == MaterialType::PLYWOOD_BIRCH || t == MaterialType::PLYWOOD_PINE;
}
// Woven composites. Carbon is the only one today; fibreglass and Kevlar would
// join it as more parameter sets for the SAME pattern, which is the property
// that keeps the pattern count from tracking the material count.
static bool IsWeave(MaterialType t) { return t == MaterialType::CARBON_FIBRE; }

TEST_CASE("Slug round-trips for every assignable material", "[materials]") {
  // MaterialTypeFromString(Slug(t)) == t for all assignable types: a mismatch
  // means a
  // saved file's material token would resolve to the wrong (or default)
  // material on load.
  for (MaterialType t : MaterialLibrary::AllTypes()) {
    const std::string slug = MaterialLibrary::Slug(t);
    CHECK(!slug.empty());
    CHECK(MaterialLibrary::MaterialTypeFromString(slug) == t);
  }
}

TEST_CASE("Slugs are unique", "[materials]") {
  std::set<std::string> seen;
  for (MaterialType t : MaterialLibrary::AllTypes()) {
    const std::string slug = MaterialLibrary::Slug(t);
    CHECK(seen.insert(slug).second); // false if a duplicate slug was inserted
  }
}

TEST_CASE("Wood serialization tokens are stable", "[materials]") {
  // These slugs live in saved project files; renaming one silently breaks every
  // file that referenced it. Pin them explicitly.
  CHECK(MaterialLibrary::MaterialTypeFromString("wood_oak") ==
        MaterialType::WOOD_OAK);
  CHECK(MaterialLibrary::MaterialTypeFromString("wood_pine") ==
        MaterialType::WOOD_PINE);
  CHECK(MaterialLibrary::MaterialTypeFromString("wood_walnut") ==
        MaterialType::WOOD_WALNUT);
  CHECK(MaterialLibrary::MaterialTypeFromString("plywood_birch") ==
        MaterialType::PLYWOOD_BIRCH);
  CHECK(MaterialLibrary::MaterialTypeFromString("plywood_pine") ==
        MaterialType::PLYWOOD_PINE);
  CHECK(MaterialLibrary::MaterialTypeFromString("mdf") == MaterialType::MDF);
}

TEST_CASE("The empty slug means default; an unknown one is refused",
          "[materials]") {
  // Empty is legal and is what set_material material="" writes.
  CHECK(MaterialLibrary::MaterialTypeFromString("") == MaterialType::DEFAULT);
  CHECK(MaterialLibrary::Slug(MaterialType::DEFAULT).empty());

  // Anything else unrecognised is REFUSED, not substituted. Returning DEFAULT
  // here meant an aluminium appearance carrying density 0: a document naming a
  // material this build lacks looked like machined aluminium while honestly
  // reporting no mass, and every material added made one more slug for an
  // older build to swallow.
  CHECK_THROWS(MaterialLibrary::MaterialTypeFromString("not_a_material"));
  // The refusal names what IS accepted (the describe/material_slugs set).
  CHECK(MaterialLibrary::SlugList().find("carbon_fibre") != std::string::npos);

  // ... and the non-throwing form reports it instead, for the draw-time
  // consumers that must not take a frame down.
  MaterialType out = MaterialType::STEEL;
  CHECK_FALSE(
      MaterialLibrary::TryMaterialTypeFromString("not_a_material", out));
  CHECK(MaterialLibrary::GetMaterialOrDefault("not_a_material").density ==
        0.0f);
}

TEST_CASE("New wood materials are enumerated for the UI", "[materials]") {
  const auto& all = MaterialLibrary::AllTypes();
  auto has = [&](MaterialType t) {
    return std::find(all.begin(), all.end(), t) != all.end();
  };
  CHECK(has(MaterialType::WOOD_OAK));
  CHECK(has(MaterialType::WOOD_WALNUT));
  CHECK(has(MaterialType::PLYWOOD_BIRCH));
  CHECK(has(MaterialType::MDF));
}

TEST_CASE("Every material carries exactly the pattern it should",
          "[materials]") {
  // The shader keys the figure off MaterialProperties::pattern; if a metal
  // accidentally carried one it would render grain, and if a wood carried
  // Flat it would render blank. Assert the exact partition, and that a
  // pattern implies usable params.
  for (MaterialType t : MaterialLibrary::AllTypes()) {
    const MaterialProperties& m = MaterialLibrary::GetMaterial(t);
    if (IsGrainWood(t)) {
      CHECK(m.pattern == SurfacePattern::Wood);
    } else if (IsPlywood(t)) {
      CHECK(m.pattern == SurfacePattern::Plywood);
    } else if (IsWeave(t)) {
      CHECK(m.pattern == SurfacePattern::Weave);
    } else {
      CHECK(m.pattern == SurfacePattern::Flat); // MDF, and everything else
    }

    if (m.pattern != SurfacePattern::Flat) {
      // A pattern with no scale would collapse the figure to a constant.
      CHECK(m.patternScale > 0.0f);
      CHECK(m.patternContrast > 0.0f);
      CHECK(m.patternContrast <= 1.0f);
    } else {
      CHECK(m.patternScale == 0.0f);
      CHECK(m.patternContrast == 0.0f);
    }
  }
}

TEST_CASE("Carbon fibre is told from black plastic by more than gloss",
          "[materials]") {
  // The two are within 0.02 of each other on every albedo channel, so before
  // the weave the only thing separating them on screen was roughness — and
  // getting them confused costs a third of an airframe's structural mass.
  const MaterialProperties& cf =
      MaterialLibrary::GetMaterial(MaterialType::CARBON_FIBRE);
  const MaterialProperties& pb =
      MaterialLibrary::GetMaterial(MaterialType::PLASTIC_BLACK);
  CHECK(glm::length(cf.baseColor - pb.baseColor) < 0.05f);
  CHECK(cf.pattern == SurfacePattern::Weave);
  CHECK(pb.pattern == SurfacePattern::Flat);
  CHECK(cf.density > pb.density * 1.4f);
}

TEST_CASE("A pattern's axis comes from the part's local extent",
          "[materials]") {
  // The rule the raster and ray-traced paths BOTH call now. They used to keep
  // a copy each, and the copies disagreed about which frame to measure in, so
  // a rotated occurrence's grain ran one way in the viewport and another in a
  // still. Nothing tested either copy.
  const glm::vec3 board(400.0f, 80.0f, 18.0f); // long, wide, thin

  // Solid wood runs along the board's length.
  CHECK(PatternAxis(SurfacePattern::Wood, board) == 0);
  CHECK(PatternAxis(SurfacePattern::Wood, glm::vec3(18, 400, 80)) == 1);
  CHECK(PatternAxis(SurfacePattern::Wood, glm::vec3(80, 18, 400)) == 2);

  // Sheet patterns run across the thickness — the sheet normal.
  CHECK(PatternAxis(SurfacePattern::Plywood, board) == 2);
  CHECK(PatternAxis(SurfacePattern::Weave, board) == 2);
  CHECK(PatternAxis(SurfacePattern::Weave, glm::vec3(2, 300, 300)) == 0);

  // A cube is degenerate but must still be deterministic, not uninitialised.
  CHECK(PatternAxis(SurfacePattern::Wood, glm::vec3(10.0f)) == 0);
  CHECK(PatternAxis(SurfacePattern::Weave, glm::vec3(10.0f)) == 0);
}

TEST_CASE("Wood materials are physically usable", "[materials]") {
  // Non-metallic, in-gamut base color, and a real density (so mass_properties
  // reports a mass) plus stiffness (so an FEM study runs rather than erroring).
  const MaterialType woods[] = {
      MaterialType::WOOD_OAK,     MaterialType::WOOD_PINE,
      MaterialType::WOOD_WALNUT,  MaterialType::PLYWOOD_BIRCH,
      MaterialType::PLYWOOD_PINE, MaterialType::MDF};
  for (MaterialType t : woods) {
    const MaterialProperties& m = MaterialLibrary::GetMaterial(t);
    CHECK(m.metallic == 0.0f);
    for (int c = 0; c < 3; c++) {
      CHECK(m.baseColor[c] >= 0.0f);
      CHECK(m.baseColor[c] <= 1.0f);
    }
    CHECK(m.density > 0.0f);  // air-dry wood/board density
    CHECK(m.youngGPa > 0.0f); // representative along-grain modulus (isotropic)
    CHECK(m.poisson > 0.0f);
    CHECK(m.poisson < 0.5f); // physically valid Poisson ratio
  }
}

TEST_CASE("Glass, and only glass, transmits light", "[materials]") {
  // The still refracts through a transmissive body and the viewport draws it
  // see-through (docs/rendering/materials.md#glass). A metal that picked up a
  // transmission by accident would vanish from every render, so the
  // partition is exact.
  for (MaterialType t : MaterialLibrary::AllTypes()) {
    const MaterialProperties& m = MaterialLibrary::GetMaterial(t);
    if (t == MaterialType::GLASS) {
      CHECK(m.transmission > 0.0f);
      CHECK(m.ior > 1.0f);
      CHECK(CADMaterials::ViewportAlpha(m) < 1.0f);
    } else {
      CHECK(m.transmission == 0.0f);
      CHECK(CADMaterials::ViewportAlpha(m) == 1.0f);
    }
  }
}
