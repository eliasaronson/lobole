#pragma once

#include "core/drawing/DrawingDocument.h"
#include "core/drawing/DrawingRender.h"

#include <glm/glm.hpp>

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
void LabelBox(const glm::dvec2& labelPos, const std::string& text,
              glm::dvec2& mn, glm::dvec2& mx);

// DefaultDimOffset with lanes: the base placement, stepped outward until
// the resulting LABEL clears everything in `ctx` (five sheets in one
// session needed 2–4 manual passes each because every below-the-view dim
// landed on the same line, and the pile ran into the title block).
glm::dvec2 PlaceDimOffset(const DrawingDim& dim, const SheetView& view,
                          const DimPlaceContext& ctx);

} // namespace drawing
