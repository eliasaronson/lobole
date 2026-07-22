#include <catch2/catch_test_macros.hpp>

#include "CADMaterials.h"

#include <algorithm>
#include <set>
#include <string>

using CADMaterials::MaterialLibrary;
using CADMaterials::MaterialProperties;
using CADMaterials::MaterialType;

// The set of procedural-textured materials. procPattern must be 1 (solid-wood
// grain) or 2 (plywood); every other material stays flat (pattern 0). MDF is a
// wood product but is deliberately flat (near-uniform in reality).
static bool IsGrainWood(MaterialType t) {
  return t == MaterialType::WOOD_OAK || t == MaterialType::WOOD_PINE ||
         t == MaterialType::WOOD_WALNUT;
}
static bool IsPlywood(MaterialType t) {
  return t == MaterialType::PLYWOOD_BIRCH || t == MaterialType::PLYWOOD_PINE;
}

TEST_CASE("Slug round-trips for every assignable material", "[materials]") {
  // FromSlug(Slug(t)) == t for all UI-assignable types: a mismatch means a
  // saved file's material token would resolve to the wrong (or default)
  // material on load.
  for (MaterialType t : MaterialLibrary::AllTypes()) {
    const std::string slug = MaterialLibrary::Slug(t);
    CHECK(!slug.empty());
    CHECK(MaterialLibrary::FromSlug(slug) == t);
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
  CHECK(MaterialLibrary::FromSlug("wood_oak") == MaterialType::WOOD_OAK);
  CHECK(MaterialLibrary::FromSlug("wood_pine") == MaterialType::WOOD_PINE);
  CHECK(MaterialLibrary::FromSlug("wood_walnut") == MaterialType::WOOD_WALNUT);
  CHECK(MaterialLibrary::FromSlug("plywood_birch") ==
        MaterialType::PLYWOOD_BIRCH);
  CHECK(MaterialLibrary::FromSlug("plywood_pine") == MaterialType::PLYWOOD_PINE);
  CHECK(MaterialLibrary::FromSlug("mdf") == MaterialType::MDF);
}

TEST_CASE("Unknown and empty slugs degrade to DEFAULT", "[materials]") {
  // Back-compat: pre-wood files (and any future/unknown token) must load rather
  // than crash or resolve arbitrarily.
  CHECK(MaterialLibrary::FromSlug("") == MaterialType::DEFAULT);
  CHECK(MaterialLibrary::FromSlug("not_a_material") == MaterialType::DEFAULT);
  CHECK(MaterialLibrary::Slug(MaterialType::DEFAULT).empty());
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

TEST_CASE("Procedural pattern is wired only on wood/plywood", "[materials]") {
  // The shader keys the grain off procPattern; if a metal accidentally carried
  // pattern > 0 it would render grain, and if a wood carried 0 it would render
  // flat. Assert the exact partition, and that a pattern implies usable params.
  for (MaterialType t : MaterialLibrary::AllTypes()) {
    const MaterialProperties& m = MaterialLibrary::GetMaterial(t);
    if (IsGrainWood(t)) {
      CHECK(m.procPattern == 1);
    } else if (IsPlywood(t)) {
      CHECK(m.procPattern == 2);
    } else {
      CHECK(m.procPattern == 0); // includes MDF and every non-wood material
    }

    if (m.procPattern > 0) {
      // A pattern with no scale would collapse the grain to a constant.
      CHECK(m.grainScale > 0.0f);
      CHECK(m.grainContrast > 0.0f);
      CHECK(m.grainContrast <= 1.0f);
    } else {
      CHECK(m.grainScale == 0.0f);
      CHECK(m.grainContrast == 0.0f);
    }
  }
}

TEST_CASE("Wood materials are physically usable", "[materials]") {
  // Non-metallic, in-gamut base color, and a real density (so mass_properties
  // reports a mass) plus stiffness (so an FEM study runs rather than erroring).
  const MaterialType woods[] = {
      MaterialType::WOOD_OAK,       MaterialType::WOOD_PINE,
      MaterialType::WOOD_WALNUT,    MaterialType::PLYWOOD_BIRCH,
      MaterialType::PLYWOOD_PINE,   MaterialType::MDF};
  for (MaterialType t : woods) {
    const MaterialProperties& m = MaterialLibrary::GetMaterial(t);
    CHECK(m.metallic == 0.0f);
    for (int c = 0; c < 3; c++) {
      CHECK(m.baseColor[c] >= 0.0f);
      CHECK(m.baseColor[c] <= 1.0f);
    }
    CHECK(m.density > 0.0f);   // air-dry wood/board density
    CHECK(m.youngGPa > 0.0f);  // representative along-grain modulus (isotropic)
    CHECK(m.poisson > 0.0f);
    CHECK(m.poisson < 0.5f);   // physically valid Poisson ratio
  }
}
