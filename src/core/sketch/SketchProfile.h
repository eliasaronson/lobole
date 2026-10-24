// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"
#include "cad/SketchPlane.h"
#include "core/ElementMap.h"
#include "core/features/SketchFeature.h"

#include <TopoDS_Wire.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sketch {

// One boundary edge of a profile loop, in plane UV space. Traversal runs from
// `a` to `b`. Arcs also carry their canonical geometry (center/radius and the
// CCW angle span from `angStart` to `angEnd`), independent of traversal
// direction — the entity defines a specific arc; ordering/orientation is fixed
// up by ShapeFix_Wire at build time.
struct LoopEdge {
  bool arc = false;
  glm::dvec2 a{0.0}, b{0.0};
  glm::dvec2 center{0.0};
  double radius = 0.0;
  double angStart = 0.0, angEnd = 0.0; // CCW span (arc only)
  uint32_t entityId = 0; // stable id of the generating sketch entity
  // Set when this edge is a B-spline: its control poles in traversal order
  // (a = poles.front(), b = poles.back()). Kept last so existing positional
  // brace-inits of line/arc edges stay valid. When BuildLoops reverses the
  // edge to fit the walk, the poles are reversed too.
  std::optional<BSpline> spline;
  // Set when this edge is an elliptical arc. Like circular arcs it keeps its
  // canonical (startParam→endParam) sweep; traversal direction is resolved by
  // the forward[] test in MakeLoopWire, so a/b may be swapped without touching
  // the params.
  std::optional<EllipseArc> ellipseArc;
  // Set (>= 0) when this edge is one segment of a TEXT label's glyph: the
  // glyph (counted over rendered characters only, so a space renumbers
  // nothing), the contour within it and the segment within that. A label is
  // ONE entity, so these are what keep its side faces apart: they are named
  // "<feature>/g<entityId>.<glyph>.<contour>.<seg>" (LoopEdgeName).
  int32_t glyph = -1;
  uint32_t contour = 0;
  uint32_t seg = 0;
};

// The face-name word of the entity edge `e` sweeps into: "g<entityId>", or
// "g<entityId>.<glyph>.<contour>.<seg>" for a text segment
// (docs/modeling/toponaming.md).
std::string LoopEdgeName(const LoopEdge& e);

// One closed loop of a sketch profile, in plane UV space.
struct Loop {
  std::optional<Circle> circle;   // set: a pure circle loop
  std::optional<Ellipse> ellipse; // set: a pure ellipse loop
  std::vector<LoopEdge> edges;    // else: ordered boundary edges (lines + arcs)

  // Shoelace area (sign = winding); circles are positive.
  double SignedArea() const;
};

// Extracts closed loops from the non-construction entities: circles,
// ellipses and self-closed B-splines are loops by construction; lines, arcs,
// ellipse arcs and open B-splines are chained by endpoint proximity
// (tolerance 1e-4) into closed loops. Open chains are discarded with a
// warning. An outline text label contributes each glyph contour as a loop of
// its own (never chained with other geometry); a single-stroke label is OPEN
// curves and contributes nothing, silently — it is for engraving
// (docs/sketching/README.md#text).
std::vector<Loop> BuildLoops(const std::vector<Entity>& entities);

// Where a profile fails to close (docs/sketching/README.md#open-ends): every
// endpoint of the chainable non-construction edges (lines, arcs, elliptical
// arcs, open splines), clustered with BuildLoops' tolerance and counted. An
// end met once is OPEN; one met three or more times (a T, an overlap) also
// stops a clean profile and is listed apart. Two open ends that are each
// other's nearest, within 2% of the geometry's extent, are the usual "almost
// closed" pair and come back as a gap too.
struct OpenEndsResult {
  std::vector<glm::dvec2> open;
  std::vector<glm::dvec2> branched;
  std::vector<std::pair<glm::dvec2, glm::dvec2>> gaps;
  bool Empty() const { return open.empty() && branched.empty(); }
};
OpenEndsResult OpenEnds(const std::vector<Entity>& entities);
// "2 open ends at (10, 0) and (10, 0.3)" -- what a refused extrude appends to
// "sketch has no closed profile". Empty when there are none.
std::string OpenEndsWords(const OpenEndsResult& ends);

