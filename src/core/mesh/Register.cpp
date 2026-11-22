// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/Register.h"

#include "core/Angles.h"
#include "core/Parallel.h"
#include "core/mesh/Fit.h" // SymmetricEigen3

#include <algorithm>
#include <cmath>
#include <limits>

namespace mesh {

namespace {

uint64_t Mix(uint64_t x) { // splitmix64
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

double Uniform(uint64_t& state) { // [0, 1)
  state = Mix(state);
  return double(state >> 11) / double(1ull << 53);
}

// The rotation by the vector `w` (its length the angle, rad).
glm::dmat3 Rodrigues(const glm::dvec3& w) {
  const double a = glm::length(w);
  if (!(a > 0.0))
    return glm::dmat3(1.0);
  const glm::dvec3 k = w / a;
  const glm::dmat3 K(0.0, k.z, -k.y, -k.z, 0.0, k.x, k.y, -k.x, 0.0);
  return glm::dmat3(1.0) + std::sin(a) * K + (1.0 - std::cos(a)) * (K * K);
}

// Solves the symmetric 6x6 system A x = b by Gaussian elimination with
// partial pivoting. False when it is singular.
bool Solve6(double A[6][6], double b[6], double x[6]) {
  double scale = 0.0;
  for (int i = 0; i < 6; i++)
    scale = std::max(scale, std::abs(A[i][i]));
  if (!(scale > 0.0))
    return false;
  for (int c = 0; c < 6; c++) {
    int piv = c;
    for (int r = c + 1; r < 6; r++)
      if (std::abs(A[r][c]) > std::abs(A[piv][c]))
        piv = r;
    if (!(std::abs(A[piv][c]) > 1e-14 * scale))
      return false;
    if (piv != c) {
      for (int k = 0; k < 6; k++)
        std::swap(A[c][k], A[piv][k]);
      std::swap(b[c], b[piv]);
    }
    for (int r = c + 1; r < 6; r++) {
      const double f = A[r][c] / A[c][c];
      for (int k = c; k < 6; k++)
        A[r][k] -= f * A[c][k];
      b[r] -= f * b[c];
    }
  }
  for (int r = 5; r >= 0; r--) {
    double s = b[r];
    for (int k = r + 1; k < 6; k++)
      s -= A[r][k] * x[k];
    x[r] = s / A[r][r];
  }
  for (int i = 0; i < 6; i++)
    if (!std::isfinite(x[i]))
      return false;
  return true;
}

// A right-handed frame of the eigenvectors of `covariance`, widest spread
// first.
glm::dmat3 PrincipalFrame(const glm::dmat3& covariance) {
  glm::dvec3 values;
  glm::dmat3 vectors;
  SymmetricEigen3(covariance, values, vectors);
  glm::dmat3 f(glm::normalize(vectors[2]), glm::normalize(vectors[1]),
               glm::normalize(vectors[0]));
  if (glm::determinant(f) < 0.0)
    f[2] = -f[2];
  return f;
}

// What one sample, laid by a pose, is matched to.
struct Match {
  glm::dvec3 at{0.0};     // the sample where the pose lays it
  glm::dvec3 onto{0.0};   // the nearest surface facing its way
  glm::dvec3 normal{0.0}; // that triangle's
  double distance = 0.0;
  bool found = false;
};

void MatchAll(const std::vector<SurfaceSample>& samples, const Rigid& pose,
              const FacingSurface& target, double minCos, double cap,
              std::vector<Match>& out) {
  out.resize(samples.size());
  for (size_t i = 0; i < samples.size(); i++) {
    Match& m = out[i];
    m.at = pose(samples[i].point);
    const glm::dvec3 facing = pose.rotation * samples[i].normal;
    const Bvh::Closest c = target.Nearest(m.at, facing, minCos, cap);
    m.found = c.found;
    if (c.found) {
      m.onto = c.point;
      m.normal = target.Normal(c.triangle);
      m.distance = c.distance;
    } else {
      m.distance = cap;
    }
  }
}

double CappedMean(const std::vector<Match>& matches, double cap) {
  if (matches.empty())
    return cap;
  double sum = 0.0;
  for (const Match& m : matches)
    sum += std::min(m.distance, cap);
  return sum / double(matches.size());
}

// The point-to-point term's weight beside point-to-plane: enough to hold
// the motions a flat region leaves free (sliding in its plane, turning
// about its normal), too little to drag a pose off the planes.
constexpr double kPointWeight = 1e-3;

// Below this shape quality (4√3 A / Σ e², 1 for an equilateral) a
// triangle's own normal is noise, and a sample on it takes its corners'
// vertex normals blended.
constexpr double kSliver = 0.2;

struct StartState {
  Rigid pose;
  double score = 0.0;
  // How far a pass looks for a pair: the cap at first, then a few times
  // the last pass's median distance -- the far ones are left out of the
  // step anyway, and a search that gives up early is a fast one.
  double reach = 0.0;
  size_t passes = 0;
  bool settled = false, live = true;
};

// One ICP step of `s`: matched, scored over every sample, the far pairs
// left out, the linearised step solved about the kept pairs' centroid.
void Step(const std::vector<SurfaceSample>& samples,
          const FacingSurface& target, const RegisterOptions& o,
          StartState& s) {
  std::vector<Match> matches;
  const double reach = s.reach > 0.0 ? s.reach : o.capMm;
  MatchAll(samples, s.pose, target, o.minFacingCos, reach, matches);
  s.score = CappedMean(matches, reach);
  s.passes++;
  std::vector<double> d;
  d.reserve(matches.size());
  for (const Match& m : matches)
    if (m.found)
      d.push_back(m.distance);
  if (d.size() < 6) {
    s.live = false; // nothing to step on: it stays where it is
    return;
  }
  const size_t mid = d.size() / 2;
  std::nth_element(d.begin(), d.begin() + mid, d.end());
  // Never under a fiftieth of the cap: near the answer the median is the
  // noise, and three times it trims away the few pairs on the walls that
  // hold the motions a broad face leaves free -- a plate 0.1 mm off along
  // its own plane stopped there, its edges left out of every step.
  const double keepBelow = std::max(3.0 * d[mid], o.capMm / 50.0);
  s.reach = std::min(o.capMm, std::max(6.0 * d[mid], o.capMm / 20.0));
  glm::dvec3 c(0.0);
  size_t kept = 0;
  for (const Match& m : matches)
    if (m.found && m.distance <= keepBelow) {
      c += m.at;
      kept++;
    }
  if (kept < 6) {
    s.live = false;
    return;
  }
  c /= double(kept);
  double A[6][6] = {}, b[6] = {};
  double extent = 0.0; // the kept pairs' farthest from their centroid
  const auto add = [&](const glm::dvec3& a, const glm::dvec3& n, double r,
                       double w) {
    const glm::dvec3 an = glm::cross(a, n);
    const double row[6] = {an.x, an.y, an.z, n.x, n.y, n.z};
    for (int i = 0; i < 6; i++) {
      b[i] += w * row[i] * r;
      for (int j = 0; j < 6; j++)
        A[i][j] += w * row[i] * row[j];
    }
  };
  for (const Match& m : matches) {
    if (!(m.found && m.distance <= keepBelow))
      continue;
    const glm::dvec3 a = m.at - c, e = m.onto - m.at;
    extent = std::max(extent, glm::length(a));
    add(a, m.normal, glm::dot(e, m.normal), 1.0);
    for (int k = 0; k < 3; k++) {
      glm::dvec3 axis(0.0);
      axis[k] = 1.0;
      add(a, axis, e[k], kPointWeight);
    }
  }
  double x[6];
  if (!Solve6(A, b, x)) {
    s.live = false;
    return;
  }
  const glm::dvec3 w(x[0], x[1], x[2]), tau(x[3], x[4], x[5]);
  const glm::dmat3 turn = Rodrigues(w);
  // y' = turn (y - c) + c + tau, composed onto the pose.
  const Rigid step{turn, c + tau - turn * c};
  s.pose = s.pose.Then(step);
  if (glm::length(w) * extent + glm::length(tau) < o.settleMm) {
    s.settled = true;
    s.live = false;
  }
}

} // namespace

double TurnAngle(const glm::dmat3& r) {
  const double c = 0.5 * (r[0][0] + r[1][1] + r[2][2] - 1.0);
  return std::acos(std::clamp(c, -1.0, 1.0));
}

std::vector<SurfaceSample>
SampleSurface(const TriMesh& m, size_t count, uint64_t seed,
              const std::function<bool(uint32_t)>& keep) {
  std::vector<double> cumulative;
  std::vector<uint32_t> which;
  cumulative.reserve(m.triangles.size());
  which.reserve(m.triangles.size());
  double total = 0.0;
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    if (keep && !keep(t))
      continue;
    const double a = m.Area(t);
    if (!(a > 0.0))
      continue;
    total += a;
    cumulative.push_back(total);
    which.push_back(t);
  }
  std::vector<SurfaceSample> out;
  if (!(total > 0.0) || count == 0)
    return out;
  out.reserve(count);
  // The normal at a point is its corners' vertex normals blended: a sliver's
  // own normal points anywhere, and the scan's surface does not.
  std::vector<glm::dvec3> vn(m.positions.size(), glm::dvec3(0.0));
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    const glm::dvec3 n = m.AreaNormal(t);
    for (int c = 0; c < 3; c++)
      vn[m.triangles[t][c]] += n;
  }
  uint64_t state = seed;
  for (size_t i = 0; i < count; i++) {
    const double r = Uniform(state) * total;
    const size_t k = std::min<size_t>(
        size_t(std::upper_bound(cumulative.begin(), cumulative.end(), r) -
               cumulative.begin()),
        which.size() - 1);
    const uint32_t t = which[k];
    double u = Uniform(state), v = Uniform(state);
    if (u + v > 1.0) {
      u = 1.0 - u;
      v = 1.0 - v;
    }
    const glm::dvec3 p0 = m.Corner(t, 0), p1 = m.Corner(t, 1),
                     p2 = m.Corner(t, 2);
    // A well-shaped triangle's own normal; a sliver's, its corners' vertex
    // normals blended. Blended across a sharp edge of a coarse CAD mesh the
    // normal leans towards the bigger face, past the angle a match allows.
    const auto& tri = m.triangles[t];
    const glm::dvec3 own = m.AreaNormal(t);
    const double edges2 = glm::dot(p1 - p0, p1 - p0) +
                          glm::dot(p2 - p1, p2 - p1) +
                          glm::dot(p0 - p2, p0 - p2);
    // 4√3 A / Σ e²: 1 equilateral, toward 0 a sliver; |own| is 2A.
    const double quality = 2.0 * std::sqrt(3.0) * glm::length(own) / edges2;
    glm::dvec3 n = own;
    if (quality < kSliver) {
      const glm::dvec3 blend =
          (1.0 - u - v) * vn[tri[0]] + u * vn[tri[1]] + v * vn[tri[2]];
      if (glm::length(blend) > 0.0 && glm::dot(blend, own) > 0.0)
        n = blend;
    }
    out.push_back({p0 + u * (p1 - p0) + v * (p2 - p0), glm::normalize(n)});
  }
  return out;
}

