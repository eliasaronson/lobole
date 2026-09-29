#pragma once

// The text sketch entity's geometry (docs/sketching/README.md#text): a
// sketch::Text's glyphs, regenerated from its fields and never saved.
//
// Two bundled fonts, by name — never a system font, so a document builds the
// same on every machine:
//   "sans"    outline: Liberation Sans (deps/fonts/LiberationSans-Regular.ttf),
//             read through OpenCASCADE's StdPrs_BRepFont (FreeType). Every
//             segment is a line or a 3/4-pole Bezier, which IS a clamped
//             B-spline of degree 2/3, so nothing is flattened.
//   "hershey" single stroke: Hershey Roman Simplex, embedded as data
//             (HersheyRomanSimplex.inc). Open polylines, for engraving; a
//             profile skips them.

#include "core/features/SketchFeature.h"

#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// The bundled fonts, in the order `describe` and the GUI list them.
const std::vector<std::string>& TextFontNames();
bool IsKnownTextFont(std::string_view font);
// A single-stroke font draws open curves: it makes no profile.
bool IsStrokeFont(std::string_view font);
// "sans, hershey" — for a refusal that names what IS accepted.
std::string TextFontList();

// JSON / RPC spellings of the alignments. Parsers refuse (nullopt) anything
// else; the callers name the accepted words.
const char* TextHAlignName(TextHAlign a);
const char* TextVAlignName(TextVAlign a);
std::optional<TextHAlign> ParseTextHAlign(std::string_view s);
std::optional<TextVAlign> ParseTextVAlign(std::string_view s);
inline constexpr const char* kTextHAlignWords = "left, center, right";
inline constexpr const char* kTextVAlignWords = "baseline, bottom, middle, top";

// One contour of one glyph, placed in sketch UV. `segments` are Line or
// BSpline entities (a Bezier: `degree` = poles - 1) carrying the label's id;
// each starts where the previous one ended. Outline contours are closed (the
// last segment ends exactly where the first starts); a stroke contour is one
// pen-down polyline and is `closed` only when it returns to its start.
struct TextContour {
  uint32_t glyph = 0;   // rendered-character index (spaces are not counted)
  uint32_t contour = 0; // within the glyph
  bool closed = true;
  std::vector<Entity> segments;
};

struct TextGeometry {
  bool stroke = false;
  std::vector<TextContour> contours;
  // The label's box from the FONT's metrics (not the string's ink), so a row
  // of labels lines up: bottom-left, bottom-right, top-right, top-left.
  std::array<glm::vec2, 4> box{};
  double width = 0.0; // advance of the whole string incl. spacing, mm
};

// Lays the label out. Refused — with the reason, naming what is accepted —
// for an unknown font, an empty or multi-line string, a character the font
// has no glyph for, invalid UTF-8, or a height that is not positive.
std::expected<TextGeometry, std::string> BuildTextGeometry(const Text& text);

// A font's own numbers at a cap height of 1: what the layout scales by the
// label's height. Measured once per font, the cap height from 'H'.
struct TextFontMetrics {
  double ascender = 0.0;    // above the baseline, cap height = 1
  double descender = 0.0;   // below it (negative)
  double capHeightEm = 0.0; // cap height as a fraction of the em (outline)
};
std::expected<TextFontMetrics, std::string>
TextFontMetricsOf(std::string_view font);

} // namespace sketch
