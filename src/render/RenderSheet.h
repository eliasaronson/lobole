// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

// V3 render_sheet: composites per-view screenshot tiles into ONE image with
// a numeric footer — the agent loop's "many views + kernel numbers in the
// same picture" primitive. Pure CPU (stb_image in/out); the tiles are
// ordinary PNGs the screenshot path already writes.
namespace rendersheet {

struct Spec {
  std::vector<std::string> tilePaths; // one PNG per view, capture order
  std::vector<std::string> labels;    // per-tile view label (top-left)
  std::vector<std::string> footer;    // digest lines under the grid
  // Crop rect applied to every tile (the 3D viewport region, so panel
  // chrome stays out of the sheet). w/h 0 = use the full tile.
  int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
  int columns = 2;
};

// Writes the composited sheet to outPath (PNG). False + *error on failure
// (unreadable tile, mismatched sizes, unwritable output).
bool Composite(const Spec& spec, const std::string& outPath,
               std::string* error);

} // namespace rendersheet
