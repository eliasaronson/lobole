#pragma once

#include "CADKernel.h"
#include "SketchPlane.h"
#include "core/SketchFeature.h"

#include <optional>
#include <vector>

namespace sketch {

// One closed loop of a sketch profile, in plane UV space.
struct Loop {
  std::optional<Circle> circle; // set: a pure circle loop
  std::vector<glm::vec2> polygon; // else: closed polyline (implicit close)

  // Shoelace area (sign = winding); circles are positive.
  double SignedArea() const;
};

// Extracts closed loops: circles and rectangles are loops by construction;
// free Line entities are chained by endpoint proximity (tolerance 1e-4) into
// closed loops. Open chains are discarded with a warning.
std::vector<Loop> BuildLoops(const std::vector<Entity>& entities);

// Builds the profile face (largest-|area| loop is the outer boundary, the
// rest become holes) and extrudes it along the plane normal. Returns an
// invalid CADShape on failure.
CADShape BuildPrism(const SketchPlane& plane, const std::vector<Loop>& loops,
                    double distance, bool reversed);

// Tessellates one entity into a UV polyline for rendering/preview: Line = 2
// points, Circle = 33 (closed, 32 segments), Point = empty (rendered as a
// marker elsewhere).
std::vector<glm::vec2> TessellateEntity(const Entity& entity);

} // namespace sketch
