// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The pocket a Cradle cuts (docs/modeling/README.md#cradle): a scan grown
// by a clearance and, optionally, swept along the direction it is lowered
// in, built in a distance field and contoured by manifold's LevelSet into a
// closed mesh. Pure data and no OpenCASCADE, so it runs on any thread; the
// field is filled under parallel::For.
//
// The field is sampled on a grid whose z axis points AGAINST the direction
// the object moves in (up, out of the cradle), so the sweep -- the running
// minimum f_swept(p) = min_{t>=0} f(p + t·dir) -- is a running minimum up
// each grid column, exact at the nodes. The grid covers only the part of the
// target the pocket can reach: the tool's box grown by the clearance, and
// with the sweep, on up through the target's top.
//
//   1. the unsigned distance to the tool, by MeshBvh within a band about it;
//   2. the sign from the generalised winding number, which an open scan
//      (one without its bottom) still answers;
//   3. the clearance as an iso-level shift, then the sweep;
//   4. manifold's LevelSet over the grid, trilinearly sampled, serial.

#include "core/mesh/TriMesh.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <functional>
#include <string>

namespace mesh {

struct PocketOptions {
  double clearanceMm = 0.0;             // the iso-level, a side
  glm::dvec3 direction{0.0, 0.0, -1.0}; // unit: the way the object moves in
  bool sweep = true;
  double cellMm = 0.0; // the grid's spacing
  // The target's box, world: the grid stops two cells past it.
  glm::dvec3 targetLo{0.0}, targetHi{0.0};
  // Called on the CALLING thread only, between pieces of work: a cancel
  // asked (true stops the build) and the fraction done of the pocket.
  std::function<bool()> cancelled;
  std::function<void(double)> progress;
};

// The grid a pocket samples, in its own frame (z up, against `direction`).
struct PocketGrid {
  glm::dmat3 toFrame{1.0}; // world -> grid frame, a rotation
  glm::dvec3 origin{0.0};  // the first node, frame coordinates
  glm::ivec3 nodes{0};
  double cellMm = 0.0;
  [[nodiscard]] size_t Cells() const {
    return size_t(nodes.x) * size_t(nodes.y) * size_t(nodes.z);
  }
  [[nodiscard]] bool Empty() const {
    return nodes.x < 2 || nodes.y < 2 || nodes.z < 2;
  }
};

// The grid `options` would sample around a tool whose box (world) is
// [toolLo, toolHi]: empty when the tool's reach misses the target's box.
// Cheap: what a cost estimate reads before anything is built.
PocketGrid PlanPocket(const glm::dvec3& toolLo, const glm::dvec3& toolHi,
                      const PocketOptions& options);

// The rotation that takes world vectors into a frame whose z is -dir.
glm::dmat3 FrameFor(const glm::dvec3& dir);

struct PocketStats {
  PocketGrid grid;
  size_t bandNodes = 0; // nodes whose distance was within the band
  size_t windingCalls = 0;
  double fieldS = 0.0;    // wall seconds filling the field
  double levelSetS = 0.0; // ... contouring it
  bool cancelled = false;
};

// The pocket: the closed mesh of { p : f_swept(p) < clearance }, world
// coordinates. `tool` may be open (a scan without its bottom); its
// adjacency need not be built. False with `error` set when there is
// nothing to cut (the tool misses the target) or LevelSet fails; false with
// stats.cancelled when `cancelled` said so.
bool Pocket(const TriMesh& tool, const PocketOptions& options, TriMesh& out,
            PocketStats& stats, std::string& error);

// The memory a pocket on `grid` takes at its peak: the field (a float a
// node) and LevelSet's voxels -- manifold pads each axis to a power of two
// past n + 2, and holds two doubles a padded node (body-centred).
double PocketBytes(const PocketGrid& grid);

// The worst a surface contoured from a grid of `cellMm` stands off the
// true iso-surface: half the cell's diagonal, the farthest a point inside a
// cell is from its nearest node. What a reply quotes as the deviation the
// resolution implies.
inline double PocketDeviationMm(double cellMm) {
  return 0.5 * cellMm * 1.7320508075688772;
}

} // namespace mesh
