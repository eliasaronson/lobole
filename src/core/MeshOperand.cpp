// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MeshOperand.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace meshop {

double DerivedDeflectionMm(const Body& body) {
  glm::dvec3 lo, hi;
  if (!BodyBounds(body, lo, hi))
    return 0.0;
  return std::max(glm::length(hi - lo) * 1e-3, 1e-6);
}

std::shared_ptr<const mesh::TriMesh>
Triangles(const Body& body, double deflectionMm, double& used) {
  used = 0.0;
  if (body.IsMesh()) {
    if (!body.mesh->neighbour.empty() || body.mesh->triangles.empty())
      return body.mesh;
    auto copy = std::make_shared<mesh::TriMesh>(*body.mesh);
    mesh::BuildAdjacency(*copy);
    return copy;
  }
  if (!body.shape.IsValid())
    return std::make_shared<const mesh::TriMesh>();
  used = deflectionMm > 0.0 ? deflectionMm : DerivedDeflectionMm(body);
  // On a copy: a finer triangulation left on the document's shape would be
  // the one every renderer draws from then on.
  const CADShape::Mesh m = body.shape.GenerateMeshDetached(used);
  // One block of vertices a face: welded, so the faces' shared edges join.
  std::vector<glm::dvec3> soup;
  soup.reserve(m.indices.size());
  for (uint32_t i : m.indices)
    soup.emplace_back(m.vertices[i].position);
  return std::make_shared<const mesh::TriMesh>(
      mesh::FromSoup(soup, mesh::Float32Quantum(soup)));
}

std::string OpenOperandRefusal(const mesh::TriMesh& m, const std::string& body,
                               const std::string& what) {
  if (m.Closed())
    return "";
  std::string why;
  if (m.boundaryEdges != 0) {
    const std::vector<double> loops = mesh::BoundaryLoops(m);
    char longest[64];
    std::snprintf(longest, sizeof longest, "%.1f",
                  loops.empty() ? 0.0 : loops.front());
    why = "it is open: " + std::to_string(loops.size()) + " boundary loop" +
          (loops.size() == 1 ? "" : "s") + ", the longest " + longest + " mm";
  } else if (m.nonManifoldEdges != 0) {
    why = std::to_string(m.nonManifoldEdges) +
          " edges are shared by three or more triangles";
  } else if (m.flippedEdges != 0) {
    why = std::to_string(m.flippedEdges) +
          " edges join triangles facing opposite ways";
  } else {
    why = "it has no triangles";
  }
  return what + " needs a closed solid, and body '" + body +
         "' is not: " + why +
         ". Close it in MeshLab, Blender or the scanning app, or cut a "
         "holder from it with a Cradle, which takes an open scan";
}

} // namespace meshop
