// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// SplitTwo and BowOf (Split.h): what a region that one surface does not
// explain is.

#include "core/mesh/Split.h"
#include "core/Angles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace mesh {

namespace {

// The split is judged on at most this many of the region's vertices, an
// even stride: two surfaces and their scatter are known well from a few
// thousand, and the fits it repeats are linear in them.
constexpr size_t kSplitPoints = 4096;
// A quadric's bow is read from at most this many.
constexpr size_t kBowPoints = 16384;
// Each surface is fitted from at least this many.
constexpr size_t kLeastEach = 24;

std::string Fmt(double v) {
  // 4.68, 12.4, 0.30, 0.050: never fewer than one decimal.
  const double a = std::abs(v);
  const int decimals = a >= 10.0 ? 1 : a >= 0.1 ? 2 : 3;
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

std::string Vec(const glm::dvec3& p) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "[%.4g, %.4g, %.4g]", p.x, p.y, p.z);
  return buf;
}

glm::dvec3 Canonical(const glm::dvec3& a) {
  int big = 0;
  for (int i = 1; i < 3; i++)
    if (std::abs(a[i]) > std::abs(a[big]))
      big = i;
  return a[big] < 0.0 ? -a : a;
}

void Basis(const glm::dvec3& n, glm::dvec3& e1, glm::dvec3& e2) {
  e1 = glm::normalize(glm::cross(n, std::abs(n.x) < 0.6 ? glm::dvec3(1, 0, 0)
                                                        : glm::dvec3(0, 1, 0)));
  e2 = glm::cross(n, e1);
}

