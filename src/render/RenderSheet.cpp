// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "render/RenderSheet.h"

#include "common/Logger.h"
#include "thirdparty/stb_image.h"
#include "thirdparty/stb_image_write.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>

namespace rendersheet {
namespace {

// 5x7 bitmap glyphs, authored as ASCII art (# = lit) so a wrong pixel is
// visible in the source. Uppercase-only; Draw() upcases its input. Enough
// vocabulary for digest footers ("VOL 1234.5 MM3", "2 X 3 = 6") — extend
// here when a new label needs a new character.
struct Glyph {
  const char* rows[7];
};
const std::map<char, Glyph>& Font() {
  static const std::map<char, Glyph> font = {
      {'0', {{" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}}},
      {'1', {{"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}}},
      {'2', {{" ### ", "#   #", "    #", "  ## ", " #   ", "#    ", "#####"}}},
      {'3', {{" ### ", "#   #", "    #", "  ## ", "    #", "#   #", " ### "}}},
      {'4', {{"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}}},
      {'5', {{"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}}},
      {'6', {{" ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "}}},
      {'7', {{"#####", "    #", "   # ", "  #  ", "  #  ", "  #  ", "  #  "}}},
      {'8', {{" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}}},
      {'9', {{" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "}}},
      {'A', {{" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}}},
      {'B', {{"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}}},
      {'C', {{" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "}}},
      {'D', {{"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}}},
      {'E', {{"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}}},
      {'F', {{"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}}},
      {'G', {{" ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ### "}}},
      {'H', {{"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}}},
      {'I', {{" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}}},
      {'J', {{"  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "}}},
      {'K', {{"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}}},
      {'L', {{"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}}},
      {'M', {{"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"}}},
      {'N', {{"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"}}},
      {'O', {{" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}}},
      {'P', {{"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}}},
      {'Q', {{" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}}},
      {'R', {{"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}}},
      {'S', {{" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}}},
      {'T', {{"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}}},
      {'U', {{"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}}},
      {'V', {{"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}}},
      {'W', {{"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"}}},
      {'X', {{"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}}},
      {'Y', {{"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}}},
      {'Z', {{"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}}},
      {' ', {{"     ", "     ", "     ", "     ", "     ", "     ", "     "}}},
      {'.', {{"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}}},
      {',', {{"     ", "     ", "     ", "     ", " ##  ", " ##  ", " #   "}}},
      {':', {{"     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "}}},
      {'(', {{"  #  ", " #   ", "#    ", "#    ", "#    ", " #   ", "  #  "}}},
      {')', {{"  #  ", "   # ", "    #", "    #", "    #", "   # ", "  #  "}}},
      {'=', {{"     ", "     ", "#####", "     ", "#####", "     ", "     "}}},
      {'-', {{"     ", "     ", "     ", "#####", "     ", "     ", "     "}}},
      {'+', {{"     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "}}},
      {'/', {{"    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "}}},
      {'%', {{"##   ", "##  #", "   # ", "  #  ", " #   ", "#  ##", "   ##"}}},
  };
  return font;
}

struct Canvas {
  std::vector<unsigned char> px; // RGBA
  int w = 0, h = 0;
  void Fill(unsigned char r, unsigned char g, unsigned char b) {
    for (int i = 0; i < w * h; i++) {
      px[i * 4 + 0] = r;
      px[i * 4 + 1] = g;
      px[i * 4 + 2] = b;
      px[i * 4 + 3] = 255;
    }
  }
};

// Draws `text` at (x, y) with per-glyph 5x7 cells scaled by `scale` (+1px
// tracking). Unknown characters render as blanks rather than failing a
// whole sheet over one label.
void DrawText(Canvas& c, int x, int y, const std::string& text, int scale,
              unsigned char r, unsigned char g, unsigned char b) {
  const auto& font = Font();
  int penX = x;
  for (char raw : text) {
    const char ch =
        static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
    const auto it = font.find(ch);
    if (it != font.end()) {
      for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++) {
          if (it->second.rows[row][col] != '#')
            continue;
          for (int sy = 0; sy < scale; sy++)
            for (int sx = 0; sx < scale; sx++) {
              const int px = penX + col * scale + sx;
              const int py = y + row * scale + sy;
              if (px < 0 || py < 0 || px >= c.w || py >= c.h)
                continue;
              unsigned char* p = &c.px[(py * c.w + px) * 4];
              p[0] = r;
              p[1] = g;
              p[2] = b;
            }
        }
    }
    penX += (5 + 1) * scale;
  }
}

} // namespace

bool Composite(const Spec& spec, const std::string& outPath,
               std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error)
      *error = why;
    return false;
  };
  if (spec.tilePaths.empty())
    return fail("render sheet has no tiles");

  struct Tile {
    std::unique_ptr<unsigned char, void (*)(unsigned char*)> data{
        nullptr, [](unsigned char* p) { stbi_image_free(p); }};
    int w = 0, h = 0;
  };
  std::vector<Tile> tiles;
  for (const std::string& path : spec.tilePaths) {
    Tile t;
    int comp = 0;
    t.data.reset(stbi_load(path.c_str(), &t.w, &t.h, &comp, 4));
    if (!t.data)
      return fail("cannot read tile " + path);
    tiles.push_back(std::move(t));
  }

  // Uniform crop (all tiles come from the same window). Clamp to the tile
  // so a stale viewport rect degrades to a smaller crop, not a crash.
  int cx = spec.cropX, cy = spec.cropY, cw = spec.cropW, ch = spec.cropH;
  if (cw <= 0 || ch <= 0) {
    cx = cy = 0;
    cw = tiles[0].w;
    ch = tiles[0].h;
  }
  cx = std::clamp(cx, 0, tiles[0].w - 1);
  cy = std::clamp(cy, 0, tiles[0].h - 1);
  cw = std::clamp(cw, 1, tiles[0].w - cx);
  ch = std::clamp(ch, 1, tiles[0].h - cy);

  const int cols =
      std::max(1, std::min<int>(spec.columns, static_cast<int>(tiles.size())));
  const int rows = (static_cast<int>(tiles.size()) + cols - 1) / cols;
  const int gap = 4;
  const int fontScale = 2;
  const int lineH = 7 * fontScale + 6;
  const int footerH = spec.footer.empty()
                          ? 0
                          : static_cast<int>(spec.footer.size()) * lineH + 10;

  Canvas sheet;
  sheet.w = cols * cw + (cols + 1) * gap;
  sheet.h = rows * ch + (rows + 1) * gap + footerH;
  sheet.px.resize(static_cast<size_t>(sheet.w) * sheet.h * 4);
  sheet.Fill(24, 26, 30); // dark ground, matches the viewport

  for (size_t i = 0; i < tiles.size(); i++) {
    const int gx = gap + static_cast<int>(i % cols) * (cw + gap);
    const int gy = gap + static_cast<int>(i / cols) * (ch + gap);
    const Tile& t = tiles[i];
    const int copyW = std::min(cw, t.w - cx);
    const int copyH = std::min(ch, t.h - cy);
    for (int y = 0; y < copyH; y++)
      std::memcpy(&sheet.px[((gy + y) * sheet.w + gx) * 4],
                  t.data.get() + ((cy + y) * t.w + cx) * 4,
                  static_cast<size_t>(copyW) * 4);
    if (i < spec.labels.size())
      DrawText(sheet, gx + 6, gy + 6, spec.labels[i], fontScale, 255, 255, 255);
  }

  int y = rows * ch + (rows + 1) * gap + 6;
  for (const std::string& line : spec.footer) {
    DrawText(sheet, gap + 2, y, line, fontScale, 220, 220, 220);
    y += lineH;
  }

  if (!stbi_write_png(outPath.c_str(), sheet.w, sheet.h, 4, sheet.px.data(),
                      sheet.w * 4))
    return fail("cannot write " + outPath);
  return true;
}

} // namespace rendersheet
