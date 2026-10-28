// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/Segment.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mesh {

size_t Segmentation::Count(Kind k) const {
  return static_cast<size_t>(
      std::count_if(regions.begin(), regions.end(),
                    [&](const Region& r) { return r.fit.kind == k; }));
}

namespace {

constexpr double kTwoPi = 6.283185307179586;
// Patches turning by less than this are smooth neighbours: the most a
// polygon of the fewest sides `min_sides` admits turns (a pentagon's 72°),
// so every polygon that could be asked for, and every one worth listing,
// is grown and judged.
const double kSmoothCos = std::cos(kTwoPi / kMinSidesLowest * (1.0 + 1e-6));
// How far past 360/min_sides a facet may turn and still count: float noise
// on a facet's angular width, not a tolerance on the shape.
constexpr double kTurnSlack = 1e-3;
// Triples tried from one seed before it is given up: a large plane with a
// hundred tangent fillets would otherwise try every pair of them.
constexpr int kMaxTriples = 256;

struct Patch {
  std::vector<uint32_t> tris;
  std::vector<uint32_t> verts; // unique, ascending
  glm::dvec3 normal{0.0};      // unit, the triangles' side
  glm::dvec3 point{0.0};
  double area = 0.0;
  double residual = 0.0; // the most a vertex stands off its plane
  uint32_t first = kNone;
  std::vector<uint32_t> neighbours; // ascending
};

class Segmenter {
public:
  Segmenter(const TriMesh& m, const SegmentOptions& o) : m_mesh(m), m_opt(o) {}

  Segmentation Run();

private:
  void Tidy(Segmentation& out) const;
  // Makes a cylinder exactly tangent to the planes it meets tangentially.
  void Tangents(Segmentation& out) const;
  void BuildPatches();
  bool Smooth(uint32_t a, uint32_t b) const {
    return glm::dot(m_patches[a].normal, m_patches[b].normal) > kSmoothCos;
  }
  // The surface through `set` (patch ids) within the tolerance, trying
  // `prefer`'s kind first when it is set. Not ok when none fits.
  FitResult FitSet(const std::vector<uint32_t>& set,
                   const FitResult* prefer) const;
  bool OnSurface(const FitResult& f, uint32_t patch) const;
  void Grow(std::vector<uint32_t>& set, FitResult& fit);
  // The most a facet of `set` turns about `fit`'s axis or centre; `total`,
  // when given, the sum of every facet's turn (a full ring's is a turn).
  double FacetTurn(const std::vector<uint32_t>& set, const FitResult& fit,
                   double* total = nullptr) const;

