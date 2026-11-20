// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What a mesh says about itself, all at once
// (docs/simulation/README.md#the-mesh-report): its counts, its worst
// elements and where they are, walls it crossed with one element, what the
// fold check straightened, refinements that did not take, its volume against
// its shape's, and on a mesh over several glued solids whether they share
// their interface. Plain data from a FemMesh; nothing here needs the mesher.

#include "core/fem/FemMesher.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace fem {

struct MeshBodyReport {
  std::size_t nodes = 0;
  std::size_t tets = 0;
  std::size_t boundaryTris = 0;

  // Per tet, parallel to mesh.tets: what the mesh view colours by.
  std::vector<float> quality; // TetMeasure::Quality
  std::vector<float> size;    // mm
  double sizeMin = 0.0, sizeMean = 0.0, sizeMax = 0.0;

  // The worst elements, worst first, at most the limit ReportMesh was given.
  struct Element {
    int tet = -1;
    glm::dvec3 at{0.0}; // its centroid, mm
    double quality = 0.0;
    double shape = 0.0;
    double jacobian = 0.0;
    double size = 0.0; // mm
  };
  std::vector<Element> worst;
  double worstQuality = 0.0; // worst.front().quality; 1 on an empty mesh

  // A wall crossed by ONE element: tets whose corners all lie on the
  // boundary and reach two faces that share no node, grouped by that pair.
  // A tet10 bends right one through (docs/simulation/README.md#meshing), so
  // this is something to know, not an error: a contact or a stress gradient
  // through the wall wants more.
  struct ThinWall {
    int faceA = -1, faceB = -1; // 0-based face indices of the meshed shape
    std::size_t tets = 0;
    double thicknessMm = 0.0; // the least distance between the two faces'
                              // corners over those tets
    glm::dvec3 at{0.0};       // the thinnest such tet's centroid
  };
  std::vector<ThinWall> thinWalls;

  // What the fold check did (docs/simulation/README.md
  // #curved-elements-that-fold).
  std::size_t straightenedNodes = 0;

  // A refinement that did not take: the face's elements are not near the
  // size it asked for.
  struct RefinementMiss {
    int faceIndex = -1;
    double askedMm = 0.0;
    double gotMm = 0.0; // mean edge of the face's triangles; 0 = none on it
    glm::dvec3 at{0.0};
    std::string why;
  };
  std::vector<RefinementMiss> refinementMisses;

  // The volume check Mesh() refuses on (0 when the mesh was not made by it).
  double volumeMm3 = 0.0;
  double shapeVolumeMm3 = 0.0;
  double volumeSlackMm3 = 0.0;

  // A mesh over several glued solids: each pair of domains and the element
  // faces they share, and nodes that coincide without being shared (a seam
  // that looks bonded and carries nothing).
  int domains = 1;
  struct Interface {
    int domainA = 0, domainB = 0;
    std::size_t sharedFaces = 0;
    glm::dvec3 at{0.0};
  };
  std::vector<Interface> interfaces;
  std::size_t unsharedNodes = 0;
  glm::dvec3 unsharedAt{0.0};
  // Domains that share no element face with any other: glued to nothing.
  std::vector<int> looseDomains;
};

// `asked` is what the mesh was made with (its per-face refinements are
// checked against what came out); `autoSize` the global size used when
// asked.maxSize is 0.
MeshBodyReport ReportMesh(const FemMesh& mesh, const MeshSettings& asked,
                          double autoSize, std::size_t worstLimit = 12);

} // namespace fem
