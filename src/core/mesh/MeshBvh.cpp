// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/MeshBvh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <utility>

namespace mesh {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kLeafSize = 4;

double BoxDist2(const glm::dvec3& lo, const glm::dvec3& hi,
                const glm::dvec3& p) {
  const glm::dvec3 d = glm::max(glm::max(lo - p, p - hi), 0.0);
  return glm::dot(d, d);
}

double BoxBoxDist2(const glm::dvec3& alo, const glm::dvec3& ahi,
                   const glm::dvec3& blo, const glm::dvec3& bhi) {
  const glm::dvec3 d = glm::max(glm::max(alo - bhi, blo - ahi), 0.0);
  return glm::dot(d, d);
}

// Slab test: the entry t of the ray into the box, or +inf on a miss.
double RayBox(const glm::dvec3& o, const glm::dvec3& inv, const glm::dvec3& lo,
              const glm::dvec3& hi, double tMax) {
  double t0 = 0.0, t1 = tMax;
  for (int i = 0; i < 3; i++) {
    double a = (lo[i] - o[i]) * inv[i], b = (hi[i] - o[i]) * inv[i];
    if (a > b)
      std::swap(a, b);
    // NaN (0 * inf, a ray in the slab's plane) keeps the old bound.
    if (a > t0)
      t0 = a;
    if (b < t1)
      t1 = b;
    if (t0 > t1)
      return std::numeric_limits<double>::infinity();
  }
  return t0;
}

// Closest points of segments p1q1 and p2q2 (Ericson 5.1.9); returns the
// squared distance.
double SegSeg(const glm::dvec3& p1, const glm::dvec3& q1, const glm::dvec3& p2,
              const glm::dvec3& q2, glm::dvec3& c1, glm::dvec3& c2) {
  const glm::dvec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
  const double a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
  double s = 0.0, t = 0.0;
  constexpr double eps = 1e-300;
  if (a <= eps && e <= eps) {
    c1 = p1;
    c2 = p2;
    return glm::dot(c1 - c2, c1 - c2);
  }
  if (a <= eps) {
    t = std::clamp(f / e, 0.0, 1.0);
  } else {
    const double c = glm::dot(d1, r);
    if (e <= eps) {
      s = std::clamp(-c / a, 0.0, 1.0);
    } else {
      const double b = glm::dot(d1, d2), denom = a * e - b * b;
      s = denom > 0.0 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
      t = (b * s + f) / e;
      if (t < 0.0) {
        t = 0.0;
        s = std::clamp(-c / a, 0.0, 1.0);
      } else if (t > 1.0) {
        t = 1.0;
        s = std::clamp((b - c) / a, 0.0, 1.0);
      }
    }
  }
  c1 = p1 + d1 * s;
  c2 = p2 + d2 * t;
  return glm::dot(c1 - c2, c1 - c2);
}

// Does segment pq cross triangle abc? Sets the crossing point.
bool SegTri(const glm::dvec3& p, const glm::dvec3& q, const glm::dvec3& a,
            const glm::dvec3& b, const glm::dvec3& c, glm::dvec3& at) {
  const glm::dvec3 dir = q - p, e1 = b - a, e2 = c - a;
  const glm::dvec3 h = glm::cross(dir, e2);
  const double det = glm::dot(e1, h);
  if (std::abs(det) < 1e-300)
    return false;
  const double inv = 1.0 / det;
  const glm::dvec3 s = p - a;
  const double u = glm::dot(s, h) * inv;
  if (u < 0.0 || u > 1.0)
    return false;
  const glm::dvec3 qv = glm::cross(s, e1);
  const double v = glm::dot(dir, qv) * inv;
  if (v < 0.0 || u + v > 1.0)
    return false;
  const double t = glm::dot(e2, qv) * inv;
  if (t < 0.0 || t > 1.0)
    return false;
  at = p + dir * t;
  return true;
}

// The squared distance between two triangles, and the points that realise
// it: 0 where they cross, else the least of the six corner-to-triangle and
// nine edge-to-edge distances.
double TriTri(const glm::dvec3* A, const glm::dvec3* B, glm::dvec3& ca,
              glm::dvec3& cb) {
  for (int i = 0; i < 3; i++) {
    glm::dvec3 at;
    if (SegTri(A[i], A[(i + 1) % 3], B[0], B[1], B[2], at) ||
        SegTri(B[i], B[(i + 1) % 3], A[0], A[1], A[2], at)) {
      ca = cb = at;
      return 0.0;
    }
  }
  double best = std::numeric_limits<double>::max();
  for (int i = 0; i < 3; i++) {
    const glm::dvec3 qb = Bvh::ClosestOnTriangle(A[i], B[0], B[1], B[2]);
    double d = glm::dot(qb - A[i], qb - A[i]);
    if (d < best) {
      best = d;
      ca = A[i];
      cb = qb;
    }
    const glm::dvec3 qa = Bvh::ClosestOnTriangle(B[i], A[0], A[1], A[2]);
    d = glm::dot(qa - B[i], qa - B[i]);
    if (d < best) {
      best = d;
      ca = qa;
      cb = B[i];
    }
    for (int j = 0; j < 3; j++) {
      glm::dvec3 x, y;
      d = SegSeg(A[i], A[(i + 1) % 3], B[j], B[(j + 1) % 3], x, y);
      if (d < best) {
        best = d;
        ca = x;
        cb = y;
      }
    }
  }
  return best;
}

} // namespace

