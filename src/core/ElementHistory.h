#pragma once

#include "cad/CADKernel.h"
#include "core/features/BooleanFeature.h" // BooleanOp
#include "core/ElementMap.h"
#include "core/Feature.h"       // Body
#include "core/features/FilletFeature.h" // EdgeBlendOp

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

// Constant-radius fillet or chamfer of the given edges (edgeIndex space,
// the ElementMap convention). Blend faces are named
// "<spineEdgeName>;G<featureTag>" via the maker's Generated (the spine edge
// is removed-but-generating, so its own name dies — references to it fail,
// as intended); adjacent support faces keep their names via Modified; fillet
// corner patches are covered by the U-then-L fallback. On failure returns an
// invalid CADShape and, when OCC reports faulty contours, a diagnostic in
// *error (may be null).
CADShape EdgeBlendNamed(const CADShape& body, const ElementMap& bodyMap,
                        const std::vector<int>& edgeIndices, EdgeBlendOp op,
                        double radiusOrDistance, uint64_t featureTag,
                        ElementMap& out, std::string* error);

// Fixed names for a primitive's output (box/sphere/cylinder/import): every
// face "<prefix>/face<i>" in index order — stable because a primitive's
// topology never changes with its dimensions — then the U/L passes for its
// edges. The memo's "Primitives: fixed names" rule, so blends can reference
// primitive edges too. `prefix` is normally the feature tag; a multi-body
// import appends its body ordinal ("<tag>/b1") so two bodies of one feature
// never share names.
void NamePrimitiveElements(const CADShape& shape, const std::string& prefix,
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

// An interior point of a polyline: the point at half its total arc length.
// The geometric probe for ResolveEdgeOnBody MUST be interior, never an
// endpoint. A straight B-Rep edge tessellates to just its two endpoints, so
// `poly[poly.size()/2]` is an endpoint — a vertex shared by every edge meeting
// there — which makes ResolveEdgeOnBody's point-to-polyline match resolve to an
// arbitrary incident edge (the "second blend edge snaps to a neighbour" bug).
glm::vec3 PolylineArcMidpoint(const std::vector<glm::vec3>& poly);

// Resolve a picked edge onto another state of its body — by name when
// `pickedName` still resolves on `input`, else geometrically: `probe` (an
// INTERIOR point of the displayed edge — use PolylineArcMidpoint, not an
// endpoint) must lie on the input edge. Edge picking uses this to store blend
// refs against the blend's INPUT body rather than its displayed (already
// blended) output, whose toponames embed the blend's own tag. nullopt = no
// counterpart on `input` (e.g. an edge the blend itself created).
std::optional<ElementRef> ResolveEdgeOnBody(const Body& input,
                                            const std::string& pickedName,
                                            const glm::vec3& probe);

} // namespace topo
