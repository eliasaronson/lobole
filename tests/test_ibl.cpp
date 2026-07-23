#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "render/IBL.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

static bool Finite(const glm::vec4& c) {
  return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z);
}

TEST_CASE("IBL precompute produces sane maps from the studio HDR", "[ibl]") {
  const std::string hdr =
      std::string(DESIGNER_SOURCE_DIR) + "/assets/hdri/brown_photostudio_02_1k.hdr";
  if (!fs::exists(hdr)) {
    WARN("HDR asset missing, skipping: " << hdr);
    return;
  }

  IBLPrecompute ibl;
  // Empty cache dir → always recompute (deterministic, no stale cache).
  REQUIRE(ibl.Build(hdr, ""));

  // Irradiance: right size, all finite, non-negative, and non-trivially bright
  // (a lit studio can't integrate to black everywhere).
  const auto& irr = ibl.Irradiance();
  REQUIRE(irr.width > 0);
  REQUIRE(irr.height > 0);
  REQUIRE(irr.pixels.size() == size_t(irr.width) * irr.height);
  double irrSum = 0.0;
  for (const auto& p : irr.pixels) {
    REQUIRE(Finite(p));
    CHECK(p.x >= 0.0f);
    CHECK(p.y >= 0.0f);
    CHECK(p.z >= 0.0f);
    irrSum += (p.x + p.y + p.z);
  }
  CHECK(irrSum > 0.0);

  // Prefilter: a mip chain with strictly decreasing dimensions, all finite.
  const auto& pf = ibl.Prefilter();
  REQUIRE(pf.size() >= 2);
  for (size_t i = 1; i < pf.size(); i++) {
    CHECK(pf[i].width <= pf[i - 1].width);
    CHECK(pf[i].height <= pf[i - 1].height);
  }
  for (const auto& m : pf)
    for (const auto& p : m.pixels) REQUIRE(Finite(p));

  // BRDF LUT: the split-sum integral is a pure function of (NdotV, roughness),
  // independent of the environment. Check the known corner: smooth grazing view
  // (high NdotV, low roughness) → scale≈1, bias≈0.
  const auto& brdf = ibl.BrdfLut();
  REQUIRE(brdf.width > 0);
  const auto& corner =
      brdf.pixels[size_t(0) * brdf.width + (brdf.width - 1)]; // row 0, last col
  CHECK_THAT(corner.x, WithinAbs(1.0f, 0.05f)); // scale (A)
  CHECK_THAT(corner.y, WithinAbs(0.0f, 0.05f)); // bias (B)
  for (const auto& p : brdf.pixels) {
    CHECK(p.x >= -0.01f);
    CHECK(p.x <= 1.01f);
    CHECK(p.y >= -0.01f);
    CHECK(p.y <= 1.01f);
  }

  // Optional: dump maps for manual inspection when DESIGNER_IBL_DUMP is set.
  if (const char* dir = std::getenv("DESIGNER_IBL_DUMP")) {
    ibl.DumpDebugPNGs(dir);
  }
}
