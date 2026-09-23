#include <catch2/catch_test_macros.hpp>

#include "render/StillOutlines.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

// The traced still's outlines
// (docs/rendering/still-render.md#outlines-on-a-traced-still). They were a
// binary 1 px staircase above ~2 Mpx, because the G-buffer they come from was
// traced whole and so could only afford 1x1 there. It is now 4x4 and traced in
// strips; these pin what that has to deliver.

namespace {

using stilloutline::Texel;

Texel Make(uint32_t id, glm::vec3 n, float dist = 100.0f) {
  Texel t{};
  t.nxy = glm::packHalf2x16(glm::vec2(n.x, n.y));
  t.nz = glm::packHalf2x16(glm::vec2(n.z, 0.0f));
  std::memcpy(&t.dist, &dist, sizeof(float));
  t.id = id;
  return t;
}

// A G-buffer of `ow` x `oh` output pixels at `scale`: body 1 below a
// diagonal (with a crease down the middle, so there is a feature edge as well
// as a silhouette), body 2 a block in the corner, sky elsewhere.
std::vector<Texel> Scene(uint32_t ow, uint32_t oh, uint32_t scale) {
  const uint32_t aw = ow * scale, ah = oh * scale;
  std::vector<Texel> g(static_cast<size_t>(aw) * ah, Make(0, glm::vec3(0)));
  for (uint32_t y = 0; y < ah; ++y)
    for (uint32_t x = 0; x < aw; ++x) {
      Texel& t = g[static_cast<size_t>(y) * aw + x];
      if (y > 0.37f * x + 0.2f * ah)
        t = Make(1, x < aw / 2 ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0));
      if (x > aw * 3 / 4 && y > ah * 3 / 4)
        t = Make(2, glm::vec3(0, 0, 1), 50.0f);
    }
  return g;
}

std::vector<float> Whole(const std::vector<Texel>& g, uint32_t ow, uint32_t oh,
                         uint32_t scale, float width) {
  std::vector<float> cov(static_cast<size_t>(ow) * oh);
  stilloutline::Coverage(g.data(), ow * scale, oh * scale, 0, oh * scale, scale,
                         width, 0, oh, cov.data());
  return cov;
}

} // namespace

TEST_CASE("Still outlines: a diagonal edge comes out antialiased",
          "[render][outline]") {
  const uint32_t ow = 64, oh = 48, s = 4;
  const std::vector<float> cov = Whole(Scene(ow, oh, s), ow, oh, s, 1.0f);
  std::set<int> levels;
  for (float c : cov) {
    REQUIRE(c >= 0.0f);
    REQUIRE(c <= 1.0f + 1e-5f);
    if (c > 0.0f)
      levels.insert(static_cast<int>(std::lround(c * 64.0f)));
  }
  // A binary mask has one level. 4x4 gives up to 16 partial ones.
  CHECK(levels.size() > 4);
}

TEST_CASE("Still outlines: a line is as wide as it was asked to be",
          "[render][outline]") {
  // A horizontal edge between two bodies, away from the block boundaries.
  const uint32_t ow = 16, oh = 16, s = 4;
  for (float width : {1.0f, 1.5f, 2.0f, 3.0f}) {
    std::vector<Texel> g(static_cast<size_t>(ow * s) * oh * s);
    for (uint32_t y = 0; y < oh * s; ++y)
      for (uint32_t x = 0; x < ow * s; ++x)
        g[static_cast<size_t>(y) * ow * s + x] =
            Make(y < 31 ? 1u : 2u, glm::vec3(0, 0, 1));
    const std::vector<float> cov = Whole(g, ow, oh, s, width);
    float column = 0.0f; // summed down one column: the line's width in px
    for (uint32_t y = 0; y < oh; ++y)
      column += cov[static_cast<size_t>(y) * ow + ow / 2];
    INFO("width " << width << " px drew " << column << " px");
    CHECK(std::abs(column - width) < 0.01f);
  }
}

TEST_CASE("Still outlines: strips compute exactly the whole-image answer",
          "[render][outline]") {
  const uint32_t ow = 64, oh = 48, s = 4;
  const std::vector<Texel> g = Scene(ow, oh, s);
  for (float width : {1.0f, 1.5f, 2.5f}) {
    const std::vector<float> whole = Whole(g, ow, oh, s, width);
    const uint32_t halo =
        static_cast<uint32_t>(stilloutline::HaloRows(width, s));
    for (uint32_t perStrip : {1u, 3u, 7u}) {
      std::vector<float> strips(whole.size(), -1.0f);
      for (uint32_t out0 = 0; out0 < oh; out0 += perStrip) {
        const uint32_t out1 = std::min(oh, out0 + perStrip);
        const uint32_t row0 = out0 * s > halo ? out0 * s - halo : 0;
        const uint32_t row1 = std::min(oh * s, out1 * s + halo);
        stilloutline::Coverage(g.data() + static_cast<size_t>(row0) * ow * s,
                               ow * s, oh * s, row0, row1 - row0, s, width,
                               out0, out1,
                               strips.data() + static_cast<size_t>(out0) * ow);
      }
      INFO("width " << width << ", " << perStrip << " rows a strip");
      CHECK(strips == whole);
    }
  }
}