  const TriMesh& m_mesh;
  const SegmentOptions& m_opt;
  double m_tol = 0.0;
  std::vector<uint32_t> m_patchOf;
  std::vector<Patch> m_patches;
  std::vector<uint32_t> m_curvedOf; // per patch: a curved region, or kNone
  mutable std::vector<uint32_t> m_stamp;
  mutable uint32_t m_stampNow = 0;
};

void Segmenter::BuildPatches() {
  const size_t n = m_mesh.triangles.size();
  m_patchOf.assign(n, kNone);
  std::vector<double> area(n);
  std::vector<glm::dvec3> unit(n);
  std::vector<bool> degenerate(n);
  for (uint32_t t = 0; t < n; t++) {
    const glm::dvec3 an = m_mesh.AreaNormal(t);
    const double len = glm::length(an);
    area[t] = 0.5 * len;
    unit[t] = len > 0.0 ? an / len : glm::dvec3(0.0);
    // A sliver lower than the tolerance has no plane of its own.
    double longest = 0.0;
    for (int e = 0; e < 3; e++)
      longest = std::max(longest, glm::length(m_mesh.Corner(t, (e + 1) % 3) -
                                              m_mesh.Corner(t, e)));
    degenerate[t] = longest == 0.0 || len / longest <= m_tol;
  }
  std::vector<uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(),
                   [&](uint32_t a, uint32_t b) { return area[a] > area[b]; });
  const auto within = [&](uint32_t t, const glm::dvec3& normal, double d) {
    for (int c = 0; c < 3; c++)
      if (std::abs(glm::dot(normal, m_mesh.Corner(t, c)) - d) > m_tol)
        return false;
    return true;
  };
  std::vector<uint32_t> stack;
  for (uint32_t seed : order) {
    if (m_patchOf[seed] != kNone || degenerate[seed])
      continue;
    const auto id = static_cast<uint32_t>(m_patches.size());
    m_patches.emplace_back();
    const glm::dvec3 normal = unit[seed];
    const double d = glm::dot(normal, m_mesh.Corner(seed, 0));
    m_patchOf[seed] = id;
    stack.push_back(seed);
    while (!stack.empty()) {
      const uint32_t t = stack.back();
      stack.pop_back();
      m_patches[id].tris.push_back(t);
      for (int e = 0; e < 3; e++) {
        const uint32_t u = m_mesh.neighbour[t][e];
        if (u == kNone || m_patchOf[u] != kNone || !within(u, normal, d))
          continue;
        m_patchOf[u] = id;
        stack.push_back(u);
      }
    }
    m_patches[id].normal = normal;
  }
  // Slivers the flood did not reach join a neighbour whose plane holds
  // them, or stand alone.
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t t = 0; t < n; t++) {
      if (m_patchOf[t] != kNone)
        continue;
      for (int e = 0; e < 3 && m_patchOf[t] == kNone; e++) {
        const uint32_t u = m_mesh.neighbour[t][e];
        if (u == kNone || m_patchOf[u] == kNone)
          continue;
        const Patch& p = m_patches[m_patchOf[u]];
        const double d = glm::dot(p.normal, m_mesh.Corner(u, 0));
        if (within(t, p.normal, d)) {
          m_patchOf[t] = m_patchOf[u];
          m_patches[m_patchOf[u]].tris.push_back(t);
          changed = true;
        }
      }
    }
  }
  for (uint32_t t = 0; t < n; t++) {
    if (m_patchOf[t] != kNone)
      continue;
    m_patchOf[t] = static_cast<uint32_t>(m_patches.size());
    m_patches.emplace_back();
    m_patches.back().tris.push_back(t);
    m_patches.back().normal = unit[t];
  }
  // Each patch's own plane: the least-squares fit of its corners, facing
  // the triangles' way.
  for (uint32_t id = 0; id < m_patches.size(); id++) {
    Patch& p = m_patches[id];
    std::sort(p.tris.begin(), p.tris.end());
    p.first = p.tris.front();
    for (uint32_t t : p.tris) {
      p.area += area[t];
      for (int c = 0; c < 3; c++)
        p.verts.push_back(m_mesh.triangles[t][c]);
    }
    std::sort(p.verts.begin(), p.verts.end());
    p.verts.erase(std::unique(p.verts.begin(), p.verts.end()), p.verts.end());
    std::vector<glm::dvec3> pts;
    pts.reserve(p.verts.size());
    for (uint32_t v : p.verts)
      pts.push_back(m_mesh.positions[v]);
    const FitResult plane = FitPlane(pts);
    if (plane.ok) {
      p.normal =
          glm::dot(plane.axis, p.normal) < 0.0 ? -plane.axis : plane.axis;
      p.point = plane.origin;
      p.residual = plane.max;
    } else {
      p.point = pts.front();
    }
  }
  for (uint32_t t = 0; t < n; t++)
    for (int e = 0; e < 3; e++) {
      const uint32_t u = m_mesh.neighbour[t][e];
      if (u != kNone && m_patchOf[u] != m_patchOf[t])
        m_patches[m_patchOf[t]].neighbours.push_back(m_patchOf[u]);
    }
  for (Patch& p : m_patches) {
    std::sort(p.neighbours.begin(), p.neighbours.end());
    p.neighbours.erase(std::unique(p.neighbours.begin(), p.neighbours.end()),
                       p.neighbours.end());
  }
}

bool Segmenter::OnSurface(const FitResult& f, uint32_t patch) const {
  for (uint32_t v : m_patches[patch].verts)
    if (std::abs(f.Distance(m_mesh.positions[v])) > m_tol)
      return false;
  return true;
}

