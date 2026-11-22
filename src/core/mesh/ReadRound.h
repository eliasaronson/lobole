// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Reading a round off a scan
// (docs/modeling/README.md#reading-a-round-off-a-scan): at a point on an edge
// of a mesh, a section square to the edge; the two faces in it as lines; and
// which of three readings the points between them carry -- a sharp corner, a
// chamfer, or a round tangent to both faces
// -- each judged against the others in the scan's own noise, the round's
// radius with an uncertainty that carries where it touches the faces.

#include "core/mesh/TriMesh.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace mesh {

enum class EdgeReading { Round, Chamfer, Sharp };
const char* ToString(EdgeReading r);

struct RoundReading {
  bool ok = false;
  std::string why; // not ok: there is no scan at the point, or no edge
  bool noScan = false;
  EdgeReading verdict = EdgeReading::Sharp;
  double radiusMm = 0.0, sigmaMm = 0.0; // a round's
  double chamferMm[2] = {0.0, 0.0};     // a chamfer's, from where the faces
                                        // meet, along each face
  double angleDeg = 0.0;                // between the faces, inside
  glm::dvec3 edgeDir{0.0};              // the section's normal
  glm::dvec3 corner{0.0};               // where the faces meet, in it
  // How much worse each reading fits than the best: the difference of
  // their AIC (residuals over the noise, squared, plus twice the
  // parameters), 0 for the verdict itself.
  double worse[3] = {0.0, 0.0, 0.0}; // indexed by EdgeReading
  // The readings within kAlsoFits of the verdict: what the scan cannot
  // tell it from.
  std::vector<EdgeReading> alsoFits;
  double noiseMm = 0.0;
  double windowMm = 0.0;
  size_t points = 0;
};

// A reading is told from another when it fits better by this much (an AIC
// difference: about a 1-in-20 chance the other is the better).
inline constexpr double kAlsoFits = 6.0;

// The distance from `point` to the nearest triangle of `meshes`, and which
// mesh it is on (nullptr, and infinity, when none has a triangle).
double NearestOnMeshes(const std::vector<const TriMesh*>& meshes,
                       const glm::dvec3& point, const TriMesh** which);

RoundReading ReadRound(const std::vector<const TriMesh*>& meshes,
                       const glm::dvec3& point);

} // namespace mesh
