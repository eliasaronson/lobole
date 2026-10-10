// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MeshCollide.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace collide {
namespace {

// --- triangle/triangle intersection ----------------------------------------
// The separating-axis test for two triangles, over SEVENTEEN axes: the 2 face
// normals, the 9 edge-edge cross products, and — the six that matter — each
// triangle's three in-plane edge normals (edge x its own face normal).
//
// The usual statement of this test gives only the first eleven, and those are
// enough ONLY when the triangles are not coplanar. When they are, every one of
// the eleven is parallel to the shared plane normal, every projection is a
// point, nothing separates, and the test declares any two coplanar triangles
// intersecting however far apart they lie. That is not a corner case in CAD:
// a pin whose end cap is flush with the face it passes through is the normal
// way parts are drawn, and the slider-crank rig found it as a 0.5 mm
// clearance hole reported as a clash at a 0.17 mm deflection
// (docs/inspect/README.md#mesh-mode). The in-plane edge normals are the 2D
// separating axes within that plane, and they decide it.

bool Separated(const glm::dvec3& axis, const glm::dvec3* A,
               const glm::dvec3* B) {
  const double len2 = glm::dot(axis, axis);
  if (len2 < 1e-18)
    return false; // degenerate axis proves nothing
  double aMin = 1e300, aMax = -1e300, bMin = 1e300, bMax = -1e300;
  for (int i = 0; i < 3; i++) {
    const double pa = glm::dot(axis, A[i]);
    aMin = std::min(aMin, pa);
    aMax = std::max(aMax, pa);
    const double pb = glm::dot(axis, B[i]);
    bMin = std::min(bMin, pb);
    bMax = std::max(bMax, pb);
  }
  return aMin > bMax || bMin > aMax;
}

bool TrianglesIntersect(const glm::dvec3* A, const glm::dvec3* B) {
  const glm::dvec3 ea[3] = {A[1] - A[0], A[2] - A[1], A[0] - A[2]};
  const glm::dvec3 eb[3] = {B[1] - B[0], B[2] - B[1], B[0] - B[2]};
  const glm::dvec3 na = glm::cross(ea[0], ea[1]);
  const glm::dvec3 nb = glm::cross(eb[0], eb[1]);
  if (Separated(na, A, B) || Separated(nb, A, B))
    return false;
  for (const glm::dvec3& u : ea)
    for (const glm::dvec3& v : eb)
      if (Separated(glm::cross(u, v), A, B))
        return false;
  // The in-plane edge normals of each triangle: the axes that decide a
  // coplanar pair, where all of the above degenerate.
  for (const glm::dvec3& u : ea)
    if (Separated(glm::cross(u, na), A, B))
      return false;
  for (const glm::dvec3& v : eb)
    if (Separated(glm::cross(v, nb), A, B))
      return false;
  return true;
}

bool BoxesOverlap(const glm::dvec3& aLo, const glm::dvec3& aHi,
                  const glm::dvec3& bLo, const glm::dvec3& bHi) {
  return aLo.x <= bHi.x && bLo.x <= aHi.x && aLo.y <= bHi.y && bLo.y <= aHi.y &&
         aLo.z <= bHi.z && bLo.z <= aHi.z;
}

// A box transformed into another frame, kept axis-aligned there (the usual
// conservative 8-corner hull — cheap, and only ever too big, which costs a
// descent and never a wrong answer).
// --- triangle/triangle DISTANCE ---------------------------------------------
// For two triangles that do not intersect the minimum is attained either
// between two edges or between a vertex and the opposite face, so nine
// segment pairs and six point-face tests decide it. Squared throughout; one
// square root at the end of the whole query, not per triangle.

double SegmentDistance2(const glm::dvec3& p1, const glm::dvec3& q1,
                        const glm::dvec3& p2, const glm::dvec3& q2) {
  const glm::dvec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
  const double a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
  constexpr double eps = 1e-18;
  double s = 0.0, t = 0.0;
  if (a <= eps && e <= eps)
    return glm::dot(r, r);
  if (a <= eps) {
    t = std::clamp(f / e, 0.0, 1.0);
  } else {
    const double c = glm::dot(d1, r);
    if (e <= eps) {
      s = std::clamp(-c / a, 0.0, 1.0);
    } else {
      const double b = glm::dot(d1, d2), denom = a * e - b * b;
      s = denom > eps ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
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
  const glm::dvec3 gap = (p1 + d1 * s) - (p2 + d2 * t);
  return glm::dot(gap, gap);
}

glm::dvec3 ClosestOnTriangle(const glm::dvec3& p, const glm::dvec3& a,
                             const glm::dvec3& b, const glm::dvec3& c) {
  const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
  const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
  if (d1 <= 0.0 && d2 <= 0.0)
    return a;
  const glm::dvec3 bp = p - b;
  const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
  if (d3 >= 0.0 && d4 <= d3)
    return b;
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    return a + ab * (d1 / (d1 - d3));
  const glm::dvec3 cp = p - c;
  const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
  if (d6 >= 0.0 && d5 <= d6)
    return c;
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    return a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
    return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double denom = 1.0 / (va + vb + vc);
  return a + ab * (vb * denom) + ac * (vc * denom);
}

double TriangleDistance2(const glm::dvec3* A, const glm::dvec3* B) {
  if (TrianglesIntersect(A, B))
    return 0.0;
  double best = 1e300;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      best = std::min(
          best, SegmentDistance2(A[i], A[(i + 1) % 3], B[j], B[(j + 1) % 3]));
  for (int i = 0; i < 3; i++) {
    const glm::dvec3 q = ClosestOnTriangle(A[i], B[0], B[1], B[2]);
    best = std::min(best, glm::dot(A[i] - q, A[i] - q));
    const glm::dvec3 r = ClosestOnTriangle(B[i], A[0], A[1], A[2]);
    best = std::min(best, glm::dot(B[i] - r, B[i] - r));
  }
  return best;
}

// Squared distance between two axis-aligned boxes; 0 when they overlap. A
// lower bound on every triangle pair inside them, which is what prunes.
double BoxDistance2(const glm::dvec3& aLo, const glm::dvec3& aHi,
                    const glm::dvec3& bLo, const glm::dvec3& bHi) {
  double total = 0.0;
  for (int i = 0; i < 3; i++) {
    const double gap =
        std::max(0.0, std::max(aLo[i] - bHi[i], bLo[i] - aHi[i]));
    total += gap * gap;
  }
  return total;
}

void TransformBox(const glm::dmat4& m, const glm::dvec3& lo,
                  const glm::dvec3& hi, glm::dvec3& outLo, glm::dvec3& outHi) {
  outLo = glm::dvec3(1e300);
  outHi = glm::dvec3(-1e300);
  for (int i = 0; i < 8; i++) {
    const glm::dvec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y,
                            (i & 4) ? hi.z : lo.z);
    const glm::dvec3 p = glm::dvec3(m * glm::dvec4(corner, 1.0));
    outLo = glm::min(outLo, p);
    outHi = glm::max(outHi, p);
  }
}

// --- the shared cache -------------------------------------------------------
std::mutex g_mutex;
// Keyed on PLACEMENT, not on geometry alone: a rigid Transformed() shares
// the TShape, so two occurrences of one part have the same GeometryId and
// meshes in different places. Keying on GeometryId would hand the second
// occurrence the first one's mesh — every pair would then be tested against
// a body sitting somewhere else.
std::map<uint64_t, std::shared_ptr<const Mesh>> g_cache;

std::shared_ptr<const Mesh> CachedMesh(const CADShape& s) {
  const uint64_t key = s.PlacementId();
  // The BUILD happens under the lock, not just the map access.
  //
  // It used to be check, unlock, build, relock, store — so two workers
  // that both missed on the same body both called Mesh::Build, and that
  // ends in CADShape::GenerateMesh, which WRITES the triangulation into
  // the shared TShape. Two OpenCASCADE meshers on one shape at once
  // segfaults inside BRepMesh_EdgeDiscret. `interference mode=mesh` runs
  // its pairs on every core and one body appears in many pairs, so it hit
  // this whenever two of them raced — intermittently, which is how it read
  // as a flaky test rather than as the crash it is.
  //
  // Serialising the builds is the point rather than a cost: the documented
  // rule for this codebase is that shapes are warmed single-threaded
  // before workers touch them (RemoteInspection's WorldBounds pass,
  // MotionStudy's MeshFor pass), and those warm passes are serial anyway.
  // This makes the cache hold that rule itself instead of trusting every
  // future caller to remember it. A hit — which is every call after the
  // first per body — takes the lock and returns.
  if (key == 0)
    return std::shared_ptr<const Mesh>(Mesh::Build(s));
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_cache.find(key);
  if (it != g_cache.end())
    return it->second;
  auto built = Mesh::Build(s);
  g_cache[key] = built;
  return std::shared_ptr<const Mesh>(built);
}

} // namespace

std::shared_ptr<const Mesh> Mesh::Build(const CADShape& shape,
                                        double deflectionMm) {
  auto mesh = std::shared_ptr<Mesh>(new Mesh());
  if (!shape.IsValid())
    return mesh;
  // Unstated, the deflection is the shape's OWN size times 0.2%, so a screw
  // and a dock are meshed to the same relative fidelity. GenerateMesh
  // already derives its linear deflection this way; asking for the default
  // keeps the two in step rather than introducing a second rule.
  //
  // Stated, it is what the triangles are cut to. This used to take a
  // fraction that changed only the number reported, not the tessellation,
  // so a caller asking for a finer check got the same triangles labelled
  // finer. A stated one meshes a COPY: BRepMesh writes its triangulation
  // into the shape, and a finer one left there is what the renderer would
  // draw from then on.
  const bool stated = deflectionMm > 0.0;
  const CADShape::Mesh tess =
      stated ? shape.GenerateMeshDetached(deflectionMm) : shape.GenerateMesh();
  if (tess.indices.size() < 3)
    return mesh;

  glm::dvec3 lo(1e300), hi(-1e300);
  mesh->m_tris.reserve(tess.indices.size() / 3);
  for (size_t i = 0; i + 2 < tess.indices.size(); i += 3) {
    Tri t;
    t.face = i / 3 < tess.triangleFaces.size() ? tess.triangleFaces[i / 3] : 0;
    t.a = glm::dvec3(tess.vertices[tess.indices[i]].position);
    t.b = glm::dvec3(tess.vertices[tess.indices[i + 1]].position);
    t.c = glm::dvec3(tess.vertices[tess.indices[i + 2]].position);
    t.lo = glm::min(t.a, glm::min(t.b, t.c));
    t.hi = glm::max(t.a, glm::max(t.b, t.c));
    lo = glm::min(lo, t.lo);
    hi = glm::max(hi, t.hi);
    mesh->m_tris.push_back(t);
  }
  mesh->m_min = lo;
  mesh->m_max = hi;
  mesh->m_deflection = stated ? deflectionMm : glm::length(hi - lo) * 0.002;

  mesh->m_order.resize(mesh->m_tris.size());
  for (uint32_t i = 0; i < mesh->m_order.size(); i++)
    mesh->m_order[i] = i;
  mesh->m_nodes.reserve(2 * mesh->m_tris.size() / 4 + 8);
  mesh->m_nodes.push_back(Node{}); // index 0 is the root
  mesh->BuildNode(0, static_cast<uint32_t>(mesh->m_order.size()), 0);
  return mesh;
}

uint32_t Mesh::BuildNode(uint32_t start, uint32_t count, int depth) {
  const uint32_t self = static_cast<uint32_t>(m_nodes.size()) - 1;
  Node node;
  node.start = start;
  node.count = count;
  node.lo = glm::dvec3(1e300);
  node.hi = glm::dvec3(-1e300);
  for (uint32_t i = 0; i < count; i++) {
    const Tri& t = m_tris[m_order[start + i]];
    node.lo = glm::min(node.lo, t.lo);
    node.hi = glm::max(node.hi, t.hi);
  }
  // A leaf below 8 triangles or 24 levels: the tree is for skipping whole
  // limbs, and past that the box tests cost more than the triangle tests.
  if (count > 8 && depth < 24) {
    const glm::dvec3 span = node.hi - node.lo;
    const int axis =
        span.x > span.y ? (span.x > span.z ? 0 : 2) : (span.y > span.z ? 1 : 2);
    const auto begin = m_order.begin() + start;
    std::sort(begin, begin + count, [&](uint32_t l, uint32_t r) {
      return m_tris[l].lo[axis] + m_tris[l].hi[axis] <
             m_tris[r].lo[axis] + m_tris[r].hi[axis];
    });
    const uint32_t half = count / 2;
    m_nodes.push_back(Node{});
    node.left = BuildNode(start, half, depth + 1);
    m_nodes.push_back(Node{});
    node.right = BuildNode(start + half, count - half, depth + 1);
    node.count = 0; // interior
  }
  m_nodes[self] = node;
  return self;
}

void Mesh::Descend(uint32_t nodeA, const Mesh& other, uint32_t nodeB,
                   const glm::dmat4& bToA, bool& hit) const {
  if (hit)
    return;
  const Node& a = m_nodes[nodeA];
  const Node& b = other.m_nodes[nodeB];
  glm::dvec3 bLo, bHi;
  TransformBox(bToA, b.lo, b.hi, bLo, bHi);
  if (!BoxesOverlap(a.lo, a.hi, bLo, bHi))
    return;
  const bool aLeaf = a.count > 0;
  const bool bLeaf = b.count > 0;
  if (aLeaf && bLeaf) {
    for (uint32_t i = 0; i < a.count && !hit; i++) {
      const Tri& ta = m_tris[m_order[a.start + i]];
      const glm::dvec3 A[3] = {ta.a, ta.b, ta.c};
      for (uint32_t j = 0; j < b.count; j++) {
        const Tri& tb = other.m_tris[other.m_order[b.start + j]];
        const glm::dvec3 B[3] = {glm::dvec3(bToA * glm::dvec4(tb.a, 1.0)),
                                 glm::dvec3(bToA * glm::dvec4(tb.b, 1.0)),
                                 glm::dvec3(bToA * glm::dvec4(tb.c, 1.0))};
        if (TrianglesIntersect(A, B)) {
          hit = true;
          return;
        }
      }
    }
    return;
  }
  // Descend the one that is not a leaf; when neither is, split the bigger
  // box so the recursion cannot stall.
  const double aSize = glm::length(a.hi - a.lo);
  const double bSize = glm::length(bHi - bLo);
  if (bLeaf || (!aLeaf && aSize >= bSize)) {
    Descend(a.left, other, nodeB, bToA, hit);
    Descend(a.right, other, nodeB, bToA, hit);
  } else {
    Descend(nodeA, other, b.left, bToA, hit);
    Descend(nodeA, other, b.right, bToA, hit);
  }
}

void Mesh::Walk(uint32_t nodeA, const Mesh& other, uint32_t nodeB,
                const glm::dmat4& bToA, double limit2, double& best2,
                std::map<std::pair<uint32_t, uint32_t>, double>* pairs) const {
  const Node& a = m_nodes[nodeA];
  const Node& b = other.m_nodes[nodeB];
  glm::dvec3 bLo, bHi;
  TransformBox(bToA, b.lo, b.hi, bLo, bHi);
  // A box pair's distance is a LOWER bound on every triangle pair below it,
  // so one no closer than the cut-off cannot improve on it. Minimising, the
  // cut-off is the best found so far and shrinks; collecting, it is fixed.
  const double cut = pairs ? limit2 : best2;
  if (BoxDistance2(a.lo, a.hi, bLo, bHi) > cut)
    return;
  const bool aLeaf = a.count > 0, bLeaf = b.count > 0;
  if (aLeaf && bLeaf) {
    for (uint32_t i = 0; i < a.count; i++) {
      const Tri& ta = m_tris[m_order[a.start + i]];
      const glm::dvec3 A[3] = {ta.a, ta.b, ta.c};
      if (BoxDistance2(ta.lo, ta.hi, bLo, bHi) > (pairs ? limit2 : best2))
        continue;
      for (uint32_t j = 0; j < b.count; j++) {
        const Tri& tb = other.m_tris[other.m_order[b.start + j]];
        const glm::dvec3 B[3] = {glm::dvec3(bToA * glm::dvec4(tb.a, 1.0)),
                                 glm::dvec3(bToA * glm::dvec4(tb.b, 1.0)),
                                 glm::dvec3(bToA * glm::dvec4(tb.c, 1.0))};
        const double d2 = TriangleDistance2(A, B);
        if (pairs != nullptr) {
          if (d2 <= limit2) {
            // The NEAREST triangle pair per face pair: that is the bound the
            // caller prunes with.
            const std::pair<uint32_t, uint32_t> key{ta.face, tb.face};
            const auto it = pairs->find(key);
            if (it == pairs->end() || d2 < it->second)
              (*pairs)[key] = d2;
          }
        } else if (d2 < best2) {
          best2 = d2;
          if (best2 <= 0.0)
            return; // touching or crossing; nothing can beat it
        }
      }
    }
    return;
  }
  // Descend the one that is not a leaf, NEARER CHILD FIRST: a tight bound
  // found early is what lets the far side prune at all.
  const double aSize = glm::length(a.hi - a.lo);
  const double bSize = glm::length(bHi - bLo);
  if (bLeaf || (!aLeaf && aSize >= bSize)) {
    const Node& l = m_nodes[a.left];
    const Node& r = m_nodes[a.right];
    const bool leftFirst = BoxDistance2(l.lo, l.hi, bLo, bHi) <=
                           BoxDistance2(r.lo, r.hi, bLo, bHi);
    Walk(leftFirst ? a.left : a.right, other, nodeB, bToA, limit2, best2,
         pairs);
    Walk(leftFirst ? a.right : a.left, other, nodeB, bToA, limit2, best2,
         pairs);
  } else {
    Walk(nodeA, other, b.left, bToA, limit2, best2, pairs);
    Walk(nodeA, other, b.right, bToA, limit2, best2, pairs);
  }
}

Mesh::Near Mesh::Nearest(const glm::dmat4& xfA, const Mesh& other,
                         const glm::dmat4& xfB, double slack) const {
  Near out;
  if (m_tris.empty() || other.m_tris.empty())
    return out;
  const glm::dmat4 bToA = glm::inverse(xfA) * xfB;
  double best2 = 1e300;
  Walk(0, other, 0, bToA, 0.0, best2, nullptr);
  out.distance = std::sqrt(std::max(0.0, best2));
  // Second pass at a FIXED cut-off, for the face pairs. Cheap next to the
  // first: the bound is tight now, so nearly everything prunes at the top.
  const double limit = out.distance + std::max(slack, 0.0);
  double ignored = 1e300;
  std::map<std::pair<uint32_t, uint32_t>, double> pairs;
  Walk(0, other, 0, bToA, limit * limit, ignored, &pairs);
  out.pairs.reserve(pairs.size());
  for (const auto& [key, d2] : pairs)
    out.pairs.push_back({key.first, key.second, std::sqrt(d2)});
  std::sort(out.pairs.begin(), out.pairs.end(),
            [](const FacePair& l, const FacePair& r) {
              return l.distance < r.distance;
            });
  return out;
}

bool Mesh::Contains(const glm::dvec3& point) const {
  if (m_tris.empty())
    return false;
  if (point.x < m_min.x || point.x > m_max.x || point.y < m_min.y ||
      point.y > m_max.y || point.z < m_min.z || point.z > m_max.z)
    return false;
  // Parity along +X. A ray that grazes a shared edge can be counted twice,
  // so three rays vote — the cheap standard defence, and enough for a test
  // whose whole point is "one body is buried in another".
  static const glm::dvec3 dirs[3] = {
      glm::normalize(glm::dvec3(1.0, 0.013, 0.007)),
      glm::normalize(glm::dvec3(0.011, 1.0, 0.017)),
      glm::normalize(glm::dvec3(0.019, 0.009, 1.0))};
  int inside = 0;
  for (const glm::dvec3& dir : dirs) {
    int crossings = 0;
    for (const Tri& t : m_tris) {
      // Möller–Trumbore, front and back faces alike.
      const glm::dvec3 e1 = t.b - t.a, e2 = t.c - t.a;
      const glm::dvec3 p = glm::cross(dir, e2);
      const double det = glm::dot(e1, p);
      if (std::abs(det) < 1e-12)
        continue;
      const double inv = 1.0 / det;
      const glm::dvec3 tv = point - t.a;
      const double u = glm::dot(tv, p) * inv;
      if (u < 0.0 || u > 1.0)
        continue;
      const glm::dvec3 q = glm::cross(tv, e1);
      const double v = glm::dot(dir, q) * inv;
      if (v < 0.0 || u + v > 1.0)
        continue;
      if (glm::dot(e2, q) * inv > 1e-9)
        crossings++;
    }
    if (crossings % 2 == 1)
      inside++;
  }
  return inside >= 2;
}

bool Mesh::Overlaps(const glm::dmat4& xfA, const Mesh& other,
                    const glm::dmat4& xfB) const {
  if (m_tris.empty() || other.m_tris.empty())
    return false;
  const glm::dmat4 bToA = glm::inverse(xfA) * xfB;
  bool hit = false;
  Descend(0, other, 0, bToA, hit);
  if (hit)
    return true;
  // No triangle crosses. One may still be wholly INSIDE the other — a bolt
  // buried in a block — which a surface test alone calls clear.
  //
  // Gated on the boxes nesting, and that gate is what keeps this module
  // fast: a containment test is a ray parity count over every triangle, and
  // running one on each side of every CLEAR pair (the common case in any
  // sweep) would cost more than the tree just saved. Containment implies box
  // containment, so the gate can only skip pairs that are genuinely clear.
  glm::dvec3 bLo, bHi;
  TransformBox(bToA, other.m_min, other.m_max, bLo, bHi);
  const auto boxInside =
      [](const glm::dvec3& innerLo, const glm::dvec3& innerHi,
         const glm::dvec3& outerLo, const glm::dvec3& outerHi) {
        return innerLo.x >= outerLo.x && innerHi.x <= outerHi.x &&
               innerLo.y >= outerLo.y && innerHi.y <= outerHi.y &&
               innerLo.z >= outerLo.z && innerHi.z <= outerHi.z;
      };
  // A POINT OF THE MESH, not the centre of its box.
  //
  // No triangle crosses, so the other shape is wholly inside this one or
  // wholly outside it, and ONE point of it decides which. That point has to
  // belong to the shape: a bounding box's centre does not, and for anything
  // that WRAPS something else it lands in the void the wrap makes. A cam
  // bearing cap straddling its shaft is the case that found this — its box
  // nests inside the camshaft's, its box centre sits 7 mm off the shaft axis
  // and therefore inside a 13 mm shaft, and the cap was reported as buried in
  // a camshaft it merely sits over. Exact booleans called the same pose
  // clear (docs/inspect/README.md#mesh-mode).
  const auto pointOf = [](const Mesh& m, const glm::dmat4& into) {
    const Tri& t = m.m_tris.front();
    const glm::dvec3 c = (t.a + t.b + t.c) / 3.0;
    return glm::dvec3(into * glm::dvec4(c, 1.0));
  };
  if (boxInside(bLo, bHi, m_min, m_max))
    return Contains(pointOf(other, bToA));
  const glm::dmat4 aToB = glm::inverse(bToA);
  glm::dvec3 aLo, aHi;
  TransformBox(aToB, m_min, m_max, aLo, aHi);
  if (boxInside(aLo, aHi, other.m_min, other.m_max))
    return other.Contains(pointOf(*this, aToB));
  return false;
}

Result Overlap(const CADShape& a, const glm::dmat4& xfA, const CADShape& b,
               const glm::dmat4& xfB) {
  const auto ma = CachedMesh(a);
  const auto mb = CachedMesh(b);
  Result out;
  out.deflection = std::max(ma->Deflection(), mb->Deflection());
  out.intersects = ma->Overlaps(xfA, *mb, xfB);
  return out;
}

Gap Nearest(const CADShape& a, const glm::dmat4& xfA, const CADShape& b,
            const glm::dmat4& xfB, double slackDeflections) {
  Gap out;
  const auto ma = CachedMesh(a);
  const auto mb = CachedMesh(b);
  if (!ma || !mb || ma->Empty() || mb->Empty())
    return out;
  out.deflection = std::max(ma->Deflection(), mb->Deflection());
  out.tolerance = ma->Deflection() + mb->Deflection();
  const Mesh::Near nearest =
      ma->Nearest(xfA, *mb, xfB, slackDeflections * out.deflection);
  out.known = true;
  out.distance = nearest.distance;
  out.pairs = nearest.pairs;
  return out;
}

void Warm(const CADShape& shape) { (void)CachedMesh(shape); }

void Forget(const CADShape& shape) {
  const uint64_t key = shape.PlacementId();
  if (key == 0)
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_cache.erase(key);
}

size_t CacheSize() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_cache.size();
}

void ClearCache() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_cache.clear();
}

} // namespace collide
