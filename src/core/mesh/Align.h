// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What placing a scan reads off its triangles
// (docs/interchange/README.md#placing-an-import): the plane a picked point
// lies on -- a flat grown from it, or the rim of the hole an open scan has
// where the table was -- and the surface's principal axes. Pure data, no
// OpenCASCADE.

#include "core/mesh/Fit.h"
#include "core/mesh/TriMesh.h"

#include <cstdint>
#include <vector>

namespace mesh {

struct PlaneRegion {
  FitResult fit; // the plane: a point on it, its unit normal, rms and max
  // Grown: the region's triangles (ascending) and their area.
  std::vector<uint32_t> triangles;
  double areaMm2 = 0.0;
  // From a rim: the boundary loop's vertices, in order.
  std::vector<uint32_t> loop;
  [[nodiscard]] bool FromLoop() const { return !loop.empty(); }
};

// The plane of the flat triangle `seed` (adjacency built) lies in: the
// region GrowFrom grows for a plane from it at `toleranceMm` -- a disk round
// the seed fitted robustly (FitRobust), grown across edges while every
// corner lies within the tolerance and the triangle faces the same side,
// then grown once more against the final fit -- so a stray lump the region
// took in does not tilt it. When `seed` has an edge on a boundary, the
// plane is fitted to that boundary loop instead: a scan of an object on a
// table is open where the table was, and the rim of that hole is its
// floor. A rim's plane is least squares, since a rim is ragged by about an
// edge length all the way round and has no band of inliers to find.
PlaneRegion GrowPlane(const TriMesh& m, uint32_t seed, double toleranceMm);

// The boundary loop through one of triangle `t`'s boundary edges, its
// vertices in order; empty when `t` has none.
std::vector<uint32_t> BoundaryLoopAt(const TriMesh& m, uint32_t t);

// The area-weighted moments of a surface, accumulated over meshes: the
// centroid and the covariance of a uniform sheet over the triangles, which
// an open scan has as a closed one does (it has no volume to weigh).
struct SurfaceMoments {
  double area = 0.0;
  glm::dvec3 first{0.0};  // ∫ x dA
  glm::dmat3 second{0.0}; // ∫ x xᵀ dA
  void Add(const TriMesh& m);
  [[nodiscard]] glm::dvec3 Centroid() const;
  [[nodiscard]] glm::dmat3 Covariance() const;
};

// The smallest rotation taking unit `from` onto unit `to`.
glm::dmat3 RotationBetween(const glm::dvec3& from, const glm::dvec3& to);

} // namespace mesh
