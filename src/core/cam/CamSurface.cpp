#include "core/cam/CamSurface.h"

#include "core/cam/CamToolpath.h" // GenerateContext (CamToolpathInternal)
#include "core/cam/CamToolpathInternal.h" // kScale

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
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

std::vector<RotaryPass> RotaryParallelPasses(const RadiusMap& cl,
                                             const RotaryParallelRequest& req) {
  std::vector<RotaryPass> out;
  if (cl.nx <= 0 || cl.nTheta <= 0)
    return out;

  double rMax = 0.0;
  for (float v : cl.r)
    rMax = std::max(rMax, static_cast<double>(v));
  if (rMax <= 0.0)
    return out;

  // The angular step, from the stepover the caller asked for in millimetres
  // of arc AT THE WIDEST POINT. Anywhere narrower the passes are closer
  // together than asked, which is the safe direction -- the scallop only
  // gets smaller. Taking it at the mean radius instead would leave the rim,
  // where the surface speed is highest and the finish matters most, cut
  // coarser than the number the user typed.
  const double stepCells =
      std::max(1.0, req.stepover_mm / std::max(1e-6, cl.ArcAt(rMax)));
  const int stride = std::max(1, static_cast<int>(std::round(stepCells)));
  const double along = req.sampleStep_mm > 0.0 ? req.sampleStep_mm : cl.res;
  const int xStride = std::max(1, static_cast<int>(std::round(along / cl.res)));

  bool forward = true;
  for (int j = 0; j < cl.nTheta; j += stride) {
    RotaryPass pass;
    pass.thetaDeg = glm::degrees(cl.CellTheta(j));
    std::vector<glm::dvec2> run;
    const auto flush = [&]() {
      // Two points is a move; one is a plunge to nowhere.
      if (run.size() >= 2) {
        RotaryPass p = pass;
        p.pts = run;
        out.push_back(std::move(p));
      }
      run.clear();
    };
    for (int n = 0; n < cl.nx; n += xStride) {
      const int i = forward ? n : cl.nx - 1 - n;
      const double r = cl.At(i, j);
      if (r <= 0.0) {
        flush(); // a gap ENDS a pass: the tool lifts rather than dragging
        continue;
      }
      run.push_back({cl.CellX(i), r});
    }
    flush();
    if (req.zigzag)
      forward = !forward;
  }
  return out;
}

namespace {

// The CL radius at (x, theta), bilinear between cell centres and wrapping
// round in theta. Zero when any of the four cells is empty: the tool reaches
// nothing there, and blending an empty cell with a full one would invent a
// surface halfway to the axis.
double SpiralRadiusAt(const RadiusMap& cl, double x, double theta) {
  const double fx = std::clamp((x - cl.x0) / cl.res - 0.5, 0.0,
                               static_cast<double>(cl.nx - 1));
  const int i0 = std::min(static_cast<int>(fx), cl.nx - 1);
  const int i1 = std::min(i0 + 1, cl.nx - 1);
  const double u = fx - i0;
  const double ft = theta / (2.0 * std::numbers::pi) * cl.nTheta - 0.5;
  const double fl = std::floor(ft);
  const double v = ft - fl;
  const int j0 = ((static_cast<int>(fl) % cl.nTheta) + cl.nTheta) % cl.nTheta;
  const int j1 = (j0 + 1) % cl.nTheta;
  const double r00 = cl.At(i0, j0), r10 = cl.At(i1, j0);
  const double r01 = cl.At(i0, j1), r11 = cl.At(i1, j1);
  if (r00 <= 0.0 || r10 <= 0.0 || r01 <= 0.0 || r11 <= 0.0)
    return 0.0;
  return (1 - u) * (1 - v) * r00 + u * (1 - v) * r10 + (1 - u) * v * r01 +
         u * v * r11;
}

} // namespace