Bvh::Bvh(std::shared_ptr<const TriMesh> mesh) : m_mesh(std::move(mesh)) {
  const TriMesh& m = *m_mesh;
  const auto n = uint32_t(m.triangles.size());
  if (n == 0)
    return;
  m_order.resize(n);
  std::iota(m_order.begin(), m_order.end(), 0u);
  std::vector<glm::dvec3> centres(n);
  for (uint32_t t = 0; t < n; t++)
    centres[t] = (m.Corner(t, 0) + m.Corner(t, 1) + m.Corner(t, 2)) / 3.0;
  m_nodes.reserve(2 * (n / kLeafSize + 1));
  Build(centres, 0, n);
}

uint32_t Bvh::Build(std::vector<glm::dvec3>& centres, uint32_t begin,
                    uint32_t end) {
  const TriMesh& m = *m_mesh;
  const auto id = uint32_t(m_nodes.size());
  m_nodes.emplace_back();
  Node n;
  n.lo = glm::dvec3(std::numeric_limits<double>::max());
  n.hi = -n.lo;
  double area = 0.0;
  for (uint32_t i = begin; i < end; i++) {
    const uint32_t t = m_order[i];
    for (int c = 0; c < 3; c++) {
      n.lo = glm::min(n.lo, m.Corner(t, c));
      n.hi = glm::max(n.hi, m.Corner(t, c));
    }
    const glm::dvec3 an = m.AreaNormal(t);
    const double a = 0.5 * glm::length(an);
    area += a;
    n.centroid += a * centres[t];
    n.areaNormal += 0.5 * an;
  }
  n.centroid = area > 0.0 ? n.centroid / area : (n.lo + n.hi) / 2.0;
  n.radius = 0.0;
  for (int c = 0; c < 8; c++) {
    const glm::dvec3 k((c & 1) ? n.hi.x : n.lo.x, (c & 2) ? n.hi.y : n.lo.y,
                       (c & 4) ? n.hi.z : n.lo.z);
    n.radius = std::max(n.radius, glm::length(k - n.centroid));
  }
  if (end - begin <= kLeafSize) {
    n.first = begin;
    n.count = end - begin;
    m_nodes[id] = n;
    return id;
  }
  glm::dvec3 clo(std::numeric_limits<double>::max()), chi = -clo;
  for (uint32_t i = begin; i < end; i++) {
    clo = glm::min(clo, centres[m_order[i]]);
    chi = glm::max(chi, centres[m_order[i]]);
  }
  const glm::dvec3 ext = chi - clo;
  const int axis =
      ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
  const uint32_t mid = begin + (end - begin) / 2;
  std::nth_element(m_order.begin() + begin, m_order.begin() + mid,
                   m_order.begin() + end, [&](uint32_t a, uint32_t b) {
                     return centres[a][axis] < centres[b][axis];
                   });
  m_nodes[id] = n; // the children's emplace_back may move the vector
  Build(centres, begin, mid);
  const uint32_t right = Build(centres, mid, end);
  m_nodes[id].right = right;
  return id;
}

std::shared_ptr<const Bvh> Bvh::For(std::shared_ptr<const TriMesh> mesh) {
  struct Entry {
    std::weak_ptr<const TriMesh> mesh;
    std::weak_ptr<const Bvh> tree;
  };
  static std::mutex mu;
  static std::map<const TriMesh*, Entry> cache;
  static std::vector<std::shared_ptr<const Bvh>> recent; // the last few
  constexpr size_t kRecent = 4;
  if (!mesh)
    return nullptr;
  const TriMesh* key = mesh.get();
  {
    std::lock_guard lock(mu);
    if (auto it = cache.find(key); it != cache.end())
      if (auto live = it->second.tree.lock();
          live && it->second.mesh.lock() == mesh)
        return live;
  }
  auto built = std::make_shared<const Bvh>(mesh);
  std::lock_guard lock(mu);
  for (auto it = cache.begin(); it != cache.end();)
    it = it->second.tree.expired() ? cache.erase(it) : std::next(it);
  cache[key] = {mesh, built};
  recent.push_back(built);
  if (recent.size() > kRecent)
    recent.erase(recent.begin());
  return built;
}