std::vector<Rigid> PrincipalStarts(const glm::dvec3& fromCentroid,
                                   const glm::dmat3& fromCovariance,
                                   const glm::dvec3& toCentroid,
                                   const glm::dmat3& toCovariance) {
  const glm::dmat3 F = PrincipalFrame(fromCovariance),
                   T = PrincipalFrame(toCovariance);
  std::vector<Rigid> out;
  static constexpr int kPerms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                                       {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  for (const auto& perm : kPerms)
    for (int signs = 0; signs < 8; signs++) {
      // P takes the from-frame's axis i onto the to-frame's axis perm[i].
      glm::dmat3 P(0.0);
      for (int i = 0; i < 3; i++)
        P[i][perm[i]] = (signs >> i) & 1 ? -1.0 : 1.0;
      if (glm::determinant(P) < 0.0)
        continue;
      Rigid r;
      r.rotation = T * P * glm::transpose(F);
      r.translation = toCentroid - r.rotation * fromCentroid;
      out.push_back(r);
    }
  return out;
}

std::vector<Rigid> SearchStarts(const std::vector<SurfaceSample>& from,
                                const TriMesh& to) {
  std::vector<Rigid> out;
  if (from.empty() || to.triangles.empty())
    return out;
  glm::dvec3 fc(0.0);
  for (const auto& p : from)
    fc += p.point;
  fc /= double(from.size());
  glm::dmat3 fcov(0.0);
  for (const auto& p : from)
    fcov += glm::outerProduct(p.point - fc, p.point - fc);
  fcov /= double(from.size());
  // The target's moments by area, as SurfaceMoments takes them.
  double area = 0.0;
  glm::dvec3 first(0.0);
  glm::dmat3 second(0.0);
  for (uint32_t t = 0; t < to.triangles.size(); t++) {
    const double a = to.Area(t);
    const glm::dvec3 p0 = to.Corner(t, 0), p1 = to.Corner(t, 1),
                     p2 = to.Corner(t, 2), s = p0 + p1 + p2;
    area += a;
    first += a * s / 3.0;
    second +=
        (a / 12.0) * (glm::outerProduct(p0, p0) + glm::outerProduct(p1, p1) +
                      glm::outerProduct(p2, p2) + glm::outerProduct(s, s));
  }
  if (!(area > 0.0))
    return out;
  const glm::dvec3 tc = first / area;
  const glm::dmat3 tcov = second / area - glm::outerProduct(tc, tc);
  const glm::dmat3 T = PrincipalFrame(tcov);
  const glm::dmat3 Tt = glm::transpose(T);
  // The target's box in its own frame.
  glm::dvec3 tlo(std::numeric_limits<double>::max()), thi(-tlo);
  for (const glm::dvec3& v : to.positions) {
    const glm::dvec3 q = Tt * v;
    tlo = glm::min(tlo, q);
    thi = glm::max(thi, q);
  }
  std::vector<glm::dmat3> turns{glm::dmat3(1.0)};
  for (int axis = 0; axis < 3; axis++) {
    glm::dvec3 w(0.0);
    w[axis] = angles::kPi / 4.0;
    turns.push_back(T * Rodrigues(w) * Tt); // about the target's axis
  }
  for (const Rigid& principal : PrincipalStarts(fc, fcov, tc, tcov))
    for (const glm::dmat3& turn : turns) {
      Rigid r;
      r.rotation = turn * principal.rotation;
      r.translation = tc - r.rotation * fc;
      out.push_back(r);
      glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-lo);
      for (const auto& p : from) {
        const glm::dvec3 q = Tt * (r.rotation * p.point);
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
      }
      for (int corner = 0; corner < 8; corner++) {
        glm::dvec3 shift;
        for (int i = 0; i < 3; i++)
          shift[i] = (corner >> i) & 1 ? thi[i] - hi[i] : tlo[i] - lo[i];
        out.push_back({r.rotation, T * shift});
      }
    }
  return out;
}

