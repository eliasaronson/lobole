// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The nearest surface that faces a given way
// (docs/interchange/README.md#laying-a-scan-onto-a-body): a point of a scan
// is matched to the nearest triangle of a body whose normal is within an
// angle of the scan's own there. On a thin wall the nearest surface past
// half the thickness is the far side, which faces the other way; matched
// by distance alone, a plate laid on its back scores as well as one the
// right way up. Pure data, no OpenCASCADE; read-only once built, so any
// number of threads may query one.

#include "core/mesh/MeshBvh.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace mesh {

class FacingSurface {
public:
  // Over the triangles of `tree`'s mesh; their unit normals computed once
  // (a degenerate triangle's is zero and faces nothing).
  explicit FacingSurface(std::shared_ptr<const Bvh> tree);

  [[nodiscard]] const Bvh& Tree() const { return *m_tree; }
  [[nodiscard]] const glm::dvec3& Normal(uint32_t triangle) const {
    return m_normals[triangle];
  }

  // The surface point nearest `p` on a triangle whose unit normal n has
  // n · `facing` >= `minCos` (`facing` a unit vector), within
  // `maxDistance`; `found` false when there is none. `facing` zero admits
  // every triangle (the nearest surface, whichever way it faces).
  [[nodiscard]] Bvh::Closest Nearest(const glm::dvec3& p,
                                     const glm::dvec3& facing, double minCos,
                                     double maxDistance) const;

private:
  std::shared_ptr<const Bvh> m_tree;
  std::vector<glm::dvec3> m_normals;
};

} // namespace mesh