// The distinct vertices of `region`, ascending.
std::vector<uint32_t> Vertices(const TriMesh& m,
                               const std::vector<uint32_t>& region) {
  std::vector<uint32_t> v;
  v.reserve(region.size() * 3);
  for (uint32_t t : region)
    for (uint32_t c : m.triangles[t])
      v.push_back(c);
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

std::vector<glm::dvec3>
Stride(const TriMesh& m, const std::vector<uint32_t>& verts, size_t most) {
  const size_t n = verts.size(), take = std::min(n, most);
  std::vector<glm::dvec3> out;
  out.reserve(take);
  for (size_t i = 0; i < take; i++)
    out.push_back(m.positions[verts[take == n ? i : i * n / take]]);
  return out;
}

double Median(std::vector<double> v) {
  if (v.empty())
    return 0.0;
  std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
  return v[v.size() / 2];
}

// Solves the 6 x 6 system `a x = b` in place, by elimination with partial
// pivoting. False when it is singular.
bool Solve6(std::array<double, 36>& a, std::array<double, 6>& b) {
  constexpr int n = 6;
  for (int col = 0; col < n; col++) {
    int pivot = col;
    for (int r = col + 1; r < n; r++)
      if (std::abs(a[r * n + col]) > std::abs(a[pivot * n + col]))
        pivot = r;
    if (!(std::abs(a[pivot * n + col]) > 1e-300))
      return false;
    if (pivot != col) {
      for (int c = 0; c < n; c++)
        std::swap(a[col * n + c], a[pivot * n + c]);
      std::swap(b[col], b[pivot]);
    }
    for (int r = col + 1; r < n; r++) {
      const double f = a[r * n + col] / a[col * n + col];
      for (int c = col; c < n; c++)
        a[r * n + c] -= f * a[col * n + c];
      b[r] -= f * b[col];
    }
  }
  for (int i = n - 1; i >= 0; i--) {
    double s = b[i];
    for (int c = i + 1; c < n; c++)
      s -= a[i * n + c] * b[c];
    b[i] = s / a[i * n + i];
  }
  return true;
}

// The height over a plane as a quadric of the in-plane coordinates:
// z = c0 + c1 u + c2 v + c3 u² + c4 uv + c5 v², u and v scaled by `scale`
// about `centre` for the solve's sake.
struct Quadric {
  bool ok = false;
  glm::dvec3 centre{0.0}, e1{0.0}, e2{0.0}, n{0.0};
  double scale = 1.0;
  std::array<double, 6> c{};
  double rms = 0.0;

  [[nodiscard]] glm::dvec2 Uv(const glm::dvec3& p) const {
    const glm::dvec3 w = p - centre;
    return {glm::dot(w, e1) / scale, glm::dot(w, e2) / scale};
  }
  [[nodiscard]] double Height(const glm::dvec2& q) const {
    return c[0] + c[1] * q.x + c[2] * q.y + c[3] * q.x * q.x +
           c[4] * q.x * q.y + c[5] * q.y * q.y;
  }
  // dz/dx and dz/dy in mm per mm.
  [[nodiscard]] glm::dvec2 Gradient(const glm::dvec2& q) const {
    return {(c[1] + 2.0 * c[3] * q.x + c[4] * q.y) / scale,
            (c[2] + c[4] * q.x + 2.0 * c[5] * q.y) / scale};
  }
};

Quadric FitQuadric(const std::vector<glm::dvec3>& pts, const glm::dvec3& n) {
  Quadric q;
  if (pts.size() < 12)
    return q;
  q.n = glm::normalize(n);
  Basis(q.n, q.e1, q.e2);
  glm::dvec3 c(0.0);
  for (const glm::dvec3& p : pts)
    c += p;
  q.centre = c / double(pts.size());
  double reach = 0.0;
  for (const glm::dvec3& p : pts) {
    const glm::dvec3 w = p - q.centre;
    reach = std::max(reach, std::max(std::abs(glm::dot(w, q.e1)),
                                     std::abs(glm::dot(w, q.e2))));
  }
  if (!(reach > 0.0))
    return q;
  q.scale = reach;
  std::array<double, 36> a{};
  std::array<double, 6> b{};
  for (const glm::dvec3& p : pts) {
    const glm::dvec2 uv = q.Uv(p);
    const double z = glm::dot(p - q.centre, q.n);
    const std::array<double, 6> f{1.0,         uv.x,        uv.y,
                                  uv.x * uv.x, uv.x * uv.y, uv.y * uv.y};
    for (int i = 0; i < 6; i++) {
      b[i] += f[i] * z;
      for (int j = 0; j < 6; j++)
        a[i * 6 + j] += f[i] * f[j];
    }
  }
  if (!Solve6(a, b))
    return q;
  q.c = b;
  double sum = 0.0;
  for (const glm::dvec3& p : pts) {
    const double r = glm::dot(p - q.centre, q.n) - q.Height(q.Uv(p));
    sum += r * r;
  }
  q.rms = std::sqrt(sum / double(pts.size()));
  q.ok = true;
  return q;
}

// The rms of the radius about `fit`'s axis from the best line in the
// distance along it: a radius varying evenly along the axis.
double EvenRadiusRms(const std::vector<glm::dvec3>& pts, const FitResult& fit) {
  const glm::dvec3 a = glm::normalize(fit.axis);
  double sh = 0.0, sr = 0.0, shh = 0.0, shr = 0.0;
  std::vector<glm::dvec2> hr(pts.size());
  for (size_t i = 0; i < pts.size(); i++) {
    const glm::dvec3 w = pts[i] - fit.origin;
    const double h = glm::dot(w, a);
    const double r = glm::length(w - h * a);
    hr[i] = {h, r};
    sh += h;
    sr += r;
  }
  const double n = double(pts.size());
  const double mh = sh / n, mr = sr / n;
  for (const glm::dvec2& p : hr) {
    shh += (p.x - mh) * (p.x - mh);
    shr += (p.x - mh) * (p.y - mr);
  }
  const double slope = shh > 0.0 ? shr / shh : 0.0;
  double sum = 0.0;
  for (const glm::dvec2& p : hr) {
    const double r = p.y - (mr + slope * (p.x - mh));
    sum += r * r;
  }
  return std::sqrt(sum / n);
}

FitResult FitOne(Kind kind, const std::vector<glm::dvec3>& pts,
                 const FitResult& start) {
  if (kind == Kind::Plane) {
    FitResult f = FitPlane(pts);
    if (f.ok && glm::dot(f.axis, start.axis) < 0.0)
      f.axis = -f.axis;
    return f;
  }
  FitResult f = FitCylinder(pts, start.axis);
  f.kind = kind;
  return f;
}

// The two surfaces of an assignment `side` (0 or 1 a point), refitted and
// reassigned until it stops changing. False when either side has too few
// points to fit.
bool TwoMeans(Kind kind, const std::vector<glm::dvec3>& pts,
              const FitResult& start, std::vector<uint8_t>& side, FitResult& a,
              FitResult& b, double& cost) {
  FitResult sa = start, sb = start;
  std::vector<glm::dvec3> pa, pb;
  for (int round = 0; round < 12; round++) {
    pa.clear();
    pb.clear();
    for (size_t i = 0; i < pts.size(); i++)
      (side[i] ? pb : pa).push_back(pts[i]);
    if (pa.size() < kLeastEach || pb.size() < kLeastEach)
      return false;
    a = FitOne(kind, pa, sa);
    b = FitOne(kind, pb, sb);
    if (!a.ok || !b.ok)
      return false;
    sa = a;
    sb = b;
    size_t changed = 0;
    cost = 0.0;
    for (size_t i = 0; i < pts.size(); i++) {
      const double da = std::abs(a.Distance(pts[i]));
      const double db = std::abs(b.Distance(pts[i]));
      const uint8_t s = db < da ? 1 : 0;
      changed += s != side[i] ? 1 : 0;
      side[i] = s;
      cost += std::min(da, db) * std::min(da, db);
    }
    if (changed == 0)
      break;
  }
  return true;
}

} // namespace