FitResult Segmenter::FitSet(const std::vector<uint32_t>& set,
                            const FitResult* prefer) const {
  if (m_stamp.size() < m_mesh.positions.size())
    m_stamp.assign(m_mesh.positions.size(), 0);
  if (++m_stampNow == 0) {
    std::fill(m_stamp.begin(), m_stamp.end(), 0);
    m_stampNow = 1;
  }
  std::vector<glm::dvec3> pts, normals, onPlane;
  std::vector<double> weights;
  for (uint32_t id : set) {
    const Patch& p = m_patches[id];
    normals.push_back(p.normal);
    weights.push_back(p.area);
    onPlane.push_back(p.point);
    for (uint32_t v : p.verts)
      if (m_stamp[v] != m_stampNow) {
        m_stamp[v] = m_stampNow;
        pts.push_back(m_mesh.positions[v]);
      }
  }
  // A wrong hypothesis misses by the size of a face; a right one, before
  // refining, by float noise along an axis read from a few normals.
  const double giveUp = 1000.0 * m_tol;
  const auto accept = [&](const FitResult& f) {
    return f.ok && f.max <= m_tol;
  };
  const auto tryKind = [&](Kind k) -> FitResult {
    double offset = 0.0, spread = 0.0;
    switch (k) {
    case Kind::Cylinder: {
      const glm::dvec3 guess =
          prefer && prefer->kind == Kind::Cylinder
              ? prefer->axis
              : AxisFromNormals(normals, weights, true, offset, spread);
      return FitCylinder(pts, guess, giveUp);
    }
    case Kind::Cone: {
      const glm::dvec3 guess =
          prefer && prefer->kind == Kind::Cone
              ? prefer->axis
              : AxisFromNormals(normals, weights, false, offset, spread);
      return FitCone(pts, normals, onPlane, guess, giveUp);
    }
    case Kind::Sphere:
      return FitSphere(pts, giveUp);
    case Kind::Plane:
      break;
    }
    return {};
  };
  if (prefer != nullptr) {
    FitResult f = tryKind(prefer->kind);
    if (accept(f))
      return f;
  }
  for (Kind k : {Kind::Cylinder, Kind::Cone, Kind::Sphere}) {
    if (prefer != nullptr && k == prefer->kind)
      continue;
    FitResult f = tryKind(k);
    if (accept(f))
      return f;
  }
  return {};
}

void Segmenter::Grow(std::vector<uint32_t>& set, FitResult& fit) {
  std::vector<uint32_t> queue;
  std::vector<bool> seen(m_patches.size(), false);
  for (uint32_t id : set)
    seen[id] = true;
  const auto enqueue = [&](uint32_t id) {
    for (uint32_t q : m_patches[id].neighbours)
      if (!seen[q] && m_curvedOf[q] == kNone && Smooth(id, q)) {
        seen[q] = true;
        queue.push_back(q);
      }
  };
  for (uint32_t id : set)
    enqueue(id);
  for (size_t i = 0; i < queue.size(); i++) {
    const uint32_t q = queue[i];
    if (OnSurface(fit, q)) {
      set.push_back(q);
      enqueue(q);
      continue;
    }
    // The fit from fewer facets may be the one that is off: refit with this
    // one in and keep it only if every vertex still fits.
    set.push_back(q);
    const FitResult refit = FitSet(set, &fit);
    if (refit.ok) {
      fit = refit;
      enqueue(q);
    } else {
      set.pop_back();
    }
  }
}

double Segmenter::FacetTurn(const std::vector<uint32_t>& set,
                            const FitResult& fit, double* total) const {
  double most = 0.0;
  if (total != nullptr)
    *total = 0.0;
  for (uint32_t id : set) {
    const Patch& p = m_patches[id];
    if (fit.kind == Kind::Sphere) {
      for (size_t i = 0; i < p.verts.size(); i++)
        for (size_t j = i + 1; j < p.verts.size(); j++) {
          const glm::dvec3 a =
              glm::normalize(m_mesh.positions[p.verts[i]] - fit.origin);
          const glm::dvec3 b =
              glm::normalize(m_mesh.positions[p.verts[j]] - fit.origin);
          most =
              std::max(most, std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0)));
        }
      continue;
    }
    // About the axis: each corner's angle measured from the patch's own
    // middle, so a patch never straddles the wrap.
    const glm::dvec3 ax = fit.axis;
    const auto radial = [&](const glm::dvec3& x) {
      const glm::dvec3 w = x - fit.origin;
      return w - glm::dot(w, ax) * ax;
    };
    glm::dvec3 mid(0.0);
    for (uint32_t v : p.verts)
      mid += m_mesh.positions[v];
    mid /= double(p.verts.size());
    const glm::dvec3 r0 = radial(mid);
    if (glm::length(r0) == 0.0)
      continue;
    const glm::dvec3 u = glm::normalize(r0), w = glm::cross(ax, u);
    double lo = 0.0, hi = 0.0;
    for (uint32_t v : p.verts) {
      const glm::dvec3 r = radial(m_mesh.positions[v]);
      if (glm::length(r) == 0.0)
        continue; // a cone's apex
      const double ang = std::atan2(glm::dot(r, w), glm::dot(r, u));
      lo = std::min(lo, ang);
      hi = std::max(hi, ang);
    }
    most = std::max(most, hi - lo);
    if (total != nullptr)
      *total += hi - lo;
  }
  return most;
}

