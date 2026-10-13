// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"
#include "core/ElementMap.h"
#include "core/Feature.h"                 // Body
#include "core/features/BooleanFeature.h" // BooleanOp
#include "core/features/FilletFeature.h"  // EdgeBlendOp

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class BRepTools_History;

// Propagation of stable element names through history-reporting OCC operations
// (topological naming, T5). Each function keeps the OCC maker alive, folds its
// Modified/Generated/IsDeleted reports into a BRepTools_History, and rewrites
// the inputs' MappedNames onto the result shape: a face/edge the op leaves
// untouched keeps its name verbatim; a Modified element appends ";M<tag>"; a
// Generated element appends ";G<tag>"; when one input becomes several, each
// piece is told apart by the faces only it touches, "<name>;M<tag>[a|b]"
// (docs/modeling/toponaming.md#split-pieces), with an ordinal ".<i>" only
// where nothing else does; whatever history misses is named by the U/L
// fallback passes; and a last pass leaves no two elements one name. Names key
// on the result's face/edge indices (the same walk GenerateMesh, picking and
// ElementMap use), so references keep resolving across regenerations.
namespace topo {

// A clean-up of a boolean's result that is part of the same operation: given
// the boolean's result, returns the cleaned shape and records its history
// from that result in `history` (OCC's Modified/Generated/Removed), or
// returns a null shape for "leave it as it is". The boolean's own history is
// composed with it (BRepTools_History::Merge) before anything is named, so a
// face the clean-up leaves alone is named exactly as the bare boolean names
// it, and two pieces it merges take the one name their original had. A
// modelled thread uses it to merge away the footprint its square end draws
// on a shoulder (ThreadFeature.cpp).
using RefineResult = std::function<TopoDS_Shape(const TopoDS_Shape& result,
                                                BRepTools_History& history)>;

// Boolean of two named shapes. `out` receives the propagated map for the
// result (boolean section edges not reached by history seed "<tag>/seam<i>").
// Returns an invalid CADShape on failure, with `out` cleared and, when OCC
// reported a reason, a diagnostic in *error (may be null). Either input
// map may be empty — the op still runs, the result just carries fewer names.
// `fuzzy` > 0 runs it with OCC's fuzzy option at that tolerance and
// NON-destructively (cad::BuildNonDestructive): the one retry a refused body
// boolean gets (CheckedBooleanNamed, core/BodyMode.h). `refine` (optional)
// cleans the result up before it is named (RefineResult).
CADShape BooleanNamed(BooleanOp op, const CADShape& target,
                      const ElementMap& targetMap, const CADShape& tool,
                      const ElementMap& toolMap, uint64_t featureTag,
                      ElementMap& out, std::string* error = nullptr,
                      double fuzzy = 0.0, const RefineResult& refine = {});

// Names for `result` from a history the caller built itself, for a chain of
// operations that stands for one boolean of `target` and `tool` (the worm
// wheel's teeth cut by replication, WormGear.h's CutSpacesBySectors): every
// face and edge of either input is placed as BooleanNamed places it —
// kept, ";M", ";G" — then the U/L passes. There are no section edges.
ElementMap NamesThroughHistory(const CADShape& target,
                               const ElementMap& targetMap,
                               const CADShape& tool, const ElementMap& toolMap,
                               const BRepTools_History& history,
                               const CADShape& result, uint64_t featureTag);
// The same for an operation on ONE shape (Refine's UnifySameDomain): a face
// it leaves alone keeps its name verbatim, so a refined body's bearing seat
// is still the reference a study load, a fit or a mate made to it.
ElementMap NamesThroughHistory(const CADShape& input, const ElementMap& map,
                               const BRepTools_History& history,
                               const CADShape& result, uint64_t featureTag);

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

// Names for one connected piece of an already-named shape. Every face and
// edge of `part` that IS an element of `whole` (same TopoDS shape, which a
// TopExp walk of a compound guarantees) takes that element's name. No history
// is involved and none is needed: the pieces of a split share their faces with
// the shape they came out of. Used where a fused result is split back into
// connected solids — a pattern's instances, which may touch and merge.
void NamesOfSubShape(const CADShape& whole, const ElementMap& wholeMap,
                     const CADShape& part, ElementMap& out);

// The U and L fallback passes alone, for builders whose history already
// seeded `map` (extrude/revolve): U — a named face names its still-unnamed
// boundary edges "<faceName>;U<localIdx>" (localIdx counts every unique
// boundary edge, so it is stable whether or not neighbours were named); L —
// an unnamed face takes its sorted boundary-edge names joined with ',' plus
// ";L". OCC history is genuinely incomplete for lower-dimensional elements,
// so these are what give edges (fillet spines!) their stable names.
void ApplyFallbackPasses(const CADShape& shape, ElementMap& map);

// Why `name` does not resolve on `map`, as the error a feature reports:
// "<what> '<name>' was split by <feature> into '<a>' and '<b>'; pick the one
// you meant" when the face or edge it named is now several pieces (none, or
// more than one, bounded by all it was bounded by), else "<what> '<name>' no
// longer exists". `featureLabel` names the splitting feature ("Slot (4)");
// without one it is "feature 4". The ONE builder of that sentence:
// tests/test_conventions.cpp fails on the literal anywhere else.
enum class ElementKind { Face, Edge };
std::string
UnresolvedReason(const ElementMap& map, const std::string& name,
                 ElementKind kind, const std::string& what,
                 const std::function<std::string(uint64_t)>& featureLabel = {});
// The same for a reference during a regeneration: the bodies `ref.featureId`
// created are searched, and features are named from the context.
std::string UnresolvedReason(const RegenContext& ctx, const ElementRef& ref,
                             ElementKind kind, const std::string& what);
// The same over a list of bodies, features named by id.
std::string UnresolvedReason(const std::vector<Body>& bodies,
                             const ElementRef& ref, ElementKind kind,
                             const std::string& what);

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
