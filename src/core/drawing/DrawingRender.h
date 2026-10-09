// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/ElementMap.h"
#include "core/drawing/DrawingProjection.h" // drawing::CurveKind

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Sheet-placed projected geometry for one drawing view: the output of the
// projection driver (which resolves a view's source document, runs HLR, and
// maps model-space curves onto the sheet at the view's scale + position) and
// the input to both the viewport renderer and the SVG/PDF exporters. Derived,
// never serialized — recomputed on explicit update. Coordinates are sheet
// millimetres (origin bottom-left, +Y up), the same space the sheet chrome
// uses.

namespace drawing {

// One projected curve, already placed on the sheet. Carries the toponaming
// refs the source geometry resolved to (the D0 index-space SourceRef joined to
// the body's ElementMap) so dimensions can attach to it — either a source 3D
// edge (sharp/smooth) or a silhouette's generating face.
struct SheetCurve {
  CurveKind kind = CurveKind::Sharp;
  bool visible = true;
  std::vector<glm::dvec2> points; // sheet mm
  uint64_t sourceId = 0; // body createdBy id, or occurrence id (assembly)
  ElementRef edge;       // resolved source edge (sharp/smooth), or unset
  ElementRef silFace;    // resolved silhouette face (outline), or unset
  int curveType = -1;    // GeomAbs curve type (line/circle/…): SVG hint
  // For a circle/ellipse curve: its centre + radius in SHEET mm (view scale
  // applied), so radius/diameter dimensions read them without re-deriving.
  // radius <= 0 means not a symbolic conic.
  glm::dvec2 conicCenter{0.0};
  double conicRadius = -1.0;
  bool closed = false; // conic interval sweeps the full circle (bore vs arc)
};

// Line styles, in sheet millimetres: alternating dash and gap lengths, and
// the stroke width. One table, because the viewport, the SVG and the PDF must
// draw the same line — a hidden edge that is dashed on screen and solid in
// the exported sheet is a drawing that means two different things.
struct LineStyle {
  std::span<const double> dash; // empty = a solid line
  double widthMM = 0.35;
};
inline constexpr double kHiddenDashMM[] = {1.6, 1.2};
// A chain line (long dash, gap, dot, gap): the drafting convention for a
// centreline, and what a flat pattern's bend lines are drawn with.
inline constexpr double kBendDashMM[] = {6.0, 1.2, 0.6, 1.2};

// How a projected curve is drawn, from what it IS.
[[nodiscard]] inline LineStyle StyleOf(const SheetCurve& c) {
  if (c.kind == CurveKind::Bend)
    return {kBendDashMM, 0.25};
  if (!c.visible)
    return {kHiddenDashMM, 0.25};
  return {{}, 0.35};
}

// Whether a projected curve gets a centre mark: a WHOLE circle seen whole.
// An arc of a circle mostly hidden marked a centre in the empty air beside a
// section, where nothing on the sheet was round.
[[nodiscard]] inline bool HasCentreMark(const SheetCurve& c) {
  return c.visible && c.conicRadius > 0.0 && c.closed;
}

// How wide `text` sets in Helvetica at `heightMM`, from its AFM advance
// widths by class (capitals and W/M wide, digits, lower case, i/l/t/f
// narrow, spaces and punctuation): a table sized for an average glyph had
// ALUMINUM run through its column rule. UTF-8 aware: Ø and × are one glyph.
[[nodiscard]] inline double TextWidthMM(const std::string& text,
                                        double heightMM = 3.5) {
  double em = 0.0;
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if ((c & 0xC0) == 0x80)
      continue; // a UTF-8 continuation byte
    if (c >= 0x80)
      em += 0.70; // Ø, ×, °: as wide as a capital
    else if (c == 'M' || c == 'W' || c == 'm' || c == 'w')
      em += 0.85;
    else if (c >= 'A' && c <= 'Z')
      em += 0.70;
    else if (c >= '0' && c <= '9')
      em += 0.56;
    else if (c == 'i' || c == 'l' || c == 'j' || c == 't' || c == 'f' ||
             c == 'r' || c == 'I')
      em += 0.30;
    else if (c >= 'a' && c <= 'z')
      em += 0.56;
    else if (c == ' ' || c == '.' || c == ',' || c == ':' || c == ';' ||
             c == '\'' || c == '!' || c == '|')
      em += 0.28;
    else
      em += 0.58;
  }
  return em * heightMM;
}

