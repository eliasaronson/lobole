#pragma once

#include "cad/CADKernel.h"
#include "core/Feature.h"

#include <string>
#include <vector>

// DXF out (docs/sheetmetal/plan.md, S6): the first file this project produces
// that a shop can cut.
//
// R12 ASCII, and only LINE / ARC / CIRCLE. Every consumer reads those — a
// laser's nesting software, a router post, a quoting portal — while
// LWPOLYLINE needs R2000 and splines are read by fewer machines than they are
// written by. An arc stays an ARC rather than being tessellated: kerf
// compensation and lead-ins are computed from the real curve, and a
// forty-segment approximation of a hole is a hole the machine cuts forty
// times.
//
// Layers are the convention a shop expects, because a DXF with the bend lines
// on the cutting layer is a part cut into strips:
//   CUT       - the outline and every hole (continuous)
//   BEND_UP   - bend lines that fold toward the viewer (dashed)
//   BEND_DOWN - bend lines that fold away (dashed)
// and the viewer looks at the blank from the side its first flange was picked
// on, so a part whose flanges were all picked on one face is all UP. A bend
// TABLE on a drawing answers the same question from its own view's side
// instead (docs/drawings/guide.md), so the two can disagree about one crease
// — they are answering "up from where?" differently, which is why neither
// stores an answer.
namespace dxf {

// What a flat pattern hands the writer.
struct FlatDrawing {
  // The planar face whose boundary is the blank's outline, with its holes.
  CADShape body;
  // The bends, with their crease centrelines in flat coordinates (what
  // sheet::Unfold leaves on the body).
  std::vector<BendRegion> creases;
};

// Writes `drawing` to `path`. Returns the reason on failure (empty = written).
//
// The body must be FLAT — one plane's worth of outline. A folded part has no
// single outline, and writing its silhouette would produce a file that looks
// like a part and cuts to nothing.
[[nodiscard]] std::string WriteFlatPattern(const FlatDrawing& drawing,
                                           const std::string& path);

} // namespace dxf
