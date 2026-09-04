#pragma once

#include "core/ElementMap.h"
#include "core/drawing/DrawingProjection.h" // drawing::CurveKind

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

// All geometry for one view, ready to render or export.
struct SheetView {
  uint64_t viewId = 0;
  std::vector<SheetCurve> curves;
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
  // (docs/agent-scale-followup-plan.md, W3).
  double updateMs = 0.0; // HLR outline building
  double hideMs = 0.0;   // HLR visibility classification
  int dsEdges = 0;       // edges in the HLR data structure
  int bodies = 0;        // solids fed to the one global HLR pass
};

} // namespace drawing
