#include "render/OutlineEdges.h"

#include <algorithm>

namespace outline {

namespace {
struct EdgeRef {
  uint64_t key; // (min vertex << 32) | max vertex
  uint32_t tri;
};
} // namespace

EdgeAdjacency BuildEdgeAdjacency(const glm::vec3* positions,
                                 size_t positionCount, size_t positionStride,
                                 const uint32_t* indices, size_t indexCount,
                                 float featureCos) {
  EdgeAdjacency adj;
  const size_t triCount = indexCount / 3;
  if (triCount == 0 || positionCount == 0)
    return adj;

  auto pos = [&](uint32_t i) -> const glm::vec3& {
    return *reinterpret_cast<const glm::vec3*>(
        reinterpret_cast<const char*>(positions) + i * positionStride);
  };

  adj.triNormal.resize(triCount);
  adj.triCentroid.resize(triCount);

  std::vector<EdgeRef> refs;
  refs.reserve(indexCount);

  for (size_t t = 0; t < triCount; ++t) {
    const uint32_t v0 = indices[3 * t];
    const uint32_t v1 = indices[3 * t + 1];
    const uint32_t v2 = indices[3 * t + 2];
    const glm::vec3& p0 = pos(v0);
    const glm::vec3& p1 = pos(v1);
    const glm::vec3& p2 = pos(v2);

    const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
    const float len = glm::length(n);
    // Degenerate triangles get a zero normal instead of NaNs; their internal
    // edges are excluded from classification below.
    adj.triNormal[t] = len > 1e-12f ? n / len : glm::vec3(0.0f);
    adj.triCentroid[t] = (p0 + p1 + p2) / 3.0f;

    auto add = [&](uint32_t a, uint32_t b) {
      if (a > b)
        std::swap(a, b);
      refs.push_back({(uint64_t(a) << 32) | b, static_cast<uint32_t>(t)});
    };
    add(v0, v1);
    add(v1, v2);
    add(v2, v0);
  }

  std::sort(refs.begin(), refs.end(),
            [](const EdgeRef& a, const EdgeRef& b) { return a.key < b.key; });

  struct InternalEdge {
    uint32_t v0, v1, triA, triB;
    float normalDot;
    bool degenerate; // either triangle has zero area
  };
  std::vector<InternalEdge> internal;
  std::vector<std::pair<uint32_t, uint32_t>> boundary;

  for (size_t i = 0; i < refs.size();) {
    size_t j = i;
    while (j < refs.size() && refs[j].key == refs[i].key)
      ++j;
    const uint32_t a = static_cast<uint32_t>(refs[i].key >> 32);
    const uint32_t b = static_cast<uint32_t>(refs[i].key);
    if (j - i == 1) {
      boundary.emplace_back(a, b);
    } else if (j - i == 2) {
      const uint32_t triA = refs[i].tri;
      const uint32_t triB = refs[i + 1].tri;
      const glm::vec3& nA = adj.triNormal[triA];
      const glm::vec3& nB = adj.triNormal[triB];
      const bool degenerate = nA == glm::vec3(0.0f) || nB == glm::vec3(0.0f);
      internal.push_back(
          {a, b, triA, triB, degenerate ? 0.0f : glm::dot(nA, nB), degenerate});
    }
    // >2 adjacent triangles: non-manifold; ignored (matches the old
    // extractor, which only handled the 1- and 2-triangle cases).
    i = j;
  }

  auto classify = [&](float cosThreshold) {
    adj.staticIndices.clear();
    adj.smoothEdges.clear();
    for (const auto& [a, b] : boundary) {
      adj.staticIndices.push_back(a);
      adj.staticIndices.push_back(b);
    }
    for (const auto& e : internal) {
      if (e.degenerate) {
        // The old extractor computed NaN normals for zero-area triangles, so
        // their edges never passed any dot-product test — keep them out of
        // both sets to match.
        continue;
      }
      if (e.normalDot < cosThreshold) {
        adj.staticIndices.push_back(e.v0);
        adj.staticIndices.push_back(e.v1);
      } else {
        adj.smoothEdges.push_back({e.v0, e.v1, e.triA, e.triB});
      }
    }
  };

  classify(featureCos);
  // Lenient fallback (old extractor behavior): if almost nothing qualified,
  // retry at cos(36°) so simple smooth meshes still show structure.
  if (adj.staticIndices.size() < 6)
    classify(0.8f);

  return adj;
}

void AppendSilhouetteIndices(const EdgeAdjacency& adj,
                             const glm::vec3& cameraLocal,
                             SilhouetteScratch& scratch,
                             std::vector<uint32_t>& out) {
  const size_t triCount = adj.triNormal.size();
  scratch.facing.resize(triCount);
  for (size_t t = 0; t < triCount; ++t) {
    scratch.facing[t] =
        glm::dot(adj.triNormal[t], cameraLocal - adj.triCentroid[t]) > 0.0f;
  }
  for (const auto& e : adj.smoothEdges) {
    if (scratch.facing[e.triA] != scratch.facing[e.triB]) {
      out.push_back(e.v0);
      out.push_back(e.v1);
    }
  }
}

} // namespace outline
