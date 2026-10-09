// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// FEM tetrahedral meshing (Netgen, gated HAVE_NETGEN). GUI-free core code.
//
// Confines every Netgen type to FemMesher.cpp: this header exposes only plain
// data (glm/std) so the solver, RPCs and tests never see netgen headers. When
// the app is built without Netgen (HAVE_NETGEN undefined) Mesh() returns
// std::nullopt with a "built without FEM support" error, mirroring the
// HAVE_PLANEGCS stub discipline.

#include <TopoDS_Shape.hxx>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fem {

// Meshing controls for one body. Sizes are in millimetres (the document unit).
struct MeshSettings {
  // Global maximum element size; 0 = auto (bounding-box diagonal / 15, the
  // adaptive-deflection spirit of CADKernel tessellation).
  double maxSize = 0.0;
  // Per-face overrides keyed by TopExp_Explorer(TopAbs_FACE) 0-based index
  // (the same face order picking, ElementMap and the rest of the code use).
  std::vector<std::pair<int, double>> faceSizes;
};

// A second-order tetrahedral mesh. Node coordinates are millimetres. Element
// and boundary node indices are 0-based into `nodes`.
struct FemMesh {
  // tet10 corner+midside node ordering, canonical for the solver:
  //   0..3   = corner nodes
  //   4 = edge(0,1), 5 = edge(1,2), 6 = edge(2,0),
  //   7 = edge(0,3), 8 = edge(1,3), 9 = edge(2,3)
  std::vector<glm::dvec3> nodes;
  std::vector<std::array<int, 10>> tets;
  // The 0-based domain (netgen: the solid) each tet lies in, parallel to
  // `tets`; empty means one domain. A mesh over several glued solids (an
  // assembly study's bonded group, S2) carries a material per domain.
  std::vector<int> tetDomain;

  // Boundary surface triangles (6-node tri6: 3 corners then 3 edge-midsides),
  // each tagged with the 0-based OCC face index it lies on — the anchor for
  // applying face boundary conditions.
  struct BoundaryTri {
    std::array<int, 6> nodes;
    int faceIndex;
  };
  std::vector<BoundaryTri> boundary;

  struct Stats {
    std::size_t nodeCount = 0;
    std::size_t tetCount = 0;
    // Minimum normalized shape quality across all tets, in [0,1]:
    // 1 = regular tetrahedron, -> 0 = degenerate sliver. Surfaced so a study
    // can warn on a bad mesh before trusting its stresses.
    double worstQuality = 0.0;
  } stats;
};

// Meshes `shape` (a single solid) to second order. Returns std::nullopt and
// sets *error (when non-null) on failure or when built without Netgen.
std::optional<FemMesh> Mesh(const TopoDS_Shape& shape,
                            const MeshSettings& settings, std::string* error);

// True when the build links Netgen; lets callers surface a clear message
// instead of a generic meshing failure.
bool Available();

} // namespace fem