std::vector<RotaryPathPoint> RotarySpiralPath(const RadiusMap& cl,
                                              const RotarySpiralRequest& req) {
  std::vector<RotaryPathPoint> out;
  if (cl.nx <= 0 || cl.nTheta <= 0 || req.pitch_mm <= 0.0 ||
      req.airRadius <= 0.0)
    return out;
  double rMax = 0.0;
  for (float v : cl.r)
    rMax = std::max(rMax, static_cast<double>(v));
  if (rMax <= 0.0)
    return out;

  // The angular step: `along` millimetres of arc at the widest point, the
  // rule the parallel passes use to turn their stepover into an angle.
  // Narrower parts of the part are sampled finer, which is the safe side.
  // Rounded to a whole number per turn so every revolution samples the same
  // angles, and never coarser than 16 a turn, whatever was asked.
  const double along = req.sampleStep_mm > 0.0 ? req.sampleStep_mm : cl.res;
  const int perTurn = std::max(
      16, static_cast<int>(std::ceil(2.0 * std::numbers::pi * rMax / along)));
  const double dTheta = 2.0 * std::numbers::pi / perTurn;

  const double xLo = cl.CellX(0);
  const double xHi = cl.CellX(cl.nx - 1);

  // How steeply the surface climbs along the axis, per column: the worst
  // slope round the part between this column and the next. Empty cells have
  // no surface to climb.
  std::vector<double> slope(static_cast<size_t>(cl.nx), 0.0);
  for (int i = 0; i + 1 < cl.nx; i++)
    for (int j = 0; j < cl.nTheta; j++) {
      const double a = cl.At(i, j), b = cl.At(i + 1, j);
      if (a > 0.0 && b > 0.0)
        slope[i] = std::max(slope[i], std::abs(b - a) / cl.res);
    }
  // The pitch is measured ALONG THE SURFACE, not along the axis. A fixed
  // axial pitch crosses a shoulder the way a planar raster crosses a steep
  // wall: in big steps, leaving a scallop the height of the wall between
  // two turns -- 3.7 mm on a test shaft whose parallel passes left 1.1. So
  // the axial advance shrinks by the slope within a pitch EITHER side of the
  // cutter, and the turns bunch up on a wall and spread out on a flat.
  // Either side, not just ahead: a scallop lies between this turn and the
  // last one at the same angle, and looking only ahead let the pitch spring
  // back to full the moment the cutter passed the foot of a wall -- so at
  // most angles the next turn landed a whole pitch past the corner.
  const int window =
      std::max(1, static_cast<int>(std::ceil(req.pitch_mm / cl.res)));
  const auto slopeNear = [&](double x) {
    const int at =
        std::clamp(static_cast<int>((x - cl.x0) / cl.res - 0.5), 0, cl.nx - 1);
    double worst = 0.0;
    for (int i = std::max(0, at - window); i < std::min(cl.nx, at + window);
         i++)
      worst = std::max(worst, slope[i]);
    return worst;
  };

  std::vector<RotaryPathPoint> raw;
  const auto sample = [&](double x, double theta) {
    double r = SpiralRadiusAt(cl, x, theta);
    r = r > 0.0 ? std::max(r, req.floorRadius) : req.airRadius;
    raw.push_back({x, r, glm::degrees(theta)});
  };
  // A ring at the low end, then the helix, then a ring at the high end. The
  // rings are what close the ends: a helix started at x0 reaches the far
  // side of the part half a pitch in.
  for (int k = 0; k < perTurn; k++)
    sample(xLo, k * dTheta);
  double x = xLo, theta = perTurn * dTheta;
  sample(x, theta);
  const double flatStep = req.pitch_mm / perTurn;
  while (x < xHi) {
    const double s = slopeNear(x);
    const double dx = flatStep / std::sqrt(1.0 + s * s);
    if (x + dx >= xHi) {
      // The last step lands ON the end, turning only its share of a step,
      // so the helix meets the ring where it should.
      theta += dTheta * (xHi - x) / dx;
      x = xHi;
    } else {
      x += dx;
      theta += dTheta;
    }
    sample(x, theta);
  }
  for (int k = 1; k <= perTurn; k++)
    sample(xHi, theta + k * dTheta);

  if (req.tolerance_mm <= 0.0)
    return raw;
  // Drop what lies on the line between its neighbours, measured in (x, r,
  // arc at the widest radius): the controller interpolates X, Z and A
  // linearly between the points it is given, so that is the space the
  // programmed path is straight in. Runs are bounded, as the post bounds
  // its own merge, so a turned stretch costs O(n) and not O(n^2).
  const double arcPerDeg = rMax * std::numbers::pi / 180.0;
  const auto p3 = [&](const RotaryPathPoint& q) {
    return glm::dvec3(q.x, q.r, q.thetaDeg * arcPerDeg);
  };
  constexpr size_t kMaxRun = 256;
  out.push_back(raw.front());
  size_t from = 0;
  while (from + 1 < raw.size()) {
    size_t best = from + 1;
    for (size_t j = from + 2; j < raw.size() && j - from <= kMaxRun; j++) {
      const glm::dvec3 a = p3(raw[from]);
      const glm::dvec3 dir = p3(raw[j]) - a;
      const double len = glm::length(dir);
      bool straight = len > 1e-12;
      for (size_t k = from + 1; k < j && straight; k++)
        straight = glm::length(glm::cross(p3(raw[k]) - a, dir)) / len <=
                   req.tolerance_mm;
      if (!straight)
        break;
      best = j;
    }
    out.push_back(raw[best]);
    from = best;
  }
  return out;
}

