#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "render/Camera.h"
#include "render/MeshRenderer.h"
#include <algorithm>
#include <iostream>
#include <limits>

FaceSelector::FaceSelector() {}

FaceSelector::~FaceSelector() { Clear(); }

void FaceSelector::AddMesh(
    uint32_t meshId, const std::vector<Vertex3D>& vertices,
    const std::vector<uint32_t>& indices,
    const std::vector<uint32_t>& triangleFaces, const glm::mat4& transform,
    const std::vector<std::vector<glm::vec3>>& edgePolylines) {
  auto meshData = std::make_unique<MeshData>();
  meshData->meshId = meshId;
  meshData->vertices = vertices;
  meshData->indices = indices;
  meshData->edgePolylines = edgePolylines;
  meshData->transform = transform;

  // Without face info, treat each triangle as its own face
  if (triangleFaces.size() == indices.size() / 3) {
    meshData->triangleFaces = triangleFaces;
  } else {
    meshData->triangleFaces.resize(indices.size() / 3);
    for (size_t t = 0; t < meshData->triangleFaces.size(); t++) {
      meshData->triangleFaces[t] = static_cast<uint32_t>(t);
    }
  }

  ExtractFaces(*meshData);

  LOG_INFO("FaceSelector: Adding mesh {} with {} triangles ({} faces)", meshId,
           indices.size() / 3, meshData->faces.size());

  m_meshes.push_back(std::move(meshData));
}

void FaceSelector::UpdateMeshTransform(uint32_t meshId,
                                       const glm::mat4& transform) {
  for (auto& mesh : m_meshes) {
    if (mesh->meshId == meshId) {
      mesh->transform = transform;
      // Re-extract faces to update transformed positions
      ExtractFaces(*mesh);
      break;
    }
  }
}

void FaceSelector::RemoveMesh(uint32_t meshId) {
  m_meshes.erase(
      std::remove_if(m_meshes.begin(), m_meshes.end(),
                     [meshId](const std::unique_ptr<MeshData>& mesh) {
                       return mesh->meshId == meshId;
                     }),
      m_meshes.end());

  // Clear selection if it was on the removed mesh
  if (m_hasSelection && m_selectedMeshId == meshId) {
    ClearSelection();
  }
  if (m_hasHover && m_hoveredMeshId == meshId) {
    ClearHover();
  }
}

void FaceSelector::Clear() {
  m_meshes.clear();
  ClearSelection();
  ClearHover();
}