Segmentation Segmenter::Run() {
  Segmentation out;
  glm::dvec3 lo, hi;
  m_mesh.Bounds(lo, hi);
  const double diagonal = glm::length(hi - lo);
  m_tol = std::max(m_opt.quanta * m_mesh.quantum, 1e-12 * diagonal);
  out.tolerance = m_tol;
  BuildPatches();
  m_curvedOf.assign(m_patches.size(), kNone);
  const double turnLimit =
      kTwoPi / std::clamp(m_opt.minSides, kMinSidesLowest, kMinSidesHighest) *
      (1.0 + kTurnSlack);

  std::vector<uint32_t> seeds(m_patches.size());
  std::iota(seeds.begin(), seeds.end(), 0u);
  std::sort(seeds.begin(), seeds.end(), [&](uint32_t a, uint32_t b) {
    return m_patches[a].first < m_patches[b].first;
  });
  std::vector<bool> tried(m_patches.size(), false), inPolygon(m_patches.size());
  std::vector<std::vector<uint32_t>> curvedSets;
  std::vector<FitResult> curvedFits;
  std::vector<double> curvedTurns;
  for (uint32_t p : seeds) {
    if (m_opt.cancelled && m_opt.cancelled()) {
      out.cancelled = true;
      return out;
    }
    if (m_curvedOf[p] != kNone || tried[p])
      continue;
    tried[p] = true;
    std::vector<uint32_t> smooth;
    for (uint32_t q : m_patches[p].neighbours)
      if (m_curvedOf[q] == kNone && Smooth(p, q))
        smooth.push_back(q);
    if (smooth.empty())
      continue;
    std::vector<uint32_t> set;
    FitResult fit;
    int triples = 0;
    for (size_t i = 0; i < smooth.size() && !fit.ok && triples < kMaxTriples;
         i++) {
      const uint32_t q = smooth[i];
      std::vector<uint32_t> third = smooth;
      for (uint32_t r : m_patches[q].neighbours)
        if (m_curvedOf[r] == kNone && Smooth(q, r))
          third.push_back(r);
      std::sort(third.begin(), third.end());
      third.erase(std::unique(third.begin(), third.end()), third.end());
      for (uint32_t r : third) {
        if (r == p || r == q || triples >= kMaxTriples)
          continue;
        triples++;
        const std::vector<uint32_t> triple = {p, q, r};
        const FitResult f = FitSet(triple, nullptr);
        if (f.ok) {
          set = triple;
          fit = f;
          break;
        }
      }
    }
    if (!fit.ok)
      continue;
    Grow(set, fit);
    double around = 0.0;
    const double turn = FacetTurn(set, fit, &around);
    if (turn > turnLimit) {
      // Round enough to fit, too coarse to be round: a polygon, kept flat.
      // Listed when it is a ring of five sides or more -- a nut trap, a
      // pentagon hole -- going at least half way round (a flat the ring
      // shares with a neighbour may be missing), not two flats that happen
      // to fit an arc.
      const int sides = turn > 0.0 ? int(std::lround(kTwoPi / turn)) : 0;
      if (fit.kind != Kind::Sphere && sides >= kMinSidesLowest &&
          around >= kTwoPi / 2) {
        Polygon poly;
        poly.sides = sides;
        poly.axis = fit.axis;
        double along = 0.0, radius = 0.0;
        size_t count = 0;
        for (uint32_t id : set)
          for (uint32_t v : m_patches[id].verts) {
            const glm::dvec3 w = m_mesh.positions[v] - fit.origin;
            const double h = glm::dot(w, fit.axis);
            along += h;
            radius += glm::length(w - h * fit.axis);
            count++;
          }
        poly.centre = fit.origin + fit.axis * (along / double(count));
        poly.circumradius = radius / double(count);
        out.polygonal.push_back(poly);
      }
      for (uint32_t id : set) {
        tried[id] = true;
        inPolygon[id] = true;
      }
      continue;
    }
    // A region one flat of which is most of it: a wall and two narrow
    // facets beside it fit a cylinder of a radius ninety times the part's,
    // and that is not a surface anyone drew -- a tessellated one is facets
    // of a size.
    double whole = 0.0, largest = 0.0;
    for (uint32_t id : set) {
      whole += m_patches[id].area;
      largest = std::max(largest, m_patches[id].area);
    }
    const bool dominated = largest > 0.5 * whole;
    if (dominated) {
      for (uint32_t id : set)
        tried[id] = true;
      continue;
    }
    const auto region = static_cast<uint32_t>(curvedSets.size());
    for (uint32_t id : set)
      m_curvedOf[id] = region;
    curvedSets.push_back(set);
    curvedFits.push_back(fit);
    curvedTurns.push_back(turn);
  }

  // Regions: each curved set, then each patch left a plane.
  const double facetLimitCos = std::cos(turnLimit);
  for (size_t c = 0; c < curvedSets.size(); c++) {
    Region r;
    r.fit = curvedFits[c];
    r.facetTurn = curvedTurns[c];
    r.sides = r.facetTurn > 0.0 ? int(std::lround(kTwoPi / r.facetTurn)) : 0;
    double reach = r.fit.radius;
    if (r.fit.kind == Kind::Cone) {
      reach = 0.0;
      for (uint32_t id : curvedSets[c])
        for (uint32_t v : m_patches[id].verts) {
          const glm::dvec3 w = m_mesh.positions[v] - r.fit.origin;
          reach = std::max(
              reach, glm::length(w - glm::dot(w, r.fit.axis) * r.fit.axis));
        }
    }
    r.sagitta = reach * (1.0 - std::cos(r.facetTurn / 2.0));
    for (uint32_t id : curvedSets[c]) {
      r.area += m_patches[id].area;
      r.triangles.insert(r.triangles.end(), m_patches[id].tris.begin(),
                         m_patches[id].tris.end());
    }
    out.regions.push_back(std::move(r));
  }
  for (uint32_t id = 0; id < m_patches.size(); id++) {
    if (m_curvedOf[id] != kNone)
      continue;
    const Patch& p = m_patches[id];
    Region r;
    r.fit.ok = true;
    r.fit.kind = Kind::Plane;
    r.fit.origin = p.point;
    r.fit.axis = p.normal;
    r.fit.max = p.residual;
    r.area = p.area;
    r.triangles = p.tris;
    out.regions.push_back(std::move(r));
    if (inPolygon[id])
      continue;
    for (uint32_t q : p.neighbours)
      if (m_curvedOf[q] == kNone &&
          glm::dot(p.normal, m_patches[q].normal) > facetLimitCos) {
        out.facetsLeft++;
        break;
      }
  }
  out.regionOf.assign(m_mesh.triangles.size(), kNone);
  for (uint32_t r = 0; r < out.regions.size(); r++)
    for (uint32_t t : out.regions[r].triangles)
      out.regionOf[t] = r;
  Tidy(out);
  Tangents(out);
  return out;
}

