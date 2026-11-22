// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// GrowFrom and ScanTolerance (Segment.h): one region grown from a picked
// triangle under a robust fit, for a scan as much as a CAD export.

#include "core/mesh/Segment.h"

#include "core/mesh/Split.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace mesh {

namespace {

std::string Mm(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.3g", v);
  return buf;
}

// What a fit of a region reads: its distinct vertices (on the surface), and
// its triangles' centres with their unit normals (the oriented samples a
// hypothesis is drawn from -- a facet's normal is the surface's, where a
// vertex's at a sharp edge is the average of two surfaces'). At most
// `most` of each, an even stride through the rest.
struct Samples {
  std::vector<glm::dvec3> points, centres, normals;
};

// The distinct vertices of `tris`, ascending. `stamp` (one a vertex) is
// zero on entry and left so.
std::vector<uint32_t> VerticesOf(const TriMesh& m,
                                 const std::vector<uint32_t>& tris,
                                 std::vector<uint8_t>& stamp) {
  std::vector<uint32_t> verts;
  for (uint32_t t : tris)
    for (uint32_t v : m.triangles[t])
      if (!stamp[v]) {
        stamp[v] = 1;
        verts.push_back(v);
      }
  for (uint32_t v : verts)
    stamp[v] = 0;
  std::sort(verts.begin(), verts.end());
  return verts;
}

glm::dvec3 Centre(const TriMesh& m, uint32_t t) {
  return (m.Corner(t, 0) + m.Corner(t, 1) + m.Corner(t, 2)) / 3.0;
}

glm::dvec3 UnitNormal(const TriMesh& m, uint32_t t) {
  const glm::dvec3 a = m.AreaNormal(t);
  const double l = glm::length(a);
  return l > 0.0 ? a / l : glm::dvec3(0.0);
}

void Gather(const TriMesh& m, const std::vector<uint32_t>& tris,
            std::vector<uint8_t>& stamp, Samples& out, size_t most) {
  const std::vector<uint32_t> verts = VerticesOf(m, tris, stamp);
  out.points.clear();
  out.centres.clear();
  out.normals.clear();
  const size_t n = verts.size(), take = std::min(n, most);
  for (size_t i = 0; i < take; i++)
    out.points.push_back(m.positions[verts[take == n ? i : i * n / take]]);
  std::vector<uint32_t> sorted = tris;
  std::sort(sorted.begin(), sorted.end());
  const size_t nt = sorted.size(), takeT = std::min(nt, most);
  for (size_t i = 0; i < takeT; i++) {
    const uint32_t t = sorted[takeT == nt ? i : i * nt / takeT];
    const glm::dvec3 a = m.AreaNormal(t);
    const double l = glm::length(a);
    if (!(l > 0.0))
      continue;
    out.centres.push_back(Centre(m, t));
    out.normals.push_back(a / l);
  }
}

FitResult Robust(Kind kind, const Samples& s, double tol,
                 const glm::dvec3* through, const Admit& admit) {
  return FitRobust(kind, s.points, s.centres, s.normals, tol, through, admit);
}

// The middle of `v` (reordered); 0 when it is empty.
double Median(std::vector<double>& v) {
  if (v.empty())
    return 0.0;
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return v[v.size() / 2];
}

// The median edge of the mesh, from a deterministic stride through at most
// 200,000 of its triangles.
double MedianEdge(const TriMesh& m) {
  const size_t n = m.triangles.size();
  const size_t step = std::max<size_t>(1, n / 200000);
  std::vector<double> e;
  e.reserve(3 * (n / step + 1));
  for (size_t t = 0; t < n; t += step)
    for (int c = 0; c < 3; c++)
      e.push_back(glm::length(m.Corner(uint32_t(t), c) -
                              m.Corner(uint32_t(t), (c + 1) % 3)));
  return Median(e);
}

// How far a triangle's normal turns from the fit's at its centre: the
// cosine, on the seed's `side` (negative when it faces the other way), or
// 1 for a triangle with no area.
double TiltCos(const TriMesh& m, const FitResult& f, uint32_t t, double side) {
  const glm::dvec3 n = UnitNormal(m, t);
  if (n == glm::dvec3(0.0))
    return 1.0;
  return side * glm::dot(n, f.Normal(Centre(m, t)));
}

// The median tilt (rad) of `tris` from `f`, over at most `most` of them.
double MedianTilt(const TriMesh& m, const FitResult& f,
                  const std::vector<uint32_t>& tris, double side, size_t most) {
  if (tris.empty())
    return 0.0;
  const size_t n = tris.size(), take = std::min(n, most);
  std::vector<double> c;
  c.reserve(take);
  for (size_t i = 0; i < take; i++)
    c.push_back(TiltCos(m, f, tris[take == n ? i : i * n / take], side));
  // acos falls as the cosine rises: the median cosine's is the median tilt.
  return std::acos(std::clamp(Median(c), -1.0, 1.0));
}

// The fewest points a least-squares fit of `kind` takes.
size_t FewestPoints(Kind k) {
  switch (k) {
  case Kind::Plane:
    return 3;
  case Kind::Cylinder:
  case Kind::Sphere:
    return 5;
  case Kind::Cone:
    return 6;
  }
  return 6;
}

constexpr size_t kFitPoints = 65536;
const double kCreaseCos = std::cos(2.0 * M_PI / kMinSidesLowest);

} // namespace

