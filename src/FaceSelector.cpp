#include "FaceSelector.h"
#include "MeshRenderer.h"
#include "Camera.h"
#include "Logger.h"
#include <algorithm>
#include <iostream>
#include <limits>

FaceSelector::FaceSelector() {
}

FaceSelector::~FaceSelector() {
    Clear();
}

void FaceSelector::AddMesh(uint32_t meshId, const std::vector<Vertex3D>& vertices,
                          const std::vector<uint32_t>& indices,
                          const std::vector<uint32_t>& triangleFaces,
                          const glm::mat4& transform,
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

    LOG_INFO("FaceSelector: Adding mesh {} with {} triangles ({} faces)",
             meshId, indices.size() / 3, meshData->faces.size());

    m_meshes.push_back(std::move(meshData));
}

void FaceSelector::UpdateMeshTransform(uint32_t meshId, const glm::mat4& transform) {
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
        m_meshes.end()
    );
    
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
    if (meshData.triangleFaces.empty()) return;

    const uint32_t faceCount =
        *std::max_element(meshData.triangleFaces.begin(),
                          meshData.triangleFaces.end()) + 1;
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
        face.normal = (len > 1e-8f) ? weightedNormal[f] / len
                                    : glm::vec3(0, 0, 1);
        face.center = (totalArea[f] > 1e-12f)
                          ? weightedCenter[f] / totalArea[f]
                          : glm::vec3(0.0f);
    }
}

Ray FaceSelector::ScreenToWorldRay(float screenX, float screenY, int screenWidth, int screenHeight,
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
    ray.direction = glm::normalize(glm::vec3(rayEndWorld) - glm::vec3(rayStartWorld));
    
    LOG_TRACE("Ray: Origin({}, {}, {}), Dir({}, {}, {})", 
              ray.origin.x, ray.origin.y, ray.origin.z,
              ray.direction.x, ray.direction.y, ray.direction.z);
    
    return ray;
}

RayHit FaceSelector::PickFace(const Ray& ray) const {
    RayHit closestHit;
    closestHit.distance = std::numeric_limits<float>::max();
    
    int totalFacesChecked = 0;
    int totalMeshes = m_meshes.size();
    
    LOG_TRACE("Testing ray against {} meshes", totalMeshes);
    
    for (size_t meshIdx = 0; meshIdx < m_meshes.size(); meshIdx++) {
        const auto& mesh = m_meshes[meshIdx];
        totalFacesChecked += mesh->faces.size();

        auto transformed = [&](uint32_t idx) {
            const auto& p = mesh->vertices[idx].position;
            return glm::vec3(mesh->transform *
                             glm::vec4(p[0], p[1], p[2], 1.0f));
        };

        // Test every triangle; the hit maps back to its B-Rep face
        for (size_t t = 0; t < mesh->triangleFaces.size(); t++) {
            const glm::vec3 v0 = transformed(mesh->indices[t * 3]);
            const glm::vec3 v1 = transformed(mesh->indices[t * 3 + 1]);
            const glm::vec3 v2 = transformed(mesh->indices[t * 3 + 2]);

            float distance;
            glm::vec3 hitPoint;
            if (RayTriangleIntersect(ray, v0, v1, v2, distance, hitPoint)) {
                if (distance < closestHit.distance && distance > 0.0f) {
                    const uint32_t faceId = mesh->triangleFaces[t];
                    closestHit.hit = true;
                    closestHit.distance = distance;
                    closestHit.point = hitPoint;
                    // Geometric normal of the hit triangle (points outward)
                    closestHit.normal =
                        glm::normalize(glm::cross(v1 - v0, v2 - v0));
                    closestHit.meshId = mesh->meshId;
                    closestHit.faceIndex = faceId;
                }
            }
        }

        LOG_TRACE("  Mesh {}: {} faces checked", mesh->meshId, mesh->faces.size());
    }
    
    // Always show hit/miss results, but only detailed info occasionally
    if (closestHit.hit) {
        LOG_DEBUG("HIT: face {} on mesh {} (dist: {})", 
                  closestHit.faceIndex, closestHit.meshId, closestHit.distance);
    } else {
        LOG_TRACE("MISS: No face intersection found");
    }
    
    return closestHit;
}

EdgeHit FaceSelector::PickEdge(const Ray& ray) const {
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
    const RayHit occluder = PickFace(ray);

    for (const auto& mesh : m_meshes) {
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
                if (t <= 0.0f) continue; // behind the eye
                const float perp = glm::length(onSeg - (ray.origin + t * u));

                const float radius = std::max(kTanPickAngle * t, kMinRadius);
                if (perp > radius) continue;
                // An edge behind the first-hit surface isn't pickable. Edges
                // on that face's own boundary/silhouette sit at the face's
                // depth (within the pick radius), so give proportional slack.
                if (occluder.hit && t > occluder.distance * 1.05f + 1e-3f)
                    continue;
                const float angular = perp / t;
                if (angular < bestAngular) {
                    bestAngular = angular;
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

bool FaceSelector::RayTriangleIntersect(const Ray& ray, const glm::vec3& v0, const glm::vec3& v1,
                                       const glm::vec3& v2, float& distance, glm::vec3& hitPoint) const {
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
    if (!m_hasSelection) return nullptr;
    
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
    if (!m_hasHover) return nullptr;
    
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

glm::vec3 FaceSelector::GetFaceNormal(uint32_t meshId, uint32_t faceIndex) const {
    for (const auto& mesh : m_meshes) {
        if (mesh->meshId == meshId && faceIndex < mesh->faces.size()) {
            return mesh->faces[faceIndex].normal;
        }
    }
    return glm::vec3(0.0f, 0.0f, 1.0f); // Default normal
}

glm::vec3 FaceSelector::GetFaceCenter(uint32_t meshId, uint32_t faceIndex) const {
    for (const auto& mesh : m_meshes) {
        if (mesh->meshId == meshId && faceIndex < mesh->faces.size()) {
            return mesh->faces[faceIndex].center;
        }
    }
    return glm::vec3(0.0f); // Default center
}