#include "core/cam/CamSurface.h"

#include "core/cam/CamToolpath.h" // GenerateContext (CamToolpathInternal)
#include "core/cam/CamToolpathInternal.h" // kScale

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

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

// --- waterline ------------------------------------------------------------

namespace {

// Marching squares over the CL surface: the iso-contour at height `z`, as
// unchained segments.
//
// The grid stores one height per cell CENTRE, so the squares here are the
// ones between four adjacent centres, and a crossing is interpolated along
// the edge rather than snapped to it. Snapping would quantise every lap to
// the cell size, which on a grid chosen to carry a 0.06 mm cusp is a
// staircase the same size as the thing being controlled.
struct Seg {
  glm::dvec2 a{0.0};
  glm::dvec2 b{0.0};
};

std::vector<Seg> IsoSegments(const HeightMap& m, double z) {
  std::vector<Seg> segs;
  const auto pt = [&](int i, int j) {
    return glm::dvec2(m.CellX(i), m.CellY(j));
  };
  const auto lerp = [&](glm::dvec2 p, double hp, glm::dvec2 q, double hq) {
    const double d = hq - hp;
    const double t = std::abs(d) < 1e-12 ? 0.5 : (z - hp) / d;
    return p + (q - p) * std::clamp(t, 0.0, 1.0);
  };
  for (int j = 0; j + 1 < m.ny; j++)
    for (int i = 0; i + 1 < m.nx; i++) {
      const double h[4] = {m.At(i, j), m.At(i + 1, j), m.At(i + 1, j + 1),
                           m.At(i, j + 1)};
      const glm::dvec2 p[4] = {pt(i, j), pt(i + 1, j), pt(i + 1, j + 1),
                               pt(i, j + 1)};
      int code = 0;
      for (int k = 0; k < 4; k++)
        if (h[k] >= z)
          code |= 1 << k;
      if (code == 0 || code == 15)
        continue;
      // Crossing point on each of the four edges, where there is one.
      glm::dvec2 e[4];
      bool has[4] = {false, false, false, false};
      for (int k = 0; k < 4; k++) {
        const int k2 = (k + 1) % 4;
        if (((code >> k) & 1) != ((code >> k2) & 1)) {
          e[k] = lerp(p[k], h[k], p[k2], h[k2]);
          has[k] = true;
        }
      }
      std::vector<int> edges;
      for (int k = 0; k < 4; k++)
        if (has[k])
          edges.push_back(k);
      // Two crossings is one segment; four is a saddle, and joining the
      // wrong pair there closes the contour through the wrong side of the
      // cell. Split it the way the cell's own average says.
      if (edges.size() == 2) {
        segs.push_back({e[edges[0]], e[edges[1]]});
      } else if (edges.size() == 4) {
        const double mid = (h[0] + h[1] + h[2] + h[3]) * 0.25;
        if (mid >= z) {
          segs.push_back({e[0], e[1]});
          segs.push_back({e[2], e[3]});
        } else {
          segs.push_back({e[1], e[2]});
          segs.push_back({e[3], e[0]});
        }
      }
    }
  return segs;
}

// Chain segments end to end into polylines. Endpoints are matched on a
// quantised key, because two segments meeting at a cell edge produce the same
// point by two different arithmetic routes.
std::vector<std::vector<glm::dvec2>> ChainSegments(const std::vector<Seg>& segs,
                                                   double res) {
  const double q = std::max(1e-9, res * 1e-3);
  const auto key = [&](const glm::dvec2& p) {
    return std::pair<int64_t, int64_t>(
        static_cast<int64_t>(std::llround(p.x / q)),
        static_cast<int64_t>(std::llround(p.y / q)));
  };
  std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> ends;
  for (size_t n = 0; n < segs.size(); n++) {
    ends[key(segs[n].a)].push_back(n);
    ends[key(segs[n].b)].push_back(n);
  }
  std::vector<bool> used(segs.size(), false);
  std::vector<std::vector<glm::dvec2>> out;
  for (size_t n = 0; n < segs.size(); n++) {
    if (used[n])
      continue;
    used[n] = true;
    std::vector<glm::dvec2> line{segs[n].a, segs[n].b};
    // Walk forward from the tail, then backward from the head.
    for (int dir = 0; dir < 2; dir++) {
      for (;;) {
        const glm::dvec2 tip = dir == 0 ? line.back() : line.front();
        const auto it = ends.find(key(tip));
        if (it == ends.end())
          break;
        size_t next = segs.size();
        for (const size_t cand : it->second)
          if (!used[cand]) {
            next = cand;
            break;
          }
        if (next == segs.size())
          break;
        used[next] = true;
        const bool fromA = key(segs[next].a) == key(tip);
        const glm::dvec2 add = fromA ? segs[next].b : segs[next].a;
        if (dir == 0)
          line.push_back(add);
        else
          line.insert(line.begin(), add);
      }
    }
    if (line.size() >= 3)
      out.push_back(std::move(line));
  }
  return out;
}

} // namespace

