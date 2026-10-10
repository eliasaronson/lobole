// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

// Do two placed solids overlap? Asked of TRIANGLES, not of the B-Rep
// (docs/inspect/README.md#mesh-mode).
//
// The exact answer is a boolean, and a boolean is what `interference` and
// `contact_report` do: correct, and the reason a 253-occurrence arm takes
// 23 s to sweep and a single pair of imported servos measured 100 s. That is
// the right price for a final verdict and the wrong one for a question asked
// over and over — a motion study sweeps every pair at every one of 48 crank
// angles, which at exact prices is twenty minutes for a slider and a piston.
//
// So this answers the same question approximately and fast — measured 10x on
// a bored plate swept past a pin, 689 ms to 68 ms over 64 poses, with every
// pose agreeing — and says which method answered. It NEVER reports a distance:
// a distance from triangles is a distance to the tessellation, not to the part,
// and reporting one would invite exactly the false precision this is not for.
// The contract is:
//
//   * `true`  — the tessellations of A and B intersect, or one is inside the
//               other. At this deflection that means the solids do too, to
//               within the deflection.
//   * `false` — they do not. A pair separated by less than the deflection may
//               read either way; anything that must be certain re-runs the
//               exact test, which is what the motion study does on its worst
//               sample.
//
// Deflection is DERIVED from each body's own size (its bounding diagonal),
// so a 2 mm screw and a 2 m dock are meshed to the same relative fidelity
// and neither carries a millimetre constant that is wrong for the other. A
// caller that knows the gap it is deciding (a motion study's
// clash_deflection_mm) can ask for a finer one with Mesh::Build.
namespace collide {

// A triangle soup with an AABB tree over it, built once per shape and reused
// for every pair: the tree is in the coordinates the shape was meshed in,
// and a query transforms boxes and triangles rather than the geometry.
class Mesh {
public:
  // Builds from `shape`, tessellated at `deflectionMm`. 0 derives it from
  // the shape's own size -- 0.2% of its bounding diagonal, the triangulation
  // the renderer already holds, so it costs nothing new. A stated deflection
  // is meshed on a COPY of the shape: a finer triangulation written into the
  // document's shape would become the one every renderer and export reads
  // (CADShape::GenerateMeshDetached). Empty when the shape has no triangles.
  static std::shared_ptr<const Mesh> Build(const CADShape& shape,
                                           double deflectionMm = 0.0);

  [[nodiscard]] bool Empty() const { return m_tris.empty(); }
  [[nodiscard]] size_t TriangleCount() const { return m_tris.size(); }
  // The deflection this was actually meshed at, in millimetres — reported so
  // a caller can say how close "clear" was decided.
  [[nodiscard]] double Deflection() const { return m_deflection; }
  [[nodiscard]] const glm::dvec3& Min() const { return m_min; }
  [[nodiscard]] const glm::dvec3& Max() const { return m_max; }

  // Does this mesh, placed by `xfA`, overlap `other` placed by `xfB`?
  [[nodiscard]] bool Overlaps(const glm::dmat4& xfA, const Mesh& other,
                              const glm::dmat4& xfB) const;

  // How far apart they are, and WHICH B-Rep faces are within `slack` of that.
  //
  // The distance is to the tessellations, so it is the part's distance to
  // within the deflection and this module still refuses to hand it out as a
  // measurement (see the header note). The faces are what it is for: they
  // narrow an exact B-Rep query from every face against every other one to
  // the two or three that can hold the answer, which is the difference
  // between 55 s and a few milliseconds on two lofted propellers.
  struct FacePair {
    uint32_t a = 0, b = 0;
    double distance = 0.0; // between their TRIANGLES
  };
  struct Near {
    double distance = 0.0; // 0 when they intersect
    // The face pairs within `slack` of it, NEAREST FIRST. Pairs, not two
    // sets of faces: fifteen faces a side is 225 combinations to ask an
    // exact query about, and only about twenty of them were ever close.
    std::vector<FacePair> pairs;
  };
  [[nodiscard]] Near Nearest(const glm::dmat4& xfA, const Mesh& other,
                             const glm::dmat4& xfB, double slack) const;

