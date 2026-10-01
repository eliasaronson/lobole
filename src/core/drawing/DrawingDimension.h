#pragma once

#include "core/Expression.h"
#include "core/Units.h"

#include "core/drawing/DrawingDocument.h"
#include "core/drawing/DrawingRender.h"

#include <glm/glm.hpp>

#include <functional>
#include <string>

// Resolving a driven drawing dimension (workstream 13, D3): match its ProjRefs
// against a view's projected curves, measure the value, and lay out the
// draw-ready geometry (extension/dimension lines or a radial leader). GUI-free
// core, so the viewport and the SVG/PDF exporters draw identical dimensions
// from one computation. Derived, never serialized — recomputed on update.

class Document;

namespace drawing {

// A dimension resolved against projected geometry, in sheet mm. `resolved` is
// false when a ProjRef no longer matches any curve (the edge was consumed
// upstream) — a first-class dangling state the renderer flags, never rebinds.
struct ResolvedDim {
  uint64_t id = 0;
  DrawingDimKind kind = DrawingDimKind::Distance;
  bool resolved = false;
  bool radial = false; // true for radius/diameter (centre->rim leader)
  // Callouts: a leader line rim -> label (no centre spoke). Value/text are
  // MODEL mm (scale-corrected), unlike the legacy sheet-mm kinds — identical
  // at 1:1; the legacy kinds' scale correction is a recorded follow-up.
  bool leader = false;
  double value = 0.0; // millimetres, or radians for Angle
  std::string text;   // formatted label (Ø.. / R.. / .. / ..°)
  glm::dvec2 labelPos{0.0};

  // Linear dimensions (distance / distance-X / distance-Y):
  glm::dvec2 ext1{0.0}, ext2{0.0}; // the measured points (extension-line feet)
  glm::dvec2 dim1{0.0}, dim2{0.0}; // the dimension line, offset by labelOffset