void FaceSelector::ExtractFaces(MeshData& meshData) {
  meshData.faces.clear();
  // The placement-derived values first, so they are right even for a mesh
  // with no triangles to walk below.
  meshData.invTransform = glm::inverse(meshData.transform);
  const glm::mat3 basis(meshData.transform);
  const float det = glm::determinant(basis);
  meshData.normalToWorld = glm::transpose(glm::mat3(meshData.invTransform)) *
                           (det < 0.0f ? -1.0f : 1.0f);
  meshData.hasBounds = false;
  meshData.boundsMin = glm::vec3(0.0f);
  meshData.boundsMax = glm::vec3(0.0f);

  // The box has to cover everything PICKABLE, which is triangles AND edge
  // polylines. A mesh can carry edges and no triangles -- the sketch and
  // datum meshes do -- and bounding only the triangles gave those an empty
  // box, which a prune reads as "nothing here" and skips: every edge of
  // every such mesh became unpickable.
  const auto cover = [&meshData](const glm::vec3& p) {
    if (!meshData.hasBounds) {
      meshData.boundsMin = meshData.boundsMax = p;
      meshData.hasBounds = true;
    } else {
      meshData.boundsMin = glm::min(meshData.boundsMin, p);
      meshData.boundsMax = glm::max(meshData.boundsMax, p);
    }
  };
  for (const auto& poly : meshData.edgePolylines)
    for (const glm::vec3& p : poly)
      cover(glm::vec3(meshData.transform * glm::vec4(p, 1.0f)));

  if (meshData.triangleFaces.empty())
    return;

  const uint32_t faceCount = *std::max_element(meshData.triangleFaces.begin(),
                                               meshData.triangleFaces.end()) +
                             1;
  meshData.faces.resize(faceCount);

  std::vector<glm::vec3> weightedNormal(faceCount, glm::vec3(0.0f));
  std::vector<glm::vec3> weightedCenter(faceCount, glm::vec3(0.0f));
  std::vector<float> totalArea(faceCount, 0.0f);
  std::vector<uint32_t> minVertex(faceCount, UINT32_MAX);
  std::vector<uint32_t> maxVertex(faceCount, 0);

  auto transformed = [&](uint32_t idx) {
    const auto& p = meshData.vertices[idx].position;
    return glm::vec3(meshData.transform * glm::vec4(p[0], p[1], p[2], 1.0f));
  };

  for (size_t t = 0; t < meshData.triangleFaces.size(); t++) {
    const uint32_t faceId = meshData.triangleFaces[t];
    const uint32_t i0 = meshData.indices[t * 3];
    const uint32_t i1 = meshData.indices[t * 3 + 1];
    const uint32_t i2 = meshData.indices[t * 3 + 2];

    const glm::vec3 v0 = transformed(i0);
    const glm::vec3 v1 = transformed(i1);
    const glm::vec3 v2 = transformed(i2);

    // The world AABB rides along with a walk that already visits every
    // triangle in world space, so it cannot disagree with the geometry a
    // pick will test — and UpdateMeshTransform re-runs this, so it cannot
    // outlive a placement either.
    cover(v0);
    cover(v1);
    cover(v2);

    const glm::vec3 cross = glm::cross(v1 - v0, v2 - v0);
    const float area = 0.5f * glm::length(cross);
    weightedNormal[faceId] += cross * 0.5f; // area-weighted normal
    weightedCenter[faceId] += area * (v0 + v1 + v2) / 3.0f;
    totalArea[faceId] += area;

    minVertex[faceId] = std::min({minVertex[faceId], i0, i1, i2});
    maxVertex[faceId] = std::max({maxVertex[faceId], i0, i1, i2});
  }

  for (uint32_t f = 0; f < faceCount; f++) {
    Face& face = meshData.faces[f];
    face.meshId = meshData.meshId;
    face.faceIndex = f;
    if (minVertex[f] != UINT32_MAX) {
      face.firstVertex = minVertex[f];
      face.vertexCount = maxVertex[f] - minVertex[f] + 1;
    }
    const float len = glm::length(weightedNormal[f]);
    face.normal = (len > 1e-8f) ? weightedNormal[f] / len : glm::vec3(0, 0, 1);
    face.center = (totalArea[f] > 1e-12f) ? weightedCenter[f] / totalArea[f]
                                          : glm::vec3(0.0f);
  }
}

Ray FaceSelector::ScreenToWorldRay(float screenX, float screenY,
                                   int screenWidth, int screenHeight,
                                   const Camera& camera) const {
  // Vulkan conventions, matching Camera's projection exactly: NDC y points
  // down (the projection carries the Y-flip) and depth runs 0 (near) to 1.
  float ndcX = (2.0f * screenX) / screenWidth - 1.0f;
  float ndcY = (2.0f * screenY) / screenHeight - 1.0f;

  LOG_TRACE("Ray: Screen({}, {}) -> NDC({}, {})", screenX, screenY, ndcX, ndcY);

  // Get view and projection matrices
  glm::mat4 view = camera.GetViewMatrix();
  glm::mat4 proj = camera.GetProjectionMatrix();

  // Create points in clip space
  glm::vec4 rayStartClip = glm::vec4(ndcX, ndcY, 0.0f, 1.0f); // Near plane
  glm::vec4 rayEndClip = glm::vec4(ndcX, ndcY, 1.0f, 1.0f);   // Far plane

  // Transform to world space
  glm::mat4 invViewProj = glm::inverse(proj * view);
  glm::vec4 rayStartWorld = invViewProj * rayStartClip;
  glm::vec4 rayEndWorld = invViewProj * rayEndClip;

  // Perspective divide
  rayStartWorld /= rayStartWorld.w;
  rayEndWorld /= rayEndWorld.w;

  Ray ray;
  ray.origin = glm::vec3(rayStartWorld);
  ray.direction =
      glm::normalize(glm::vec3(rayEndWorld) - glm::vec3(rayStartWorld));

  LOG_TRACE("Ray: Origin({}, {}, {}), Dir({}, {}, {})", ray.origin.x,
            ray.origin.y, ray.origin.z, ray.direction.x, ray.direction.y,
            ray.direction.z);

  return ray;
}