double ScanTolerance(const TriMesh& m) {
  const double floor = 4.0 * m.quantum;
  if (m.neighbour.size() != m.triangles.size() || m.triangles.empty())
    return floor;
  // A deterministic stride through the triangles: a median is known well
  // from a few hundred thousand edges.
  const size_t n = m.triangles.size();
  const size_t step = std::max<size_t>(1, n / 200000);
  std::vector<double> d;
  d.reserve(3 * (n / step + 1));
  for (size_t t = 0; t < n; t += step) {
    const glm::dvec3 an = m.AreaNormal(uint32_t(t));
    const double l = glm::length(an);
    if (!(l > 0.0))
      continue;
    const glm::dvec3 unit = an / l, a = m.Corner(uint32_t(t), 0);
    for (int e = 0; e < 3; e++) {
      const uint32_t u = m.neighbour[t][e];
      if (u == kNone || u < t)
        continue;
      // A crease is a feature, not noise: an edge turning more than the
      // fewest-sided polygon recognition admits (kMinSidesLowest) is left
      // out, or a box's would set its tolerance at its own size.
      const glm::dvec3 un = m.AreaNormal(u);
      const double ul = glm::length(un);
      if (!(ul > 0.0) || glm::dot(unit, un / ul) < kCreaseCos)
        continue;
      // Two triangles in one plane to the file's precision say nothing of
      // the noise: a CAD export's flats would put a cylinder's facets'
      // own bend, which a fit from them must allow, below the median.
      const int opposite = (m.neighbourEdge[t][e] + 2) % 3;
      const double off = std::abs(glm::dot(m.Corner(u, opposite) - a, unit));
      if (off > floor)
        d.push_back(off);
    }
  }
  if (d.empty())
    return floor;
  std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
  return std::max(floor, 3.0 * 1.4826 * d[d.size() / 2]);
}

