// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "render/OutlineEdges.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <utility>
#include <vector>

using outline::AppendSilhouetteIndices;
using outline::BuildEdgeAdjacency;
using outline::EdgeAdjacency;
using outline::SilhouetteScratch;

namespace {

using EdgeSet = std::set<std::pair<uint32_t, uint32_t>>;

EdgeSet ToEdgeSet(const std::vector<uint32_t>& lineIndices) {
  EdgeSet set;
  for (size_t i = 0; i + 1 < lineIndices.size(); i += 2) {
    uint32_t a = lineIndices[i], b = lineIndices[i + 1];
    if (a > b)
      std::swap(a, b);
    set.insert({a, b});
  }
  return set;
}

// Faithful port of the old OutlineRenderer::ExtractEdges (map-based, with
// the lenient fallback) — the reference the new static classification must
// reproduce.
std::vector<uint32_t> ReferenceStaticEdges(const std::vector<glm::vec3>& verts,
                                           const std::vector<uint32_t>& idx) {
  std::map<std::pair<uint32_t, uint32_t>, std::vector<glm::vec3>> edgeToNormals;
  std::vector<uint32_t> edges;

  for (size_t i = 0; i < idx.size(); i += 3) {
    uint32_t v0 = idx[i], v1 = idx[i + 1], v2 = idx[i + 2];
    glm::vec3 normal = glm::normalize(
        glm::cross(verts[v1] - verts[v0], verts[v2] - verts[v0]));
    auto addEdgeNormal = [&](uint32_t a, uint32_t b) {
      if (a > b)
        std::swap(a, b);
      edgeToNormals[{a, b}].push_back(normal);
    };
    addEdgeNormal(v0, v1);
    addEdgeNormal(v1, v2);
    addEdgeNormal(v2, v0);
  }

  const float featureAngleThreshold = 0.5f;
  for (const auto& edgeData : edgeToNormals) {
    bool include = false;
    if (edgeData.second.size() == 1)
      include = true;
    else if (edgeData.second.size() == 2)
      include = glm::dot(edgeData.second[0], edgeData.second[1]) <
                featureAngleThreshold;
    if (include) {
      edges.push_back(edgeData.first.first);
      edges.push_back(edgeData.first.second);
    }
  }

  if (edges.size() < 6) {
    edges.clear();
    const float lenientThreshold = 0.8f;
    for (const auto& edgeData : edgeToNormals) {
      if (edgeData.second.size() == 1) {
        edges.push_back(edgeData.first.first);
        edges.push_back(edgeData.first.second);
      } else if (edgeData.second.size() == 2) {
        if (glm::dot(edgeData.second[0], edgeData.second[1]) <
            lenientThreshold) {
          edges.push_back(edgeData.first.first);
          edges.push_back(edgeData.first.second);
        }
      }
    }
  }
  return edges;
}

// Reference silhouette classification: the old UpdateMeshEdgesForView facing
// rule, computed in world space on transformed vertices, restricted to the
// smooth internal edges (the set the new code classifies per view).
EdgeSet ReferenceSilhouettes(const std::vector<glm::vec3>& verts,
                             const std::vector<uint32_t>& idx,
                             const glm::mat4& transform,
                             const glm::vec3& cameraWorld,
                             const EdgeAdjacency& adj) {
  std::vector<uint8_t> facing(idx.size() / 3);
  for (size_t t = 0; t < idx.size() / 3; ++t) {
    glm::vec3 p0 = glm::vec3(transform * glm::vec4(verts[idx[3 * t]], 1.0f));
    glm::vec3 p1 =
        glm::vec3(transform * glm::vec4(verts[idx[3 * t + 1]], 1.0f));
    glm::vec3 p2 =
        glm::vec3(transform * glm::vec4(verts[idx[3 * t + 2]], 1.0f));
    glm::vec3 normal = glm::normalize(glm::cross(p1 - p0, p2 - p0));
    glm::vec3 center = (p0 + p1 + p2) / 3.0f;
    facing[t] = glm::dot(normal, cameraWorld - center) > 0.0f;
  }
  EdgeSet set;
  for (const auto& e : adj.smoothEdges) {
    if (facing[e.triA] != facing[e.triB]) {
      uint32_t a = e.v0, b = e.v1;
      if (a > b)
        std::swap(a, b);
      set.insert({a, b});
    }
  }
  return set;
}

EdgeAdjacency Build(const std::vector<glm::vec3>& verts,
                    const std::vector<uint32_t>& idx) {
  return BuildEdgeAdjacency(verts.data(), verts.size(), sizeof(glm::vec3),
                            idx.data(), idx.size());
}

// Shared-vertex unit cube: 8 vertices, 12 triangles, 18 unique edges — the
// 12 sharp perimeter edges plus 6 coplanar face diagonals.
void MakeCube(std::vector<glm::vec3>& verts, std::vector<uint32_t>& idx) {
  verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
           {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
  idx = {// -Z (outward = 0,0,-1)
         0, 2, 1, 0, 3, 2,
         // +Z
         4, 5, 6, 4, 6, 7,
         // -Y
         0, 1, 5, 0, 5, 4,
         // +Y
         3, 6, 2, 3, 7, 6,
         // -X
         0, 4, 7, 0, 7, 3,
         // +X
         1, 2, 6, 1, 6, 5};
}

// Open-ended tube (cylinder side surface) with shared vertices: boundary
// rings top and bottom, smooth vertical/diagonal edges in between.
void MakeTube(std::vector<glm::vec3>& verts, std::vector<uint32_t>& idx,
              int segments) {
  verts.clear();
  idx.clear();
  for (int ring = 0; ring < 2; ++ring) {
    for (int s = 0; s < segments; ++s) {
      float a =
          2.0f * std::numbers::pi_v<float> * static_cast<float>(s) / segments;
      verts.push_back({std::cos(a), std::sin(a), static_cast<float>(ring)});
    }
  }
  for (int s = 0; s < segments; ++s) {
    uint32_t s1 = (s + 1) % segments;
    uint32_t b0 = s, b1 = s1;
    uint32_t t0 = segments + s, t1 = segments + s1;
    // Outward winding
    idx.insert(idx.end(), {b0, b1, t1});
    idx.insert(idx.end(), {b0, t1, t0});
  }
}

// UV sphere with shared vertices (poles duplicated per column kept simple:
// one vertex per (row, col)); smooth everywhere at this tessellation.
void MakeSphere(std::vector<glm::vec3>& verts, std::vector<uint32_t>& idx,
                int rows, int cols) {
  verts.clear();
  idx.clear();
  for (int r = 0; r <= rows; ++r) {
    float phi = std::numbers::pi_v<float> * static_cast<float>(r) / rows;
    for (int c = 0; c < cols; ++c) {
      float theta =
          2.0f * std::numbers::pi_v<float> * static_cast<float>(c) / cols;
      verts.push_back({std::sin(phi) * std::cos(theta),
                       std::sin(phi) * std::sin(theta), std::cos(phi)});
    }
  }
  auto at = [&](int r, int c) -> uint32_t {
    return static_cast<uint32_t>(r * cols + (c % cols));
  };
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      idx.insert(idx.end(), {at(r, c), at(r + 1, c), at(r + 1, c + 1)});
      idx.insert(idx.end(), {at(r, c), at(r + 1, c + 1), at(r, c + 1)});
    }
  }
}

} // namespace