// Sheet furniture that is not projected geometry -- a section's hatching and
// its cutting-plane line, a balloon, a datum symbol, a ruled table -- as the
// three primitives every surface can draw: lines, filled polygons and text.
// ONE description, built in core, so the viewport, the SVG and the PDF draw
// the same symbol rather than three hand-copies of it.
struct SheetText {
  glm::dvec2 pos{0.0}; // baseline, sheet mm
  std::string text;
  bool center = true;    // centred on pos.x; false = starts at pos.x
  double heightMM = 3.5; // cap-height-ish: the font size
  // Turned counter-clockwise about `pos` (v151): an aligned dimension's
  // value reads along its line. `center` then centres along the turned
  // baseline.
  double angleRad = 0.0;
};
struct SheetGraphics {
  struct Line {
    glm::dvec2 a{0.0}, b{0.0};
    double widthMM = 0.25;
    bool chain = false;  // a chain line (kBendDashMM): a cutting plane
    bool dashed = false; // a hidden line (kHiddenDashMM): a hidden thread
  };
  std::vector<Line> lines;
  std::vector<std::vector<glm::dvec2>> fills; // filled convex polygons
  // Filled white, under everything else here: a frame's or a value's
  // background, knocked out of whatever it stands on.
  std::vector<std::vector<glm::dvec2>> knockouts;
  std::vector<SheetText> texts;
  [[nodiscard]] bool empty() const {
    return lines.empty() && fills.empty() && texts.empty() && knockouts.empty();
  }
  void Append(const SheetGraphics& o) {
    lines.insert(lines.end(), o.lines.begin(), o.lines.end());
    fills.insert(fills.end(), o.fills.begin(), o.fills.end());
    knockouts.insert(knockouts.end(), o.knockouts.begin(), o.knockouts.end());
    texts.insert(texts.end(), o.texts.begin(), o.texts.end());
  }
};

// All geometry for one view, ready to render or export.
struct SheetView {
  uint64_t viewId = 0;
  std::vector<SheetCurve> curves;
  // What the view draws besides its projected curves: a section's hatching
  // and caption, the cutting-plane lines of other views' sections (v147).
  SheetGraphics extras;
  // A section view: a value inside its outline stands on hatching knocked
  // out for it, which is how drafting dimensions a section.
  bool sectioned = false;
  glm::dvec2 bboxMin{0.0}; // sheet-mm bounds of the projected geometry
  glm::dvec2 bboxMax{0.0};
  bool hasGeometry = false;
  // The source Document::Version() at projection time; a later source version
  // means this view is stale and wants an update. 0 = never projected.
  uint64_t solvedSourceVersion = 0;
  std::string error; // non-empty if projection failed (keeps last-good curves)
  // What that projection COST. Kept because a drawing view holds no geometry
  // until it is exported, so nothing else can tell a caller what it just
  // spent: an assembly view is 66-86 s and looked like a hang
  // (docs/drawings/README.md#what-a-view-costs).
  ProjectionResult::Stats projectionStats;
  // The same hiding time, folded onto the SEMANTIC source — the occurrence on
  // an assembly view, the body's owning feature on a part view — because that
  // is the name a reader can act on: `hidden` takes occurrences, or a part's
  // body-owning features, not body slots. One imported driver was 16 s of an
  // 18 s view and nothing said so (docs/drawings/README.md#what-a-view-costs).
  // Shares OVERLAP: a pair's cost counts for both of its members, since
  // leaving out either avoids it.
  std::vector<ProjectionResult::Stats::BodyCost> costBySource;
  double updateMs = 0.0; // HLR outline building
  double hideMs = 0.0;   // HLR visibility classification
  int dsEdges = 0;       // edges in the HLR data structure
  int bodies = 0;        // solids fed to the one global HLR pass
};

} // namespace drawing
