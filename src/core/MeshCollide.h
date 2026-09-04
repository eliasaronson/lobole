#pragma once

#include "cad/CADKernel.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

// Do two placed solids overlap? Asked of TRIANGLES, not of the B-Rep
// (agent-scale plan G3, docs/agent-scale-plan.md).
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
// and neither carries a millimetre constant that is wrong for the other.
namespace collide {

// A triangle soup with an AABB tree over it, built once per shape and reused
// for every pair: the tree is in the coordinates the shape was meshed in,
// and a query transforms boxes and triangles rather than the geometry.
class Mesh {
public:
  // Builds from `shape`. `deflectionFraction` multiplies the bounding
  // diagonal (0.002 = the same 0.2% the renderer meshes at). Empty when the
  // shape has no triangles.
  static std::shared_ptr<const Mesh> Build(const CADShape& shape,
                                           double deflectionFraction = 0.002);

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

  // Is `point` (in this mesh's local frame) inside the closed surface? A ray
  // parity count, so a body wholly inside another still counts as a clash
  // even though no triangle of theirs crosses.
  [[nodiscard]] bool Contains(const glm::dvec3& point) const;

private:
  struct Tri {
    glm::dvec3 a, b, c;
    glm::dvec3 lo, hi;
  };
  struct Node {
    glm::dvec3 lo{0.0}, hi{0.0};
    uint32_t start = 0, count = 0; // leaf range into m_order
    uint32_t left = 0, right = 0;  // 0 = none (the root is never a child)
  };

  uint32_t BuildNode(uint32_t start, uint32_t count, int depth);
  void Descend(uint32_t nodeA, const Mesh& other, uint32_t nodeB,
               const glm::dmat4& bToA, bool& hit) const;

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

// Convenience: build (or reuse) meshes for two shapes and test them.
// Cached by CADShape::PlacementId — geometry AND placement, because a rigid
// Transformed() shares the underlying shape — so a sweep over N occurrences
// meshes each placed body once rather than once per pair.
Result Overlap(const CADShape& a, const glm::dmat4& xfA, const CADShape& b,
               const glm::dmat4& xfB);

// The shared cache's current size, and a way to drop it (a document closing,
// or a test that wants a cold measurement).
size_t CacheSize();
void ClearCache();

} // namespace collide