TEST_CASE("Cube adjacency: 12 feature edges, 6 smooth diagonals, no boundary",
          "[outline]") {
  std::vector<glm::vec3> verts;
  std::vector<uint32_t> idx;
  MakeCube(verts, idx);

  EdgeAdjacency adj = Build(verts, idx);
  CHECK(adj.staticIndices.size() == 24); // 12 sharp edges
  CHECK(adj.smoothEdges.size() == 6);    // 6 coplanar face diagonals

  // Coplanar diagonal triangles can never disagree on facing → a cube has
  // no view-dependent silhouettes from any generic viewpoint.
  SilhouetteScratch scratch;
  std::vector<uint32_t> out;
  for (const glm::vec3 cam :
       {glm::vec3(3.0f, 2.0f, 5.0f), glm::vec3(-4.0f, 1.5f, -2.0f),
        glm::vec3(0.3f, 7.0f, 0.9f)}) {
    out.clear();
    AppendSilhouetteIndices(adj, cam, scratch, out);
    CHECK(out.empty());
  }
}

TEST_CASE("Static edges match the old extractor on canonical shapes",
          "[outline]") {
  std::vector<glm::vec3> verts;
  std::vector<uint32_t> idx;

  SECTION("cube") { MakeCube(verts, idx); }
  SECTION("tube") { MakeTube(verts, idx, 32); }
  SECTION("sphere (degenerate pole triangles, NaN-exclusion parity)") {
    MakeSphere(verts, idx, 12, 16);
  }

  EdgeAdjacency adj = Build(verts, idx);
  CHECK(ToEdgeSet(adj.staticIndices) ==
        ToEdgeSet(ReferenceStaticEdges(verts, idx)));
}

