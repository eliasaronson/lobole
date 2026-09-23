#include "render/StillOutlines.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace stilloutline {

Pen PenFor(float lineWidthPx, uint32_t scale) {
  // A line `w` texels across, centred on the marked texel: `core` texels
  // either side are covered, and whatever is left of the width is a fringe
  // ring at partial weight. So a 1 px line at 4x is 4 texels, not 3 or 5 --
  // an integer box could only step the width by half a pixel at a time.
  const float w = std::max(1.0f, lineWidthPx * static_cast<float>(scale));
  const float half = (w - 1.0f) * 0.5f;
  Pen pen;
  pen.core = static_cast<int>(std::floor(half));
  pen.fringe = half - static_cast<float>(pen.core);
  if (pen.fringe < 1e-3f)
    pen.fringe = 0.0f;
  return pen;
}

int HaloRows(float lineWidthPx, uint32_t scale) {
  // The pen reaches `reach` rows, and a texel's edge mark reads the row on
  // either side of it.
  return PenFor(lineWidthPx, scale).reach() + 1;
}

void Coverage(const Texel* strip, uint32_t auxWidth, uint32_t auxHeight,
              uint32_t stripRow0, uint32_t stripRows, uint32_t scale,
              float lineWidthPx, uint32_t outRow0, uint32_t outRow1,
              float* out) {
  (void)auxHeight; // the strip is already clipped to the image
  const uint32_t aw = auxWidth;
  const size_t count = static_cast<size_t>(aw) * stripRows;

  std::vector<glm::vec3> nrm(count);
  std::vector<float> dist(count);
  std::vector<uint32_t> ids(count);
  for (size_t i = 0; i < count; ++i) {
    const glm::vec2 nxy = glm::unpackHalf2x16(strip[i].nxy);
    const glm::vec2 nz = glm::unpackHalf2x16(strip[i].nz);
    nrm[i] = glm::vec3(nxy.x, nxy.y, nz.x);
    std::memcpy(&dist[i], &strip[i].dist, sizeof(float));
    ids[i] = strip[i].id;
  }

  constexpr float kFeatureCos = 0.5f;
  auto separated = [&](size_t a, size_t b) {
    if (ids[a] != ids[b])
      return true;
    if (ids[a] == 0u) // both background: nothing to outline
      return false;
    // No depth test. A surface seen at a grazing angle changes distance fast
    // between neighbouring texels, so any threshold loose enough to ignore
    // that is too loose to catch the self-occlusion it would be for -- it
    // drew three times the lines the viewport does.
    return glm::dot(nrm[a], nrm[b]) < kFeatureCos;
  };
  // Mark the NEARER texel of a differing pair, not both: marking both doubles
  // every line, and putting it on the front surface keeps a silhouette on the
  // body instead of bleeding into the sky behind it.
  auto nearer = [&](size_t a, size_t b) {
    if (ids[a] == 0u)
      return b;
    if (ids[b] == 0u)
      return a;
    return dist[a] <= dist[b] ? a : b;
  };
  std::vector<uint8_t> mark(count, 0);
  for (uint32_t y = 0; y < stripRows; ++y)
    for (uint32_t x = 0; x < aw; ++x) {
      const size_t i = static_cast<size_t>(y) * aw + x;
      if (x + 1 < aw && separated(i, i + 1))
        mark[nearer(i, i + 1)] = 1;
      if (y + 1 < stripRows && separated(i, i + aw))
        mark[nearer(i, i + aw)] = 1;
    }

  // Chebyshev distance to the nearest mark, capped past the pen's reach:
  // along each row first (two sweeps), then down the columns.
  const Pen pen = PenFor(lineWidthPx, scale);
  const int reach = pen.reach();
  const uint8_t beyond = static_cast<uint8_t>(reach + 1);
  std::vector<uint8_t> across(count, beyond);
  for (uint32_t y = 0; y < stripRows; ++y) {
    const size_t row = static_cast<size_t>(y) * aw;
    uint8_t d = beyond;
    for (uint32_t x = 0; x < aw; ++x) {
      d = mark[row + x] ? 0
                        : static_cast<uint8_t>(std::min<int>(beyond, d + 1));
      across[row + x] = d;
    }
    d = beyond;
    for (uint32_t x = aw; x-- > 0;) {
      d = mark[row + x] ? 0
                        : static_cast<uint8_t>(std::min<int>(beyond, d + 1));
      across[row + x] = std::min(across[row + x], d);
    }
  }

  const uint32_t ow = aw / scale;
  const float inv = 1.0f / static_cast<float>(scale * scale);
  std::fill(out, out + static_cast<size_t>(outRow1 - outRow0) * ow, 0.0f);
  for (uint32_t oy = outRow0; oy < outRow1; ++oy) {
    float* dst = out + static_cast<size_t>(oy - outRow0) * ow;
    for (uint32_t sy = 0; sy < scale; ++sy) {
      const int y = static_cast<int>(oy * scale + sy - stripRow0);
      const int y0 = std::max(0, y - reach);
      const int y1 = std::min(static_cast<int>(stripRows) - 1, y + reach);
      for (uint32_t x = 0; x < aw; ++x) {
        int d = beyond;
        for (int yy = y0; yy <= y1; ++yy)
          d = std::min(d,
                       std::max(std::abs(yy - y),
                                static_cast<int>(
                                    across[static_cast<size_t>(yy) * aw + x])));
        const float w = d <= pen.core                             ? 1.0f
                        : (d == pen.core + 1 && pen.fringe > 0.f) ? pen.fringe
                                                                  : 0.0f;
        dst[x / scale] += w * inv;
      }
    }
  }
}

} // namespace stilloutline
