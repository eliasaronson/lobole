// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "render/MeshRenderer.h"

using Catch::Matchers::WithinAbs;

namespace {

// uv the shader would sample for a fragment at framebuffer coordinate p.
glm::vec2 UVAt(const glm::vec4& map, glm::vec2 p) {
  return p * glm::vec2(map.x, map.y) + glm::vec2(map.z, map.w);
}

} // namespace

// The scene is drawn into the 3D viewport — a SUB-RECT of the framebuffer,
// because the panels take the rest — while a screen-space prepass (SSAO)
// draws the same projection across the whole of its own target. A lookup
// that divides gl_FragCoord by the FRAMEBUFFER size therefore reads the wrong
// texel: on a 1280x720 window with the viewport at (0,44) 1280x640 it was off
// by 44 px and stretched 12.5% vertically, painting displaced copies of the
// railings onto a flat deck.
TEST_CASE("Screen-space lookups map the viewport rect, not the framebuffer",
          "[render][screenspace]") {
  SECTION("the viewport's corners are the target's corners") {
    // The exact layout the bug was found on.
    const glm::vec4 map = ViewportToTargetUV(0, 44, 1280, 640);
    const glm::vec2 topLeft = UVAt(map, glm::vec2(0.0f, 44.0f));
    const glm::vec2 bottomRight = UVAt(map, glm::vec2(1280.0f, 684.0f));
    CHECK_THAT(topLeft.x, WithinAbs(0.0, 1e-6));
    CHECK_THAT(topLeft.y, WithinAbs(0.0, 1e-6));
    CHECK_THAT(bottomRight.x, WithinAbs(1.0, 1e-6));
    CHECK_THAT(bottomRight.y, WithinAbs(1.0, 1e-6));
  }

  SECTION("a fragment's uv does not depend on where the viewport sits") {
    // Same viewport SIZE, three different positions (docked left, docked
    // right, floating): the same point *within* the viewport must sample the
    // same texel. This is the invariant the old code broke, and the one that
    // catches the whole family of sub-rectangle bugs.
    const glm::vec2 within(0.3f, 0.7f); // fraction of the viewport
    float first = -1.0f;
    for (const glm::ivec2 origin :
         {glm::ivec2(0, 44), glm::ivec2(320, 44), glm::ivec2(96, 200)}) {
      const glm::vec4 map = ViewportToTargetUV(origin.x, origin.y, 900, 500);
      const glm::vec2 uv =
          UVAt(map, glm::vec2(origin) + within * glm::vec2(900.0f, 500.0f));
      CHECK_THAT(uv.x, WithinAbs(within.x, 1e-6));
      CHECK_THAT(uv.y, WithinAbs(within.y, 1e-6));
      if (first < 0.0f)
        first = uv.x;
      CHECK_THAT(uv.x, WithinAbs(first, 1e-6));
    }
  }

  SECTION("a full-window viewport is the identity divide") {
    // The case that hid the bug on a maximized/undocked window — and why it
    // survived every screenshot review until a model with tall geometry
    // (railings) made the displacement obvious.
    const glm::vec4 map = ViewportToTargetUV(0, 0, 1920, 1080);
    const glm::vec2 uv = UVAt(map, glm::vec2(960.0f, 540.0f));
    CHECK_THAT(uv.x, WithinAbs(0.5, 1e-6));
    CHECK_THAT(uv.y, WithinAbs(0.5, 1e-6));
  }

  SECTION("a degenerate viewport does not divide by zero") {
    const glm::vec4 map = ViewportToTargetUV(0, 0, 0, 0);
    const glm::vec2 uv = UVAt(map, glm::vec2(0.0f, 0.0f));
    CHECK(std::isfinite(uv.x));
    CHECK(std::isfinite(uv.y));
  }
}