TEST_CASE("Tube silhouettes: side view yields flip edges that track the "
          "camera",
          "[outline]") {
  std::vector<glm::vec3> verts;
  std::vector<uint32_t> idx;
  MakeTube(verts, idx, 48);
  EdgeAdjacency adj = Build(verts, idx);

  // Boundary rings are static; everything else is smooth at 48 segments.
  CHECK(!adj.smoothEdges.empty());

  SilhouetteScratch scratch;
  std::vector<uint32_t> out;
  AppendSilhouetteIndices(adj, glm::vec3(10.0f, 0.0f, 0.5f), scratch, out);
  CHECK(!out.empty());
  EdgeSet fromX = ToEdgeSet(out);

  out.clear();
  AppendSilhouetteIndices(adj, glm::vec3(0.0f, 10.0f, 0.5f), scratch, out);
  EdgeSet fromY = ToEdgeSet(out);

  // The silhouette must move with the camera (a quarter-turn apart here).
  CHECK(fromX != fromY);

  // And each emitted edge must genuinely separate a front from a back face.
  EdgeSet ref = ReferenceSilhouettes(verts, idx, glm::mat4(1.0f),
                                     glm::vec3(10.0f, 0.0f, 0.5f), adj);
  CHECK(fromX == ref);
}

TEST_CASE("Mesh-local camera equals world-space facing under a rigid "
          "transform",
          "[outline]") {
  std::vector<glm::vec3> verts;
  std::vector<uint32_t> idx;
  MakeSphere(verts, idx, 10, 14);
  EdgeAdjacency adj = Build(verts, idx);

  std::mt19937 rng(7);
  std::uniform_real_distribution<float> d(-1.0f, 1.0f);
  for (int i = 0; i < 20; ++i) {
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f),
                       glm::vec3(d(rng) * 5.0f, d(rng) * 5.0f, d(rng) * 5.0f));
    transform = glm::rotate(transform, d(rng) * 3.0f,
                            glm::normalize(glm::vec3(d(rng), d(rng), d(rng)) +
                                           glm::vec3(0.0f, 0.0f, 1.5f)));
    glm::vec3 cameraWorld(d(rng) * 10.0f, d(rng) * 10.0f, d(rng) * 10.0f);

    glm::vec3 cameraLocal =
        glm::vec3(glm::inverse(transform) * glm::vec4(cameraWorld, 1.0f));
    SilhouetteScratch scratch;
    std::vector<uint32_t> out;
    AppendSilhouetteIndices(adj, cameraLocal, scratch, out);

    CHECK(ToEdgeSet(out) ==
          ReferenceSilhouettes(verts, idx, transform, cameraWorld, adj));
  }
}

TEST_CASE("Degenerate triangles produce finite normals and static edges",
          "[outline]") {
  // A zero-area triangle sharing an edge with a real one.
  std::vector<glm::vec3> verts = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 0, 0}};
  std::vector<uint32_t> idx = {0, 1, 2,  // real
                               0, 1, 1}; // degenerate (repeated vertex)
  EdgeAdjacency adj = Build(verts, idx);
  for (const glm::vec3& n : adj.triNormal) {
    CHECK(std::isfinite(n.x));
    CHECK(std::isfinite(n.y));
    CHECK(std::isfinite(n.z));
  }
  // No smooth edge may involve the degenerate triangle.
  for (const auto& e : adj.smoothEdges) {
    CHECK(adj.triNormal[e.triA] != glm::vec3(0.0f));
    CHECK(adj.triNormal[e.triB] != glm::vec3(0.0f));
  }
}

TEST_CASE("Empty and trivial meshes don't blow up", "[outline]") {
  EdgeAdjacency adj =
      BuildEdgeAdjacency(nullptr, 0, sizeof(glm::vec3), nullptr, 0);
  CHECK(adj.staticIndices.empty());
  CHECK(adj.smoothEdges.empty());
  CHECK(adj.MaxIndexCount() == 0);

  std::vector<glm::vec3> verts = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  std::vector<uint32_t> idx = {0, 1, 2};
  adj = Build(verts, idx);
  CHECK(ToEdgeSet(adj.staticIndices) ==
        EdgeSet{{0, 1}, {0, 2}, {1, 2}}); // all boundary
  SilhouetteScratch scratch;
  std::vector<uint32_t> out;
  AppendSilhouetteIndices(adj, glm::vec3(0, 0, 5), scratch, out);
  CHECK(out.empty());
}