// Slab test against a world-space AABB. Callers check `hasBounds` first: a
// mesh with no triangles has nothing to hit, and an empty box must read as a
// MISS rather than as everywhere.
bool FaceSelector::RayHitsBounds(const Ray& ray, const glm::vec3& boundsMin,
                                 const glm::vec3& boundsMax) {
  float tMin = 0.0f;
  float tMax = std::numeric_limits<float>::max();
  for (int a = 0; a < 3; a++) {
    const float d = ray.direction[a];
    if (std::abs(d) < 1e-12f) {
      // Parallel to this pair of planes: inside them, or nowhere.
      if (ray.origin[a] < boundsMin[a] || ray.origin[a] > boundsMax[a])
        return false;
      continue;
    }
    const float inv = 1.0f / d;
    float t0 = (boundsMin[a] - ray.origin[a]) * inv;
    float t1 = (boundsMax[a] - ray.origin[a]) * inv;
    if (t0 > t1)
      std::swap(t0, t1);
    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMin > tMax)
      return false;
  }
  return true;
}

RayHit FaceSelector::PickFace(const Ray& ray) const {
  RayHit closestHit;
  closestHit.distance = std::numeric_limits<float>::max();

  LOG_TRACE("Testing ray against {} meshes", m_meshes.size());

  for (const auto& mesh : m_meshes) {
    // The box first. On an assembly the ray crosses one or two parts and
    // misses the rest, and a MISS used to cost exactly as much as a hit —
    // every triangle of every occurrence, whether the ray came near it or
    // not. On a 241-occurrence engine that was 42% of the app's CPU, every
    // frame the cursor was over the viewport
    // (docs/performance/hover-pick.md).
    if (!mesh->hasBounds ||
        !RayHitsBounds(ray, mesh->boundsMin, mesh->boundsMax))
      continue;

    // The ray goes into MESH space; the vertices stay where they are. The
    // local direction is deliberately NOT renormalised, so `t` remains the
    // parameter along the WORLD ray — distances stay comparable between
    // meshes of different scale, and the hit point comes back exactly.
    Ray local;
    local.origin = glm::vec3(mesh->invTransform * glm::vec4(ray.origin, 1.0f));
    local.direction = glm::mat3(mesh->invTransform) * ray.direction;

    // Test every triangle; the hit maps back to its B-Rep face
    for (size_t t = 0; t < mesh->triangleFaces.size(); t++) {
      const glm::vec3& v0 = mesh->vertices[mesh->indices[t * 3]].position;
      const glm::vec3& v1 = mesh->vertices[mesh->indices[t * 3 + 1]].position;
      const glm::vec3& v2 = mesh->vertices[mesh->indices[t * 3 + 2]].position;

      float distance;
      glm::vec3 hitPoint;
      if (RayTriangleIntersect(local, v0, v1, v2, distance, hitPoint)) {
        if (distance < closestHit.distance && distance > 0.0f) {
          const uint32_t faceId = mesh->triangleFaces[t];
          closestHit.hit = true;
          closestHit.distance = distance;
          // From the WORLD ray at the same parameter, not by transforming
          // the local point back: same answer, one fewer matrix.
          closestHit.point = ray.origin + ray.direction * distance;
          // Geometric normal of the hit triangle (points outward)
          closestHit.normal = glm::normalize(mesh->normalToWorld *
                                             glm::cross(v1 - v0, v2 - v0));
          closestHit.meshId = mesh->meshId;
          closestHit.faceIndex = faceId;
        }
      }
    }

    LOG_TRACE("  Mesh {}: {} faces checked", mesh->meshId, mesh->faces.size());
  }

  // Always show hit/miss results, but only detailed info occasionally
  if (closestHit.hit) {
    LOG_DEBUG("HIT: face {} on mesh {} (dist: {})", closestHit.faceIndex,
              closestHit.meshId, closestHit.distance);
  } else {
    LOG_TRACE("MISS: No face intersection found");
  }

  return closestHit;
}

EdgeHit FaceSelector::PickEdge(const Ray& ray) const {
  return PickEdge(ray, PickFace(ray));
}