namespace {

struct Hasher {
  uint64_t h = 1469598103934665603ULL;
  void Bytes(const void* p, size_t n) {
    const unsigned char* c = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= c[i];
      h *= 1099511628211ULL;
    }
  }
  void D(double v) { Bytes(&v, sizeof v); }
  void I(int64_t v) { Bytes(&v, sizeof v); }
};

} // namespace

uint64_t SurfaceKey(const Setup& setup, const Operation& op, const Tool& tool,
                    uint64_t bodiesKey) {
  // Every field the surfacing strategies read, and the caller's fingerprint
  // of the geometry. A read added to GenerateSurface or GenerateRotarySurface
  // that is not added here fails `test_conventions` ("the surface key covers
  // every field the pass reads") -- the same gate the adaptive key has, and
  // for the same reason: the drift is invisible in review and shows up as a
  // stale toolpath nobody can explain.
  Hasher hash;
  hash.I(static_cast<int64_t>(bodiesKey));
  hash.I(static_cast<int64_t>(op.type));
  hash.I(static_cast<int64_t>(op.id));
  hash.D(op.topZ);
  hash.D(op.bottomZ);
  hash.D(op.stockToLeave);
  hash.D(op.stepover);
  hash.D(op.sampleStep);
  hash.D(op.zStep);
  hash.D(op.cutAngleDeg);
  hash.D(op.stepdown);
  hash.I(static_cast<int64_t>(op.rotaryPattern));
  hash.I(static_cast<int64_t>(op.targets.size()));
  for (const Target& t : op.targets) {
    hash.I(static_cast<int64_t>(t.jobPartId));
    hash.I(static_cast<int64_t>(t.ref.featureId));
    for (char c : t.ref.name)
      hash.I(c);
  }
  hash.D(setup.safeZ);
  hash.D(setup.rotationA);
  // A rotary layer starts from the BLANK, whose radius is the part's plus
  // this margin (RotaryBlankRadius) -- read through a helper, which is why
  // it is named here rather than caught by the gate's grep.
  hash.D(setup.stock.margin.y);
  hash.D(setup.stock.margin.z);
  hash.I(setup.machine.hasRotaryAxis ? 1 : 0);
  hash.D(tool.diameter);
  hash.I(static_cast<int64_t>(tool.type));
  hash.D(tool.tipAngleDeg);
  hash.D(tool.feedCut);
  hash.D(tool.feedPlunge);
  return hash.h;
}

const SurfaceResult* SurfaceCache::Find(uint64_t key) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    misses++;
    return nullptr;
  }
  hits++;
  return &it->second;
}

bool SurfaceCache::Contains(uint64_t key) const {
  return m_entries.find(key) != m_entries.end();
}

void SurfaceCache::Put(uint64_t key, SurfaceResult result) {
  if (const auto it = m_entries.find(key); it != m_entries.end()) {
    m_moves -= it->second.moves.size();
    it->second = std::move(result);
    m_moves += it->second.moves.size();
    return;
  }
  m_moves += result.moves.size();
  m_entries.emplace(key, std::move(result));
  m_order.push_back(key);
  while (m_entries.size() > kMaxEntries ||
         (m_moves > kMaxMoves && m_entries.size() > 1)) {
    const uint64_t oldest = m_order.front();
    m_order.pop_front();
    if (const auto it = m_entries.find(oldest); it != m_entries.end()) {
      m_moves -= it->second.moves.size();
      m_entries.erase(it);
    }
  }
}

void SurfaceCache::Clear() {
  m_entries.clear();
  m_order.clear();
  m_moves = 0;
  hits = 0;
  misses = 0;
}

} // namespace cam