  // Radial dimensions (radius / diameter):
  glm::dvec2 center{0.0};
  double radius = 0.0;
  glm::dvec2 rim{0.0}; // leader endpoint on the circle, offset outward
};

// What a callout kind needs beyond the projected view: the source part
// document (to resolve a circle back to its HoleFeature) and the view scale
// (sheet mm -> model mm). Legacy kinds ignore it, so the default keeps every
// existing caller compiling; a missing sourceDoc makes callouts dangle.
struct DimContext {
  const Document* sourceDoc = nullptr;
  double viewScale = 1.0;
  // The units the numbers are WRITTEN in. Dimension text ignored the document
  // entirely until 2026-09-10 and always printed millimetres, so an inch part
  // drew as mm with nothing on the sheet to say so. Defaulted, so the auto-
  // placement probes that only need a label WIDTH still construct one.
  units::System units;
  // The part document an OCCURRENCE places, for an assembly view. A fastener
  // callout names the standard part it points at, and that name lives in the
  // occurrence's own document rather than in the assembly the view is of —
  // which is why this is a resolver and not a second `sourceDoc`.
  std::function<const Document*(uint64_t occurrenceId)> occurrenceDoc;
};

// Resolves `dim` against its view's projected geometry. `view` must be the
// SheetView for dim.viewId.
ResolvedDim ResolveDim(const DrawingDim& dim, const SheetView& view,
                       const DimContext& ctx = {});

// Gap between a view's bbox and an auto-placed dimension line, sheet mm.
inline constexpr double kDimOffsetGapMM = 8.0;

// A sensible labelOffset when the caller gave none: the dimension line lands
// a gap BELOW the view's bbox (distance / distance_x / angle), LEFT of it
// (distance_y), or — for radius/diameter — the leader points away from the
// bbox centre. Falls back to {0, -12} (the historical GUI default) when the
// dim doesn't resolve or the view has no geometry. Anchors are sheet mm
// (view position included), matching ResolveDim's per-kind offset semantics
// exactly.
glm::dvec2 DefaultDimOffset(const DrawingDim& dim, const SheetView& view);

// A note ready to draw: the text at its anchor, plus the leader line end
// when the note points at picked geometry (resolved like a dim ref).
struct ResolvedNote {
  bool resolved = true;
  std::string error;
  uint64_t id = 0;
  std::string text;
  glm::dvec2 pos{0.0};
  bool hasLeader = false;
  glm::dvec2 anchor{0.0};
  NoteStyle style = NoteStyle::Text; // v147
};

// A table note's text: rows split by '\n', cells by '\t'; a row with no tab
// spans the table (a caption). Drawn ruled, its bottom-right corner at the
// note's position (v147, the parts list).
[[nodiscard]] bool IsTableText(const std::string& text);

// How a note is drawn when it is more than a line of text with an optional
// arrow: a balloon, a datum symbol, a ruled table. Empty for a plain note,
// which every surface draws as its centred text and arrowed leader.
[[nodiscard]] SheetGraphics NoteGraphics(const ResolvedNote& note);

// The box a note occupies on the sheet: LabelBox for a plain note, the
// symbol or the table for the rest -- what placement avoids and
// check_drawing flags overlaps against.
void NoteBox(const ResolvedNote& note, glm::dvec2& mn, glm::dvec2& mx);

// Resolve a note against its view's projection (`view` may be null for a
// free note; a leader that no longer matches degrades to text-only rather
// than dropping the note — the words still carry their meaning).
ResolvedNote ResolveNote(const DrawingNote& note, const SheetView* view,
                         const SymbolTable& symbols = {});

// The rows of a bend table (a note with `table` Bends): one line per bend of
// `doc`'s own bodies, numbered in body order. Derived ghosts are skipped —
// they are another part's bends — so a part carrying one is numbered here
// differently from the sheet-metal panel, which lists every body. With angle,
// inside radius, K and, on a flat crease, the allowance it eats and whether it
// folds UP or DOWN as seen by a reader looking along `viewDir` (a crease the
// view does not look square onto has no direction to state). `caption` heads
// it. Empty when the part has no bends, which the caller turns into an
// unresolved note.
[[nodiscard]] std::string BendTableText(const Document& doc,
                                        const std::string& caption,
                                        const glm::dvec3& viewDir);

// What auto-placement must stay clear of: rectangles already taken (every
// view's bbox, the title block, existing dims' label boxes) and the sheet
// border. Built by ui::AutoDimOffset from live state.
struct DimPlaceContext {
  std::vector<std::pair<glm::dvec2, glm::dvec2>> avoid;
  glm::dvec2 borderMin{0.0}, borderMax{0.0};
};

// The label's box on the sheet. ONE estimate shared by auto-placement and
// check_drawing's overlap validator, so what placement avoids is exactly
// what the validator flags — two estimates would drift and one of them
// would lie.
// One line of label text, sheet mm. A note with newlines in it occupies
// this much per line, growing downward from its position.
inline constexpr double kLabelLineMM = 5.0;

void LabelBox(const glm::dvec2& labelPos, const std::string& text,
              glm::dvec2& mn, glm::dvec2& mx);

// Where a leader from `from` stops at a label centred on `labelPos`: at the
// side of its text facing `from`, on the first line's middle. A leader drawn
// to the label's CENTRE ran through the words it introduces.
[[nodiscard]] glm::dvec2 LeaderEnd(const glm::dvec2& from,
                                   const glm::dvec2& labelPos,
                                   const std::string& text);

// The white box a label's text knocks out of whatever it sits on (the
// dimension line it interrupts, a hatch), drawn under the text: what the
// viewport always did and the exporters did not, so a "12.7" sat with its
// dimension line through it on paper. Text is centred on `labelPos`, its
// first line's middle there, later lines below.
void TextKnockout(const glm::dvec2& labelPos, const std::string& text,
                  glm::dvec2& mn, glm::dvec2& mx);

// DefaultDimOffset with lanes: the base placement, stepped outward until
// the resulting LABEL clears everything in `ctx` (five sheets in one
// session needed 2–4 manual passes each because every below-the-view dim
// landed on the same line, and the pile ran into the title block).
// `resolved` is the dim resolved with a ZERO offset by a caller that has the
// context to do it. Without one this re-resolves with no DimContext, and a
// callout needs the source document to resolve at all — so every hole
// callout fell straight through to the fallback offset and landed on the
// part it points at.
glm::dvec2 PlaceDimOffset(const DrawingDim& dim, const SheetView& view,
                          const DimPlaceContext& ctx,
                          const ResolvedDim* resolved = nullptr);

} // namespace drawing
