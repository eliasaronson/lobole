// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/ReadThread.h"

#include "core/Angles.h"
#include "core/ThreadStandards.h"
#include "core/mesh/ReadRound.h"
#include "core/mesh/Slice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace mesh {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr int kSectors = 12;
constexpr int kProfileBins = 48;

// The axis of a surface of revolution from its normals' second moment
// (Jacobi rotations): the eigenvector whose eigenvalue stands apart.
glm::dvec3 AxisEigenvector(double A[3][3], glm::dvec3* all = nullptr) {
  double V[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 50; sweep++) {
    double off = std::abs(A[0][1]) + std::abs(A[0][2]) + std::abs(A[1][2]);
    if (off < 1e-15)
      break;
    for (int p = 0; p < 2; p++)
      for (int q = p + 1; q < 3; q++) {
        if (std::abs(A[p][q]) < 1e-18)
          continue;
        const double th = 0.5 * std::atan2(2.0 * A[p][q], A[q][q] - A[p][p]);
        const double c = std::cos(th), s = std::sin(th);
        for (int k = 0; k < 3; k++) {
          const double akp = A[k][p], akq = A[k][q];
          A[k][p] = c * akp - s * akq;
          A[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < 3; k++) {
          const double apk = A[p][k], aqk = A[q][k];
          A[p][k] = c * apk - s * aqk;
          A[q][k] = s * apk + c * aqk;
        }
        for (int k = 0; k < 3; k++) {
          const double vkp = V[k][p], vkq = V[k][q];
          V[k][p] = c * vkp - s * vkq;
          V[k][q] = s * vkp + c * vkq;
        }
      }
  }
  // The odd one out: a surface of revolution's normals lie on a cone (a
  // thread's flanks) or a plane (a plain cylinder) about its axis, so
  // the other two eigenvalues are equal and the axis's is not.
  int m = 0;
  double best = -1.0;
  for (int k = 0; k < 3; k++) {
    const int i = (k + 1) % 3, j = (k + 2) % 3;
    const double odd = std::abs(A[k][k] - 0.5 * (A[i][i] + A[j][j])) -
                       std::abs(A[i][i] - A[j][j]);
    if (odd > best) {
      best = odd;
      m = k;
    }
  }
  if (all != nullptr)
    for (int k = 0; k < 3; k++)
      all[k] = glm::normalize(glm::dvec3(V[0][k], V[1][k], V[2][k]));
  return glm::normalize(glm::dvec3(V[0][m], V[1][m], V[2][m]));
}

double Median(std::vector<double> v) {
  if (v.empty())
    return 0.0;
  const size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + m, v.end());
  return v[m];
}

// The circle through 2D points, algebraically (Kåsa): its centre and
// radius.
bool FitCircle(const std::vector<glm::dvec2>& pts, glm::dvec2& c, double& r) {
  if (pts.size() < 3)
    return false;
  glm::dvec2 m(0.0);
  for (const glm::dvec2& p : pts)
    m += p;
  m /= double(pts.size());
  double suu = 0, suv = 0, svv = 0, su3 = 0, sv3 = 0, s2 = 0;
  for (const glm::dvec2& p : pts) {
    const glm::dvec2 q = p - m;
    const double r2 = glm::dot(q, q);
    suu += q.x * q.x;
    suv += q.x * q.y;
    svv += q.y * q.y;
    su3 += q.x * r2;
    sv3 += q.y * r2;
    s2 += r2;
  }
  const double det = suu * svv - suv * suv;
  if (!(std::abs(det) > 1e-18 * (suu + svv) * (suu + svv)))
    return false;
  const double a = 0.5 * (su3 * svv - sv3 * suv) / det;
  const double b = 0.5 * (sv3 * suu - su3 * suv) / det;
  c = m + glm::dvec2(a, b);
  r = std::sqrt(a * a + b * b + s2 / double(pts.size()));
  return std::isfinite(r);
}

// A signal's normalised autocorrelation at lag L.
double AutoCorr(const std::vector<double>& f, size_t first, size_t count,
                double L) {
  const size_t lo = static_cast<size_t>(std::floor(L));
  const double w = L - double(lo);
  double num = 0.0, den = 0.0;
  for (size_t i = first; i < first + count; i++) {
    den += f[i] * f[i];
    if (i + lo + 1 < first + count)
      num += f[i] * ((1.0 - w) * f[i + lo] + w * f[i + lo + 1]);
  }
  return den > 0.0 ? num / den : 0.0;
}

