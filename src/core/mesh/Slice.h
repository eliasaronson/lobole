// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A plane through a TriMesh (docs/interchange/README.md#mesh-bodies): the
// polylines it cuts, chained exactly -- each crossing point is keyed on the
// mesh EDGE it lies on, so two triangles sharing an edge share the point
// and no tolerance joins anything -- and, for a closed mesh, the cap that
// fills them. Pure data, no OpenCASCADE.

#include "core/mesh/TriMesh.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace mesh {

struct SlicePolyline {
  std::vector<glm::dvec3> points; // a closed loop does not repeat its first
  bool closed = false;
  // Closed loops of a closed, outward mesh run counter-clockwise about the
  // plane's normal round material and clockwise round a hole, so the
  // signed area below is positive for an outer loop.
  double signedArea = 0.0; // mm², in the plane
  double length = 0.0;     // mm
};

struct SliceResult {
  std::vector<SlicePolyline> polylines;
  // The material's area in the plane: the sum of the closed loops' signed
  // areas. Meaningful when every loop is closed (`Closed()`).
  double area = 0.0;
  [[nodiscard]] bool Closed() const {
    for (const SlicePolyline& p : polylines)
      if (!p.closed)
        return false;
    return !polylines.empty();
  }
};

// The plane through `origin` with normal `normal` (any length) cutting
// `m`. A vertex exactly on the plane counts as above it (simulation of
// simplicity), so every crossing triangle cuts one segment.
SliceResult Slice(const TriMesh& m, const glm::dvec3& origin,
                  const glm::dvec3& normal);

// The cap of a closed slice: its loops triangulated (holes as holes), in
// the plane, facing along `normal`. Empty when the slice is not closed or
// the triangulation fails.
struct SliceCap {
  std::vector<glm::dvec3> positions;
  std::vector<std::array<uint32_t, 3>> triangles;
};
SliceCap Cap(const SliceResult& slice, const glm::dvec3& origin,
             const glm::dvec3& normal);

// Douglas-Peucker: drops points while the polyline stays within
// `tolerance` of the original.
std::vector<glm::dvec3> Simplify(const std::vector<glm::dvec3>& points,
                                 bool closed, double tolerance);

// The plane's in-plane axes: u × v = normal (normalised).
void PlaneAxes(const glm::dvec3& normal, glm::dvec3& u, glm::dvec3& v);

} // namespace mesh
