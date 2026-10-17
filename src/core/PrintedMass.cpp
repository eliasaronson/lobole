// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/PrintedMass.h"

#include "core/Feature.h"
#include "core/Parallel.h"
#include "core/PrintAllowance.h"
#include "core/PrintSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>
#include <vector>

namespace printmass {

namespace {

// How fine the raster may be: a quarter of the line width, coarsened so a
// layer holds at most kMaxLayerPixels and the whole body kMaxPixels (the
// bits of every layer are kept, two per pixel).
constexpr double kMaxLayerPixels = 2.0e6;
constexpr double kMaxPixels = 2.0e8;

struct Grid {
  int nx = 0, ny = 0;
  double u0 = 0.0, v0 = 0.0, p = 0.0;
  [[nodiscard]] size_t Words() const {
    return (static_cast<size_t>(nx) * ny + 63) / 64;
  }
};

using Bits = std::vector<uint64_t>;
inline bool Get(const Bits& b, size_t i) {
  return (b[i >> 6] >> (i & 63)) & 1u;
}
inline void Set(Bits& b, size_t i) { b[i >> 6] |= uint64_t{1} << (i & 63); }

struct Tri {
  glm::dvec3 a, b, c; // u, v, w
};

// One layer's section at height `zc`, even-odd filled at the pixel centres.
Bits Rasterise(const std::vector<Tri>& tris, const std::vector<uint32_t>& ids,
               const Grid& g, double zc) {
  std::vector<std::vector<double>> rows(static_cast<size_t>(g.ny));
  const auto cut = [&](const glm::dvec3& p, const glm::dvec3& q) {
    const double t = (zc - p.z) / (q.z - p.z);
    return glm::dvec2(p.x + t * (q.x - p.x), p.y + t * (q.y - p.y));
  };
  for (uint32_t id : ids) {
    const Tri& t = tris[id];
    const glm::dvec3* v[3] = {&t.a, &t.b, &t.c};
    // A vertex AT the height counts as above it, so a section through a
    // vertex or along a horizontal face is still a closed polygon.
    bool up[3];
    int above = 0;
    for (int k = 0; k < 3; k++) {
      up[k] = v[k]->z >= zc;
      above += up[k] ? 1 : 0;
    }
    if (above == 0 || above == 3)
      continue;
    glm::dvec2 s[2];
    int n = 0;
    for (int k = 0; k < 3 && n < 2; k++) {
      const int m = (k + 1) % 3;
      if (up[k] != up[m])
        s[n++] = cut(*v[k], *v[m]);
    }
    if (n != 2)
      continue;
    // Every pixel row whose centre the segment spans, half-open.
    const double lo = std::min(s[0].y, s[1].y), hi = std::max(s[0].y, s[1].y);
    int j0 = static_cast<int>(std::ceil((lo - g.v0) / g.p - 0.5));
    int j1 = static_cast<int>(std::ceil((hi - g.v0) / g.p - 0.5)) - 1;
    j0 = std::max(j0, 0);
    j1 = std::min(j1, g.ny - 1);
    for (int j = j0; j <= j1; j++) {
      const double vc = g.v0 + (j + 0.5) * g.p;
      if ((s[0].y <= vc) == (s[1].y <= vc))
        continue;
      const double f = (vc - s[0].y) / (s[1].y - s[0].y);
      rows[static_cast<size_t>(j)].push_back(s[0].x + f * (s[1].x - s[0].x));
    }
  }
  Bits in(g.Words(), 0);
  for (int j = 0; j < g.ny; j++) {
    std::vector<double>& xs = rows[static_cast<size_t>(j)];
    if (xs.size() < 2)
      continue;
    std::sort(xs.begin(), xs.end());
    for (size_t k = 0; k + 1 < xs.size(); k += 2) {
      int i0 = static_cast<int>(std::ceil((xs[k] - g.u0) / g.p - 0.5));
      int i1 = static_cast<int>(std::ceil((xs[k + 1] - g.u0) / g.p - 0.5)) - 1;
      i0 = std::max(i0, 0);
      i1 = std::min(i1, g.nx - 1);
      for (int i = i0; i <= i1; i++)
        Set(in, static_cast<size_t>(j) * g.nx + i);
    }
  }
  return in;
}

// Felzenszwalb & Huttenlocher's 1-D squared distance transform, in place.
// "Inside" is a large finite value rather than infinity: every row and
// column has an outside pixel (the raster's border), so nothing stays large.
constexpr double kFar = 1e20;
void Edt1d(std::vector<double>& f, int n, std::vector<double>& d,
           std::vector<int>& v, std::vector<double>& z) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  const auto cross = [&](int q, int r) {
    return ((f[q] + double(q) * q) - (f[r] + double(r) * r)) /
           (2.0 * q - 2.0 * r);
  };
  int k = 0;
  v[0] = 0;
  z[0] = -kInf;
  z[1] = kInf;
  for (int q = 1; q < n; q++) {
    double s = cross(q, v[k]);
    while (s <= z[k]) {
      k--;
      s = cross(q, v[k]);
    }
    k++;
    v[k] = q;
    z[k] = s;
    z[k + 1] = kInf;
  }
  k = 0;
  for (int q = 0; q < n; q++) {
    while (z[k + 1] < q)
      k++;
    d[q] = (double(q) - v[k]) * (double(q) - v[k]) + f[v[k]];
  }
  for (int q = 0; q < n; q++)
    f[q] = d[q];
}

// The pixels within `wallPx` (pixel units) of the section's outline: the
// distance from a pixel's centre to the outline is its distance to the
// nearest outside pixel's centre less half a pixel.
Bits Walls(const Bits& in, const Grid& g, double wallPx) {
  const size_t n = static_cast<size_t>(g.nx) * g.ny;
  std::vector<double> f(n);
  for (size_t i = 0; i < n; i++)
    f[i] = Get(in, i) ? kFar : 0.0;
  const int m = std::max(g.nx, g.ny);
  std::vector<double> col(m + 1), d(m + 1), z(m + 2);
  std::vector<int> v(m + 1);
  for (int i = 0; i < g.nx; i++) {
    for (int j = 0; j < g.ny; j++)
      col[j] = f[static_cast<size_t>(j) * g.nx + i];
    Edt1d(col, g.ny, d, v, z);
    for (int j = 0; j < g.ny; j++)
      f[static_cast<size_t>(j) * g.nx + i] = col[j];
  }
  for (int j = 0; j < g.ny; j++) {
    for (int i = 0; i < g.nx; i++)
      col[i] = f[static_cast<size_t>(j) * g.nx + i];
    Edt1d(col, g.nx, d, v, z);
    for (int i = 0; i < g.nx; i++)
      f[static_cast<size_t>(j) * g.nx + i] = col[i];
  }
  Bits w(g.Words(), 0);
  // A pixel whose centre is exactly the thickness in is not wall: the
  // tolerance keeps a cube whose wall is whole pixels exact.
  const double limit = wallPx + 0.5 - 1e-9;
  for (size_t i = 0; i < n; i++)
    if (Get(in, i) && std::sqrt(f[i]) < limit)
      Set(w, i);
  return w;
}

struct Sums {
  double solid = 0, shell = 0, core = 0;
  double m0 = 0, mu = 0, mv = 0, mw = 0;
  double muu = 0, mvv = 0, mww = 0, muv = 0, muw = 0, mvw = 0;
};

} // namespace