TwoSurfaces SplitTwo(const TriMesh& m, const std::vector<uint32_t>& region,
                     const FitResult& fit, double noiseMm) {
  TwoSurfaces out;
  const Kind kind = fit.kind;
  if (!fit.ok || (kind != Kind::Plane && kind != Kind::Cylinder) ||
      !(fit.rms > kSplitAbove * noiseMm))
    return out;
  const std::vector<uint32_t> verts = Vertices(m, region);
  const std::vector<glm::dvec3> pts = Stride(m, verts, kSplitPoints);
  if (pts.size() < 2 * kLeastEach)
    return out;
  out.examined = true;
  const size_t n = pts.size();

  // The starts: the residual's sign, and the region cut across at its
  // quartiles -- along the axis for a cylinder, along both of a plane's
  // in-plane principal directions.
  std::vector<std::vector<uint8_t>> starts;
  {
    std::vector<uint8_t> s(n);
    for (size_t i = 0; i < n; i++)
      s[i] = fit.Distance(pts[i]) < 0.0 ? 0 : 1;
    starts.push_back(std::move(s));
  }
  std::vector<glm::dvec3> across;
  if (kind == Kind::Cylinder) {
    across.push_back(glm::normalize(fit.axis));
  } else {
    glm::dvec3 e1, e2;
    Basis(fit.axis, e1, e2);
    glm::dvec3 c(0.0);
    for (const glm::dvec3& p : pts)
      c += p;
    c /= double(n);
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (const glm::dvec3& p : pts) {
      const double x = glm::dot(p - c, e1), y = glm::dot(p - c, e2);
      sxx += x * x;
      sxy += x * y;
      syy += y * y;
    }
    const double th = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
    across.push_back(std::cos(th) * e1 + std::sin(th) * e2);
    across.push_back(-std::sin(th) * e1 + std::cos(th) * e2);
  }
  for (const glm::dvec3& dir : across) {
    std::vector<double> h(n);
    for (size_t i = 0; i < n; i++)
      h[i] = glm::dot(pts[i], dir);
    std::vector<double> sorted = h;
    std::sort(sorted.begin(), sorted.end());
    for (const double q : {0.25, 0.5, 0.75}) {
      const double cut = sorted[size_t(q * double(n - 1))];
      std::vector<uint8_t> s(n);
      for (size_t i = 0; i < n; i++)
        s[i] = h[i] <= cut ? 0 : 1;
      starts.push_back(std::move(s));
    }
  }

  std::vector<uint8_t> best;
  FitResult a, b;
  double bestCost = std::numeric_limits<double>::max();
  for (std::vector<uint8_t>& s : starts) {
    FitResult fa, fb;
    double cost = 0.0;
    if (TwoMeans(kind, pts, fit, s, fa, fb, cost) && cost < bestCost) {
      bestCost = cost;
      best = s;
      a = fa;
      b = fb;
    }
  }
  if (best.empty())
    return out;

  // Each refitted over its own points within three robust sigma of it: the
  // vertices of a shoulder between them belong to neither.
  const auto own = [&](size_t i) {
    return best[i] ? b.Distance(pts[i]) : a.Distance(pts[i]);
  };
  for (int pass = 0; pass < 2; pass++) {
    std::array<std::vector<double>, 2> mag;
    for (size_t i = 0; i < n; i++)
      mag[best[i]].push_back(std::abs(own(i)));
    std::array<double, 2> cut{3.0 * 1.4826 * Median(mag[0]),
                              3.0 * 1.4826 * Median(mag[1])};
    std::array<std::vector<glm::dvec3>, 2> keep;
    for (size_t i = 0; i < n; i++)
      if (std::abs(own(i)) <= cut[best[i]])
        keep[best[i]].push_back(pts[i]);
    if (keep[0].size() < kLeastEach || keep[1].size() < kLeastEach)
      break;
    const FitResult ra = FitOne(kind, keep[0], a);
    const FitResult rb = FitOne(kind, keep[1], b);
    if (!ra.ok || !rb.ok)
      break;
    a = ra;
    b = rb;
  }

  // The figures the split is judged by.
  std::array<size_t, 2> count{0, 0};
  std::array<std::vector<double>, 2> mag, other;
  double sum = 0.0;
  for (size_t i = 0; i < n; i++) {
    const int s = best[i];
    const double d = own(i);
    count[s]++;
    mag[s].push_back(std::abs(d));
    other[s].push_back(std::abs(s ? a.Distance(pts[i]) : b.Distance(pts[i])));
    sum += d * d;
  }
  out.twoRms = std::sqrt(sum / double(n));
  out.scatter =
      1.4826 * std::max(Median(mag[0]), Median(mag[1])); // robust sigma
  out.apart = std::min(Median(other[0]), Median(other[1]));
  // For cylinders, the axis the two agree on: the one fit across both is
  // tilted by the step it averages (0.85 deg on a stepped bore).
  FitResult axial = a;
  if (kind == Kind::Cylinder) {
    const glm::dvec3 ab =
        glm::dot(a.axis, b.axis) < 0.0 ? a.axis - b.axis : a.axis + b.axis;
    if (glm::length(ab) > 0.0)
      axial.axis = glm::normalize(ab);
  }
  if (kind == Kind::Plane) {
    const Quadric q = FitQuadric(pts, fit.axis);
    out.smoothRms = q.ok ? q.rms : 0.0;
  } else {
    // Whichever axis explains it better: across a taper the two halves'
    // axes lean, across a step the one fit's does.
    out.smoothRms =
        std::min(EvenRadiusRms(pts, fit), EvenRadiusRms(pts, axial));
  }

  // Order them: the smaller radius, or the lower plane, first.
  const auto mean = [&](int s) {
    glm::dvec3 c(0.0);
    for (size_t i = 0; i < n; i++)
      if (best[i] == s)
        c += pts[i];
    return c / double(std::max<size_t>(count[s], 1));
  };
  glm::dvec3 ca = mean(0), cb = mean(1);
  const bool swap = kind == Kind::Cylinder ? b.radius < a.radius
                                           : glm::dot(cb - ca, fit.axis) < 0.0;
  if (swap) {
    std::swap(a, b);
    std::swap(ca, cb);
    std::swap(count[0], count[1]);
    for (uint8_t& s : best)
      s ^= 1;
  }
  out.a = a;
  out.b = b;
  out.shareA = double(count[0]) / double(n);
  out.shareB = double(count[1]) / double(n);
  out.two = out.shareA >= kSplitShare && out.shareB >= kSplitShare &&
            out.apart > kSplitApart * out.scatter &&
            out.smoothRms > kSplitSmooth * out.twoRms;

  if (kind == Kind::Cylinder) {
    // Where they step: the cut along the axis that the fewest points of
    // either stand on the wrong side of.
    out.axis = Canonical(axial.axis);
    const glm::dvec3 o = axial.origin;
    double hmin = std::numeric_limits<double>::max();
    for (uint32_t v : verts)
      hmin = std::min(hmin, glm::dot(m.positions[v] - o, out.axis));
    out.from = o + hmin * out.axis;
    std::vector<std::pair<double, uint8_t>> hs(n);
    for (size_t i = 0; i < n; i++)
      hs[i] = {glm::dot(pts[i] - o, out.axis), best[i]};
    std::sort(hs.begin(), hs.end());
    // Either may be the lower one.
    for (int low = 0; low < 2; low++) {
      // A cut below everything puts every point of the lower one wrong.
      long wrong = 0;
      for (const auto& [h, s] : hs)
        wrong += s == low ? 1 : 0;
      long fewest = wrong;
      size_t at = 0;
      for (size_t k = 0; k < n; k++) {
        wrong += hs[k].second == low ? -1 : 1;
        if (wrong < fewest) {
          fewest = wrong;
          at = k + 1;
        }
      }
      if (at > 0 && at < n && double(fewest) < 0.05 * double(n)) {
        out.steps = true;
        out.stepAt = 0.5 * (hs[at - 1].first + hs[at].first) - hmin;
        break;
      }
    }
  } else {
    out.throughA = a.Project(ca);
    out.throughB = b.Project(cb);
    out.offset = std::abs(b.Distance(out.throughA));
    out.angle = std::acos(std::min(1.0, std::abs(glm::dot(a.axis, b.axis))));
  }

  // The triangles, by the surface most of their corners are nearer.
  if (out.two)
    for (uint32_t t : region) {
      int onA = 0;
      for (uint32_t v : m.triangles[t])
        onA += std::abs(out.a.Distance(m.positions[v])) <=
                       std::abs(out.b.Distance(m.positions[v]))
                   ? 1
                   : 0;
      (onA >= 2 ? out.trianglesA : out.trianglesB).push_back(t);
    }
  return out;
}

