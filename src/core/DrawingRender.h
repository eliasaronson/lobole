#pragma once

#include "core/DrawingProjection.h"  // drawing::CurveKind
#include "core/ElementMap.h"

#include <glm/glm.hpp>

#include <cstdint>
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
  std::vector<glm::dvec2> points;  // sheet mm
  uint64_t sourceId = 0;           // body createdBy id, or occurrence id (assembly)
  ElementRef edge;                 // resolved source edge (sharp/smooth), or unset
  ElementRef silFace;              // resolved silhouette face (outline), or unset
  int curveType = -1;              // GeomAbs curve type (line/circle/…): SVG hint
};

// All geometry for one view, ready to render or export.
struct SheetView {
  uint64_t viewId = 0;
  std::vector<SheetCurve> curves;
  glm::dvec2 bboxMin{0.0};  // sheet-mm bounds of the projected geometry
  glm::dvec2 bboxMax{0.0};
  bool hasGeometry = false;
  // The source Document::Version() at projection time; a later source version
  // means this view is stale and wants an update. 0 = never projected.
  uint64_t solvedSourceVersion = 0;
  std::string error;  // non-empty if projection failed (keeps last-good curves)
};

}  // namespace drawing