std::optional<Result> Estimate(const Body& body, const Input& in,
                               std::string* why) {
  const auto refuse = [&](const std::string& s) -> std::optional<Result> {
    if (why != nullptr)
      *why = s;
    return std::nullopt;
  };
  if (!(in.layerMm > 0.0) || !(in.lineWidthMm > 0.0))
    return refuse("a printed mass needs the layer height and the wall line "
                  "width (the print profile's layer_height_mm and "
                  "line_width_mm)");
  if (in.walls < 1 || in.topLayers < 0 || in.bottomLayers < 0 ||
      !(in.infill >= 0.0 && in.infill <= 1.0))
    return refuse("walls, shells or infill out of range");
  if (!body.shape.IsValid())
    return refuse("body '" + body.name + "' has no shape");
  const double ul = glm::length(in.up);
  if (!(ul > 1e-9))
    return refuse("no print direction");
  const glm::dvec3 up = in.up / ul;
  const glm::dvec3 wx(1.0, 0.0, 0.0), wy(0.0, 1.0, 0.0);
  glm::dvec3 bx = wx - glm::dot(wx, up) * up;
  if (glm::length(bx) < 1e-6)
    bx = wy - glm::dot(wy, up) * up;
  bx = glm::normalize(bx);
  const glm::dvec3 by = glm::cross(up, bx);

  // A chord well under the pixel: the raster, not the mesh, is the error.
  const CADShape::Mesh mesh =
      body.shape.GenerateMeshDetached(std::max(0.005, in.lineWidthMm / 10.0));
  if (mesh.indices.size() < 3)
    return refuse("body '" + body.name + "' did not mesh");
  std::vector<Tri> tris;
  tris.reserve(mesh.indices.size() / 3);
  glm::dvec3 lo(std::numeric_limits<double>::infinity());
  glm::dvec3 hi(-std::numeric_limits<double>::infinity());
  const auto frame = [&](uint32_t i) {
    const glm::dvec3 p(mesh.vertices[i].position);
    const glm::dvec3 q(glm::dot(p, bx), glm::dot(p, by), glm::dot(p, up));
    lo = glm::min(lo, q);
    hi = glm::max(hi, q);
    return q;
  };
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
    tris.push_back({frame(mesh.indices[t]), frame(mesh.indices[t + 1]),
                    frame(mesh.indices[t + 2])});

  const double height = hi.z - lo.z;
  const int layers =
      std::max(1, static_cast<int>(std::ceil(height / in.layerMm - 1e-6)));
  const double su = hi.x - lo.x, sv = hi.y - lo.y;
  double p = in.lineWidthMm / 4.0;
  p = std::max(p, std::sqrt(su * sv / kMaxLayerPixels));
  p = std::max(p, std::sqrt(su * sv * layers / kMaxPixels));
  Grid g;
  g.p = p;
  // One pixel of air all round, so the outline always has an outside.
  g.nx = static_cast<int>(std::ceil(su / p)) + 2;
  g.ny = static_cast<int>(std::ceil(sv / p)) + 2;
  g.u0 = lo.x - p;
  g.v0 = lo.y - p;

  // Each triangle in every layer whose sampling height it spans.
  std::vector<std::vector<uint32_t>> byLayer(static_cast<size_t>(layers));
  const auto zOf = [&](int i) {
    const double z0 = lo.z + i * in.layerMm;
    return z0 + 0.5 * std::min(in.layerMm, hi.z - z0);
  };
  for (uint32_t id = 0; id < tris.size(); id++) {
    const Tri& t = tris[id];
    const double zl = std::min({t.a.z, t.b.z, t.c.z});
    const double zh = std::max({t.a.z, t.b.z, t.c.z});
    int i0 = std::max(0, static_cast<int>((zl - lo.z) / in.layerMm) - 1);
    for (int i = i0; i < layers; i++) {
      const double zc = zOf(i);
      if (zc > zh)
        break;
      if (zc >= zl)
        byLayer[static_cast<size_t>(i)].push_back(id);
    }
  }

  const double wallPx = static_cast<double>(in.walls) * in.lineWidthMm / g.p;
  std::vector<Bits> inside(static_cast<size_t>(layers));
  std::vector<Bits> solid(static_cast<size_t>(layers));
  parallel::For(static_cast<size_t>(layers), [&](size_t i) {
    inside[i] = Rasterise(tris, byLayer[i], g, zOf(static_cast<int>(i)));
    solid[i] = Walls(inside[i], g, wallPx);
  });

  // The shells: a pixel with fewer than `bottom` material layers under it,
  // or fewer than `top` over it, is solid.
  const size_t n = static_cast<size_t>(g.nx) * g.ny;
  std::vector<uint16_t> run(n, 0);
  for (int i = 0; i < layers; i++) {
    const Bits& here = inside[static_cast<size_t>(i)];
    Bits& s = solid[static_cast<size_t>(i)];
    for (size_t k = 0; k < n; k++) {
      if (!Get(here, k)) {
        run[k] = 0;
        continue;
      }
      if (run[k] < in.bottomLayers)
        Set(s, k);
      if (run[k] < std::numeric_limits<uint16_t>::max())
        run[k]++;
    }
  }
  std::fill(run.begin(), run.end(), 0);
  Sums sum;
  const double area = g.p * g.p;
  for (int i = layers - 1; i >= 0; i--) {
    const Bits& here = inside[static_cast<size_t>(i)];
    Bits& s = solid[static_cast<size_t>(i)];
    const double z0 = lo.z + i * in.layerMm;
    const double t = std::min(in.layerMm, hi.z - z0);
    const double zc = z0 + 0.5 * t;
    const double dv = area * t;
    for (int j = 0; j < g.ny; j++) {
      const double v = g.v0 + (j + 0.5) * g.p;
      for (int ii = 0; ii < g.nx; ii++) {
        const size_t k = static_cast<size_t>(j) * g.nx + ii;
        if (!Get(here, k)) {
          run[k] = 0;
          continue;
        }
        if (run[k] < in.topLayers)
          Set(s, k);
        if (run[k] < std::numeric_limits<uint16_t>::max())
          run[k]++;
        const bool shell = Get(s, k);
        const double w = (shell ? 1.0 : in.infill) * dv;
        sum.solid += dv;
        (shell ? sum.shell : sum.core) += dv;
        const double u = g.u0 + (ii + 0.5) * g.p;
        sum.m0 += w;
        sum.mu += w * u;
        sum.mv += w * v;
        sum.mw += w * zc;
        sum.muu += w * (u * u + area / 12.0);
        sum.mvv += w * (v * v + area / 12.0);
        sum.mww += w * (zc * zc + t * t / 12.0);
        sum.muv += w * u * v;
        sum.muw += w * u * zc;
        sum.mvw += w * v * zc;
      }
    }
  }
  if (!(sum.solid > 0.0))
    return refuse("body '" + body.name + "' sliced to nothing");

  Result r;
  r.solidMm3 = sum.solid;
  r.shellMm3 = sum.shell;
  r.coreMm3 = sum.core;
  r.effectiveMm3 = sum.m0;
  r.layers = layers;
  r.pixelMm = g.p;
  if (sum.m0 > 0.0) {
    const double cu = sum.mu / sum.m0, cv = sum.mv / sum.m0,
                 cw = sum.mw / sum.m0;
    r.centerOfMass = bx * cu + by * cv + up * cw;
    const double suu = sum.muu - sum.m0 * cu * cu;
    const double svv = sum.mvv - sum.m0 * cv * cv;
    const double sww = sum.mww - sum.m0 * cw * cw;
    const double suv = sum.muv - sum.m0 * cu * cv;
    const double suw = sum.muw - sum.m0 * cu * cw;
    const double svw = sum.mvw - sum.m0 * cv * cw;
    // The inertia tensor in the print frame, then turned to the world's.
    const glm::dmat3 f(svv + sww, -suv, -suw, //
                       -suv, suu + sww, -svw, //
                       -suw, -svw, suu + svv);
    const glm::dmat3 rot(bx, by, up); // columns: frame -> world
    const glm::dmat3 w = rot * f * glm::transpose(rot);
    for (int a = 0; a < 3; a++)
      for (int b = 0; b < 3; b++)
        r.inertia[a][b] = w[b][a];
  }
  return r;
}