std::string TwoSurfaces::Describe() const {
  if (a.kind == Kind::Cylinder) {
    std::string s = "two cylinders here: R" + Fmt(a.radius) + " mm and R" +
                    Fmt(b.radius) + " mm";
    if (steps)
      s += ", stepping at " + Fmt(stepAt) + " mm along the axis " + Vec(axis) +
           " from " + Vec(from);
    else
      s += ", side by side";
    s += "; click one, or give ";
    s += std::abs(b.radius - a.radius) > 2.0 * scatter
             ? "radius_mm"
             : "within (a box round the one you mean)";
    return s;
  }
  std::string s = "two planes here: " + Fmt(offset) + " mm apart and " +
                  Fmt(angles::ToDegrees(angle)) +
                  " deg to each other, through " + Vec(throughA) + " and " +
                  Vec(throughB) + "; click one, or give ";
  s += angles::ToDegrees(angle) > 1.0 ? "axis (the normal of the one you mean)"
                                      : "within (a box round the one you mean)";
  return s;
}

bool PlaneBow::Crown(bool bowsPastNoise) const {
  return spreadRad > angles::ToRadians(kCrownSpreadDeg) ||
         (cover < kCrownCover && bowsPastNoise);
}

PlaneBow BowOf(const TriMesh& m, const std::vector<uint32_t>& region,
               const FitResult& plane, const glm::dvec2& extent, double area) {
  PlaneBow bow;
  const double box = extent.x * extent.y;
  bow.cover = box > 0.0 ? area / box : 1.0;
  const std::vector<glm::dvec3> pts =
      Stride(m, Vertices(m, region), kBowPoints);
  const Quadric q = FitQuadric(pts, plane.axis);
  if (!q.ok)
    return bow;
  // The gradients' spread across the region: its widest extent over a
  // half turn of directions, two normals at +-g apart turning 2 atan(g).
  std::vector<glm::dvec2> g(pts.size());
  for (size_t i = 0; i < pts.size(); i++)
    g[i] = q.Gradient(q.Uv(pts[i]));
  double widest = 0.0;
  for (int k = 0; k < 8; k++) {
    const double th = M_PI * k / 8.0;
    const glm::dvec2 d(std::cos(th), std::sin(th));
    double lo = std::numeric_limits<double>::max(), hi = -lo;
    for (const glm::dvec2& v : g) {
      lo = std::min(lo, glm::dot(v, d));
      hi = std::max(hi, glm::dot(v, d));
    }
    widest = std::max(widest, hi - lo);
  }
  bow.spreadRad = 2.0 * std::atan(0.5 * widest);
  return bow;
}

} // namespace mesh