glm::dvec3 Bvh::ClosestOnTriangle(const glm::dvec3& p, const glm::dvec3& a,
                                  const glm::dvec3& b, const glm::dvec3& c) {
  const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
  const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
  if (d1 <= 0 && d2 <= 0)
    return a;
  const glm::dvec3 bp = p - b;
  const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
  if (d3 >= 0 && d4 <= d3)
    return b;
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0)
    return a + ab * (d1 / (d1 - d3));
  const glm::dvec3 cp = p - c;
  const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
  if (d6 >= 0 && d5 <= d6)
    return c;
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0)
    return a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
    return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double denom = va + vb + vc;
  if (denom == 0.0)
    return a; // degenerate: all three corners on one point
  return a + ab * (vb / denom) + ac * (vc / denom);
}

Bvh::Hit Bvh::Ray(const glm::dvec3& origin, const glm::dvec3& dir,
                  double tMax) const {
  Hit best;
  if (m_nodes.empty())
    return best;
  const TriMesh& m = *m_mesh;
  const glm::dvec3 inv(1.0 / dir.x, 1.0 / dir.y, 1.0 / dir.z);
  double bestT = tMax;
  std::vector<uint32_t> stack{0};
  while (!stack.empty()) {
    const Node& n = m_nodes[stack.back()];
    stack.pop_back();
    if (RayBox(origin, inv, n.lo, n.hi, bestT) > bestT)
      continue;
    if (n.count) {
      for (uint32_t i = n.first; i < n.first + n.count; i++) {
        const uint32_t t = m_order[i];
        const glm::dvec3 a = m.Corner(t, 0), e1 = m.Corner(t, 1) - a,
                         e2 = m.Corner(t, 2) - a;
        const glm::dvec3 h = glm::cross(dir, e2);
        const double det = glm::dot(e1, h);
        if (det == 0.0)
          continue;
        const double f = 1.0 / det;
        const glm::dvec3 s = origin - a;
        const double u = f * glm::dot(s, h);
        if (u < 0.0 || u > 1.0)
          continue;
        const glm::dvec3 q = glm::cross(s, e1);
        const double v = f * glm::dot(dir, q);
        if (v < 0.0 || u + v > 1.0)
          continue;
        const double tt = f * glm::dot(e2, q);
        if (tt >= 0.0 && tt <= bestT) {
          bestT = tt;
          best.hit = true;
          best.t = tt;
          best.triangle = t;
        }
      }
      continue;
    }
    const uint32_t left = uint32_t(&n - m_nodes.data()) + 1, right = n.right;
    const double tl = RayBox(origin, inv, m_nodes[left].lo, m_nodes[left].hi,
                             bestT),
                 tr = RayBox(origin, inv, m_nodes[right].lo, m_nodes[right].hi,
                             bestT);
    // The nearer child last, so it is popped first.
    if (tl < tr) {
      stack.push_back(right);
      stack.push_back(left);
    } else {
      stack.push_back(left);
      stack.push_back(right);
    }
  }
  if (best.hit)
    best.point = origin + dir * best.t;
  return best;
}

Bvh::Closest Bvh::ClosestPoint(const glm::dvec3& p, double maxDistance) const {
  Closest best;
  if (m_nodes.empty())
    return best;
  const TriMesh& m = *m_mesh;
  double best2 = maxDistance < std::sqrt(std::numeric_limits<double>::max())
                     ? maxDistance * maxDistance
                     : std::numeric_limits<double>::max();
  std::vector<uint32_t> stack{0};
  while (!stack.empty()) {
    const Node& n = m_nodes[stack.back()];
    stack.pop_back();
    if (BoxDist2(n.lo, n.hi, p) > best2)
      continue;
    if (n.count) {
      for (uint32_t i = n.first; i < n.first + n.count; i++) {
        const uint32_t t = m_order[i];
        const glm::dvec3 q = ClosestOnTriangle(p, m.Corner(t, 0),
                                               m.Corner(t, 1), m.Corner(t, 2));
        const double d2 = glm::dot(q - p, q - p);
        if (d2 <= best2) {
          best2 = d2;
          best.found = true;
          best.triangle = t;
          best.point = q;
        }
      }
      continue;
    }
    const uint32_t left = uint32_t(&n - m_nodes.data()) + 1, right = n.right;
    if (BoxDist2(m_nodes[left].lo, m_nodes[left].hi, p) <
        BoxDist2(m_nodes[right].lo, m_nodes[right].hi, p)) {
      stack.push_back(right);
      stack.push_back(left);
    } else {
      stack.push_back(left);
      stack.push_back(right);
    }
  }
  if (best.found)
    best.distance = std::sqrt(best2);
  return best;
}

