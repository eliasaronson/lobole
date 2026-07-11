#pragma once

#include "CADKernel.h"
#include "core/BooleanFeature.h" // BooleanOp
#include "core/ElementMap.h"

#include <cstdint>

// Propagation of stable element names through history-reporting OCC operations
// (topological naming, T5). Each function keeps the OCC maker alive, folds its
// Modified/Generated/IsDeleted reports into a BRepTools_History, and rewrites
// the inputs' MappedNames onto the result shape: a face/edge the op leaves
// untouched keeps its name verbatim; a Modified element appends ";M<tag>"
// (".<i>" when one input splits into N); a Generated element appends ";G<tag>";
// whatever history misses is named by the U/L fallback passes. Names key on
// the result's face/edge indices (the same walk GenerateMesh, picking and
// ElementMap use), so references keep resolving across regenerations.
namespace topo {

// Boolean of two named shapes. `out` receives the propagated map for the
// result (boolean section edges not reached by history seed "<tag>/seam<i>").
// Returns an invalid CADShape on failure, with `out` cleared. Either input
// map may be empty — the op still runs, the result just carries fewer names.
CADShape BooleanNamed(BooleanOp op, const CADShape& target,
                      const ElementMap& targetMap, const CADShape& tool,
                      const ElementMap& toolMap, uint64_t featureTag,
                      ElementMap& out);

// The U and L fallback passes alone, for builders whose history already
// seeded `map` (extrude/revolve): U — a named face names its still-unnamed
// boundary edges "<faceName>;U<localIdx>" (localIdx counts every unique
// boundary edge, so it is stable whether or not neighbours were named); L —
// an unnamed face takes its sorted boundary-edge names joined with ',' plus
// ";L". OCC history is genuinely incomplete for lower-dimensional elements,
// so these are what give edges (fillet spines!) their stable names.
void ApplyFallbackPasses(const CADShape& shape, ElementMap& map);

// 0-based index of `edge` within `shape`'s deduplicated edge order
// (TopExp::MapShapes(TopAbs_EDGE) first-appearance order) — the edgeIndex
// convention ElementMap::edgeNames uses. -1 if absent.
int EdgeIndexOf(const TopoDS_Shape& shape, const TopoDS_Shape& edge);

} // namespace topo
