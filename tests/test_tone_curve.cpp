// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The display transform (render/ToneCurve.h) and its GPU copy
// (src/shaders/tonecurve.glsl): the viewport and the path-traced still must
// turn one radiance into one byte, and that byte must be the sRGB encode of
// what the exposure-corrected ACES fit gives.

#include "TestPaths.h"
#include "render/ToneCurve.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

namespace {

std::string ReadSource(const std::string& rel) {
  std::ifstream f(std::filesystem::path(TestSourceDir()) / rel);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

} // namespace

TEST_CASE("tone curve: the shader's constants are the CPU's",
          "[render][tone]") {
  const std::string glsl = ReadSource("src/shaders/tonecurve.glsl");
  REQUIRE_FALSE(glsl.empty());
  std::map<std::string, float> shader;
  const std::regex decl(R"(const\s+float\s+(\w+)\s*=\s*([-+0-9.eE]+)\s*;)");
  for (auto it = std::sregex_iterator(glsl.begin(), glsl.end(), decl);
       it != std::sregex_iterator(); ++it)
    shader[(*it)[1].str()] = std::stof((*it)[2].str());

  // Every constant in both directions: one added to the shader and not to
  // kShaderConstants is a curve the still does not apply.
  CHECK(shader.size() == std::size(tone::kShaderConstants));
  for (const tone::ShaderConstant& c : tone::kShaderConstants) {
    INFO(c.name);
    REQUIRE(shader.count(c.name) == 1);
    CHECK(shader[c.name] == c.value);
  }

  // And mesh.frag applies it, with no second curve after it.
  const std::string frag = ReadSource("src/shaders/mesh.frag");
  CHECK(frag.find("#include \"tonecurve.glsl\"") != std::string::npos);
  CHECK(frag.find("toneToDisplay(") != std::string::npos);
  CHECK(frag.find("2.2") == std::string::npos);
}

TEST_CASE("tone curve: sRGB encode, exposure-corrected ACES",
          "[render][tone]") {
  using tone::ToByte;
  // The encode alone is sRGB's: linear 0.2 is byte 124, 0.5 is 188, and the
  // toe is the linear segment.
  CHECK(ToByte(tone::SrgbEncode(0.2f)) == 124);
  CHECK(ToByte(tone::SrgbEncode(0.5f)) == 188);
  CHECK(ToByte(tone::SrgbEncode(1.0f)) == 255);
  CHECK_THAT(tone::SrgbEncode(0.001f),
             Catch::Matchers::WithinAbs(0.01292, 1e-6));

  // The whole curve at unit radiance: a linear 0.2 card lands on 112 — the
  // filmic toe sits a little under the plain encode's 124. The old curve
  // (no exposure, pow 1/2.2, x1.1) put it at 162.
  CHECK(ToByte(tone::ToDisplay(0.2f)) == 112);
  CHECK(ToByte(tone::ToDisplay(0.0f)) == 0);
  // Monotone, and a shoulder rather than a clip.
  float prev = -1.0f;
  for (float r = 0.0f; r <= 8.0f; r += 0.05f) {
    const float v = tone::ToDisplay(r);
    CHECK(v >= prev);
    prev = v;
  }
  CHECK(tone::ToDisplay(4.0f) < 1.0f);
}
