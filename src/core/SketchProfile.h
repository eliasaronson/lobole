#pragma once

#include "CADKernel.h"
#include "SketchPlane.h"
#include "core/SketchFeature.h"

#include <optional>
#include <vector>

namespace sketch {

// One boundary edge of a profile loop, in plane UV space. Traversal runs from
// `a` to `b`. Arcs also carry their canonical geometry (center/radius and the
// CCW angle span from `angStart` to `angEnd`), independent of traversal
// direction — the entity defines a specific arc; ordering/orientation is fixed
// up by ShapeFix_Wire at build time.
struct LoopEdge {
  bool arc = false;
  glm::vec2 a{0.0f}, b{0.0f};
  glm::vec2 center{0.0f};
  double radius = 0.0;
  double angStart = 0.0, angEnd = 0.0; // CCW span (arc only)
};

// One closed loop of a sketch profile, in plane UV space.
struct Loop {
  std::optional<Circle> circle;  // set: a pure circle loop
  std::vector<LoopEdge> edges;   // else: ordered boundary edges (lines + arcs)

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

// If segments [a1,b1] and [a2,b2] properly cross, returns the crossing's
// parameter t in [0,1] along the *first* segment; nullopt if parallel or the
// crossing lies outside either segment. Used to find trim cut points.
std::optional<float> SegmentIntersect(glm::vec2 a1, glm::vec2 b1, glm::vec2 a2,
                                      glm::vec2 b2);

} // namespace sketch