std::optional<Rigid> FromPairs(const std::array<glm::dvec3, 3>& from,
                               const std::array<glm::dvec3, 3>& to) {
  // Each three as a frame about their centroid: along the first side, the
  // triangle's normal, and the third square to both. Laid frame on frame.
  const auto frame = [](const std::array<glm::dvec3, 3>& p, glm::dmat3& f,
                        glm::dvec3& centre) {
    const glm::dvec3 u = p[1] - p[0], n = glm::cross(u, p[2] - p[0]);
    const double span = std::max(
        {glm::length(u), glm::length(p[2] - p[0]), glm::length(p[2] - p[1])});
    if (!(span > 0.0) || !(glm::length(n) > 1e-6 * span * span))
      return false;
    const glm::dvec3 e1 = glm::normalize(u), e3 = glm::normalize(n);
    f = glm::dmat3(e1, glm::cross(e3, e1), e3);
    centre = (p[0] + p[1] + p[2]) / 3.0;
    return true;
  };
  glm::dmat3 F, T;
  glm::dvec3 cf, ct;
  if (!frame(from, F, cf) || !frame(to, T, ct))
    return std::nullopt;
  Rigid r;
  r.rotation = T * glm::transpose(F);
  r.translation = ct - r.rotation * cf;
  return r;
}

