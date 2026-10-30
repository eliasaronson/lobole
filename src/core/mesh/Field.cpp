// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/Field.h"

#include "core/Parallel.h"
#include "core/mesh/MeshBvh.h"

#include <manifold/manifold.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace mesh {

namespace {

double Now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

glm::dmat3 FrameFor(const glm::dvec3& dir) {
  const glm::dvec3 z = -glm::normalize(dir);
  // The world axis least along z seeds x, so world -Z gives the identity.
  glm::dvec3 seed(1.0, 0.0, 0.0);
  if (std::abs(z.x) > std::abs(z.y) && std::abs(z.x) > std::abs(z.z))
    seed = glm::dvec3(0.0, 1.0, 0.0);
  const glm::dvec3 x = glm::normalize(seed - glm::dot(seed, z) * z);
  const glm::dvec3 y = glm::cross(z, x);
  // Rows are the frame's axes: toFrame * v gives v's frame coordinates.
  return glm::transpose(glm::dmat3(x, y, z));
}

namespace {

// The box (frame) of `corners` carried into the frame.
void FrameBox(const glm::dmat3& toFrame, const glm::dvec3& lo,
              const glm::dvec3& hi, glm::dvec3& flo, glm::dvec3& fhi) {
  flo = glm::dvec3(std::numeric_limits<double>::max());
  fhi = -flo;
  for (int c = 0; c < 8; c++) {
    const glm::dvec3 p((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y,
                       (c & 4) ? hi.z : lo.z);
    const glm::dvec3 q = toFrame * p;
    flo = glm::min(flo, q);
    fhi = glm::max(fhi, q);
  }
}

} // namespace

PocketGrid PlanPocket(const glm::dvec3& toolLo, const glm::dvec3& toolHi,
                      const PocketOptions& o) {
  PocketGrid g;
  g.cellMm = o.cellMm;
  if (!(o.cellMm > 0.0) || !(o.clearanceMm >= 0.0))
    return g;
  g.toFrame = FrameFor(o.direction);
  glm::dvec3 tlo, thi, blo, bhi;
  FrameBox(g.toFrame, toolLo, toolHi, tlo, thi);
  FrameBox(g.toFrame, o.targetLo, o.targetHi, blo, bhi);
  const double h = o.cellMm;
  // The tool's reach: its box grown by the clearance and two cells, so the
  // iso-surface and the nodes either side of it are on the grid.
  const double grow = o.clearanceMm + 2.0 * h;
  glm::dvec3 lo = tlo - grow, hi = thi + grow;
  // The swept pocket runs on up, out through the target's top.
  if (o.sweep)
    hi.z = std::max(hi.z, bhi.z + 2.0 * h);
  // ... and nowhere past the target: two cells beyond it, so the pocket's
  // caps at the grid's bounds lie outside the target and cut nothing.
  lo = glm::max(lo, blo - 2.0 * h);
  hi = glm::min(hi, bhi + 2.0 * h);
  if (lo.x >= hi.x || lo.y >= hi.y || lo.z >= hi.z)
    return g;
  for (int a = 0; a < 3; a++)
    g.nodes[a] = int(std::ceil((hi[a] - lo[a]) / h)) + 1;
  g.origin = lo;
  return g;
}

namespace {

// manifold's ComputeGridPow: the bits an axis of n nodes is indexed by.
double PaddedAxis(int n) {
  double p = 1.0;
  while (p < double(n) + 3.0)
    p *= 2.0;
  return p;
}

} // namespace

double PocketBytes(const PocketGrid& grid) {
  if (grid.Empty())
    return 0.0;
  return 4.0 * double(grid.Cells()) + 2.0 * 8.0 * PaddedAxis(grid.nodes.x) *
                                          PaddedAxis(grid.nodes.y) *
                                          PaddedAxis(grid.nodes.z);
}

bool Pocket(const TriMesh& toolIn, const PocketOptions& o, TriMesh& out,
            PocketStats& stats, std::string& error) {
  stats = PocketStats{};
  if (toolIn.triangles.empty()) {
    error = "the tool has no triangles";
    return false;
  }
  glm::dvec3 toolLo, toolHi;
  toolIn.Bounds(toolLo, toolHi);
  const PocketGrid grid = PlanPocket(toolLo, toolHi, o);
  stats.grid = grid;
  if (grid.Empty()) {
    error = "the tool, grown by the clearance, does not reach the target";
    return false;
  }
  const double h = grid.cellMm, c = o.clearanceMm;
  const bool identity = grid.toFrame == glm::dmat3(1.0);
  // The tool in the grid's frame, so the queries need no transform.
  auto tool = std::make_shared<TriMesh>();
  if (identity) {
    *tool = toolIn;
  } else {
    *tool = toolIn;
    for (glm::dvec3& p : tool->positions)
      p = grid.toFrame * p;
  }
  const bool closed = [&] {
    TriMesh probe;
    if (!toolIn.neighbour.empty())
      return toolIn.Closed();
    probe = toolIn;
    BuildAdjacency(probe);
    return probe.Closed();
  }();
  const Bvh bvh(tool);

  const std::thread::id caller = std::this_thread::get_id();
  std::atomic<bool> stop{false};
  const auto checkCancel = [&] {
    if (std::this_thread::get_id() == caller && o.cancelled && o.cancelled())
      stop = true;
    return stop.load(std::memory_order_relaxed);
  };
  const auto report = [&](double f) {
    if (std::this_thread::get_id() == caller && o.progress)
      o.progress(f);
  };

  // 1-3. The field, one column (along the frame's z) at a time: the
  // distance within the band, its sign, then the running minimum up the
  // column. Stored column-contiguous.
  const int nx = grid.nodes.x, ny = grid.nodes.y, nz = grid.nodes.z;
  // Values past the band are clamped to it: the iso-level at `c` and its
  // trilinear neighbours a cell off need no more.
  const double band = c + 2.0 * h;
  std::vector<float> field(grid.Cells());
  std::atomic<size_t> bandNodes{0}, windings{0}, rowsDone{0};
  const double t0 = Now();
  // The progress: the field is about a tenth of the pocket's time, the
  // level set the rest (measured on the fixtures).
  constexpr double kFieldShare = 0.1, kLevelShare = 0.9;
  parallel::For(size_t(ny), [&](size_t jRow) {
    if (checkCancel())
      return;
    const int j = int(jRow);
    size_t myBand = 0, myWind = 0;
    for (int i = 0; i < nx; i++) {
      float* col = field.data() + (size_t(j) * nx + i) * nz;
      bool prevPastBand = false, prevInside = false;
      for (int k = 0; k < nz; k++) {
        const glm::dvec3 p = grid.origin + glm::dvec3(i, j, k) * h;
        const Bvh::Closest cp = bvh.ClosestPoint(p, band);
        const bool pastBand = !cp.found;
        bool inside;
        // Between two nodes both farther than the band (> h/2) from the
        // surface the column cannot cross it -- on a CLOSED tool. An open
        // one's inside ends at no surface (across its missing bottom), so
        // there every node asks the winding number.
        if (closed && pastBand && prevPastBand) {
          inside = prevInside;
        } else {
          inside = std::abs(bvh.Winding(p)) > 0.5;
          myWind++;
        }
        if (!pastBand)
          myBand++;
        const double d = pastBand ? band : cp.distance;
        col[k] = float(inside ? -d : d);
        prevPastBand = pastBand;
        prevInside = inside;
      }
      if (o.sweep)
        for (int k = 1; k < nz; k++)
          col[k] = std::min(col[k], col[k - 1]);
    }
    bandNodes += myBand;
    windings += myWind;
    const size_t done = ++rowsDone;
    report(kFieldShare * double(done) / double(ny));
  });
  stats.bandNodes = bandNodes;
  stats.windingCalls = windings;
  stats.fieldS = Now() - t0;
  if (stop || checkCancel()) {
    stats.cancelled = true;
    return false;
  }

  // 4. The iso-surface at the clearance. Positive inside the pocket, as
  // LevelSet wants: c - f.
  const auto sample = [&](const manifold::vec3& q) -> double {
    const double x = (q.x - grid.origin.x) / h, y = (q.y - grid.origin.y) / h,
                 z = (q.z - grid.origin.z) / h;
    if (!(x >= 0.0 && y >= 0.0 && z >= 0.0 && x <= nx - 1 && y <= ny - 1 &&
          z <= nz - 1))
      return -band;
    const int i = std::min(int(x), nx - 2), j = std::min(int(y), ny - 2),
              k = std::min(int(z), nz - 2);
    const double fx = x - i, fy = y - j, fz = z - k;
    const auto at = [&](int a, int b, int cc) {
      return double(field[(size_t(j + b) * nx + (i + a)) * nz + (k + cc)]);
    };
    const double c00 = at(0, 0, 0) * (1 - fz) + at(0, 0, 1) * fz,
                 c10 = at(1, 0, 0) * (1 - fz) + at(1, 0, 1) * fz,
                 c01 = at(0, 1, 0) * (1 - fz) + at(0, 1, 1) * fz,
                 c11 = at(1, 1, 0) * (1 - fz) + at(1, 1, 1) * fz;
    const double f = (c00 * (1 - fx) + c10 * fx) * (1 - fy) +
                     (c01 * (1 - fx) + c11 * fx) * fy;
    return c - f;
  };
  // One LevelSet, on the calling thread. Slabs of it on every core,
  // overlapping by a cell and unioned, were measured slower: the union of
  // the seams cost more than the threads saved
  // (docs/modeling/README.md#what-a-cradle-costs). A cancel reaches it
  // through its own samples.
  const double t1 = Now();
  // LevelSet's grid matches the field's: its spacing is bounds / (n - 1)
  // with n = bounds / edge + 1 truncated, so the edge is a hair under h.
  const double edge = h * (1.0 - 1e-9);
  const manifold::Box box(
      manifold::vec3(grid.origin.x, grid.origin.y, grid.origin.z),
      manifold::vec3(grid.origin.x + (nx - 1) * h, grid.origin.y + (ny - 1) * h,
                     grid.origin.z + (nz - 1) * h));
  manifold::ExecutionContext ctx;
  size_t calls = 0;
  // Two samples a node (the body-centred grid), most of the level set's
  // work.
  const double expected = 2.0 * double(grid.Cells());
  const auto sdf = [&](manifold::vec3 q) {
    if ((++calls & 0xffff) == 0) {
      if (checkCancel())
        ctx.Cancel();
      report(kFieldShare +
             kLevelShare * std::min(1.0, double(calls) / expected));
    }
    return sample(q);
  };
  // canParallel off: manifold is built serial (MANIFOLD_PAR off).
  const manifold::Manifold pocket =
      ctx.LevelSet(sdf, box, edge, 0.0, -1.0, false);
  if (stop || checkCancel()) {
    stats.cancelled = true;
    return false;
  }
  if (pocket.Status() != manifold::Manifold::Error::NoError) {
    error = "the level set failed";
    return false;
  }
  const manifold::MeshGL64 gl = pocket.GetMeshGL64();
  stats.levelSetS = Now() - t1;
  if (gl.NumTri() == 0) {
    error = "the tool, grown by the clearance, does not reach the target";
    return false;
  }
  TriMesh result;
  result.quantum = toolIn.quantum;
  const glm::dmat3 toWorld = glm::transpose(grid.toFrame);
  result.positions.reserve(gl.NumVert());
  for (size_t v = 0; v < gl.NumVert(); v++) {
    const glm::dvec3 p(gl.vertProperties[v * gl.numProp],
                       gl.vertProperties[v * gl.numProp + 1],
                       gl.vertProperties[v * gl.numProp + 2]);
    result.positions.push_back(identity ? p : toWorld * p);
  }
  result.triangles.reserve(gl.NumTri());
  for (size_t t = 0; t < gl.NumTri(); t++)
    result.triangles.push_back({uint32_t(gl.triVerts[3 * t]),
                                uint32_t(gl.triVerts[3 * t + 1]),
                                uint32_t(gl.triVerts[3 * t + 2])});
  BuildAdjacency(result);
  out = std::move(result);
  return true;
}

} // namespace mesh