// The lag in [lo, hi] (bins) where a's and b's correlation peaks:
// sum a(i) b(i + L), refined by a parabola.
double PeakLag(const std::vector<double>& a, const std::vector<double>& b,
               int lo, int hi, double* value = nullptr) {
  const int n = static_cast<int>(std::min(a.size(), b.size()));
  const auto corr = [&](int L) {
    double s = 0.0;
    for (int i = std::max(0, -L); i < n && i + L < n; i++)
      s += a[i] * b[i + L];
    return s;
  };
  int best = lo;
  double bv = -kInf;
  for (int L = lo; L <= hi; L++)
    if (const double v = corr(L); v > bv) {
      bv = v;
      best = L;
    }
  if (value != nullptr)
    *value = bv;
  if (best <= lo || best >= hi)
    return best;
  const double y0 = corr(best - 1), y1 = bv, y2 = corr(best + 1);
  const double d = y0 - 2.0 * y1 + y2;
  return d < 0.0 ? best + 0.5 * (y0 - y2) / d : best;
}

struct Trapezoidal {
  double d, p;
};
// ISO 2904 trapezoidal threads, the preferred pitch of each size.
const Trapezoidal kTrapezoidal[] = {
    {8, 1.5}, {10, 2}, {12, 3}, {14, 3},  {16, 4},  {18, 4},  {20, 4},  {22, 5},
    {24, 5},  {26, 5}, {28, 5}, {30, 6},  {32, 6},  {36, 6},  {40, 7},  {44, 7},
    {48, 8},  {52, 8}, {60, 9}, {70, 10}, {80, 10}, {90, 12}, {100, 12}};

std::string Number(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

} // namespace