  // Is `point` (in this mesh's local frame) inside the closed surface? A ray
  // parity count, so a body wholly inside another still counts as a clash
  // even though no triangle of theirs crosses.
  [[nodiscard]] bool Contains(const glm::dvec3& point) const;

private:
  struct Tri {
    glm::dvec3 a, b, c;
    glm::dvec3 lo, hi;
    uint32_t face = 0; // the B-Rep face it was tessellated from
  };
  struct Node {
    glm::dvec3 lo{0.0}, hi{0.0};
    uint32_t start = 0, count = 0; // leaf range into m_order
    uint32_t left = 0, right = 0;  // 0 = none (the root is never a child)
  };

  uint32_t BuildNode(uint32_t start, uint32_t count, int depth);
  void Descend(uint32_t nodeA, const Mesh& other, uint32_t nodeB,
               const glm::dmat4& bToA, bool& hit) const;
  // One recursion, two jobs: with `faces` null it MINIMISES (pruning against
  // the best found so far, which shrinks); with `faces` it COLLECTS every
  // face pair within the fixed `limit`.
  void Walk(uint32_t nodeA, const Mesh& other, uint32_t nodeB,
            const glm::dmat4& bToA, double limit2, double& best2,
            std::map<std::pair<uint32_t, uint32_t>, double>* pairs) const;

  std::vector<Tri> m_tris;
  std::vector<uint32_t> m_order;
  std::vector<Node> m_nodes;
  glm::dvec3 m_min{0.0}, m_max{0.0};
  double m_deflection = 0.0;
};

// The result of one pair test.
struct Result {
  bool intersects = false;
  double deflection = 0.0; // the coarser of the two meshes', mm
};

// What a pair's mesh distance says, and how coarsely.
struct Gap {
  bool known = false;      // false when either shape has no triangles
  double distance = 0.0;   // to the TESSELLATIONS, +/- `tolerance`
  double deflection = 0.0; // the coarser of the two meshes', mm
  // What |mesh - exact| is actually bounded by: the SUM, not the coarser.
  // Each tessellation lies within its own deflection of its surface and the
  // two errors can point opposite ways, so a caller deciding "is there
  // 2 mm of clearance" has to subtract both.
  double tolerance = 0.0;
  std::vector<Mesh::FacePair> pairs;
};

// The same, from shapes, through the same cache. `slackDeflections` widens
// the face-pair net in multiples of the deflection: a caller refining the
// answer exactly needs at least TWO, since the tessellation can be that
// much nearer or further than the surface it stands for.
Gap Nearest(const CADShape& a, const glm::dmat4& xfA, const CADShape& b,
            const glm::dmat4& xfB, double slackDeflections);

// Convenience: build (or reuse) meshes for two shapes and test them.
// Cached by CADShape::PlacementId — geometry AND placement, because a rigid
// Transformed() shares the underlying shape — so a sweep over N occurrences
// meshes each placed body once rather than once per pair.
Result Overlap(const CADShape& a, const glm::dmat4& xfA, const CADShape& b,
               const glm::dmat4& xfB);

// Builds (or finds) `shape`'s cached mesh now. A sweep calls it for every
// body, single-threaded, before its workers start: building a mesh writes a
// triangulation onto the shape's TShape, and a worker running extrema or a
// boolean on that shape must not meet a mesher (core/Parallel.h).
void Warm(const CADShape& shape);

// Drops `shape`'s cached mesh. For a caller that meshed its own short-lived
// copies (an async contact_report's): the cache keys on the TShape's
// address, and a copy freed with its entry still cached would hand that
// mesh to whatever shape is allocated there next.
void Forget(const CADShape& shape);

// The shared cache's current size, and a way to drop it (a document closing,
// or a test that wants a cold measurement).
size_t CacheSize();
void ClearCache();

} // namespace collide
