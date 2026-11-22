// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/Facing.h"

namespace mesh {

FacingSurface::FacingSurface(std::shared_ptr<const Bvh> tree)
    : m_tree(std::move(tree)) {
  const TriMesh& m = m_tree->Mesh();
  m_normals.resize(m.triangles.size(), glm::dvec3(0.0));
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    const glm::dvec3 n = m.AreaNormal(t);
    const double len = glm::length(n);
    if (len > 0.0)
      m_normals[t] = n / len;
  }
}

Bvh::Closest FacingSurface::Nearest(const glm::dvec3& p,
                                    const glm::dvec3& facing, double minCos,
                                    double maxDistance) const {
  if (facing == glm::dvec3(0.0))
    return m_tree->ClosestPoint(p, maxDistance);
  return m_tree->ClosestPoint(p, maxDistance, [&](uint32_t t) {
    return glm::dot(m_normals[t], facing) >= minCos;
  });
}

} // namespace mesh