// The outer boundary wire of a single-profile sketch, in world space on the
// plane — the section input for a loft (modeling M8). Null wire when the
// sketch has no single outer profile (empty, or several disjoint outers).
TopoDS_Wire BuildProfileWire(const SketchPlane& plane,
                             const std::vector<Loop>& loops);

// An OPEN chain of a sketch's non-construction line/arc entities as a world-
// space wire — the spine for a sweep (modeling M8). Chains edges by endpoint
// proximity like BuildLoops, but keeps the result open (a closed chain, a
// lone circle, or a branching/disconnected set returns a null wire).
TopoDS_Wire BuildOpenPathWire(const SketchPlane& plane,
                              const std::vector<Entity>& entities);

// Builds the profile faces — loops grouped by containment depth: a loop
// inside an even number of others is an outer boundary, inside an odd number
// a hole of its innermost container — and extrudes them along the plane
// normal. Returns an invalid CADShape on failure.
CADShape BuildPrism(const SketchPlane& plane, const std::vector<Loop>& loops,
                    double distance, bool reversed);

// Like BuildPrism, but also fills `out` with stable names for the prism's
// faces (topological naming, T2): each side face is named
// "<featureTag>/g<entityId>" from the sketch entity that generated it, the two
// caps "<featureTag>/cap0" (base) and "cap1" (top). The names key on the
// faceIndex (TopExp_Explorer order) that GenerateMesh and picking use, so they
// survive upstream sketch edits. `loops` must carry entity ids (see
// BuildLoops). On failure `error` (optional) receives the reason in
// user-grade prose for Feature::SetError.
CADShape BuildPrismNamed(const SketchPlane& plane,
                         const std::vector<Loop>& loops, double distance,
                         bool reversed, uint64_t featureTag, ElementMap& out,
                         std::string* error = nullptr);

// A twisted extrude (docs/modeling/README.md#twisted-extrudes): each profile
// group swept `distance` along the plane normal (negated when `reversed`)
// while it turns `twistRad` about the normal through the plane's origin,
// right-handed about the direction of travel when positive — one screw
// motion, so the side faces are helicoids, one per sketch edge, named as
// BuildPrismNamed names a prism's. `herringbone` climbs the first half at
// that rate and the second half back (the lower half mirrored in the
// mid-plane, glue-fused): its upper side faces are "<tag>/g<id>@h2", its
// ends cap0 and cap1. `symmetric` centres it on the sketch plane, turned so
// the section there is the sketch. `error` (optional) receives the reason.
CADShape BuildTwistedPrismNamed(const SketchPlane& plane,
                                const std::vector<Loop>& loops, double distance,
                                bool reversed, double twistRad,
                                bool herringbone, bool symmetric,
                                uint64_t featureTag, ElementMap& out,
                                std::string* error = nullptr);

// Which side of the profile curve a thin extrude's wall grows toward.
enum class ThinSide { Out, In, Both };

// Thin/wall extrude (M3.4): every closed loop becomes a hollow wall ring of
// `thickness` (offset via OCC MakeOffset, arc-joined corners), swept
// `distance`. Loops are walled individually — holes become walls too, and
// grouping does not apply. Caps are named per loop ("cap0@g<entityId>" when
// several); wall side faces get names via the fallback passes. Invalid
// CADShape when an offset self-intersects or thickness <= 0; `error`
// (optional) receives the reason.
CADShape BuildThinPrismNamed(const SketchPlane& plane,
                             const std::vector<Loop>& loops, double distance,
                             bool reversed, double thickness, ThinSide side,
                             uint64_t featureTag, ElementMap& out,
                             std::string* error = nullptr);

// Builds the profile face and revolves it about the given world-space axis
// (origin + unit direction) through `angleRadians`. `reversed` sweeps the
// opposite hand; `symmetric` centers the sweep on the profile plane (half the
// angle each way — ignored at a full revolution and moot when reversed). The
// axis must lie in the sketch plane and the profile must not cross it; a
// self-intersecting or degenerate result is rejected (invalid CADShape).
CADShape BuildRevolve(const SketchPlane& plane, const std::vector<Loop>& loops,
                      const glm::dvec3& axisOrigin, const glm::dvec3& axisDir,
                      double angleRadians, bool reversed, bool symmetric);