namespace {

Region Failed(Kind kind, std::string* why, std::vector<uint32_t>* covered,
              std::string msg, const std::vector<uint32_t>* reached = nullptr) {
  if (why != nullptr)
    *why = std::move(msg);
  if (covered != nullptr && reached != nullptr)
    *covered = *reached;
  Region failed;
  failed.fit.kind = kind;
  return failed;
}

// The used points of a region fitted by `fit` (step 5 of GrowFrom), what
// they say, and the refusals: a curved kind on a flat, a fit outside a
// hint.
Region Settle(const TriMesh& m, std::vector<uint32_t> region, Kind kind,
              double tol, FitResult fit, double side,
              const glm::dvec3& seedCentre, const glm::dvec3& seedNormal,
              const GrowOptions& opt, std::vector<uint8_t>& stamp,
              std::string* why, std::vector<uint32_t>* covered, bool judge) {
  Region r;
  r.fit.kind = kind;
  const FitHints& hints = opt.hints;
  const double precision = 4.0 * m.quantum;
  // 5. The used points: past clipSigma robust sigma of the fit left out and
  // the fit made over the rest, until the set stops changing.
  const std::vector<uint32_t> verts = VerticesOf(m, region, stamp);
  std::vector<double> d(verts.size());
  const auto residuals = [&] {
    for (size_t i = 0; i < verts.size(); i++)
      d[i] = fit.Distance(m.positions[verts[i]]);
  };
  residuals();
  std::vector<uint8_t> use(verts.size(), 1);
  if (opt.clipSigma > 0) {
    std::vector<double> mag(verts.size());
    std::vector<uint8_t> next(verts.size());
    for (int round = 0; round < 20; round++) {
      for (size_t i = 0; i < d.size(); i++)
        mag[i] = std::abs(d[i]);
      const double sigma = 1.4826 * Median(mag);
      const double cut = std::max(double(opt.clipSigma) * sigma, precision);
      size_t count = 0;
      for (size_t i = 0; i < d.size(); i++) {
        next[i] = std::abs(d[i]) <= cut ? 1 : 0;
        count += next[i];
      }
      if (round > 0 && next == use)
        break;
      if (count < FewestPoints(kind))
        break;
      use = next;
      const size_t take = std::min(count, kFitPoints);
      std::vector<glm::dvec3> pts;
      pts.reserve(take);
      for (size_t i = 0, k = 0; i < verts.size(); i++)
        if (use[i]) {
          // An even stride through the used points.
          if (k * take / count != (k + 1) * take / count)
            pts.push_back(m.positions[verts[i]]);
          k++;
        }
      std::vector<glm::dvec3> at, nrm;
      if (kind == Kind::Cone)
        for (uint32_t t : region) {
          const glm::dvec3 c = Centre(m, t), n = UnitNormal(m, t);
          if (n != glm::dvec3(0.0) && std::abs(fit.Distance(c)) <= cut) {
            at.push_back(c);
            nrm.push_back(n);
          }
        }
      FitResult g = FitLeastSquares(kind, fit, pts, at, nrm);
      if (!g.ok || !(std::abs(g.Distance(seedCentre)) <= tol))
        break;
      g.kind = kind;
      fit = g;
      residuals();
    }
  }

  // What the region and the used points say.
  double sum = 0.0, worst = 0.0, usedSum = 0.0;
  double lo = std::numeric_limits<double>::max(), hi = -lo;
  size_t used = 0;
  for (size_t i = 0; i < d.size(); i++) {
    sum += d[i] * d[i];
    worst = std::max(worst, std::abs(d[i]));
    if (!use[i])
      continue;
    used++;
    usedSum += d[i] * d[i];
    lo = std::min(lo, d[i]);
    hi = std::max(hi, d[i]);
  }
  fit.rms = d.empty() ? 0.0 : std::sqrt(sum / double(d.size()));
  fit.max = worst;
  fit.inliers = d.size();
  r.used = used;
  r.sigma = used == 0 ? 0.0 : std::sqrt(usedSum / double(used));
  r.residualLo = used == 0 ? 0.0 : lo;
  r.residualHi = used == 0 ? 0.0 : hi;
  // 6. Two surfaces the one fit averages are refused naming both, unless
  // a hint picks one of them out.
  if (judge) {
    fit.kind = kind;
    const TwoSurfaces two =
        SplitTwo(m, region, fit, opt.noiseMm > 0.0 ? opt.noiseMm : tol);
    if (two.two) {
      if (hints.outside || hints.radiusMin || hints.radiusMax || hints.axis) {
        // The one the hints describe, from its triangle nearest the seed.
        const std::vector<uint32_t>* meets = nullptr;
        int meeting = 0;
        uint32_t nearest = 0;
        for (const auto* tris : {&two.trianglesA, &two.trianglesB}) {
          if (tris->empty())
            continue;
          uint32_t t0 = tris->front();
          double d0 = std::numeric_limits<double>::max();
          for (uint32_t t : *tris)
            if (const double d = glm::length(Centre(m, t) - seedCentre);
                d < d0) {
              d0 = d;
              t0 = t;
            }
          const FitResult& f = tris == &two.trianglesA ? two.a : two.b;
          if (hints.Miss(f, Centre(m, t0), UnitNormal(m, t0)).empty()) {
            meeting++;
            meets = tris;
            nearest = t0;
          }
        }
        if (meeting == 1) {
          GrowOptions inner = opt;
          inner.judge = false;
          Region one = FitTriangles(m, *meets, nearest, kind, tol, why, inner);
          if (!one.fit.ok && covered != nullptr)
            *covered = region;
          return one;
        }
      }
      Region refused = Failed(kind, why, covered, two.Describe(), &region);
      refused.twoSurfaces = true;
      return refused;
    }
  }
  if (kind != Kind::Plane) {
    std::vector<glm::dvec3> pts;
    pts.reserve(verts.size());
    for (uint32_t v : verts)
      pts.push_back(m.positions[v]);
    const FitResult flat = FitPlane(pts);
    if (!flat.ok || flat.max <= tol)
      return Failed(kind, why, covered,
                    std::string("the surface at the point is flat to ") +
                        Mm(tol) + " mm over the " +
                        std::to_string(region.size()) +
                        (region.size() == 1 ? " triangle" : " triangles") +
                        " round it, so no " + KindName(kind) +
                        " is defined by it: fit a plane, or, if it is curved "
                        "and only rougher than that, a larger tolerance_mm",
                    &region);
  } else if (side < 0.0) {
    fit.axis = -fit.axis; // the way the triangles face
  }
  if (const std::string miss = hints.Miss(fit, seedCentre, seedNormal);
      !miss.empty())
    return Failed(kind, why, covered, miss, &region);

  // The extent of the used points: a cylinder's length and arc, a
  // sphere's cap, a plane's two spans.
  const auto usedPoint = [&](size_t i) { return m.positions[verts[i]]; };
  if (kind == Kind::Cylinder) {
    glm::dvec3 e1 = glm::normalize(
        glm::cross(fit.axis, std::abs(fit.axis.x) < 0.6 ? glm::dvec3(1, 0, 0)
                                                        : glm::dvec3(0, 1, 0)));
    const glm::dvec3 e2 = glm::cross(fit.axis, e1);
    constexpr int kBins = 3600; // a tenth of a degree
    std::vector<uint8_t> bins(kBins, 0);
    double a0 = std::numeric_limits<double>::max(), a1 = -a0;
    for (size_t i = 0; i < verts.size(); i++) {
      if (!use[i])
        continue;
      const glm::dvec3 w = usedPoint(i) - fit.origin;
      const double h = glm::dot(w, fit.axis);
      a0 = std::min(a0, h);
      a1 = std::max(a1, h);
      stamp[verts[i]] = 1;
    }
    r.length = used == 0 ? 0.0 : a1 - a0;
    // The turn the surface covers: each triangle with two used corners
    // marks the arc between its corners, so a coarse export's facets cover
    // what their corners span.
    const auto angle = [&](const glm::dvec3& p) {
      const glm::dvec3 w = p - fit.origin;
      return std::atan2(glm::dot(w, e2), glm::dot(w, e1));
    };
    for (uint32_t t : region) {
      int usedCorners = 0;
      for (uint32_t v : m.triangles[t])
        usedCorners += stamp[v];
      if (usedCorners < 2)
        continue;
      const double base = angle(m.Corner(t, 0));
      double lo = 0.0, hi = 0.0;
      for (int c = 1; c < 3; c++) {
        const double dth =
            std::remainder(angle(m.Corner(t, c)) - base, 2.0 * M_PI);
        lo = std::min(lo, dth);
        hi = std::max(hi, dth);
      }
      if (!(hi - lo < M_PI))
        continue; // round the axis: not a facet of the side
      const auto bin = [&](double th) {
        const double u = std::fmod(th + 3.0 * M_PI, 2.0 * M_PI);
        return std::min(kBins - 1, int(u / (2.0 * M_PI) * kBins));
      };
      const int b0 = bin(base + lo);
      const int n = std::max(1, int((hi - lo) / (2.0 * M_PI) * kBins) + 1);
      for (int k = 0; k < n; k++)
        bins[(b0 + k) % kBins] = 1;
    }
    for (uint32_t v : verts)
      stamp[v] = 0;
    // The arc is the turn less its largest empty run of bins, round the
    // wrap.
    int gap = 0, run = 0, filled = 0;
    for (int i = 0; i < 2 * kBins; i++) {
      if (bins[i % kBins]) {
        run = 0;
        filled += i < kBins ? 1 : 0;
      } else {
        gap = std::max(gap, std::min(++run, kBins));
      }
    }
    r.arc = filled == 0 ? 0.0 : 2.0 * M_PI * double(kBins - gap) / kBins;
  } else if (kind == Kind::Sphere) {
    glm::dvec3 mean(0.0);
    for (size_t i = 0; i < verts.size(); i++)
      if (use[i])
        mean += glm::normalize(usedPoint(i) - fit.origin);
    double widest = M_PI;
    if (glm::length(mean) > 1e-9 * double(std::max<size_t>(used, 1))) {
      mean = glm::normalize(mean);
      widest = 0.0;
      for (size_t i = 0; i < verts.size(); i++)
        if (use[i])
          widest = std::max(
              widest,
              std::acos(std::clamp(
                  glm::dot(glm::normalize(usedPoint(i) - fit.origin), mean),
                  -1.0, 1.0)));
    }
    r.arc = 2.0 * widest;
  } else if (kind == Kind::Plane) {
    glm::dvec3 e1 = glm::normalize(
        glm::cross(fit.axis, std::abs(fit.axis.x) < 0.6 ? glm::dvec3(1, 0, 0)
                                                        : glm::dvec3(0, 1, 0)));
    const glm::dvec3 e2 = glm::cross(fit.axis, e1);
    glm::dvec2 c(0.0);
    for (size_t i = 0; i < verts.size(); i++)
      if (use[i]) {
        const glm::dvec3 w = usedPoint(i) - fit.origin;
        c += glm::dvec2(glm::dot(w, e1), glm::dot(w, e2));
      }
    c /= double(std::max<size_t>(used, 1));
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (size_t i = 0; i < verts.size(); i++)
      if (use[i]) {
        const glm::dvec3 w = usedPoint(i) - fit.origin;
        const glm::dvec2 q = glm::dvec2(glm::dot(w, e1), glm::dot(w, e2)) - c;
        sxx += q.x * q.x;
        sxy += q.x * q.y;
        syy += q.y * q.y;
      }
    // The principal direction of the in-plane scatter.
    const double th = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
    const glm::dvec2 u(std::cos(th), std::sin(th)), v(-u.y, u.x);
    glm::dvec2 lo2(std::numeric_limits<double>::max()), hi2(-lo2);
    for (size_t i = 0; i < verts.size(); i++)
      if (use[i]) {
        const glm::dvec3 w = usedPoint(i) - fit.origin;
        const glm::dvec2 q(glm::dot(w, e1), glm::dot(w, e2));
        const glm::dvec2 s(glm::dot(q, u), glm::dot(q, v));
        lo2 = glm::min(lo2, s);
        hi2 = glm::max(hi2, s);
      }
    const glm::dvec2 span = used == 0 ? glm::dvec2(0.0) : hi2 - lo2;
    r.extent = {std::max(span.x, span.y), std::min(span.x, span.y)};
  }

  std::sort(region.begin(), region.end());
  r.fit = fit;
  r.firstTriangle = region.front();
  for (uint32_t t : region)
    r.area += m.Area(t);
  r.triangles = std::move(region);
  return r;
}

} // namespace