ThreadReading ReadThread(const std::vector<const TriMesh*>& meshes,
                         const glm::dvec3& point) {
  ThreadReading out;
  const TriMesh* m = nullptr;
  const double nearest = NearestOnMeshes(meshes, point, &m);
  if (m == nullptr) {
    out.noScan = true;
    out.why = "no scan: the body has no mesh";
    return out;
  }
  const double spacing = std::max(MedianEdgeLength(*m), 1e-9);
  if (nearest > 30.0 * spacing) {
    out.noScan = true;
    out.why = "no scan at the point: the nearest of the mesh is " +
              Number(nearest) + " mm away";
    return out;
  }
  // The cylinder: its axis from the triangles' normals (square to it), its
  // centre and radius from their corners, regrown about it.
  glm::dvec3 axis(0.0, 0.0, 1.0), centre = point;
  double radius = 0.0;
  bool internal = false;
  const auto centroid = [&](uint32_t t) {
    return (m->Corner(t, 0) + m->Corner(t, 1) + m->Corner(t, 2)) / 3.0;
  };
  // First a ball about the point, grown until it spans as much of the
  // surface as its radius: a patch narrower than that holds too little of
  // the round for its normals to name the axis (a thread's flanks tilt
  // them along it more than a narrow patch turns them round it).
  glm::dvec3 lo, hi;
  m->Bounds(lo, hi);
  const double diagonal = glm::length(hi - lo);
  double reach = 30.0 * spacing, halfLength = kInf;
  std::vector<uint32_t> band;
  int grown = 0;
  for (int pass = 0; pass < 4; pass++) {
    band.clear();
    double A[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (uint32_t t = 0; t < m->triangles.size(); t++) {
      const glm::dvec3 c = centroid(t);
      if (pass == 0) {
        if (glm::length(c - point) > reach)
          continue;
      } else {
        const glm::dvec3 q = c - centre;
        const double z = glm::dot(q, axis);
        const double rho = glm::length(q - axis * z);
        if (std::abs(z - glm::dot(point - centre, axis)) > halfLength ||
            std::abs(rho - radius) > 0.5 * radius)
          continue;
      }
      const glm::dvec3 an = m->AreaNormal(t);
      const double a = glm::length(an);
      if (!(a > 0.0))
        continue;
      const glm::dvec3 n = an / a;
      for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
          A[i][j] += a * n[i] * n[j];
      band.push_back(t);
    }
    if (band.size() < 50) {
      out.why = "too little of the scan about the point to read a cylinder";
      return out;
    }
    // The axis: the normals' odd principal direction. On a patch narrower
    // than the round that can be something else, so the ball below grows
    // until it spans the round.
    axis = AxisEigenvector(A);
    glm::dvec3 u, v;
    PlaneAxes(axis, u, v);
    std::vector<glm::dvec2> flat;
    for (uint32_t t : band) {
      const glm::dvec3 q = centroid(t) - point;
      flat.push_back({glm::dot(q, u), glm::dot(q, v)});
    }
    glm::dvec2 c2(0.0);
    const bool fitted = FitCircle(flat, c2, radius);
    if (!fitted || (radius > 10.0 * reach && reach >= 0.5 * diagonal)) {
      out.noThread = true;
      out.why = "no thread at the point: the surface there is not round";
      return out;
    }
    // How far round the fitted axis the patch reaches.
    double cover = 0.0;
    if (pass == 0) {
      std::vector<double> ang;
      for (const glm::dvec2& f : flat)
        ang.push_back(std::atan2(f.y - c2.y, f.x - c2.x));
      std::sort(ang.begin(), ang.end());
      double gap = ang.empty() ? angles::kTwoPi
                               : ang.front() + angles::kTwoPi - ang.back();
      for (size_t k = 1; k < ang.size(); k++)
        gap = std::max(gap, ang[k] - ang[k - 1]);
      cover = angles::kTwoPi - gap;
    }
    if (pass == 0 &&
        (2.0 * radius > reach || cover < angles::ToRadians(150.0)) &&
        reach < 0.5 * diagonal && grown < 12) {
      reach = std::min(0.5 * diagonal, 1.6 * reach);
      grown++;
      pass = -1; // the ball again, wider
      continue;
    }
    centre = point + u * c2.x + v * c2.y;
    halfLength = 4.0 * radius;
    reach = 3.0 * radius;
  }
  // Which way the triangles face: away from the axis for a bolt.
  double facing = 0.0;
  for (uint32_t t : band) {
    const glm::dvec3 q = centroid(t) - centre;
    facing += glm::dot(m->AreaNormal(t), q - axis * glm::dot(q, axis));
  }
  internal = facing < 0.0;
  out.internal = internal;
  out.axisDir = axis;
  out.axisPoint = centre;
  // Radius against axial position, in sectors, from the band's vertices.
  glm::dvec3 u, v;
  PlaneAxes(axis, u, v);
  std::vector<uint8_t> seen(m->positions.size(), 0);
  struct Sample {
    double z, theta, rho;
  };
  std::vector<Sample> samples;
  double zlo = kInf, zhi = -kInf;
  for (uint32_t t : band)
    for (int k = 0; k < 3; k++) {
      const uint32_t vi = m->triangles[t][k];
      if (seen[vi])
        continue;
      seen[vi] = 1;
      const glm::dvec3 q = m->positions[vi] - centre;
      const double z = glm::dot(q, axis);
      const double x = glm::dot(q, u), y = glm::dot(q, v);
      samples.push_back({z, std::atan2(y, x), std::hypot(x, y)});
      zlo = std::min(zlo, z);
      zhi = std::max(zhi, z);
    }
  out.lengthMm = zhi - zlo;
  const double dz = spacing;
  const int bins = static_cast<int>(std::floor((zhi - zlo) / dz)) + 1;
  if (bins < 40) {
    out.noThread = true;
    out.why = "no thread at the point: the cylinder read is too short";
    return out;
  }
  std::vector<std::vector<double>> sum(kSectors,
                                       std::vector<double>(bins, 0.0));
  std::vector<std::vector<int>> cnt(kSectors, std::vector<int>(bins, 0));
  for (const Sample& s : samples) {
    double th = s.theta < 0.0 ? s.theta + angles::kTwoPi : s.theta;
    const int sec = std::min(kSectors - 1,
                             static_cast<int>(th / angles::kTwoPi * kSectors));
    const int b = std::min(bins - 1, static_cast<int>((s.z - zlo) / dz));
    sum[sec][b] += s.rho;
    cnt[sec][b]++;
  }
  std::vector<std::vector<double>> f(kSectors);
  for (int s = 0; s < kSectors; s++) {
    std::vector<double>& g = f[s];
    g.assign(bins, std::numeric_limits<double>::quiet_NaN());
    for (int b = 0; b < bins; b++)
      if (cnt[s][b] > 0)
        g[b] = sum[s][b] / cnt[s][b];
    // Empty bins filled between their neighbours.
    int last = -1;
    for (int b = 0; b < bins; b++) {
      if (std::isnan(g[b]))
        continue;
      if (last < 0)
        for (int k = 0; k < b; k++)
          g[k] = g[b];
      else
        for (int k = last + 1; k < b; k++)
          g[k] = g[last] + (g[b] - g[last]) * double(k - last) / (b - last);
      last = b;
    }
    if (last < 0)
      g.assign(bins, radius);
    for (int k = last + 1; k < bins; k++)
      g[k] = g[last];
    double mean = 0.0;
    for (double x : g)
      mean += x;
    mean /= bins;
    for (double& x : g)
      x -= mean;
  }
  // The pitch: the first strong peak of the sectors' mean autocorrelation.
  const int maxLag = bins / 3;
  std::vector<double> ac(maxLag + 1, 0.0);
  for (int L = 0; L <= maxLag; L++) {
    for (int s = 0; s < kSectors; s++)
      ac[L] += AutoCorr(f[s], 0, bins, L);
    ac[L] /= kSectors;
  }
  int firstNeg = 1;
  while (firstNeg <= maxLag && ac[firstNeg] > 0.0)
    firstNeg++;
  double top = -kInf;
  for (int L = firstNeg; L <= maxLag; L++)
    top = std::max(top, ac[L]);
  int peak = -1;
  for (int L = std::max(firstNeg, 1); L < maxLag; L++)
    if (ac[L] >= ac[L - 1] && ac[L] >= ac[L + 1] && ac[L] >= 0.8 * top) {
      peak = L;
      break;
    }
  if (peak < 0 || top < 0.5) {
    out.noThread = true;
    out.why = "no thread at the point: a plain cylinder of diameter " +
              Number(2.0 * radius) +
              " mm (its radius repeats along the "
              "axis no more than noise does)";
    return out;
  }
  // Windows of four turns, each sector's own pitch in it.
  const double p0 = peak * dz;
  const int win = std::min(bins, static_cast<int>(std::ceil(4.0 * peak)));
  std::vector<double> pitches;
  for (int s = 0; s < kSectors; s++)
    for (int start = 0; start + win <= bins; start += std::max(1, peak)) {
      const int lo = std::max(1, static_cast<int>(0.6 * peak));
      const int hi = std::min(win - 2, static_cast<int>(1.5 * peak) + 1);
      double best = -kInf;
      int bl = -1;
      for (int L = lo; L <= hi; L++)
        if (const double c = AutoCorr(f[s], start, win, L); c > best) {
          best = c;
          bl = L;
        }
      if (bl <= lo || bl >= hi)
        continue;
      const double y0 = AutoCorr(f[s], start, win, bl - 1),
                   y2 = AutoCorr(f[s], start, win, bl + 1);
      const double d = y0 - 2.0 * best + y2;
      pitches.push_back((d < 0.0 ? bl + 0.5 * (y0 - y2) / d : bl) * dz);
    }
  if (pitches.empty()) {
    out.noThread = true;
    out.why = "no thread at the point: no window of a few turns repeats";
    return out;
  }
  double pitch = Median(pitches);
  out.windows = static_cast<int>(pitches.size());
  std::vector<double> dev;
  for (double p : pitches) {
    out.windowsAgree += std::abs(p - pitch) < 0.05 * pitch;
    dev.push_back(std::abs(p - pitch));
  }
  if (out.windowsAgree * 2 < out.windows) {
    out.noThread = true;
    out.why = "no thread at the point: windows of a few turns do not agree "
              "on a pitch (" +
              std::to_string(out.windowsAgree) + " of " +
              std::to_string(out.windows) + " near " + Number(pitch) + " mm)";
    return out;
  }
  out.pitchMm = pitch;
  // Windows of one sector overlap and neighbouring sectors share the scan:
  // counted as independent only per sector's distinct turns.
  const double perSector = std::max(1.0, double(out.windows) / kSectors);
  out.pitchSigmaMm = 1.4826 * Median(dev) / std::sqrt(perSector);
  (void)p0;
  // The hand: how far each sector's pattern is shifted from the one before
  // it, round the axis (a right-hand thread advances with the angle).
  const double pb = pitch / dz;
  const int span = std::max(1, static_cast<int>(std::ceil(0.5 * pb)));
  double shift = 0.0;
  for (int s = 0; s < kSectors; s++)
    shift += PeakLag(f[s], f[(s + 1) % kSectors], -span, span);
  shift /= kSectors;
  out.phaseShare = shift / (pb / kSectors);
  out.rightHand = out.phaseShare > 0.0;
  const double hand = out.rightHand ? 1.0 : -1.0;
  // The pitch refined: the one whose fold over every turn and sector
  // leaves the sharpest profile (the most variance between its bins). The
  // autocorrelation is read on bins a mesh edge wide; the fold is not.
  const auto sharpness = [&](double P) {
    std::vector<double> sum(kProfileBins, 0.0), sq(kProfileBins, 0.0);
    std::vector<int> n(kProfileBins, 0);
    for (const Sample& smp : samples) {
      double ph = (smp.z - hand * P * smp.theta / angles::kTwoPi) / P;
      ph -= std::floor(ph);
      const int b =
          std::min(kProfileBins - 1, static_cast<int>(ph * kProfileBins));
      sum[b] += smp.rho;
      n[b]++;
    }
    double mean = 0.0, all = 0.0;
    for (int b = 0; b < kProfileBins; b++)
      if (n[b] > 0) {
        mean += sum[b];
        all += n[b];
      }
    mean /= std::max(all, 1.0);
    double between = 0.0;
    for (int b = 0; b < kProfileBins; b++)
      if (n[b] > 0)
        between += n[b] * std::pow(sum[b] / n[b] - mean, 2);
    return between;
  };
  {
    double lo = 0.97 * pitch, hi = 1.03 * pitch;
    // A coarse scan first (the sharpness has side lobes a turn apart over
    // the length read), then golden sections about the best.
    double best = pitch, bv = sharpness(pitch);
    for (int i = 0; i <= 60; i++) {
      const double P = lo + (hi - lo) * i / 60.0;
      if (const double v = sharpness(P); v > bv) {
        bv = v;
        best = P;
      }
    }
    double a = best - (hi - lo) / 60.0, b = best + (hi - lo) / 60.0;
    const double g = 0.5 * (std::sqrt(5.0) - 1.0);
    double x1 = b - g * (b - a), x2 = a + g * (b - a);
    double f1 = sharpness(x1), f2 = sharpness(x2);
    for (int it = 0; it < 40; it++) {
      if (f1 > f2) {
        b = x2;
        x2 = x1;
        f2 = f1;
        x1 = b - g * (b - a);
        f1 = sharpness(x1);
      } else {
        a = x1;
        x1 = x2;
        f1 = f2;
        x2 = a + g * (b - a);
        f2 = sharpness(x2);
      }
    }
    pitch = 0.5 * (a + b);
    out.pitchMm = pitch;
  }
  // The profile folded over every turn and sector.
  std::vector<double> prof(kProfileBins, 0.0);
  std::vector<int> pc(kProfileBins, 0);
  for (const Sample& s : samples) {
    double ph = (s.z - hand * pitch * s.theta / angles::kTwoPi) / pitch;
    ph -= std::floor(ph);
    const int b =
        std::min(kProfileBins - 1, static_cast<int>(ph * kProfileBins));
    prof[b] += s.rho;
    pc[b]++;
  }
  double rmax = -kInf, rmin = kInf;
  for (int b = 0; b < kProfileBins; b++) {
    if (pc[b] == 0)
      continue;
    prof[b] /= pc[b];
    rmax = std::max(rmax, prof[b]);
    rmin = std::min(rmin, prof[b]);
  }
  out.majorDiameterMm = 2.0 * rmax;
  out.minorDiameterMm = 2.0 * rmin;
  // The flanks: the profile's slope between a third and two thirds of its
  // depth.
  std::vector<double> slopes;
  const double depth = rmax - rmin;
  for (int b = 0; b < kProfileBins; b++) {
    const int nb = (b + 1) % kProfileBins;
    if (pc[b] == 0 || pc[nb] == 0)
      continue;
    const double mid = 0.5 * (prof[b] + prof[nb]) - rmin;
    if (mid < depth / 3.0 || mid > 2.0 * depth / 3.0)
      continue;
    slopes.push_back(std::abs(prof[nb] - prof[b]) / (pitch / kProfileBins));
  }
  const double slope = Median(slopes);
  out.flankDeg = slope > 0.0 ? angles::ToDegrees(std::atan(1.0 / slope)) : 0.0;
  // The nearest standard: trapezoidal for a shallow flank, else metric or
  // unified.
  double score = kInf;
  const double D = out.majorDiameterMm;
  const auto consider = [&](const std::string& name, double d, double p) {
    const double sc = std::abs(D - d) / d + std::abs(pitch - p) / p;
    if (sc < score) {
      score = sc;
      out.nearest = name;
      out.nearestMajorMm = d;
      out.nearestPitchMm = p;
    }
  };
  if (out.flankDeg > 0.0 && out.flankDeg < 22.5) {
    for (const Trapezoidal& t : kTrapezoidal)
      consider("Tr" + Number(t.d) + "x" + Number(t.p), t.d, t.p);
  } else {
    for (const threads::Standard& s : threads::All())
      consider(s.designation, s.majorDia, s.pitch);
  }
  if (!out.rightHand && !out.nearest.empty())
    out.nearest += " LH";
  out.ok = true;
  return out;
}

} // namespace mesh
