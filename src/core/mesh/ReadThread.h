// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Reading a thread off a scan
// (docs/modeling/README.md#reading-a-thread-off-a-scan): at a point on a
// threaded surface of a mesh, the cylinder it is about; its radius against
// axial position in angular sectors; the pitch the autocorrelation of most
// windows of a few turns agrees on; the hand from the phase between
// neighbouring sectors; the diameters from the profile folded over every turn;
// and the nearest standard thread.

#include "core/mesh/TriMesh.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace mesh {

struct ThreadReading {
  bool ok = false;
  std::string why;
  bool noScan = false;   // no mesh at the point
  bool noThread = false; // a surface, but no thread on it
  double pitchMm = 0.0, pitchSigmaMm = 0.0;
  double majorDiameterMm = 0.0, minorDiameterMm = 0.0;
  double flankDeg = 0.0; // from the radial plane: 30 metric, 15 trapezoidal
  bool rightHand = true;
  bool internal = false; // a tapped hole's, not a bolt's
  glm::dvec3 axisPoint{0.0}, axisDir{0.0, 0.0, 1.0};
  // Windows of a few turns whose own pitch agrees with the reading, of all.
  int windowsAgree = 0, windows = 0;
  // The neighbouring sectors' shift, as a share of pitch / sectors: 1 for a
  // single-start thread read cleanly (its sign is the hand).
  double phaseShare = 0.0;
  // The nearest standard thread and how far the reading is from it.
  std::string nearest;
  double nearestMajorMm = 0.0, nearestPitchMm = 0.0;
  double lengthMm = 0.0; // of the cylinder read
};

ThreadReading ReadThread(const std::vector<const TriMesh*>& meshes,
                         const glm::dvec3& point);

} // namespace mesh
