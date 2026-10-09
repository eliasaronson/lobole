// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/cam/CamDropCutter.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cmath>

namespace cam {

namespace {

// A cell of the dilation kernel: a grid offset, and how far the cutter's
// surface stands above its tip there.
struct KernelCell {
  int di = 0;
  int dj = 0;
  double height = 0.0;
};

std::vector<KernelCell> BuildKernel(const Tool& tool, double res) {
  std::vector<KernelCell> cells;
  const double radius = tool.diameter * 0.5;
  const int reach = static_cast<int>(std::ceil(radius / res)) + 1;
  cells.reserve(static_cast<size_t>(2 * reach + 1) *
                static_cast<size_t>(2 * reach + 1));
  for (int dj = -reach; dj <= reach; dj++)
    for (int di = -reach; di <= reach; di++) {
      // A sample stands for the SQUARE of material around it, so the cutter
      // meets it at the square's nearest point, not at its centre. Measured
      // at the centre, the model ended at the last sample inside a sharp
      // convex edge -- up to half a cell short of the real one -- and near
      // its rim a ball's tip is steep enough to turn that into most of a
      // millimetre: a finish dipped 0.27 mm into a plate's corner, hidden
      // until the verifier carved sloped moves correctly. The nearest point
      // errs toward leaving a fraction of a cell, never toward the part.
      const double dx = std::max(0.0, std::abs(di) * res - res * 0.5);
      const double dy = std::max(0.0, std::abs(dj) * res - res * 0.5);
      const double d = std::hypot(dx, dy);
      // The kernel is the cutter's FOOTPRINT, so a cell outside its radius is
      // not part of the tool and must not lift it.
      if (d > radius)
        continue;
      cells.push_back({di, dj, TipHeight(tool, d)});
    }
  return cells;
}

} // namespace

double DerivedCusp(const Tool& tool, double requested) {
  if (requested > 0.0)
    return requested;
  // The same 2% of the cutter's radius `DerivedStepover` already solves the
  // ball-nose cusp equation against, so a pass planned here and a stepover
  // derived there are talking about one scallop.
  return std::max(1e-4, tool.diameter * 0.5 * 0.02);
}

double DerivedDropRes(const Tool& tool, double requestedRes, double cusp) {
  if (requestedRes > 0.0)
    return requestedRes;
  const double r = std::max(1e-6, tool.diameter * 0.5);
  // The lateral size of the feature this surface has to represent is the
  // stepover that cusp implies; eight samples across it is enough to carry
  // the scallop without paying for a grid nothing reads.
  const double step =
      2.0 * std::sqrt(std::max(0.0, 2.0 * r * cusp - cusp * cusp));
  return std::clamp(step / 8.0, 0.01, 0.5);
}

CLSurface DropCut(const std::vector<Body>& bodies, const Tool& tool,
                  const DropRequest& req) {
  CLSurface out;
  if (tool.diameter <= 0.0) {
    out.why = "tool " + std::to_string(tool.number) +
              " has no diameter, so there is no cutter to drop";
    return out;
  }
  if (req.max.x <= req.min.x || req.max.y <= req.min.y) {
    out.why = "the region to surface is empty";
    return out;
  }
  // A rotated drop frame is the rotary case, and this file is written so that
  // it is one parameter rather than a second implementation. It is not built:
  // refused, because a frame silently ignored would drop along the wrong axis
  // and produce a plausible surface of the wrong part of the model.
  if (std::abs(req.frame.w - 1.0) > 1e-9 || std::abs(req.frame.x) > 1e-9 ||
      std::abs(req.frame.y) > 1e-9 || std::abs(req.frame.z) > 1e-9) {
    out.why = "a rotated drop frame is the continuous rotary case and is not "
              "implemented; pass the identity for a 3-axis job";
    return out;
  }

  const double cusp = DerivedCusp(tool, req.cusp);
  const double res = DerivedDropRes(tool, req.res, cusp);
  const double radius = tool.diameter * 0.5;

  // Sample the model over a box grown by the tool's radius. The cutter at the
  // very edge of the region still rests on material OUTSIDE it, and a grid
  // that stops at the boundary would let it drop into the part there -- a
  // gouge exactly one radius wide all the way round.
  const int pad = static_cast<int>(std::ceil(radius / res)) + 1;
  HeightMap grid;
  grid.res = res;
  grid.x0 = req.min.x - pad * res;
  grid.y0 = req.min.y - pad * res;
  grid.nx =
      static_cast<int>(std::ceil((req.max.x - req.min.x) / res)) + 2 * pad + 1;
  grid.ny =
      static_cast<int>(std::ceil((req.max.y - req.min.y) / res)) + 2 * pad + 1;
  if (grid.nx <= 0 || grid.ny <= 0) {
    out.why = "the region to surface is smaller than one sample";
    return out;
  }
  grid.z.assign(static_cast<size_t>(grid.nx) * static_cast<size_t>(grid.ny),
                0.0f);

  const HeightMap surface = SampleModel(bodies, grid, req.floorZ);

  const std::vector<KernelCell> kernel = BuildKernel(tool, res);
  HeightMap cl = grid;
  cl.z.assign(cl.z.size(), static_cast<float>(req.floorZ));

  // Over rows: each writes only its own, so nothing is shared.
  parallel::For(static_cast<size_t>(grid.ny), [&](size_t jj) {
    const int j = static_cast<int>(jj);
    for (int i = 0; i < grid.nx; i++) {
      double best = req.floorZ;
      bool touched = false;
      for (const KernelCell& k : kernel) {
        const int si = i + k.di;
        const int sj = j + k.dj;
        // Off the grid is off the part: nothing there to rest on.
        if (si < 0 || si >= grid.nx || sj < 0 || sj >= grid.ny)
          continue;
        const double s = surface.At(si, sj);
        if (s <= req.floorZ)
          continue;
        // Where the TIP must sit for the cutter's surface to touch this cell.
        const double tip = s - k.height;
        if (!touched || tip > best) {
          best = tip;
          touched = true;
        }
      }
      cl.Set(i, j, static_cast<float>(touched ? best : req.floorZ));
    }
  });

  for (size_t n = 0; n < cl.z.size(); n++)
    if (cl.z[n] <= static_cast<float>(req.floorZ))
      out.unreached++;

  out.z = std::move(cl);
  out.ok = true;
  return out;
}

} // namespace cam
