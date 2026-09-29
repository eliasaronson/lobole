#pragma once

#include "core/ElementMap.h"
#include "core/cam/CamJob.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

class Document;
struct Body;

// Engraving: a cut that follows SKETCH curves on the curve
// (docs/cam/operations.md#engrave). The one CAM operation whose target is not
// a face of a body, so it has two halves here: the RESOLVER, which turns
// "entity g<id> of sketch feature <fid>" into curves in the part's model
// space, and the pieces the generator, the derivation and the verifier share.
namespace cam {

// One stroke of a sketch entity: a polyline in the part's MODEL space. Closed
// when it returns to its own start (a circle, a closed spline); an open one
// may be cut from either end.
struct SketchStroke {
  std::vector<glm::dvec3> pts;
  bool closed = false;
};

// The curves one engraving target names, as the document had them when the
// caller resolved them. Resolved where the documents are (the job's part
// documents, in the UI; a test's own document) and handed to the generator
// in `GenerateContext::sketchCurves`, because a generator sees bodies only
// and a sketch makes none.
struct SketchCurves {
  uint64_t jobPartId = 0;
  ElementRef ref; // {sketch feature id, "g<entityId>"}
  std::vector<SketchStroke> strokes;
  // The sketch's plane, model space: what faces the spindle or does not.
  glm::dvec3 planeOrigin{0.0};
  glm::dvec3 planeNormal{0.0, 0.0, 1.0};
  // The entity's own size when it has one -- a text label's cap height -- and
  // 0 for a line, an arc or a spline. The default groove width derives from
  // it when it is there (a label's cap / 8).
  double textHeight = 0.0;
  // Non-empty: this target names no curves, and why, in words.
  std::string why;
};

// THE resolver (the one function a new sketch entity type plugs into): the
// curves of sketch entity `ref.name` ("g<entityId>") of sketch feature
// `ref.featureId` in `doc`, in the part's model space. Refuses, in `why`, a
// feature that is not a sketch, an element that is not "g<id>", an entity
// the sketch does not have, construction geometry and a point.
[[nodiscard]] SketchCurves SketchEntityCurves(const Document& doc,
                                              const ElementRef& ref);

// The resolved curves for one target, or null when nobody resolved them.
[[nodiscard]] const SketchCurves*
FindSketchCurves(const std::vector<SketchCurves>& curves, const Target& t);

// The groove an engraving cuts, from its op and its tool: the depth the tip
// goes below the surface and the width at the surface. For a V-bit one of
// width and depth is the other -- depth = TipHeight(tool, width / 2), the
// verifier's own function -- and a width past the cone is refused. A flat or
// a ball takes a depth only; a drill is not an engraving tool.
struct EngraveGroove {
  bool ok = false;
  double depth = 0.0;
  double width = 0.0;
  std::string why;
};
[[nodiscard]] EngraveGroove EngraveGrooveFor(const Operation& op,
                                             const Tool& tool);

// The groove an engraving is derived with when nobody said: a text label's
// cap height / 8, else a quarter of the cutter -- as a V-bit's width, or a
// flat's or a ball's depth. Never wider than the cone.
[[nodiscard]] double DefaultEngraveWidth(const Tool& tool, double textHeight);
[[nodiscard]] double DefaultEngraveDepth(const Tool& tool);

// The curves in THIS setup's frame: each stroke's XY, and the surface height
// the plane lands at. Refused, naming its angle, when the plane does not face
// the spindle. `setup` may be one turned to an index.
struct EngraveFrame {
  bool ok = false;
  std::string why;
  double surfaceZ = 0.0;
  std::vector<std::vector<glm::dvec2>> strokes;
  std::vector<bool> closed;
};
[[nodiscard]] EngraveFrame EngraveFrameOf(const Setup& setup,
                                          const SketchCurves& curves);

// Whether the strokes lie ON the part's surface: material just under the
// plane and none just over it, probed along them in `framed` bodies (setup
// space). Empty when they do; otherwise where they leave it and which way.
[[nodiscard]] std::string EngraveSurfaceRefusal(const std::vector<Body>& framed,
                                                const EngraveFrame& frame);

// The order the strokes are cut in, and which way round: nearest neighbour
// from `from` (an open stroke reversed when its far end is nearer, a closed
// one started at its nearest point), then 2-opt. `optimise` false keeps the
// drawn order and direction -- the control the tests measure against.
struct StrokeRun {
  std::vector<glm::dvec2> pts; // in cutting order
  size_t source = 0;           // index into the input
  bool reversed = false;
};
[[nodiscard]] std::vector<StrokeRun>
OrderStrokes(const std::vector<std::vector<glm::dvec2>>& strokes,
             const std::vector<bool>& closed, const glm::dvec2& from,
             bool optimise = true);
// The travel between runs, end to start, from `from`: what the order saves.
[[nodiscard]] double StrokeTravel(const std::vector<StrokeRun>& runs,
                                  const glm::dvec2& from);

} // namespace cam