void Segmenter::Tangents(Segmentation& out) const {
  // A fillet or a rounded corner meets its flats tangentially. Each fit is
  // within the file's precision on its own, but two fits are not tangent to
  // each other: the cylinder sits a hair inside or outside the plane, and an
  // edit that extends both (Offset Face) finds them crossing twice, or not
  // meeting, where the part had them touch. The planes are fitted to far
  // more of the part, so the cylinder moves: its axis made square to the
  // planes' normals and placed exactly its radius from each.
  std::vector<std::vector<uint32_t>> touching(out.regions.size());
  for (uint32_t t = 0; t < m_mesh.triangles.size(); t++)
    for (int e = 0; e < 3; e++) {
      const uint32_t u = m_mesh.neighbour[t][e];
      if (u == kNone)
        continue;
      const uint32_t a = out.regionOf[t], b = out.regionOf[u];
      if (a != b)
        touching[a].push_back(b);
    }
  for (uint32_t c = 0; c < out.regions.size(); c++) {
    FitResult& f = out.regions[c].fit;
    if (f.kind != Kind::Cylinder)
      continue;
    auto& adjacent = touching[c];
    std::sort(adjacent.begin(), adjacent.end());
    adjacent.erase(std::unique(adjacent.begin(), adjacent.end()),
                   adjacent.end());
    struct Flat {
      glm::dvec3 n;
      double offset; // where the axis must be: n . o = offset
    };
    std::vector<Flat> flats;
    for (uint32_t p : adjacent) {
      const FitResult& pl = out.regions[p].fit;
      if (pl.kind != Kind::Plane)
        continue;
      const glm::dvec3 n = glm::normalize(pl.axis);
      // Tangent: the plane runs along the axis, a radius from it.
      if (std::abs(glm::dot(n, f.axis)) > m_tol / std::max(f.radius, m_tol))
        continue;
      const double d = glm::dot(f.origin - pl.origin, n); // axis to plane
      if (std::abs(std::abs(d) - f.radius) > m_opt.quanta * m_tol)
        continue;
      flats.push_back(
          {n, glm::dot(pl.origin, n) + (d > 0 ? 1.0 : -1.0) * f.radius});
    }
    if (flats.empty())
      continue;
    // The axis square to every normal: the plane through them, or the
    // cross of two that are not parallel.
    glm::dvec3 axis = f.axis;
    for (const Flat& fl : flats)
      axis = glm::normalize(axis - glm::dot(axis, fl.n) * fl.n);
    // Its point: least squares over the conditions n . o = offset, in the
    // plane square to the axis through the fitted point.
    glm::dvec3 e1 = glm::normalize(glm::cross(axis, std::abs(axis.x) < 0.6
                                                        ? glm::dvec3(1, 0, 0)
                                                        : glm::dvec3(0, 1, 0)));
    glm::dvec3 e2 = glm::cross(axis, e1);
    double a11 = 0, a12 = 0, a22 = 0, b1 = 0, b2 = 0;
    for (const Flat& fl : flats) {
      const double p1 = glm::dot(fl.n, e1), p2 = glm::dot(fl.n, e2);
      const double r = fl.offset - glm::dot(fl.n, f.origin);
      a11 += p1 * p1, a12 += p1 * p2, a22 += p2 * p2;
      b1 += p1 * r, b2 += p2 * r;
    }
    double s1 = 0.0, s2 = 0.0;
    const double det = a11 * a22 - a12 * a12;
    if (std::abs(det) > 1e-12) {
      s1 = (b1 * a22 - b2 * a12) / det;
      s2 = (a11 * b2 - a12 * b1) / det;
    } else { // one direction only (parallel planes): along it
      const double len = a11 + a22;
      if (len > 0.0) {
        s1 = b1 / len;
        s2 = b2 / len;
      }
    }
    FitResult moved = f;
    moved.axis = glm::dot(axis, f.axis) < 0.0 ? -axis : axis;
    moved.origin = f.origin + s1 * e1 + s2 * e2;
    // Kept only if the region's own vertices still sit on it.
    bool holds = true;
    for (uint32_t t : out.regions[c].triangles)
      for (int k = 0; k < 3 && holds; k++)
        holds = std::abs(moved.Distance(m_mesh.Corner(t, k))) <=
                m_opt.quanta * m_tol;
    if (holds)
      f = moved;
  }
}