RegisterResult Register(const std::vector<SurfaceSample>& samples,
                        const FacingSurface& target, const RegisterOptions& o) {
  RegisterResult result;
  std::vector<StartState> states(o.starts.size());
  for (size_t i = 0; i < o.starts.size(); i++)
    states[i].pose = o.starts[i];
  std::vector<size_t> live;
  for (size_t pass = 0; pass < o.maxPasses; pass++) {
    if (o.cancel != nullptr && o.cancel->load()) {
      result.cancelled = true;
      return result;
    }
    live.clear();
    for (size_t i = 0; i < states.size(); i++)
      if (states[i].live)
        live.push_back(i);
    if (live.empty())
      break;
    parallel::For(live.size(),
                  [&](size_t k) { Step(samples, target, o, states[live[k]]); });
    if (o.progress)
      o.progress(double(pass + 1) / double(o.maxPasses + 1));
  }
  if (o.cancel != nullptr && o.cancel->load()) {
    result.cancelled = true;
    return result;
  }
  // Every start scored where it ended, on every sample.
  result.starts.resize(states.size());
  parallel::For(states.size(), [&](size_t i) {
    std::vector<Match> matches;
    MatchAll(samples, states[i].pose, target, o.minFacingCos, o.capMm, matches);
    result.starts[i] = {states[i].pose, CappedMean(matches, o.capMm),
                        states[i].passes, states[i].settled};
  });
  for (size_t i = 1; i < result.starts.size(); i++)
    if (result.starts[i].scoreMm < result.starts[result.winner].scoreMm)
      result.winner = i;
  if (o.progress)
    o.progress(1.0);
  return result;
}