Region GrowFrom(const TriMesh& m, uint32_t seed, Kind kind, double tol,
                std::string* why, const GrowOptions& opt,
                std::vector<uint32_t>* covered) {
  Region r;
  r.fit.kind = kind;
  if (covered != nullptr)
    covered->clear();
  const auto fail = [&](std::string msg,
                        const std::vector<uint32_t>* reached = nullptr) {
    if (why != nullptr)
      *why = std::move(msg);
    if (covered != nullptr && reached != nullptr)
      *covered = *reached;
    Region failed;
    failed.fit.kind = kind;
    return failed;
  };
  const FitHints& hints = opt.hints;
  if (seed >= m.triangles.size() || m.neighbour.size() != m.triangles.size())
    return fail("the point is on no triangle of the mesh");
  if (!(tol > 0.0) || !std::isfinite(tol))
    return fail("the tolerance is not a number of mm greater than 0");
  if (opt.clipSigma < 0 || opt.clipSigma > 3)
    return fail("clip_sigma is 1, 2, 3 or \"all\"");
  if (const char* h = hints.NotFor(kind))
    return fail(std::string(h) + " does not apply to a " + KindName(kind));
  if (!(m.Area(seed) > 0.0))
    return fail("the triangle at the point has no area");
  std::vector<uint8_t> stamp(m.positions.size(), 0);
  std::vector<uint8_t> in(m.triangles.size(), 0);
  Samples smp;
  const bool boxed = hints.withinLo && hints.withinHi;
  const auto inBox = [&](uint32_t t) {
    if (!boxed)
      return true;
    for (uint32_t v : m.triangles[t])
      if (!hints.Inside(m.positions[v]))
        return false;
    return true;
  };

  // 1. The seed disk.
  std::vector<uint32_t> disk{seed}, frontier{seed};
  in[seed] = 1;
  size_t lastFit = 0;
  for (int ring = 0; !frontier.empty(); ring++) {
    std::vector<uint32_t> next;
    for (uint32_t t : frontier)
      for (int e = 0; e < 3; e++) {
        const uint32_t u = m.neighbour[t][e];
        if (u != kNone && !in[u] && inBox(u)) {
          in[u] = 1;
          next.push_back(u);
        }
      }
    disk.insert(disk.end(), next.begin(), next.end());
    frontier = std::move(next);
    if (ring < 1 || disk.size() < 12 || disk.size() < lastFit * 3 / 2)
      continue;
    lastFit = disk.size();
    if (kind == Kind::Plane)
      break;
    Gather(m, disk, stamp, smp, kFitPoints);
    const FitResult flat = FitPlane(smp.points);
    if (flat.ok && flat.max > 3.0 * tol)
      break;
  }
  for (uint32_t t : disk)
    in[t] = 0;
  Gather(m, disk, stamp, smp, kFitPoints);
  // The surface the seed is on: through its centre, and (a hint's) not
  // drawn outside a hint.
  const glm::dvec3 seedCentre = Centre(m, seed);
  const glm::dvec3 seedNormal = UnitNormal(m, seed);
  Admit admit;
  if (hints.outside || hints.radiusMin || hints.radiusMax || hints.axis)
    admit = [&](const FitResult& f) {
      return hints.Miss(f, seedCentre, seedNormal).empty();
    };
  FitResult fit = Robust(kind, smp, tol, &seedCentre, admit);
  if (!fit.ok)
    return fail(std::string("no ") + KindName(kind) +
                (admit ? " meeting " + hints.Describe() : std::string()) +
                " fits the surface round the point to " + Mm(tol) + " mm");

  // 2. The normal band: the scan's own tilt, from the disk under its fit.
  const double side =
      glm::dot(m.AreaNormal(seed), fit.Normal(seedCentre)) < 0.0 ? -1.0 : 1.0;
  const double edge = MedianEdge(m);
  const double floorTilt = edge > 0.0 ? std::atan(tol / edge) : 0.0;
  const double band = std::min(
      M_PI / 2.0,
      std::max(3.0 * 1.4826 * MedianTilt(m, fit, disk, side, disk.size()),
               floorTilt));
  const double cosBand = std::cos(band);
  // A triangle so thin that the file's precision cannot fix its normal to
  // the band is judged by what that precision allows its least height.
  const double precision = 4.0 * m.quantum;
  const auto takes = [&](const FitResult& f, uint32_t t) {
    for (uint32_t v : m.triangles[t])
      if (!(std::abs(f.Distance(m.positions[v])) <= tol))
        return false;
    if (!inBox(t))
      return false;
    const glm::dvec3 an = m.AreaNormal(t);
    const double l = glm::length(an);
    if (!(l > 0.0))
      return false;
    const double c = side * glm::dot(an / l, f.Normal(Centre(m, t)));
    if (!(c > 0.0))
      return false; // facing the other way
    if (c >= cosBand)
      return true;
    double e3[3];
    for (int e = 0; e < 3; e++)
      e3[e] = glm::length(m.Corner(t, e) - m.Corner(t, (e + 1) % 3));
    std::sort(e3, e3 + 3);
    if (!(precision > 0.0))
      return false;
    const double allow = std::atan(precision / (l / e3[2]));
    return allow > band && c >= std::cos(std::min(M_PI / 2.0, allow));
  };

  // 3. Regrown from the seed under each new fit, each regrowth capped at
  // three times the one before, until two in a row stop short of the cap.
  const auto grow = [&](const FitResult& f, size_t cap) {
    std::vector<uint32_t> region{seed};
    in[seed] = 1;
    for (size_t i = 0; i < region.size() && region.size() < cap; i++) {
      const uint32_t t = region[i];
      for (int e = 0; e < 3 && region.size() < cap; e++) {
        const uint32_t u = m.neighbour[t][e];
        if (u == kNone || in[u] || !takes(f, u))
          continue;
        in[u] = 1;
        region.push_back(u);
      }
    }
    for (uint32_t t : region)
      in[t] = 0;
    return region;
  };
  std::vector<uint32_t> region;
  size_t prev = std::max<size_t>(disk.size(), 4);
  bool wasShort = false;
  for (int round = 0; round < 64; round++) {
    const size_t cap = 3 * prev;
    region = grow(fit, cap);
    const bool capped = region.size() >= cap;
    Gather(m, region, stamp, smp, kFitPoints);
    if (const FitResult g = Robust(kind, smp, tol, &seedCentre, admit); g.ok)
      fit = g;
    if (!capped && wasShort)
      break;
    wasShort = !capped;
    prev = region.size();
  }

  // 4. The rim peeled, ring by ring, while it leans off the fit more than
  // the region does.
  {
    const double own = MedianTilt(m, fit, region, side, 200000);
    // Never below what the file's precision tilts a triangle of the median
    // edge by: a clean export's flats lean off their fit by nothing, and
    // twice nothing is not a rim.
    const double limit =
        std::max(2.0 * own, edge > 0.0 ? std::atan(precision / edge) : 0.0);
    double area = 0.0;
    for (uint32_t t : region) {
      in[t] = 1;
      area += m.Area(t);
    }
    const double least = 0.5 * area;
    std::vector<uint32_t> ring;
    for (uint32_t t : region)
      for (int e = 0; e < 3; e++) {
        const uint32_t u = m.neighbour[t][e];
        if (u == kNone || !in[u]) {
          ring.push_back(t);
          break;
        }
      }
    std::vector<uint8_t> next(m.triangles.size(), 0);
    bool peeled = false;
    while (!ring.empty()) {
      if (!(MedianTilt(m, fit, ring, side, ring.size()) > limit))
        break;
      double ringArea = 0.0;
      for (uint32_t t : ring)
        ringArea += m.Area(t);
      if (area - ringArea < least)
        break;
      for (uint32_t t : ring)
        in[t] = 0;
      area -= ringArea;
      peeled = true;
      std::vector<uint32_t> inner;
      for (uint32_t t : ring)
        for (int e = 0; e < 3; e++) {
          const uint32_t u = m.neighbour[t][e];
          if (u != kNone && in[u] && !next[u]) {
            next[u] = 1;
            inner.push_back(u);
          }
        }
      for (uint32_t u : inner)
        next[u] = 0;
      ring = std::move(inner);
    }
    if (peeled) {
      std::vector<uint32_t> kept;
      for (uint32_t t : region)
        if (in[t])
          kept.push_back(t);
      region = std::move(kept);
      Gather(m, region, stamp, smp, kFitPoints);
      if (const FitResult g = Robust(kind, smp, tol, &seedCentre, admit); g.ok)
        fit = g;
    }
    for (uint32_t t : region)
      in[t] = 0;
  }

  Region settled =
      Settle(m, std::move(region), kind, tol, fit, side, seedCentre, seedNormal,
             opt, stamp, why, covered, opt.judge);
  if (!settled.fit.ok || kind != Kind::Plane || !opt.judge)
    return settled;

  // 6. A plane on the crown of a round: refused naming the round.
  const double noise = opt.noiseMm > 0.0 ? opt.noiseMm : tol;
  const PlaneBow bow =
      BowOf(m, settled.triangles, settled.fit, settled.extent, settled.area);
  if (!bow.Crown(settled.fit.rms > kSplitAbove * noise))
    return settled;
  char buf[160];
  std::string msg = "the plane fitted here is a strip along the crown of a "
                    "round: ";
  if (bow.spreadRad > kCrownSpreadDeg * M_PI / 180.0) {
    std::snprintf(buf, sizeof buf,
                  "its normals turn %.1f deg across it (a flat's stay "
                  "within %.0f)",
                  bow.spreadRad * 180.0 / M_PI, kCrownSpreadDeg);
  } else {
    std::snprintf(buf, sizeof buf,
                  "it covers %.0f%% of its bounding rectangle (a flat face "
                  "at least %.0f%%) and bows past the noise, rms %.3g mm",
                  100.0 * bow.cover, 100.0 * kCrownCover, settled.fit.rms);
  }
  msg += buf;
  // What does fit there: the round's cylinder, or a sphere. The plane's
  // own hints (a normal) are not a round's; the box is.
  GrowOptions round = opt;
  round.hints = FitHints{};
  round.hints.withinLo = hints.withinLo;
  round.hints.withinHi = hints.withinHi;
  std::string roundWhy;
  const Region cyl = GrowFrom(m, seed, Kind::Cylinder, tol, &roundWhy, round);
  if (cyl.fit.ok) {
    glm::dvec3 a = cyl.fit.axis;
    int big = 0;
    for (int i = 1; i < 3; i++)
      if (std::abs(a[i]) > std::abs(a[big]))
        big = i;
    if (a[big] < 0.0)
      a = -a;
    std::snprintf(buf, sizeof buf,
                  "; a cylinder fits there: R%.4g mm, axis [%.4g, %.4g, "
                  "%.4g] through [%.4g, %.4g, %.4g] -- fit kind cylinder",
                  cyl.fit.radius, a.x, a.y, a.z, cyl.fit.origin.x,
                  cyl.fit.origin.y, cyl.fit.origin.z);
    msg += buf;
  } else if (const Region sph =
                 GrowFrom(m, seed, Kind::Sphere, tol, nullptr, round);
             sph.fit.ok) {
    std::snprintf(buf, sizeof buf,
                  "; a sphere fits there: R%.4g mm about [%.4g, %.4g, "
                  "%.4g] -- fit kind sphere",
                  sph.fit.radius, sph.fit.origin.x, sph.fit.origin.y,
                  sph.fit.origin.z);
    msg += buf;
  } else {
    msg += "; no cylinder or sphere fits there either (" + roundWhy + ")";
  }
  return fail(msg, &settled.triangles);
}

