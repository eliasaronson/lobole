// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// An AABB tree over a TriMesh's triangles
// (docs/interchange/README.md#mesh-bodies): ray casts for picking, the closest
// surface point for a measure, the distance between two meshes, and the
// generalised winding number (Barill et al. 2018), which tells inside from
// outside for an OPEN mesh where a ray's parity is wrong. Pure data, no
// OpenCASCADE; a built tree is read-only, so any number of threads may query
// it.
//
// A median split, four triangles a leaf: stage 0 measured it at 0.9 µs a
// triangle to build (1.8 s for 2M on one thread) and a closest point that
// grows as log n (docs/plans/organic-meshes-and-scans.md).

#include "core/mesh/TriMesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace mesh {

class Bvh {
public:
  // Holds `mesh` alive; the tree is in the mesh's own coordinates.
  explicit Bvh(std::shared_ptr<const TriMesh> mesh);

  // The cached tree for `mesh` (the same shared mesh: a body's), built on
  // first use and kept while anything holds it, and for the last few built
  // besides, so a measure asked twice does not build twice. Thread-safe.
  static std::shared_ptr<const Bvh> For(std::shared_ptr<const TriMesh> mesh);

  [[nodiscard]] const TriMesh& Mesh() const { return *m_mesh; }
  [[nodiscard]] bool Empty() const { return m_nodes.empty(); }

  struct Hit {
    bool hit = false;
    double t = 0.0; // along the ray, in units of |dir|
    uint32_t triangle = kNone;
    glm::dvec3 point{0.0};
  };
  // The nearest triangle the ray origin + t·dir (t in [0, tMax]) crosses,
  // from either side (Möller–Trumbore).
  [[nodiscard]] Hit Ray(const glm::dvec3& origin, const glm::dvec3& dir,
                        double tMax = std::numeric_limits<double>::max()) const;

  struct Closest {
    bool found = false;
    double distance = 0.0;
    uint32_t triangle = kNone;
    glm::dvec3 point{0.0};
  };
  // The surface point nearest `p`; `found` false when none is within
  // `maxDistance`.
  [[nodiscard]] Closest
  ClosestPoint(const glm::dvec3& p,
               double maxDistance = std::numeric_limits<double>::max()) const;

  // The generalised winding number at `p`: 1 inside a closed outward mesh,
  // 0 outside, and in between near an open mesh's holes. First order far
  // field, exact solid angles near.
  [[nodiscard]] double Winding(const glm::dvec3& p) const;

  struct Gap {
    double distance = 0.0; // 0 when they meet
    glm::dvec3 onThis{0.0}, onOther{0.0};
  };
  // The distance between this mesh and `other` placed by `otherToThis`
  // (a rigid transform into this mesh's frame), exact to the triangles.
  [[nodiscard]] Gap Distance(const Bvh& other,
                             const glm::dmat4& otherToThis) const;

  // The closest point on triangle abc to p (Ericson 5.1.5).
  static glm::dvec3 ClosestOnTriangle(const glm::dvec3& p, const glm::dvec3& a,
                                      const glm::dvec3& b, const glm::dvec3& c);

private:
  struct Node {
    glm::dvec3 lo{0.0}, hi{0.0};
    uint32_t first = 0, count = 0; // a leaf: count > 0, its range in m_order
    uint32_t right = 0;            // an inner node: left is this + 1
    glm::dvec3 centroid{0.0}, areaNormal{0.0};
    double radius = 0.0;
  };
  uint32_t Build(std::vector<glm::dvec3>& centres, uint32_t begin,
                 uint32_t end);

  std::shared_ptr<const TriMesh> m_mesh;
  std::vector<uint32_t> m_order;
  std::vector<Node> m_nodes;
};

} // namespace mesh
