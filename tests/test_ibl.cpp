// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TestPaths.h"
#include "common/RuntimePaths.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "render/IBL.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

static bool Finite(const glm::vec4& c) {
  return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z);
}

TEST_CASE("IBL precompute produces sane maps from the studio HDR", "[ibl]") {
  const std::string hdr = std::string(TestSourceDir()) + "/assets/hdri/" +
                          std::string(RuntimePaths::kEnvironmentHdriFile);
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
    for (const auto& p : m.pixels)
      REQUIRE(Finite(p));

  // BRDF LUT: the split-sum integral is a pure function of (NdotV, roughness),
  // independent of the environment. Check the known corner: smooth grazing view
  // (high NdotV, low roughness) → scale≈1, bias≈0.
  const auto& brdf = ibl.BrdfLut();
  REQUIRE(brdf.width > 0);
  const auto& corner =
      brdf.pixels[size_t(0) * brdf.width + (brdf.width - 1)]; // row 0, last col
  CHECK_THAT(corner.x, WithinAbs(1.0f, 0.05f));               // scale (A)
  CHECK_THAT(corner.y, WithinAbs(0.0f, 0.05f));               // bias (B)
  for (const auto& p : brdf.pixels) {
    CHECK(p.x >= -0.01f);
    CHECK(p.x <= 1.01f);
    CHECK(p.y >= -0.01f);
    CHECK(p.y <= 1.01f);
  }

  // Optional: dump maps for manual inspection when LOBOLE_IBL_DUMP is set.
  if (const char* dir = std::getenv("LOBOLE_IBL_DUMP")) {
    ibl.DumpDebugPNGs(dir);
  }
}

namespace {
// IBL.cpp's DirToEquirectUV (world Z-up -> the HDRI's Y-polar map), restated
// so the test can find a direction's pixel.
glm::vec4 SkyAt(const IBLPrecompute::Image& img, const glm::vec3& world) {
  const glm::vec3 d = glm::normalize(world);
  const glm::vec3 e(d.x, d.z, -d.y);
  const float u = std::atan2(e.z, e.x) / (2.0f * 3.14159265f) + 0.5f;
  const float v = std::acos(std::clamp(e.y, -1.0f, 1.0f)) / 3.14159265f;
  const int x = std::clamp(int(u * img.width), 0, img.width - 1);
  const int y = std::clamp(int(v * img.height), 0, img.height - 1);
  return img.pixels[size_t(y) * img.width + x];
}
float Luma(const glm::vec4& c) {
  return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}
} // namespace

// The procedural sky behind and over a `backdrop: sky` still
// (docs/rendering/still-render.md#the-sky): blue overhead, ground below the
// horizon, the sun where it was asked to stand — and it lights from that side
// once convolved.
TEST_CASE("Procedural sky: gradient, ground, sun, and it lights", "[ibl]") {
  const float el = glm::radians(40.0f);
  const glm::vec3 sun(std::cos(el), 0.0f, std::sin(el)); // toward +X
  const IBLPrecompute::Image sky = IBLPrecompute::GenerateSky(sun, 512, 256);
  REQUIRE(sky.pixels.size() == size_t(512) * 256);
  for (const auto& p : sky.pixels) {
    REQUIRE(Finite(p));
    REQUIRE(p.x >= 0.0f);
  }
  // Deterministic: a sky is a function of its sun.
  CHECK(IBLPrecompute::GenerateSky(sun, 512, 256).pixels == sky.pixels);

  const glm::vec4 zenith = SkyAt(sky, {0, 0, 1});
  CHECK(zenith.z > 2.0f * zenith.x); // blue overhead
  const glm::vec4 horizon = SkyAt(sky, {0, 1, 0.02f});
  CHECK(horizon.x > zenith.x); // paler at the horizon
  const glm::vec4 ground = SkyAt(sky, {0, 0.3f, -1});
  CHECK(ground.x >= ground.z);                         // not sky: a warm grey
  CHECK(Luma(SkyAt(sky, sun)) > 10.0f * Luma(zenith)); // the disc
  CHECK(Luma(SkyAt(sky, {-sun.x, 0, sun.z})) < Luma(SkyAt(sky, sun)) / 10.0f);

  IBLPrecompute ibl;
  ibl.BuildFromImage(sky);
  REQUIRE(ibl.PrefilterMipCount() > 0);
  // A face toward the sun gathers more light than one turned away from it,
  // and an upward face more than a downward one. Only somewhat more: the
  // sun's direct light is the still's KEY light, not the map's.
  CHECK(Luma(SkyAt(ibl.Irradiance(), {1, 0, 0})) >
        1.1f * Luma(SkyAt(ibl.Irradiance(), {-1, 0, 0})));
  CHECK(Luma(SkyAt(ibl.Irradiance(), {0, 0, 1})) >
        Luma(SkyAt(ibl.Irradiance(), {0, 0, -1})));
}