EdgeHit FaceSelector::PickEdge(const Ray& ray, const RayHit& occluder) const {
  // Pick radius as an angle off the ray (~0.8° ≈ a handful of pixels at a
  // typical FOV), so the tolerance tracks zoom without needing the camera.
  constexpr float kTanPickAngle = 0.014f;
  constexpr float kMinRadius = 1e-4f; // absolute floor very close to the eye

  EdgeHit best;
  float bestAngular = std::numeric_limits<float>::max();

  // Occlusion reference: the surface the same ray hits first. Checked per
  // candidate inside the loop — an occluded edge can be the *angular* best
  // (e.g. a view down a box diagonal lines up the far corner's edges with
  // the near corner's) and must lose to a visible one, not veto the pick.
  // This is a depth tolerance, not a screen-pick tolerance: scaling it by
  // camera distance admits visibly hidden edges on ordinary-sized parts.
  constexpr float kOcclusionDepthEpsilon = 1e-2f; // mm, tessellation slack

  for (const auto& mesh : m_meshes) {
    // The edges of a mesh the ray cannot reach cannot be picked either. The
    // box is grown by the pick radius at the mesh's own depth, because an
    // edge is pickable from slightly OUTSIDE the surface it bounds — a
    // silhouette edge is exactly on the box, and clipping it to the box
    // would make the outline of every part unclickable.
    if (!mesh->hasBounds)
      continue;
    {
      const glm::vec3 c = 0.5f * (mesh->boundsMin + mesh->boundsMax);
      const float depth = glm::dot(c - ray.origin, ray.direction);
      const float pad =
          std::max(kTanPickAngle * std::max(depth, 0.0f), kMinRadius);
      if (!RayHitsBounds(ray, mesh->boundsMin - glm::vec3(pad),
                         mesh->boundsMax + glm::vec3(pad)))
        continue;
    }
    for (size_t e = 0; e < mesh->edgePolylines.size(); e++) {
      const auto& poly = mesh->edgePolylines[e];
      for (size_t i = 0; i + 1 < poly.size(); i++) {
        const glm::vec3 p0 =
            glm::vec3(mesh->transform * glm::vec4(poly[i], 1.0f));
        const glm::vec3 p1 =
            glm::vec3(mesh->transform * glm::vec4(poly[i + 1], 1.0f));

        // Closest approach between the ray (o + t·d, t ≥ 0) and the
        // segment p0 + s·(p1 − p0), s ∈ [0,1].
        const glm::vec3 u = ray.direction; // unit
        const glm::vec3 v = p1 - p0;
        const glm::vec3 w = ray.origin - p0;
        const float b = glm::dot(u, v);
        const float c = glm::dot(v, v);
        const float d = glm::dot(u, w);
        const float ee = glm::dot(v, w);
        const float denom = c - b * b; // ≥ 0; 0 when parallel
        float s = (denom > 1e-12f) ? (ee - b * d) / denom : 0.0f;
        s = glm::clamp(s, 0.0f, 1.0f);
        const glm::vec3 onSeg = p0 + s * v;
        const float t = glm::dot(onSeg - ray.origin, u);
        if (t <= 0.0f)
          continue; // behind the eye
        const float perp = glm::length(onSeg - (ray.origin + t * u));

        const float radius = std::max(kTanPickAngle * t, kMinRadius);
        if (perp > radius)
          continue;
        // An edge behind the first-hit surface isn't pickable. Edges
        // on its boundary/silhouette have the same ray depth apart
        // from tessellation noise, so only a small absolute slack is
        // appropriate. The former 5%-of-camera-distance allowance
        // made hidden, differently oriented edges win a click.
        if (occluder.hit && t > occluder.distance + kOcclusionDepthEpsilon)
          continue;
        const float angular = perp / t;
        // Ties go to the NEAREST, and the tie case is not exotic: looking
        // straight down a bore, the ray is COLLINEAR with every rim in the
        // stack, so `perp` is zero for all of them and every angular error is
        // zero. A strictly-less comparison then leaves the winner to mesh
        // registration order, and it handed back the bottom rim of the lowest
        // plate — 20 mm behind the cursor, with nothing occluding it because
        // you are looking through the hole. A fastener derived from that goes
        // in from the wrong side and looks plausible.
        //
        // One hundredth of the pick radius: well under a pixel, so this only
        // fires between edges nobody could aim between, and no pick that was
        // already decided changes.
        constexpr float kAngularTie = kTanPickAngle * 0.01f;
        const bool first = !best.hit;
        const bool better = angular < bestAngular - kAngularTie;
        const bool nearerTie =
            !first && angular < bestAngular + kAngularTie && t < best.distance;
        if (first || better || nearerTie) {
          // The bar stays the BEST angle seen, not the accepted one, so a run
          // of nearer-but-slightly-worse candidates cannot walk it outward.
          bestAngular = std::min(bestAngular, angular);
          best.hit = true;
          best.distance = t;
          best.point = onSeg;
          best.meshId = mesh->meshId;
          best.edgeIndex = static_cast<uint32_t>(e);
        }
      }
    }
  }

  return best;
}