SurfacePaths WaterlinePasses(const CLSurface& cl,
                             const std::vector<Loop>& region,
                             const WaterlineRequest& req) {
  SurfacePaths out;
  if (!cl.ok || cl.z.nx <= 0 || cl.z.ny <= 0) {
    out.why = "there is no cutter-location surface to follow";
    return out;
  }
  if (req.zStep_mm <= 0.0) {
    out.why = "the Z step is zero, so the laps would never descend";
    return out;
  }
  if (req.bottomZ >= req.topZ) {
    out.why = "the band to lap has no depth";
    return out;
  }

  // The region, as a clip. Empty is the whole surface.
  Clipper2Lib::Paths64 clip;
  for (const Loop& l : region) {
    if (l.pts.size() >= 3)
      clip.push_back(ToClipper(l.pts));
  }
  if (!clip.empty())
    clip = Clipper2Lib::Union(clip, Clipper2Lib::FillRule::NonZero);

  // Levels from the top down, the last landing exactly on bottomZ.
  std::vector<double> levels;
  for (double z = req.topZ - req.zStep_mm; z > req.bottomZ + 1e-9;
       z -= req.zStep_mm)
    levels.push_back(z);
  levels.push_back(req.bottomZ);

  const double sample = req.sampleStep_mm > 0.0 ? req.sampleStep_mm : cl.z.res;
  for (const double z : levels) {
    if (z <= req.floorZ + 1e-9)
      continue;
    const std::vector<Seg> segs = IsoSegments(cl.z, z);
    if (segs.empty())
      continue;
    for (std::vector<glm::dvec2>& line : ChainSegments(segs, cl.z.res)) {
      Clipper2Lib::Paths64 pieces{ToClipper(line)};
      if (!clip.empty()) {
        Clipper2Lib::Clipper64 clipper;
        clipper.AddOpenSubject(pieces);
        clipper.AddClip(clip);
        Clipper2Lib::Paths64 closed, open;
        if (!clipper.Execute(Clipper2Lib::ClipType::Intersection,
                             Clipper2Lib::FillRule::NonZero, closed, open))
          continue;
        pieces = open;
        pieces.insert(pieces.end(), closed.begin(), closed.end());
      }
      for (const Clipper2Lib::Path64& piece : pieces) {
        if (piece.size() < 2)
          continue;
        const std::vector<glm::dvec2> pts = FromClipper(piece);
        // Resample along the lap so the point spacing is the one asked for
        // rather than whatever the contour extraction happened to produce.
        SurfacePath path;
        for (size_t k = 0; k + 1 < pts.size(); k++) {
          const glm::dvec2 a = pts[k], b = pts[k + 1];
          const double len = glm::length(b - a);
          const int steps =
              std::max(1, static_cast<int>(std::ceil(len / sample)));
          for (int t = 0; t < steps; t++) {
            const glm::dvec2 p = a + (b - a) * (static_cast<double>(t) / steps);
            path.push_back({p.x, p.y, z});
          }
        }
        path.push_back({pts.back().x, pts.back().y, z});
        if (path.size() >= 2)
          out.passes.push_back(std::move(path));
      }
    }
  }

  if (out.passes.empty()) {
    out.why = "nothing in that band has a wall to lap — a waterline needs a "
              "surface that rises through it, and a raster suits a shallow "
              "one better";
    return out;
  }
  out.ok = true;
  return out;
}

} // namespace cam