SearchResult Search(const std::vector<SurfaceSample>& samples,
                    const std::vector<SurfaceSample>& fine,
                    const FacingSurface& target, const SearchOptions& so,
                    const FacingSurface* rough) {
  SearchResult out;
  const FacingSurface& early = rough != nullptr ? *rough : target;

  const auto report = [&](double lo, double hi) {
    return so.progress ? std::function<void(double)>([&so, lo, hi](double f) {
      so.progress(lo + (hi - lo) * f);
    })
                       : std::function<void(double)>();
  };
  RegisterOptions o;
  o.capMm = so.capMm;
  o.minFacingCos = so.minFacingCos;
  o.settleMm = so.settleMm;
  o.cancel = so.cancel;
  // Screened: a few passes of every start on a few points.
  std::vector<size_t> order(so.starts.size());
  for (size_t i = 0; i < order.size(); i++)
    order[i] = i;
  std::vector<Rigid> kept = so.starts;
  if (so.starts.size() > so.keep) {
    const std::vector<SurfaceSample> few(
        samples.begin(),
        samples.begin() + std::min(so.screenSamples, samples.size()));
    o.starts = so.starts;
    o.maxPasses = so.screenPasses;
    // Matched near: a start far off is ranked low either way, and a
    // search that gives up at a quarter of the cap is several times faster.
    o.capMm = so.capMm / 4.0;
    o.progress = report(0.0, 0.3);
    const RegisterResult screen = Register(few, early, o);
    if (screen.cancelled) {
      out.cancelled = true;
      return out;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return screen.starts[a].scoreMm < screen.starts[b].scoreMm;
    });
    // The best `keep` that stand apart: two screened starts gone the same
    // way are one start, and a second attitude kept is what says how close
    // another comes.
    glm::dvec3 centre(0.0);
    for (const SurfaceSample& p : few)
      centre += p.point;
    centre /= double(std::max<size_t>(1, few.size()));
    std::vector<size_t> chosen;
    for (size_t i : order) {
      if (chosen.size() == so.keep)
        break;
      const Rigid& a = screen.starts[i].pose;
      bool apart = true;
      for (size_t j : chosen) {
        const Rigid& b = screen.starts[j].pose;
        if (TurnAngle(glm::transpose(a.rotation) * b.rotation) <
                angles::ToRadians(10.0) &&
            glm::length(a(centre) - b(centre)) < so.distinctMm) {
          apart = false;
          break;
        }
      }
      if (apart)
        chosen.push_back(i);
    }
    order = chosen;
    kept.clear();
    for (size_t i : order)
      kept.push_back(screen.starts[i].pose);
  }
  // Run until they settle.
  o.capMm = so.capMm;
  o.starts = kept;
  o.maxPasses = so.passes;
  o.progress = report(0.3, 0.85);
  const RegisterResult run = Register(samples, early, o);
  if (run.cancelled) {
    out.cancelled = true;
    return out;
  }
  std::vector<size_t> rank(run.starts.size());
  for (size_t i = 0; i < rank.size(); i++)
    rank[i] = i;
  std::stable_sort(rank.begin(), rank.end(), [&](size_t a, size_t b) {
    return run.starts[a].scoreMm < run.starts[b].scoreMm;
  });
  for (size_t i : rank) {
    out.kept.push_back(run.starts[i]);
    out.keptFrom.push_back(order[i]);
  }
  // The winner on every point.
  o.starts = {out.kept.front().pose};
  o.maxPasses = so.finePasses;
  o.progress = report(0.85, 1.0);
  const RegisterResult refined = Register(fine, target, o);
  if (refined.cancelled) {
    out.cancelled = true;
    return out;
  }
  out.pose = refined.Best().pose;
  return out;
}