bool FaceSelector::RayTriangleIntersect(const Ray& ray, const glm::vec3& v0,
                                        const glm::vec3& v1,
                                        const glm::vec3& v2, float& distance,
                                        glm::vec3& hitPoint) const {
  const float EPSILON = 0.0000001f;

  glm::vec3 edge1 = v1 - v0;
  glm::vec3 edge2 = v2 - v0;
  glm::vec3 h = glm::cross(ray.direction, edge2);
  float a = glm::dot(edge1, h);

  if (a > -EPSILON && a < EPSILON) {
    return false; // Ray is parallel to triangle
  }

  float f = 1.0f / a;
  glm::vec3 s = ray.origin - v0;
  float u = f * glm::dot(s, h);

  if (u < 0.0f || u > 1.0f) {
    return false;
  }

  glm::vec3 q = glm::cross(s, edge1);
  float v = f * glm::dot(ray.direction, q);

  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }

  float t = f * glm::dot(edge2, q);

  if (t > EPSILON) {
    distance = t;
    hitPoint = ray.origin + ray.direction * t;
    return true;
  }

  return false; // Line intersection but not ray intersection
}

const Face* FaceSelector::GetSelectedFace() const {
  if (!m_hasSelection)
    return nullptr;

  for (const auto& mesh : m_meshes) {
    if (mesh->meshId == m_selectedMeshId) {
      if (m_selectedFaceIndex < mesh->faces.size()) {
        return &mesh->faces[m_selectedFaceIndex];
      }
    }
  }
  return nullptr;
}

const Face* FaceSelector::GetHoveredFace() const {
  if (!m_hasHover)
    return nullptr;

  for (const auto& mesh : m_meshes) {
    if (mesh->meshId == m_hoveredMeshId) {
      if (m_hoveredFaceIndex < mesh->faces.size()) {
        return &mesh->faces[m_hoveredFaceIndex];
      }
    }
  }
  return nullptr;
}

void FaceSelector::SetSelectedFace(uint32_t meshId, uint32_t faceIndex) {
  m_selectedMeshId = meshId;
  m_selectedFaceIndex = faceIndex;
  m_hasSelection = true;
}

void FaceSelector::SetHoveredFace(uint32_t meshId, uint32_t faceIndex) {
  m_hoveredMeshId = meshId;
  m_hoveredFaceIndex = faceIndex;
  m_hasHover = true;
}

void FaceSelector::ClearSelection() {
  m_hasSelection = false;
  m_selectedMeshId = 0;
  m_selectedFaceIndex = 0;
}

void FaceSelector::ClearHover() {
  m_hasHover = false;
  m_hoveredMeshId = 0;
  m_hoveredFaceIndex = 0;
}

bool FaceSelector::GetFaceVertexRange(uint32_t meshId, uint32_t faceIndex,
                                      uint32_t& firstVertex,
                                      uint32_t& vertexCount) const {
  for (const auto& mesh : m_meshes) {
    if (mesh->meshId == meshId && faceIndex < mesh->faces.size()) {
      firstVertex = mesh->faces[faceIndex].firstVertex;
      vertexCount = mesh->faces[faceIndex].vertexCount;
      return vertexCount > 0;
    }
  }
  return false;
}

glm::vec3 FaceSelector::GetFaceNormal(uint32_t meshId,
                                      uint32_t faceIndex) const {
  for (const auto& mesh : m_meshes) {
    if (mesh->meshId == meshId && faceIndex < mesh->faces.size()) {
      return mesh->faces[faceIndex].normal;
    }
  }
  return glm::vec3(0.0f, 0.0f, 1.0f); // Default normal
}

glm::vec3 FaceSelector::GetFaceCenter(uint32_t meshId,
                                      uint32_t faceIndex) const {
  for (const auto& mesh : m_meshes) {
    if (mesh->meshId == meshId && faceIndex < mesh->faces.size()) {
      return mesh->faces[faceIndex].center;
    }
  }
  return glm::vec3(0.0f); // Default center
}