// P0 baseline: the old per-frame algorithm (map rebuild + per-vertex
// transforms) vs. build-once + flat reclassify, on an assembly-sized scene.
// Hidden by default; run with `designer_tests "[.outline-perf]"`.
TEST_CASE("Silhouette classification perf: old per-frame vs new",
          "[.outline-perf]") {
  constexpr int kMeshes = 212; // the Waveshare repro's solid count
  std::vector<glm::vec3> verts;
  std::vector<uint32_t> idx;
  MakeSphere(verts, idx, 24, 32); // ~1.5k triangles per mesh
  const glm::mat4 transform =
      glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
  const glm::vec3 cameraWorld(8.0f, 4.0f, 6.0f);

  using clock = std::chrono::steady_clock;

  // Old: per frame, per mesh — transform every vertex, rebuild the edge map.
  const auto oldStart = clock::now();
  size_t oldEdges = 0;
  for (int m = 0; m < kMeshes; ++m) {
    std::map<std::pair<uint32_t, uint32_t>,
             std::vector<std::pair<glm::vec3, bool>>>
        edgeToTriangles;
    for (size_t i = 0; i < idx.size(); i += 3) {
      glm::vec3 p0 = glm::vec3(transform * glm::vec4(verts[idx[i]], 1.0f));
      glm::vec3 p1 = glm::vec3(transform * glm::vec4(verts[idx[i + 1]], 1.0f));
      glm::vec3 p2 = glm::vec3(transform * glm::vec4(verts[idx[i + 2]], 1.0f));
      glm::vec3 normal = glm::normalize(glm::cross(p1 - p0, p2 - p0));
      glm::vec3 center = (p0 + p1 + p2) / 3.0f;
      bool isFrontFacing = glm::dot(normal, cameraWorld - center) > 0.0f;
      auto add = [&](uint32_t a, uint32_t b) {
        if (a > b)
          std::swap(a, b);
        edgeToTriangles[{a, b}].push_back({normal, isFrontFacing});
      };
      add(idx[i], idx[i + 1]);
      add(idx[i + 1], idx[i + 2]);
      add(idx[i + 2], idx[i]);
    }
    for (const auto& e : edgeToTriangles) {
      if (e.second.size() == 2 && e.second[0].second != e.second[1].second)
        oldEdges++;
    }
  }
  const double oldMs =
      std::chrono::duration<double, std::milli>(clock::now() - oldStart)
          .count();

  // New: build once (amortized across the session), then flat reclassify.
  const auto buildStart = clock::now();
  EdgeAdjacency adj = Build(verts, idx);
  const double buildMs =
      std::chrono::duration<double, std::milli>(clock::now() - buildStart)
          .count();

  const glm::vec3 cameraLocal =
      glm::vec3(glm::inverse(transform) * glm::vec4(cameraWorld, 1.0f));
  SilhouetteScratch scratch;
  std::vector<uint32_t> out;
  const auto newStart = clock::now();
  size_t newEdges = 0;
  for (int m = 0; m < kMeshes; ++m) {
    out.clear();
    AppendSilhouetteIndices(adj, cameraLocal, scratch, out);
    newEdges += out.size() / 2;
  }
  const double newMs =
      std::chrono::duration<double, std::milli>(clock::now() - newStart)
          .count();

  std::printf("outline-perf: %d meshes x %zu tris\n", kMeshes, idx.size() / 3);
  std::printf("outline-perf: old per-frame pass: %.1f ms (%zu flip edges, "
              "feature+NaN included)\n",
              oldMs, oldEdges);
  std::printf("outline-perf: new build-once: %.2f ms/mesh, reclassify all: "
              "%.2f ms (%zu edges)\n",
              buildMs, newMs, newEdges);

  // The counts differ by design (the old rule also flagged feature edges and
  // edges beside NaN-normal degenerates); correctness parity is covered by
  // the [outline] cases. Here, check the new pass agrees with the reference
  // on the smooth-edge set, and that it is actually faster.
  const size_t expected =
      ReferenceSilhouettes(verts, idx, transform, cameraWorld, adj).size();
  CHECK(newEdges == kMeshes * expected);
  CHECK(newMs < oldMs);
}
