#pragma once

#include "CADKernel.h"
#include "SketchPlane.h"
#include "core/ElementMap.h"
#include "core/SketchFeature.h"

#include <TopoDS_Wire.hxx>

#include <cstdint>
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
  uint32_t entityId = 0;               // stable id of the generating sketch entity
};

// One closed loop of a sketch profile, in plane UV space.
struct Loop {
  std::optional<Circle> circle;   // set: a pure circle loop
  std::optional<Ellipse> ellipse; // set: a pure ellipse loop
  std::vector<LoopEdge> edges;    // else: ordered boundary edges (lines + arcs)

  // Shoelace area (sign = winding); circles are positive.
  double SignedArea() const;
};

// Extracts closed loops: circles and rectangles are loops by construction;
// free Line entities are chained by endpoint proximity (tolerance 1e-4) into
// closed loops. Open chains are discarded with a warning.
std::vector<Loop> BuildLoops(const std::vector<Entity>& entities);

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

// Builds the profile face (largest-|area| loop is the outer boundary, the
// rest become holes) and extrudes it along the plane normal. Returns an
// invalid CADShape on failure.
CADShape BuildPrism(const SketchPlane& plane, const std::vector<Loop>& loops,
                    double distance, bool reversed);

// Like BuildPrism, but also fills `out` with stable names for the prism's
// faces (topological naming, T2): each side face is named
// "<featureTag>/g<entityId>" from the sketch entity that generated it, the two
// caps "<featureTag>/cap0" (base) and "cap1" (top). The names key on the
// faceIndex (TopExp_Explorer order) that GenerateMesh and picking use, so they
// survive upstream sketch edits. `loops` must carry entity ids (see BuildLoops).
CADShape BuildPrismNamed(const SketchPlane& plane,
                         const std::vector<Loop>& loops, double distance,
                         bool reversed, uint64_t featureTag, ElementMap& out);

// Which side of the profile curve a thin extrude's wall grows toward.
enum class ThinSide { Out, In, Both };

// Thin/wall extrude (M3.4): every closed loop becomes a hollow wall ring of
// `thickness` (offset via OCC MakeOffset, arc-joined corners), swept
// `distance`. Loops are walled individually — holes become walls too, and
// grouping does not apply. Caps are named per loop ("cap0@g<entityId>" when
// several); wall side faces get names via the fallback passes. Invalid
// CADShape when an offset self-intersects or thickness <= 0.
CADShape BuildThinPrismNamed(const SketchPlane& plane,
                             const std::vector<Loop>& loops, double distance,
                             bool reversed, double thickness, ThinSide side,
                             uint64_t featureTag, ElementMap& out);

// Builds the profile face and revolves it about the given world-space axis
// (origin + unit direction) through `angleRadians`. `reversed` sweeps the
// opposite hand; `symmetric` centers the sweep on the profile plane (half the
// angle each way — ignored at a full revolution and moot when reversed). The
// axis must lie in the sketch plane and the profile must not cross it; a
// self-intersecting or degenerate result is rejected (invalid CADShape).
CADShape BuildRevolve(const SketchPlane& plane, const std::vector<Loop>& loops,
                      const glm::vec3& axisOrigin, const glm::vec3& axisDir,
                      double angleRadians, bool reversed, bool symmetric);

// Like BuildRevolve, but also fills `out` with stable names (topological
// naming, T5): side faces "<featureTag>/g<entityId>" from the generating
// sketch entity, the two planar ends of a partial revolve "cap0"/"cap1" (a
// full revolution has none), degenerate pole edges at the axis
// "<featureTag>/pole<n>", and the U/L fallback passes for everything else.
CADShape BuildRevolveNamed(const SketchPlane& plane,
                           const std::vector<Loop>& loops,
                           const glm::vec3& axisOrigin,
                           const glm::vec3& axisDir, double angleRadians,
                           bool reversed, bool symmetric, uint64_t featureTag,
                           ElementMap& out);

// Tessellates one entity into a UV polyline for rendering/preview: Line = 2
// points, Circle = 33 (closed, 32 segments), Point = empty (rendered as a
// marker elsewhere).
std::vector<glm::vec2> TessellateEntity(const Entity& entity);

// If segments [a1,b1] and [a2,b2] properly cross, returns the crossing's
// parameter t in [0,1] along the *first* segment; nullopt if parallel or the
// crossing lies outside either segment. Used to find trim cut points.
std::optional<float> SegmentIntersect(glm::vec2 a1, glm::vec2 b1, glm::vec2 a2,
                                      glm::vec2 b2);

// Offsets a single closed loop formed by `chain` (lines + arcs) by `distance`
// (signed) on the given plane, delegating corner treatment to OCCT
// (BRepOffsetAPI_MakeOffset: `joinArc` = arcs at convex corners, else
// extend/trim to the intersection). Returns the offset boundary as new sketch
// entities in plane-UV, or empty on failure or if `chain` isn't one loop.
std::vector<Entity> OffsetLoop(const SketchPlane& plane,
                               const std::vector<Entity>& chain, double distance,
                               bool joinArc);

// Region selection (U2): a region is referenced by the sorted, deduplicated
// entity ids of its OUTER loop's boundary — stable across sketch edits that
// don't touch the boundary; a vanished loop is a first-class regen failure.
std::vector<uint32_t> LoopEntityIds(const Loop& loop);

// Even-odd point-in-loop test in plane UV (arcs sampled to segments).
bool PointInLoop(const Loop& loop, const glm::vec2& p);

// The loop's boundary as a sampled UV polygon (region-pick overlays).
std::vector<glm::vec2> LoopBoundaryPolygon(const Loop& loop);

// Projects a body face's boundary edges into plane-UV sketch entities
// (construction): lines and full circles map exactly, circular arcs map to
// arcs, other curve types are skipped. Used when sketching ON a face so its
// outline is snappable and dimensionable reference geometry. Ids are 0 —
// the caller adds them via AddEntity.
std::vector<Entity> ProjectFaceBoundary(const CADShape& shape, int faceIndex,
                                        const SketchPlane& plane);

// Keeps only the loops whose id set matches one of `regions`, plus every
// loop nested inside a kept one (holes ride along with their outer). Returns
// false — leaving `loops` untouched — when a region matches no current loop.
bool FilterLoopsToRegions(std::vector<Loop>& loops,
                          const std::vector<std::vector<uint32_t>>& regions);

} // namespace sketch