void Segmenter::Tidy(Segmentation& out) const {
  // Where two surfaces meet, a small triangle can be on both to the file's
  // precision and land in the wrong one: an island of one region inside
  // another, or a notch in their boundary. A triangle with two or three
  // neighbours in another region whose surface holds all its corners joins
  // that region.
  for (int pass = 0; pass < 8; pass++) {
    bool changed = false;
    for (uint32_t t = 0; t < m_mesh.triangles.size(); t++) {
      const uint32_t own = out.regionOf[t];
      uint32_t across[3];
      for (int e = 0; e < 3; e++) {
        const uint32_t u = m_mesh.neighbour[t][e];
        across[e] = u == kNone ? kNone : out.regionOf[u];
      }
      for (int e = 0; e < 3; e++) {
        const uint32_t other = across[e];
        if (other == kNone || other == own)
          continue;
        int shared = 0;
        for (int k = 0; k < 3; k++)
          shared += across[k] == other;
        if (shared < 2)
          continue;
        const FitResult& f = out.regions[other].fit;
        bool on = true;
        for (int c = 0; c < 3 && on; c++)
          on = std::abs(f.Distance(m_mesh.Corner(t, c))) <= m_tol;
        // Its middle too, no further off than that region's own facets
        // stand: a disc's fan has every corner on the rim's cylinder.
        const glm::dvec3 mid =
            (m_mesh.Corner(t, 0) + m_mesh.Corner(t, 1) + m_mesh.Corner(t, 2)) /
            3.0;
        on = on &&
             std::abs(f.Distance(mid)) <= out.regions[other].sagitta + m_tol;
        if (!on)
          continue;
        out.regionOf[t] = other;
        changed = true;
        break;
      }
    }
    if (!changed)
      break;
  }
  std::vector<std::vector<uint32_t>> tris(out.regions.size());
  for (uint32_t t = 0; t < m_mesh.triangles.size(); t++)
    tris[out.regionOf[t]].push_back(t);
  std::vector<Region> kept;
  for (uint32_t r = 0; r < out.regions.size(); r++) {
    if (tris[r].empty())
      continue;
    Region region = std::move(out.regions[r]);
    region.triangles = std::move(tris[r]);
    region.area = 0.0;
    for (uint32_t t : region.triangles)
      region.area += m_mesh.Area(t);
    kept.push_back(std::move(region));
  }
  out.regions = std::move(kept);
  for (Region& r : out.regions)
    r.firstTriangle = r.triangles.front();
  std::sort(out.regions.begin(), out.regions.end(),
            [](const Region& a, const Region& b) {
              return a.firstTriangle < b.firstTriangle;
            });
  out.regionOf.assign(m_mesh.triangles.size(), kNone);
  for (uint32_t r = 0; r < out.regions.size(); r++)
    for (uint32_t t : out.regions[r].triangles)
      out.regionOf[t] = r;
}

} // namespace