double Bvh::Winding(const glm::dvec3& p) const {
  if (m_nodes.empty())
    return 0.0;
  const TriMesh& m = *m_mesh;
  double w = 0.0;
  std::vector<uint32_t> stack{0};
  while (!stack.empty()) {
    const Node& n = m_nodes[stack.back()];
    stack.pop_back();
    const glm::dvec3 r = n.centroid - p;
    const double dist = glm::length(r);
    if (dist > 2.0 * n.radius) {
      w += glm::dot(r, n.areaNormal) / (4.0 * kPi * dist * dist * dist);
      continue;
    }
    if (n.count) {
      for (uint32_t i = n.first; i < n.first + n.count; i++) {
        const uint32_t t = m_order[i];
        const glm::dvec3 a = m.Corner(t, 0) - p, b = m.Corner(t, 1) - p,
                         c = m.Corner(t, 2) - p;
        const double la = glm::length(a), lb = glm::length(b),
                     lc = glm::length(c);
        const double num = glm::dot(a, glm::cross(b, c));
        const double den = la * lb * lc + glm::dot(a, b) * lc +
                           glm::dot(b, c) * la + glm::dot(c, a) * lb;
        w += std::atan2(num, den) / (2.0 * kPi);
      }
      continue;
    }
    stack.push_back(uint32_t(&n - m_nodes.data()) + 1);
    stack.push_back(n.right);
  }
  return w;
}

Bvh::Gap Bvh::Distance(const Bvh& other, const glm::dmat4& otherToThis) const {
  Gap gap;
  if (m_nodes.empty() || other.m_nodes.empty())
    return gap;
  const TriMesh& ma = *m_mesh;
  const TriMesh& mb = *other.m_mesh;
  // Every node of `other` as a box in this frame, once.
  std::vector<std::pair<glm::dvec3, glm::dvec3>> boxB(other.m_nodes.size());
  for (size_t i = 0; i < other.m_nodes.size(); i++) {
    const Node& n = other.m_nodes[i];
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi = -lo;
    for (int c = 0; c < 8; c++) {
      const glm::dvec3 k((c & 1) ? n.hi.x : n.lo.x, (c & 2) ? n.hi.y : n.lo.y,
                         (c & 4) ? n.hi.z : n.lo.z);
      const glm::dvec3 w = glm::dvec3(otherToThis * glm::dvec4(k, 1.0));
      lo = glm::min(lo, w);
      hi = glm::max(hi, w);
    }
    boxB[i] = {lo, hi};
  }
  double best2 = std::numeric_limits<double>::max();
  std::vector<std::pair<uint32_t, uint32_t>> stack{{0u, 0u}};
  while (!stack.empty() && best2 > 0.0) {
    const auto [ia, ib] = stack.back();
    stack.pop_back();
    const Node& a = m_nodes[ia];
    const Node& b = other.m_nodes[ib];
    if (BoxBoxDist2(a.lo, a.hi, boxB[ib].first, boxB[ib].second) > best2)
      continue;
    if (a.count && b.count) {
      for (uint32_t i = a.first; i < a.first + a.count; i++) {
        const uint32_t ta = m_order[i];
        const glm::dvec3 A[3] = {ma.Corner(ta, 0), ma.Corner(ta, 1),
                                 ma.Corner(ta, 2)};
        for (uint32_t j = b.first; j < b.first + b.count; j++) {
          const uint32_t tb = other.m_order[j];
          glm::dvec3 B[3];
          for (int c = 0; c < 3; c++)
            B[c] = glm::dvec3(otherToThis * glm::dvec4(mb.Corner(tb, c), 1.0));
          glm::dvec3 ca, cb;
          const double d2 = TriTri(A, B, ca, cb);
          if (d2 < best2) {
            best2 = d2;
            gap.onThis = ca;
            gap.onOther = cb;
          }
        }
      }
      continue;
    }
    // Split the larger node (or the inner one).
    const bool splitA =
        b.count ||
        (!a.count && glm::length(a.hi - a.lo) >=
                         glm::length(boxB[ib].second - boxB[ib].first));
    if (splitA) {
      stack.push_back({ia + 1, ib});
      stack.push_back({a.right, ib});
    } else {
      stack.push_back({ia, ib + 1});
      stack.push_back({ia, b.right});
    }
  }
  gap.distance = std::sqrt(best2);
  return gap;
}

} // namespace mesh