Region FitTriangles(const TriMesh& m, std::vector<uint32_t> triangles,
                    uint32_t seed, Kind kind, double tol, std::string* why,
                    const GrowOptions& opt) {
  if (seed >= m.triangles.size() || triangles.empty())
    return Failed(kind, why, nullptr, "no triangles to fit");
  for (uint32_t t : triangles)
    if (t >= m.triangles.size())
      return Failed(kind, why, nullptr, "a triangle is not in the mesh");
  if (!(tol > 0.0) || !std::isfinite(tol))
    return Failed(kind, why, nullptr,
                  "the tolerance is not a number of mm greater than 0");
  if (const char* h = opt.hints.NotFor(kind))
    return Failed(kind, why, nullptr,
                  std::string(h) + " does not apply to a " + KindName(kind));
  std::vector<uint8_t> stamp(m.positions.size(), 0);
  Samples smp;
  Gather(m, triangles, stamp, smp, kFitPoints);
  const glm::dvec3 seedCentre = Centre(m, seed);
  const glm::dvec3 seedNormal = UnitNormal(m, seed);
  const FitHints& hints = opt.hints;
  Admit admit;
  if (hints.outside || hints.radiusMin || hints.radiusMax || hints.axis)
    admit = [&](const FitResult& f) {
      return hints.Miss(f, seedCentre, seedNormal).empty();
    };
  const FitResult fit = Robust(kind, smp, tol, &seedCentre, admit);
  if (!fit.ok)
    return Failed(kind, why, nullptr,
                  std::string("no ") + KindName(kind) +
                      " fits the triangles to " + Mm(tol) + " mm");
  const double side =
      glm::dot(m.AreaNormal(seed), fit.Normal(seedCentre)) < 0.0 ? -1.0 : 1.0;
  return Settle(m, std::move(triangles), kind, tol, fit, side, seedCentre,
                seedNormal, opt, stamp, why, nullptr, /*judge=*/false);
}

} // namespace mesh