namespace {

void Renumber(Segmentation& s, size_t triangles) {
  for (Region& r : s.regions) {
    std::sort(r.triangles.begin(), r.triangles.end());
    r.firstTriangle = r.triangles.front();
  }
  std::sort(s.regions.begin(), s.regions.end(),
            [](const Region& a, const Region& b) {
              return a.firstTriangle < b.firstTriangle;
            });
  s.regionOf.assign(triangles, kNone);
  for (uint32_t r = 0; r < s.regions.size(); r++)
    for (uint32_t t : s.regions[r].triangles)
      s.regionOf[t] = r;
}

std::string Plural(size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

} // namespace

void Demote(Segmentation& s, const TriMesh& mesh, uint32_t region) {
  Demote(s, mesh, std::vector<uint32_t>{region});
}

void Demote(Segmentation& s, const TriMesh& mesh,
            const std::vector<uint32_t>& regions) {
  std::vector<uint32_t> tris;
  std::vector<bool> drop(s.regions.size(), false);
  for (uint32_t r : regions)
    if (r < s.regions.size() && s.regions[r].fit.kind != Kind::Plane &&
        !drop[r]) {
      drop[r] = true;
      tris.insert(tris.end(), s.regions[r].triangles.begin(),
                  s.regions[r].triangles.end());
    }
  if (tris.empty())
    return;
  std::vector<Region> kept;
  for (uint32_t r = 0; r < s.regions.size(); r++)
    if (!drop[r])
      kept.push_back(std::move(s.regions[r]));
  s.regions = std::move(kept);
  // Within them, coplanar triangles flood together as BuildPatches floods
  // the whole mesh.
  std::vector<uint32_t> order = tris;
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return mesh.Area(a) > mesh.Area(b);
  });
  std::vector<bool> inRegion(mesh.triangles.size(), false),
      done(mesh.triangles.size(), false);
  for (uint32_t t : tris)
    inRegion[t] = true;
  std::vector<uint32_t> stack;
  for (uint32_t seed : order) {
    if (done[seed])
      continue;
    const glm::dvec3 an = mesh.AreaNormal(seed);
    const double len = glm::length(an);
    const glm::dvec3 n = len > 0.0 ? an / len : glm::dvec3(0, 0, 1);
    const double d = glm::dot(n, mesh.Corner(seed, 0));
    Region r;
    r.fit.ok = true;
    r.fit.kind = Kind::Plane;
    r.fit.axis = n;
    r.fit.origin = mesh.Corner(seed, 0);
    done[seed] = true;
    stack.push_back(seed);
    while (!stack.empty()) {
      const uint32_t t = stack.back();
      stack.pop_back();
      r.triangles.push_back(t);
      r.area += mesh.Area(t);
      for (int e = 0; e < 3; e++) {
        const uint32_t u = mesh.neighbour[t][e];
        if (u == kNone || !inRegion[u] || done[u])
          continue;
        bool flat = true;
        for (int c = 0; c < 3 && flat; c++)
          flat = std::abs(glm::dot(n, mesh.Corner(u, c)) - d) <= s.tolerance;
        if (!flat)
          continue;
        done[u] = true;
        stack.push_back(u);
      }
    }
    s.regions.push_back(std::move(r));
    s.facetsLeft++;
  }
  Renumber(s, mesh.triangles.size());
}

