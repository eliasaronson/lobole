// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// An indexed triangle mesh, the foundation of src/core/mesh/
// (docs/interchange/README.md#recognising-a-faceted-mesh). Pure data and no
// OpenCASCADE, so any of it can run on a worker thread.
//
// Positions are double (the model is double, docs/conventions.md); a file's
// float32 values convert exactly. `quantum` is the precision the FILE was
// written at -- the float32 spacing at its largest coordinate, or an ASCII
// file's last printed digit -- and every tolerance the fits and the
// segmentation use is a small multiple of it: a vertex of an exported CAD
// mesh lies on its true surface to within that, wherever the part sits and
// whatever its size.

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mesh {

inline constexpr uint32_t kNone = 0xffffffffu;

struct TriMesh {
  std::vector<glm::dvec3> positions;
  std::vector<std::array<uint32_t, 3>> triangles;
  // The precision the file was written at, in its own units (see above).
  double quantum = 0.0;

  // Filled by BuildAdjacency(). For triangle t's edge e (from corner e to
  // corner e+1 mod 3): the triangle across it and that triangle's edge, or
  // kNone where the edge is a boundary or is shared by more than two.
  std::vector<std::array<uint32_t, 3>> neighbour;
  std::vector<std::array<uint8_t, 3>> neighbourEdge;
  size_t boundaryEdges = 0;    // edges on one triangle
  size_t nonManifoldEdges = 0; // edges on three or more
  size_t flippedEdges = 0;     // pairs walking an edge the same way

  [[nodiscard]] size_t TriangleCount() const { return triangles.size(); }
  // Edge-manifold, consistently oriented and without a boundary: a solid.
  [[nodiscard]] bool Closed() const {
    return !neighbour.empty() && boundaryEdges == 0 && nonManifoldEdges == 0 &&
           flippedEdges == 0;
  }
  [[nodiscard]] glm::dvec3 Corner(uint32_t t, int c) const {
    return positions[triangles[t][c]];
  }
  // Unnormalised (twice the area long), right-handed about the corners.
  [[nodiscard]] glm::dvec3 AreaNormal(uint32_t t) const;
  [[nodiscard]] double Area(uint32_t t) const;
  // Signed, by the divergence theorem: positive for an outward-facing closed
  // mesh. Meaningful only when Closed().
  [[nodiscard]] double Volume() const;
  [[nodiscard]] double SurfaceArea() const;
  void Bounds(glm::dvec3& lo, glm::dvec3& hi) const;
  // A hash of the positions and the triangles, which is the mesh's identity.
  [[nodiscard]] uint64_t Hash() const;
};

// Welds a triangle soup -- three points a triangle, in file order -- into a
// TriMesh: points equal to the bit are one vertex first (a CAD exporter
// writes a shared vertex identically), then points within `quantum` of one
// another. A triangle whose corners weld together is dropped. Builds the
// adjacency.
TriMesh FromSoup(const std::vector<glm::dvec3>& soup, double quantum);

// Fills the neighbour tables and the edge counts above.
void BuildAdjacency(TriMesh& m);

// Each triangle's connected component (across shared edges), numbered from
// 0 in order of each component's first triangle. Returns the count.
size_t Components(const TriMesh& m, std::vector<uint32_t>& componentOf);

// The float32 spacing at the largest coordinate of `positions`: what a
// binary STL can say.
double Float32Quantum(const std::vector<glm::dvec3>& positions);

} // namespace mesh