const char* MethodNote() {
  return "an estimate: the body sliced at the profile's layer and "
         "rasterised, a wall taken as walls x line width, the top and bottom "
         "shells as the slicer finds them, the rest weighted by the infill "
         "density. The slicer's real paths differ at small features";
}

std::optional<Result> EstimatePrinted(const printfit::Process* proc,
                                      const printset::Settings& s,
                                      const Body& body, std::string* why) {
  const auto refuse = [&](const std::string& m) -> std::optional<Result> {
    if (why != nullptr)
      *why = m;
    return std::nullopt;
  };
  if (proc == nullptr)
    return refuse("the part is not printed (set_print_process)");
  if (!proc->profile)
    return refuse("printed with the built-in table, which states no layer "
                  "or line width: print with a profile that states "
                  "layer_height_mm and line_width_mm");
  const printfit::Profile& prof = *proc->profile;
  std::string missing;
  const auto need = [&](bool have, const char* what) {
    if (!have)
      missing += (missing.empty() ? "" : ", ") + std::string(what);
  };
  need(prof.layerHeightMm > 0.0, "the profile's layer_height_mm");
  need(prof.lineWidthMm > 0.0, "the profile's line_width_mm");
  need(s.walls.has_value(), "walls");
  need(s.topLayers.has_value(), "top_layers");
  need(s.bottomLayers.has_value(), "bottom_layers");
  need(s.infillPct.has_value(), "infill_pct");
  if (!missing.empty())
    return refuse("no printed mass without " + missing +
                  " (set_print_settings; the slicer's own defaults are not "
                  "Loböle's to guess)");
  // Printed solid all through, the print IS the solid: its mass properties
  // exactly, off the B-Rep, with no raster to round them. (A pendulum's
  // period is computed from these, and a raster's pixel moving with the
  // rod's length would make the length search chase it.)
  if (*s.infillPct >= 100.0) {
    CADShape::MassProps mp;
    if (!body.shape.ComputeMassProps(mp) || !(mp.volume > 0.0))
      return refuse("body '" + body.name + "' has no volume to weigh");
    Result r;
    r.solidMm3 = r.shellMm3 = r.effectiveMm3 = mp.volume;
    r.centerOfMass = mp.centerOfMass;
    for (int a = 0; a < 3; a++)
      for (int b = 0; b < 3; b++)
        r.inertia[a][b] = mp.inertia[a][b];
    r.exact = true;
    return r;
  }
  Input in;
  in.layerMm = prof.layerHeightMm;
  in.lineWidthMm = prof.lineWidthMm;
  in.walls = *s.walls;
  in.topLayers = *s.topLayers;
  in.bottomLayers = *s.bottomLayers;
  in.infill = *s.infillPct / 100.0;
  if (s.direction)
    in.up = *s.direction;

  // Remembered: a generator weighs the same pendulum many times.
  using Key = std::tuple<uint64_t, double, double, int, int, int, double,
                         double, double, double>;
  static std::mutex mutex;
  // Each entry holds its shape, so the TShape the key's pointer names
  // cannot be freed and another shape born at that address while it is
  // remembered.
  static std::map<Key, std::pair<CADShape, Result>> cache;
  const Key key{body.shape.PlacementId(),
                in.layerMm,
                in.lineWidthMm,
                in.walls,
                in.topLayers,
                in.bottomLayers,
                in.infill,
                in.up.x,
                in.up.y,
                in.up.z};
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (const auto it = cache.find(key); it != cache.end())
      return it->second.second;
  }
  auto r = Estimate(body, in, why);
  if (r) {
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() > 64)
      cache.clear();
    cache.emplace(key, std::make_pair(body.shape, *r));
  }
  return r;
}

} // namespace printmass
