#include "core/cam/CamSurface.h"

#include "core/cam/CamToolpath.h" // GenerateContext (CamToolpathInternal)
#include "core/cam/CamToolpathInternal.h" // kScale

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>

namespace cam {

using namespace detail;

namespace {

// The CL surface is a grid of tool positions, so a pass reading it between
// cells wants the surface BETWEEN them, not the nearer one: nearest-cell
// sampling turns a smooth slope into a staircase one cell wide, which is
// exactly the artefact this whole workstream exists to remove.
double Bilinear(const HeightMap& m, double x, double y) {
  const double fx = (x - m.x0) / m.res - 0.5;
  const double fy = (y - m.y0) / m.res - 0.5;
  const int i0 = static_cast<int>(std::floor(fx));
  const int j0 = static_cast<int>(std::floor(fy));
  const double tx = fx - i0;
  const double ty = fy - j0;
  const auto at = [&](int i, int j) {
    i = std::clamp(i, 0, m.nx - 1);
    j = std::clamp(j, 0, m.ny - 1);
    return static_cast<double>(m.At(i, j));
  };
  const double a = at(i0, j0) * (1 - tx) + at(i0 + 1, j0) * tx;
  const double b = at(i0, j0 + 1) * (1 - tx) + at(i0 + 1, j0 + 1) * tx;
  return a * (1 - ty) + b * ty;
}

} // namespace

double SampleCL(const CLSurface& cl, double x, double y) {
  return Bilinear(cl.z, x, y);
}

SurfacePaths ParallelPasses(const CLSurface& cl,
                            const std::vector<Loop>& region,
                            const ParallelRequest& req) {
  SurfacePaths out;
  if (!cl.ok || cl.z.nx <= 0 || cl.z.ny <= 0) {
    out.why = "there is no cutter-location surface to follow";
    return out;
  }
  if (req.stepover_mm <= 0.0) {
    out.why = "the stepover is zero, so the passes would never advance";
    return out;
  }
  const double sample = req.sampleStep_mm > 0.0 ? req.sampleStep_mm : cl.z.res;
  if (sample <= 0.0) {
    out.why = "the sampling step is zero";
    return out;
  }

  // Work in a frame where the raster runs along +X, so the passes are rows of
  // constant Y and the whole thing is one nested loop. Rotating the GEOMETRY
  // rather than walking at an angle is what keeps the spacing exactly
  // `stepover_mm` whatever the angle is.
  const double a = glm::radians(req.cutAngleDeg);
  const double ca = std::cos(a), sa = std::sin(a);
  const auto toCut = [&](glm::dvec2 p) {
    return glm::dvec2(p.x * ca + p.y * sa, -p.x * sa + p.y * ca);
  };
  const auto toWorld = [&](glm::dvec2 p) {
    return glm::dvec2(p.x * ca - p.y * sa, p.x * sa + p.y * ca);
  };

  // The region to stay inside, rotated into the cut frame. Empty means the
  // whole surface, which is the no-pick form of the verb.
  Clipper2Lib::Paths64 clip;
  glm::dvec2 lo(0.0), hi(0.0);
  bool any = false;
  if (region.empty()) {
    const glm::dvec2 corners[4] = {
        {cl.z.x0, cl.z.y0},
        {cl.z.x0 + cl.z.nx * cl.z.res, cl.z.y0},
        {cl.z.x0 + cl.z.nx * cl.z.res, cl.z.y0 + cl.z.ny * cl.z.res},
        {cl.z.x0, cl.z.y0 + cl.z.ny * cl.z.res}};
    Clipper2Lib::Path64 box;
    for (const glm::dvec2& c : corners) {
      const glm::dvec2 r = toCut(c);
      box.push_back({static_cast<int64_t>(r.x * kScale),
                     static_cast<int64_t>(r.y * kScale)});
      lo = any ? glm::min(lo, r) : r;
      hi = any ? glm::max(hi, r) : r;
      any = true;
    }
    if (Clipper2Lib::Area(box) < 0)
      std::reverse(box.begin(), box.end());
    clip.push_back(std::move(box));
  } else {
    for (const Loop& l : region) {
      Clipper2Lib::Path64 p;
      for (const glm::dvec2& w : l.pts) {
        const glm::dvec2 r = toCut(w);
        p.push_back({static_cast<int64_t>(r.x * kScale),
                     static_cast<int64_t>(r.y * kScale)});
        lo = any ? glm::min(lo, r) : r;
        hi = any ? glm::max(hi, r) : r;
        any = true;
      }
      if (p.size() >= 3)
        clip.push_back(std::move(p));
    }
    clip = Clipper2Lib::Union(clip, Clipper2Lib::FillRule::NonZero);
  }
  if (!any || clip.empty()) {
    out.why = "the region to surface is empty";
    return out;
  }

  // One open segment per pass, spanning the region, clipped to it. Clipper
  // returns the pieces that survive, so a region with a hole in it comes back
  // as several segments and the tool lifts between them.
  Clipper2Lib::Paths64 lines;
  const int rows =
      static_cast<int>(std::floor((hi.y - lo.y) / req.stepover_mm)) + 1;
  for (int r = 0; r < rows; r++) {
    const double y = lo.y + r * req.stepover_mm;
    lines.push_back({{static_cast<int64_t>((lo.x - 1.0) * kScale),
                      static_cast<int64_t>(y * kScale)},
                     {static_cast<int64_t>((hi.x + 1.0) * kScale),
                      static_cast<int64_t>(y * kScale)}});
  }

  Clipper2Lib::Clipper64 clipper;
  clipper.AddOpenSubject(lines);
  clipper.AddClip(clip);
  Clipper2Lib::Paths64 closed, open;
  if (!clipper.Execute(Clipper2Lib::ClipType::Intersection,
                       Clipper2Lib::FillRule::NonZero, closed, open)) {
    out.why = "the raster could not be clipped to the region";
    return out;
  }

  // Back into pass order: Clipper does not promise the pieces come out sorted,
  // and a raster whose rows are visited out of order is a rapid between every
  // pair of them.
  std::stable_sort(
      open.begin(), open.end(),
      [](const Clipper2Lib::Path64& p, const Clipper2Lib::Path64& q) {
        if (p.empty() || q.empty())
          return q.empty() < p.empty();
        if (p.front().y != q.front().y)
          return p.front().y < q.front().y;
        return p.front().x < q.front().x;
      });

  // Drop the degenerate pieces before reading a first point off one.
  std::erase_if(open,
                [](const Clipper2Lib::Path64& p) { return p.size() < 2; });
  if (open.empty()) {
    out.why = "the raster fell entirely outside the region";
    return out;
  }
  int row = 0;
  int64_t lastY = open.front().front().y;
  for (const Clipper2Lib::Path64& seg : open) {
    if (seg.size() < 2)
      continue;
    if (seg.front().y != lastY) {
      row++;
      lastY = seg.front().y;
    }
    double x0 = static_cast<double>(seg.front().x) / kScale;
    double x1 = static_cast<double>(seg.back().x) / kScale;
    const double y = static_cast<double>(seg.front().y) / kScale;
    if (x1 < x0)
      std::swap(x0, x1);
    // Alternate direction so the tool does not rapid back to the same side
    // between every pass. A raster is half climb whichever way it is walked,
    // which is why `climb` is not a question a surfacing op answers.
    const bool reverse = req.zigzag && (row % 2 == 1);

    SurfacePath path;
    const int steps =
        std::max(1, static_cast<int>(std::ceil((x1 - x0) / sample)));
    for (int k = 0; k <= steps; k++) {
      const double t = static_cast<double>(k) / steps;
      const double x = x0 + (x1 - x0) * (reverse ? 1.0 - t : t);
      const glm::dvec2 w = toWorld({x, y});
      const double z = Bilinear(cl.z, w.x, w.y);
      // Where the tool reaches nothing, the pass ENDS: cutting to the floor
      // across a void is a plunge through whatever is beside it.
      if (z <= req.floorZ + 1e-9) {
        if (path.size() >= 2)
          out.passes.push_back(std::move(path));
        path.clear();
        continue;
      }
      path.push_back({w.x, w.y, z});
    }
    if (path.size() >= 2)
      out.passes.push_back(std::move(path));
  }

  if (out.passes.empty()) {
    out.why = "nothing in that region is above the floor, so there is no "
              "surface to finish";
    return out;
  }
  out.ok = true;
  return out;
}

} // namespace cam