// Like BuildRevolve, but also fills `out` with stable names (topological
// naming, T5): side faces "<featureTag>/g<entityId>" from the generating
// sketch entity, the two planar ends of a partial revolve "cap0"/"cap1" (a
// full revolution has none), degenerate pole edges at the axis
// "<featureTag>/pole<n>", and the U/L fallback passes for everything else.
// On failure `error` (optional) receives the reason.
CADShape BuildRevolveNamed(const SketchPlane& plane,
                           const std::vector<Loop>& loops,
                           const glm::dvec3& axisOrigin,
                           const glm::dvec3& axisDir, double angleRadians,
                           bool reversed, bool symmetric, uint64_t featureTag,
                           ElementMap& out, std::string* error = nullptr);

// Tessellates one entity into a UV polyline for rendering/preview: Line = 2
// points, Circle = 33 (closed, 32 segments), Point = empty (rendered as a
// marker elsewhere).
std::vector<glm::dvec2> TessellateEntity(const Entity& entity);

// Every polyline an entity draws as: TessellateEntity's one for most kinds,
// one per glyph contour for a text label (whose glyphs must not be joined).
// A label that cannot be laid out draws nothing.
std::vector<std::vector<glm::dvec2>>
TessellateEntityPolylines(const Entity& entity);

// If segments [a1,b1] and [a2,b2] properly cross, returns the crossing's
// parameter t in [0,1] along the *first* segment; nullopt if parallel or the
// crossing lies outside either segment. Used to find trim cut points.
std::optional<double> SegmentIntersect(glm::dvec2 a1, glm::dvec2 b1,
                                       glm::dvec2 a2, glm::dvec2 b2);

// Offsets a single closed loop formed by `chain` (lines + arcs) by `distance`
// (signed) on the given plane, delegating corner treatment to OCCT
// (BRepOffsetAPI_MakeOffset: `joinArc` = arcs at convex corners, else
// extend/trim to the intersection). Returns the offset boundary as new sketch
// entities in plane-UV, or empty on failure or if `chain` isn't one loop.
std::vector<Entity> OffsetLoop(const SketchPlane& plane,
                               const std::vector<Entity>& chain,
                               double distance, bool joinArc);

// Region selection (U2): a region is referenced by the sorted, deduplicated
// entity ids of its OUTER loop's boundary — stable across sketch edits that
// don't touch the boundary; a vanished loop is a first-class regen failure.
std::vector<uint32_t> LoopEntityIds(const Loop& loop);

// Even-odd point-in-loop test in plane UV (arcs sampled to segments).
bool PointInLoop(const Loop& loop, const glm::dvec2& p);

// The loop's boundary as a sampled UV polygon (region-pick overlays).
std::vector<glm::dvec2> LoopBoundaryPolygon(const Loop& loop);

// Projects a body face's boundary edges into plane-UV sketch entities
// (construction): lines and full circles map exactly, circular arcs map to
// arcs, other curve types are skipped. Used when sketching ON a face so its
// outline is snappable and dimensionable reference geometry. Ids are 0 —
// the caller adds them via AddEntity.
std::vector<Entity> ProjectFaceBoundary(const CADShape& shape, int faceIndex,
                                        const SketchPlane& plane);

// Projects a body FACE (its boundary edges) or a single EDGE onto the sketch
// plane as construction entities — the geometry behind live external geometry
// (SketchFeature::external). `index` is the 0-based face/edge index (the
// TopExp::MapShapes order the ElementMap uses). Coplanar-parallel curves keep
// their exact type (line/circle/arc/ellipse); anything else — an out-of-plane
// curve whose shadow changes type, or a B-spline — is sampled into a B-spline.
// Ids are 0; the caller assigns them.
std::vector<Entity> ProjectElement(const CADShape& shape, ExternalKind kind,
                                   int index, const SketchPlane& plane);

// Why a profile consumer (extrude, revolve) got nothing from single-stroke
// text, naming the label and its font: a region that picks one, or a sketch
// whose only would-be profile is one. Empty when no stroke text is involved.
// Stroke text is open curves for engraving and is otherwise skipped silently
// (docs/sketching/README.md#text).
std::string
StrokeTextProfileRefusal(const std::vector<Entity>& entities,
                         const std::vector<std::vector<uint32_t>>& regions,
                         bool noLoops);

// Keeps only the loops whose id set matches one of `regions`, plus every
// loop nested inside a kept one (holes ride along with their outer). Returns
// false — leaving `loops` untouched — when a region matches no current loop.
bool FilterLoopsToRegions(std::vector<Loop>& loops,
                          const std::vector<std::vector<uint32_t>>& regions);

} // namespace sketch
