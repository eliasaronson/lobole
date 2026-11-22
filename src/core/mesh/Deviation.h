// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// How far a body is from a scan
// (docs/inspect/README.md#how-far-a-body-is-from-a-scan): the signed distance
// from every vertex of the scan to the body's surface, positive where the
// scan stands outside the body (the object has material the model lacks)
// and negative inside it; the share of the body no scan lies under; and the
// patches past the tolerance, grouped by distance. Pure data, no
// OpenCASCADE: a B-Rep body arrives as its tessellation with each
// triangle's face (scancompare::Gather), so any of it runs on a worker.
//
// The sign is the angle-weighted pseudonormal's (Bærentzen and Aanæs 2005)
// at the closest feature of the body -- a triangle's inside, an edge or a
// vertex -- which is exact on a closed surface, where a triangle's own
// normal speckles at every sharp edge. A scan point is measured to the
// nearest surface that faces its way (mesh/Facing.h): on a thin wall the
// nearest surface past half the thickness is the far side, and the reading
// would be of the wrong side with the wrong sign.

#include "core/mesh/TriMesh.h"

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mesh {

struct DeviationOptions {
  // > 0: the tolerance asked for; 0: the scan's own noise (ScanTolerance).
  // Never under `chordMm`.
  double toleranceMm = 0.0;
  // How far the body's triangles stand off its true surface: a B-Rep's
  // tessellation chord, 0 for a mesh body. The floor under the map.
  double chordMm = 0.0;
  // A scan point and the surface it is measured to agree within this (cos);
  // -1 measures every point to the nearest surface whichever way it faces
  // (a body rebuilt from the very mesh, where the nearest is the right one).
  double minFacingCos = 0.3;
  const std::atomic<bool>* cancel = nullptr; // read between blocks
  std::function<void(double)> progress;      // 0-1
};

// A patch of the scan past the tolerance, one sign.
struct DeviationHotspot {
  glm::dvec3 atMm{0.0};  // the scan vertex at its peak
  double depthMm = 0.0;  // the signed peak
  double areaMm2 = 0.0;  // the scan's area it covers
  uint32_t face = kNone; // the body's face at the peak (kNone: a mesh)
  size_t vertices = 0;
  glm::dvec3 lo{0.0}, hi{0.0}; // its bounding box, for framing it
  bool feature = false;        // else texture: small or shallow
};

struct Deviation {
  bool cancelled = false;
  // Per scan vertex: the signed distance (NaN where no surface of the body
  // lies within `reachMm`) and the body's face it was measured to.
  std::vector<float> distanceMm;
  std::vector<uint32_t> faceOf;
  double toleranceMm = 0.0;
  std::string toleranceFrom; // "asked", "scan_noise" or "chord"
  double noiseMm = 0.0;      // the scan's (ScanTolerance)
  double chordMm = 0.0;
  double reachMm = 0.0;   // past this a scan point has no body
  double spacingMm = 0.0; // the scan's median edge
  // Over the scan's area that has a body within reach, each vertex
  // weighted by its share of the area.
  double within = 0.0; // the share inside the tolerance
  double meanMm = 0.0, rmsMm = 0.0, p95Mm = 0.0; // p95 of |distance|
  double minMm = 0.0, maxMm = 0.0;
  double unmatched = 0.0; // the share of the scan's area with no body
  // The share of the scan's area measured to a surface facing against it
  // (none faced its way within reach, or every surface was admitted): an
  // inside-out body, or a scan of the other side.
  double opposed = 0.0;
  // The share of the body's area no scan facing its way lies under.
  double noScan = 0.0;
  size_t bodySamples = 0;
  std::vector<DeviationHotspot> hotspots; // worst |depth| first
};

// `scan` and `body` in one frame. `bodyFaces` gives each of `body`'s
// triangles its face (empty: a mesh body, every face kNone). `body` needs
// its adjacency (FromSoup builds it) for the edges' pseudonormals.
Deviation CompareToBody(std::shared_ptr<const TriMesh> scan,
                        std::shared_ptr<const TriMesh> body,
                        const std::vector<uint32_t>& bodyFaces,
                        const DeviationOptions& options);

// A patch smaller than this, or shallower than twice the tolerance, is
// texture rather than a feature.
inline constexpr double kHotspotFeatureAreaMm2 = 4.0;

} // namespace mesh
