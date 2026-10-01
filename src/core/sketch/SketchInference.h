#pragma once

#include "core/features/SketchFeature.h"

#include <glm/glm.hpp>
#include <optional>
#include <vector>

namespace sketch {

// What kind of ghost glyph to draw for an inferred constraint.
enum class GlyphKind {
  Coincident,
  PointOnLine,
  Horizontal,
  Vertical,
  Parallel,
  Perpendicular,
};

// The result of inferring constraints for a point being placed while drawing.
// The referenced constraints describe "the new point": the caller fills in the
// new entity's id once it exists (see the commit path).
struct Snap {
  glm::dvec2 uv{0.0};                  // the point, snapped (or unchanged)
  std::optional<PointRef> coincident;  // snap the point coincident to this
  std::optional<uint32_t> pointOnLine; // else bind the point onto this line id
  // Axis alignment with another vertex: the new point shares that vertex's X
  // (vertical alignment → a two-point Vertical constraint) or Y (horizontal
  // alignment → a two-point Horizontal constraint). Independent per axis and of
  // the segment-level snaps below.
  std::optional<PointRef> alignShareX; // new point shares X with this vertex
  std::optional<PointRef> alignShareY; // new point shares Y with this vertex
  // Segment-level (only when a segment's other end is known):
  bool horizontal = false;
  bool vertical = false;
  std::optional<uint32_t> parallelTo;
  std::optional<uint32_t> perpendicularTo;
  std::vector<GlyphKind> glyphs; // what to draw at the cursor
};

// Infers snaps for a point being placed at `uv`. `other` is the segment's fixed
// end (the pending start) when drawing a line — enables H/V/parallel/perp;
// nullopt disables segment-level inference. `skipId` (the entity being drawn,
// 0 if none yet) is excluded from candidates. `pixelsPerUV` converts the pixel
// tolerances to UV space (from the current zoom).
Snap InferPoint(const std::vector<Entity>& entities, glm::dvec2 uv,
                std::optional<glm::dvec2> other, uint32_t skipId,
                double pixelsPerUV);

} // namespace sketch