TriMesh Surfels(const TriMesh& m, size_t count, uint64_t seed) {
  TriMesh out;
  const std::vector<SurfaceSample> s = SampleSurface(m, count, seed);
  if (s.empty())
    return out;
  const double r = 0.75 * std::sqrt(m.SurfaceArea() / double(s.size()));
  out.positions.reserve(3 * s.size());
  out.triangles.reserve(s.size());
  for (const SurfaceSample& p : s) {
    const glm::dvec3 a =
        std::abs(p.normal.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    const glm::dvec3 u = glm::normalize(glm::cross(p.normal, a)),
                     v = glm::cross(p.normal, u);
    const uint32_t base = static_cast<uint32_t>(out.positions.size());
    for (int k = 0; k < 3; k++) {
      const double t = angles::kTwoPi * k / 3.0;
      out.positions.push_back(p.point +
                              r * (std::cos(t) * u + std::sin(t) * v));
    }
    out.triangles.push_back({base, base + 1, base + 2}); // u x v = normal
  }
  out.quantum = m.quantum;
  return out;
}

Distances MeasureDistances(const std::vector<SurfaceSample>& samples,
                           const Rigid& pose, const FacingSurface& target,
                           double minFacingCos, double capMm, double withinMm) {
  Distances out;
  if (samples.empty())
    return out;
  // In bands, on every core: a final measure is the densest sample.
  const size_t bands =
      std::max<size_t>(1, std::min<size_t>(64, samples.size() / 256));
  std::vector<std::vector<Match>> parts(bands);
  parallel::For(bands, [&](size_t b) {
    const size_t lo = samples.size() * b / bands,
                 hi = samples.size() * (b + 1) / bands;
    const std::vector<SurfaceSample> slice(samples.begin() + lo,
                                           samples.begin() + hi);
    MatchAll(slice, pose, target, minFacingCos, capMm, parts[b]);
  });
  std::vector<double> d;
  d.reserve(samples.size());
  size_t within = 0, unmatched = 0;
  for (const auto& part : parts)
    for (const Match& m : part) {
      const double v = std::min(m.distance, capMm);
      d.push_back(v);
      out.meanMm += v;
      within += m.found && m.distance <= withinMm;
      unmatched += !m.found;
    }
  out.meanMm = d.size() > unmatched ? (out.meanMm - capMm * double(unmatched)) /
                                          double(d.size() - unmatched)
                                    : capMm;
  out.maxMm = *std::max_element(d.begin(), d.end());
  const size_t k95 = std::min(d.size() - 1, size_t(0.95 * double(d.size())));
  std::nth_element(d.begin(), d.begin() + k95, d.end());
  out.p95Mm = d[k95];
  out.within = double(within) / double(d.size());
  out.unmatched = double(unmatched) / double(d.size());
  return out;
}

} // namespace mesh
