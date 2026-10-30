// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "cad/CADMaterials.h"
#ifdef HAVE_CAM
#include "core/cam/CamFeeds.h"
#endif

#include <algorithm>
#include <set>
#include <stdexcept>
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
         t == MaterialType::WOOD_WALNUT || t == MaterialType::WOOD_MAPLE ||
         t == MaterialType::WOOD_BEECH;
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
  CHECK(MaterialLibrary::MaterialTypeFromString("wood_maple") ==
        MaterialType::WOOD_MAPLE);
  CHECK(MaterialLibrary::MaterialTypeFromString("wood_beech") ==
        MaterialType::WOOD_BEECH);
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

TEST_CASE("PETG is a material of its own, heavier than generic plastic",
          "[materials]") {
  // A printed part in PETG weighed as generic plastic reads a fifth light,
  // and FEM wants the filament's own modulus.
  CHECK(MaterialLibrary::MaterialTypeFromString("petg") == MaterialType::PETG);
  CHECK(MaterialLibrary::Slug(MaterialType::PETG) == "petg");
  const MaterialProperties& petg =
      MaterialLibrary::GetMaterial(MaterialType::PETG);
  CHECK(petg.density == 1270.0f);
  CHECK(petg.density >
        MaterialLibrary::GetMaterial(MaterialType::PLASTIC_WHITE).density);
  CHECK(petg.youngGPa > 2.0f);
  CHECK(petg.yieldMPa > 0.0f);
  CHECK(petg.metallic == 0.0f);
}

TEST_CASE("TPU is a material of its own: an elastomer, with no yield",
          "[materials]") {
  // A printed gasket weighed and studied as rubber or plastic reads the
  // wrong stiffness by orders of magnitude: TPU 95A's modulus is 7.8 MPa,
  // three hundred times under PLA's.
  CHECK(MaterialLibrary::MaterialTypeFromString("tpu") == MaterialType::TPU);
  CHECK(MaterialLibrary::Slug(MaterialType::TPU) == "tpu");
  const MaterialProperties& tpu =
      MaterialLibrary::GetMaterial(MaterialType::TPU);
  CHECK(tpu.density == 1220.0f);
  CHECK(tpu.youngGPa > 0.0f);
  CHECK(tpu.youngGPa <
        MaterialLibrary::GetMaterial(MaterialType::PLA).youngGPa / 100.0f);
  // No yield: a study reports its stress without a factor of safety.
  CHECK(tpu.yieldMPa == 0.0f);
  CHECK(tpu.metallic == 0.0f);
}

TEST_CASE("PLA is a material of its own, with its datasheet's numbers",
          "[materials]") {
  CHECK(MaterialLibrary::MaterialTypeFromString("pla") == MaterialType::PLA);
  CHECK(MaterialLibrary::Slug(MaterialType::PLA) == "pla");
  const MaterialProperties& pla =
      MaterialLibrary::GetMaterial(MaterialType::PLA);
  CHECK(pla.density == 1240.0f);
  CHECK(pla.youngGPa > 2.0f);
  CHECK(pla.yieldMPa > 0.0f);
  CHECK(glm::length(
            pla.baseColor -
            MaterialLibrary::GetMaterial(MaterialType::PETG).baseColor) > 0.1f);
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

TEST_CASE("A grade is its own row and prints as the grade",
          "[materials][drawing]") {
  // What a drawing orders: the parts list and the title block print the
  // grade, and a generic row its name, which says no grade was chosen.
  CHECK(MaterialLibrary::GradeOf("aluminum_6061_t6") == "6061-T6");
  CHECK(MaterialLibrary::GradeOf("aluminum_6082_t6") == "6082-T6");
  CHECK(MaterialLibrary::GradeOf("steel") == "STEEL");
  CHECK(MaterialLibrary::GradeOf("petg") == "PETG");
  CHECK(MaterialLibrary::GradeOf("").empty());
  // A grade carries its specification's minimum yield, and the two grades
  // share a density and a look.
  const MaterialProperties& g6082 =
      MaterialLibrary::GetMaterial(MaterialType::ALUMINUM_6082_T6);
  const MaterialProperties& g6061 =
      MaterialLibrary::GetMaterial(MaterialType::ALUMINUM_6061_T6);
  CHECK(g6082.yieldMPa == 240.0);
  CHECK(g6082.density == g6061.density);
  CHECK(g6082.baseColor == g6061.baseColor);
  // There is no generic aluminium: its conductivity and yield depend on the
  // alloy, so a document naming one is refused, naming the grades.
  MaterialType t{};
  CHECK_FALSE(MaterialLibrary::TryMaterialTypeFromString("aluminum", t));
  try {
    (void)MaterialLibrary::MaterialTypeFromString("aluminum");
    FAIL("generic aluminum accepted");
  } catch (const std::runtime_error& e) {
    CHECK(std::string(e.what()).find("aluminum_6061_t6") != std::string::npos);
  }
  // A consumer that knows only families (CAM's chiploads) reads it as
  // aluminium; every other slug is its own family.
  CHECK(MaterialLibrary::FamilySlug("aluminum_6061_t6") == "aluminum");
  CHECK(MaterialLibrary::FamilySlug("petg") == "petg");
}

// The building materials (v165): a house's mass and parts list mean
// something only when each has a density, and CAM says by name that no
// router cuts them rather than reading them as no material.
TEST_CASE("Building materials have a density, a look and a cut class",
          "[materials]") {
  using MT = MaterialType;
  const struct {
    MT type;
    const char* slug;
    double lo, hi; // the handbook range the density sits in, kg/m3
    bool stiff;    // carries a modulus for FEM
  } rows[] = {
      {MT::BRICK, "brick", 1800, 1900, true},
      {MT::ROOF_TILE, "roof_tile", 2100, 2400, true},
      {MT::PLASTER, "plaster", 1000, 1300, true},
      {MT::ROOFING_FELT, "roofing_felt", 1050, 1150, false},
      {MT::RENDER, "render", 1800, 2000, true},
      {MT::SOIL, "soil", 1500, 1800, false},
  };
  for (const auto& r : rows) {
    INFO(r.slug);
    CHECK(MaterialLibrary::Slug(r.type) == r.slug);
    CHECK(MaterialLibrary::MaterialTypeFromString(r.slug) == r.type);
    const MaterialProperties& m = MaterialLibrary::GetMaterial(r.type);
    CHECK(m.density >= r.lo);
    CHECK(m.density <= r.hi);
    CHECK(m.metallic == 0.0f);
    CHECK(m.roughness >= 0.7f); // matte, every one
    CHECK(m.pattern == SurfacePattern::Flat);
    CHECK((m.youngGPa > 0.0) == r.stiff);
    CHECK(m.yieldMPa == 0.0); // brittle or not a solid: no factor of safety
#ifdef HAVE_CAM // the feed table is CAM's; -DENABLE_CAM=OFF does not link it
    CHECK(cam::CutClassOf(r.slug) == cam::CutClass::NotCut);
    CHECK_FALSE(cam::BandFor(cam::CutClassOf(r.slug), 6.0).known);
#endif
  }
  // Brick reads red-brown, plaster pale, felt near black.
  const glm::vec3 brick = MaterialLibrary::GetMaterial(MT::BRICK).baseColor;
  CHECK(brick.r > brick.g);
  CHECK(brick.g > brick.b);
  CHECK(MaterialLibrary::GetMaterial(MT::PLASTER).baseColor.g > 0.6f);
  CHECK(MaterialLibrary::GetMaterial(MT::ROOFING_FELT).baseColor.r < 0.05f);
}
