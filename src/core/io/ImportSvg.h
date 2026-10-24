// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/io/SketchImport.h"

#include <optional>
#include <string>
#include <vector>

// SVG in (docs/sketching/README.md#importing-dxf-and-svg): the outlines of an
// SVG, read with OpenCASCADE's LDOMParser (nothing vendored), as baked sketch
// entities. Every `transform` is composed with a final Y flip (SVG is
// Y-down), and an affine image of a conic stays a conic, so a skewed circle is
// an Ellipse and its arc an EllipseArc — exact, never tessellated.
//
// Read whole or refused whole, naming the element: `clip-path` and `mask`
// (the cut would not be the picture) and any element that is neither drawn
// nor known to draw nothing. `text` and `image` are left out and listed, as
// are hidden elements.
namespace svgimport {

// What one user unit is when the root's width/height carry no absolute
// unit: "mm", "cm", "m", "in", "px", "pt". Empty = px (1/96 in), as the spec
// defines. Refused when the root fixes the scale itself.
struct Options {
  std::string units;
};

// "mm, cm, m, in, px, pt", for a refusal's text.
[[nodiscard]] const char* AcceptedUnits();

// Reads `path`. Throws std::runtime_error naming what it refused.
[[nodiscard]] sketchimport::Drawing Read(const std::string& path,
                                         const Options& options);

} // namespace svgimport
