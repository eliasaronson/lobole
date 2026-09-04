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
// (docs/agent-scale-plan.md, G2). The in-plane edge normals are the 2D
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

} // namespace

std::shared_ptr<const Mesh> Mesh::Build(const CADShape& shape,
                                        double deflectionFraction) {
  auto mesh = std::shared_ptr<Mesh>(new Mesh());
  if (!shape.IsValid())
    return mesh;
  // The deflection is the shape's OWN size times the fraction, so a screw
  // and a dock are meshed to the same relative fidelity. GenerateMesh
  // already derives its linear deflection this way; asking for the default
  // keeps the two in step rather than introducing a second rule.
  const CADShape::Mesh tess = shape.GenerateMesh();
  if (tess.indices.size() < 3)
    return mesh;

  glm::dvec3 lo(1e300), hi(-1e300);
  mesh->m_tris.reserve(tess.indices.size() / 3);
  for (size_t i = 0; i + 2 < tess.indices.size(); i += 3) {
    Tri t;
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
  const double diag = glm::length(hi - lo);
  mesh->m_deflection = diag * deflectionFraction;

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
  if (boxInside(bLo, bHi, m_min, m_max))
    return Contains((bLo + bHi) * 0.5);
  const glm::dmat4 aToB = glm::inverse(bToA);
  glm::dvec3 aLo, aHi;
  TransformBox(aToB, m_min, m_max, aLo, aHi);
  if (boxInside(aLo, aHi, other.m_min, other.m_max))
    return other.Contains((aLo + aHi) * 0.5);
  return false;
}

Result Overlap(const CADShape& a, const glm::dmat4& xfA, const CADShape& b,
               const glm::dmat4& xfB) {
  const auto get = [](const CADShape& s) {
    const uint64_t key = s.PlacementId();
    if (key != 0) {
      std::lock_guard<std::mutex> lock(g_mutex);
      const auto it = g_cache.find(key);
      if (it != g_cache.end())
        return it->second;
    }
    auto built = Mesh::Build(s);
    if (key != 0) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_cache[key] = built;
    }
    return std::shared_ptr<const Mesh>(built);
  };
  const auto ma = get(a);
  const auto mb = get(b);
  Result out;
  out.deflection = std::max(ma->Deflection(), mb->Deflection());
  out.intersects = ma->Overlaps(xfA, *mb, xfB);
  return out;
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
