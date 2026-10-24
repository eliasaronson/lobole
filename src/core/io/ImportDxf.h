// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Units.h"
#include "core/io/SketchImport.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// DXF in (docs/sketching/README.md#importing-dxf-and-svg): an ASCII DXF, R12
// to R2018, read into baked sketch entities. Ours, with no dependency — the
// format is code/value line pairs, as ExportDxf.cpp writes them — and
// numbers are read by std::from_chars, never through the locale.
//
// A file is read whole or refused whole: a plate missing one curve cuts
// wrong, so a refusal names the entity's kind and handle and imports
// nothing. Annotation (TEXT, DIMENSION, HATCH, ...) is not geometry, and is
// left out and listed rather than refused.
namespace dxfimport {

struct Options {
  // The unit the drawing is in when it declares none ($INSUNITS 0 or
  // absent). Refused when it contradicts a unit the file declares.
  std::optional<units::Length> units;
  // Only these layers (empty = all); a name the file does not have is
  // refused, listing the ones it does.
  std::vector<std::string> layers;
  // These come in as construction geometry (a flat pattern's BEND_* lines).
  std::vector<std::string> constructionLayers;
};

// Reads `path`. Throws std::runtime_error naming what it refused.
[[nodiscard]] sketchimport::Drawing Read(const std::string& path,
                                         const Options& options);
// The same, from the file's text (what the tests feed it).
[[nodiscard]] sketchimport::Drawing Parse(std::string_view text,
                                          const Options& options);

} // namespace dxfimport
