#pragma once

#include "core/DrawingDocument.h"
#include "core/DrawingRender.h"

#include <glm/glm.hpp>

#include <string>

// Resolving a driven drawing dimension (workstream 13, D3): match its ProjRefs
// against a view's projected curves, measure the value, and lay out the
// draw-ready geometry (extension/dimension lines or a radial leader). GUI-free
// core, so the viewport and the SVG/PDF exporters draw identical dimensions
// from one computation. Derived, never serialized — recomputed on update.

namespace drawing {

// A dimension resolved against projected geometry, in sheet mm. `resolved` is
// false when a ProjRef no longer matches any curve (the edge was consumed
// upstream) — a first-class dangling state the renderer flags, never rebinds.
struct ResolvedDim {
  uint64_t id = 0;
  DrawingDimKind kind = DrawingDimKind::Distance;
  bool resolved = false;
  bool radial = false;     // true for radius/diameter (centre->rim leader)
  double value = 0.0;      // millimetres, or radians for Angle
  std::string text;        // formatted label (Ø.. / R.. / .. / ..°)
  glm::dvec2 labelPos{0.0};

  // Linear dimensions (distance / distance-X / distance-Y):
  glm::dvec2 ext1{0.0}, ext2{0.0};  // the measured points (extension-line feet)
  glm::dvec2 dim1{0.0}, dim2{0.0};  // the dimension line, offset by labelOffset

  // Radial dimensions (radius / diameter):
  glm::dvec2 center{0.0};
  double radius = 0.0;
  glm::dvec2 rim{0.0};  // leader endpoint on the circle, offset outward
};

// Resolves `dim` against its view's projected geometry. `view` must be the
// SheetView for dim.viewId.
ResolvedDim ResolveDim(const DrawingDim& dim, const SheetView& view);

}  // namespace drawing