size_t Recognition::Count(Kind k) const {
  return static_cast<size_t>(
      std::count_if(rounds.begin(), rounds.end(),
                    [&](const RecognizedRound& r) { return r.kind == k; }));
}

void Recognition::Transform(const glm::dmat4& m, double scale) {
  const glm::dmat3 rot(m);
  const auto point = [&](const glm::dvec3& p) {
    return glm::dvec3(m * glm::dvec4(p * scale, 1.0));
  };
  for (RecognizedRound& r : rounds) {
    r.centre = point(r.centre);
    r.axis = glm::normalize(rot * r.axis);
    r.radius *= scale;
    r.sagitta *= scale;
  }
  for (Polygon& p : polygonal) {
    p.centre = point(p.centre);
    p.axis = glm::normalize(rot * p.axis);
    p.circumradius *= scale;
  }
  toleranceMm *= scale;
}

std::string Recognition::Summary() const {
  std::string out;
  const auto add = [&](size_t n, const char* one, const char* many) {
    if (n == 0)
      return;
    out += (out.empty() ? "" : ", ") + Plural(n, one, many);
  };
  add(Count(Kind::Cylinder), "cylinder", "cylinders");
  add(Count(Kind::Cone), "cone", "cones");
  add(Count(Kind::Sphere), "sphere", "spheres");
  add(planes, "plane", "planes");
  for (const Polygon& p : polygonal)
    out += "; a " + std::to_string(p.sides) + "-sided pocket or boss left flat";
  if (facetsLeft > 0)
    out += "; " + Plural(facetsLeft, "facet", "facets") + " left";
  if (!fellBack.empty())
    out = "kept as facets: " + fellBack;
  return out;
}

Recognition Summarize(const TriMesh& mesh, const Segmentation& s,
                      int minSides) {
  Recognition out;
  out.minSides = minSides;
  out.triangles = mesh.triangles.size();
  out.polygonal = s.polygonal;
  out.facetsLeft = s.facetsLeft;
  out.toleranceMm = s.tolerance;
  for (const Region& r : s.regions) {
    if (r.fit.kind == Kind::Plane) {
      out.planes++;
      continue;
    }
    RecognizedRound round;
    round.kind = r.fit.kind;
    round.axis = r.fit.axis;
    round.radius = r.fit.radius;
    round.halfAngle = r.fit.halfAngle;
    round.sides = r.sides;
    round.sagitta = r.sagitta;
    round.centre = r.fit.origin;
    if (r.fit.kind != Kind::Sphere) {
      double lo = 0.0, hi = 0.0, widest = 0.0;
      bool first = true;
      for (uint32_t t : r.triangles)
        for (int c = 0; c < 3; c++) {
          const glm::dvec3 w = mesh.Corner(t, c) - r.fit.origin;
          const double h = glm::dot(w, r.fit.axis);
          widest = std::max(widest, glm::length(w - h * r.fit.axis));
          lo = first ? h : std::min(lo, h);
          hi = first ? h : std::max(hi, h);
          first = false;
        }
      round.centre = r.fit.origin + r.fit.axis * (0.5 * (lo + hi));
      if (r.fit.kind == Kind::Cone)
        round.radius = widest;
    }
    out.rounds.push_back(round);
  }
  return out;
}

Segmentation SegmentAll(const TriMesh& mesh, const SegmentOptions& options) {
  if (mesh.triangles.empty() || mesh.neighbour.size() != mesh.triangles.size())
    return {};
  Segmenter s(mesh, options);
  return s.Run();
}

} // namespace mesh
