#include "FaceSelector.h"
#include "MeshRenderer.h"
#include "Camera.h"
#include <algorithm>
#include <iostream>
#include <limits>

FaceSelector::FaceSelector() {
}

FaceSelector::~FaceSelector() {
    Clear();
}

void FaceSelector::AddMesh(uint32_t meshId, const std::vector<Vertex3D>& vertices, 
                          const std::vector<uint32_t>& indices, const glm::mat4& transform) {
    auto meshData = std::make_unique<MeshData>();
    meshData->meshId = meshId;
    meshData->vertices = vertices;
    meshData->indices = indices;
    meshData->transform = transform;
    
    ExtractFaces(*meshData);
    
    std::cout << "FaceSelector: Adding mesh " << meshId << " with " 
              << indices.size() / 3 << " triangles (" << meshData->faces.size() << " faces)" << std::endl;
    
    // Debug: Show first few faces
    if (meshData->faces.size() > 0) {
        const auto& firstFace = meshData->faces[0];
        std::cout << "  First face - vertices: " << firstFace.vertexIndices[0] << ", " 
                  << firstFace.vertexIndices[1] << ", " << firstFace.vertexIndices[2] << std::endl;
        std::cout << "  First face - center: (" << firstFace.center.x << ", " << firstFace.center.y << ", " << firstFace.center.z << ")" << std::endl;
        std::cout << "  First face - normal: (" << firstFace.normal.x << ", " << firstFace.normal.y << ", " << firstFace.normal.z << ")" << std::endl;
    }
    
    m_meshes.push_back(std::move(meshData));
    
    std::cout << "FaceSelector: Total meshes now: " << m_meshes.size() << std::endl;
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
    
    for (size_t i = 0; i < meshData.indices.size(); i += 3) {
        Face face;
        face.meshId = meshData.meshId;
        face.faceIndex = static_cast<uint32_t>(i / 3);
        face.transform = meshData.transform;
        
        // Get triangle vertex indices
        face.vertexIndices = {
            meshData.indices[i],
            meshData.indices[i + 1], 
            meshData.indices[i + 2]
        };
        
        // Get vertex positions and transform them
        glm::vec3 v0 = glm::vec3(meshData.transform * glm::vec4(
            meshData.vertices[face.vertexIndices[0]].position[0],
            meshData.vertices[face.vertexIndices[0]].position[1],
            meshData.vertices[face.vertexIndices[0]].position[2], 1.0f));
        glm::vec3 v1 = glm::vec3(meshData.transform * glm::vec4(
            meshData.vertices[face.vertexIndices[1]].position[0],
            meshData.vertices[face.vertexIndices[1]].position[1],
            meshData.vertices[face.vertexIndices[1]].position[2], 1.0f));
        glm::vec3 v2 = glm::vec3(meshData.transform * glm::vec4(
            meshData.vertices[face.vertexIndices[2]].position[0],
            meshData.vertices[face.vertexIndices[2]].position[1],
            meshData.vertices[face.vertexIndices[2]].position[2], 1.0f));
        
        // Calculate face normal and center
        face.normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
        face.center = (v0 + v1 + v2) / 3.0f;
        
        meshData.faces.push_back(face);
    }
}

Ray FaceSelector::ScreenToWorldRay(float screenX, float screenY, int screenWidth, int screenHeight, 
                                  const Camera& camera) const {
    // Convert screen coordinates to normalized device coordinates
    float ndcX = (2.0f * screenX) / screenWidth - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY) / screenHeight; // Flip Y axis
    
    // Minimal debug output
    static int rayCount = 0;
    rayCount++;
    if (rayCount % 5 == 0) { // Only show every 5th ray to reduce spam
        std::cout << "Ray: Screen(" << screenX << ", " << screenY << ") -> NDC(" << ndcX << ", " << ndcY << ")" << std::endl;
    }
    
    // Get view and projection matrices
    glm::mat4 view = camera.GetViewMatrix();
    glm::mat4 proj = camera.GetProjectionMatrix();
    
    // Create points in clip space
    glm::vec4 rayStartClip = glm::vec4(ndcX, ndcY, -1.0f, 1.0f); // Near plane
    glm::vec4 rayEndClip = glm::vec4(ndcX, ndcY, 1.0f, 1.0f);    // Far plane
    
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
    
    if (rayCount % 5 == 0) { // Only show every 5th ray
        std::cout << "Ray: Origin(" << ray.origin.x << ", " << ray.origin.y << ", " << ray.origin.z 
                  << "), Dir(" << ray.direction.x << ", " << ray.direction.y << ", " << ray.direction.z << ")" << std::endl;
    }
    
    return ray;
}

RayHit FaceSelector::PickFace(const Ray& ray) const {
    RayHit closestHit;
    closestHit.distance = std::numeric_limits<float>::max();
    
    int totalFacesChecked = 0;
    int totalMeshes = m_meshes.size();
    
    // Reduced debug output for face picking
    static int pickCount = 0;
    pickCount++;
    bool showDebug = (pickCount % 5 == 0); // Only show every 5th pick
    
    if (showDebug) {
        std::cout << "Testing ray against " << totalMeshes << " meshes:" << std::endl;
    }
    
    for (size_t meshIdx = 0; meshIdx < m_meshes.size(); meshIdx++) {
        const auto& mesh = m_meshes[meshIdx];
        totalFacesChecked += mesh->faces.size();
        
        // Test all faces for this mesh
        for (const auto& face : mesh->faces) {
            // Get transformed triangle vertices
            glm::vec3 v0 = glm::vec3(face.transform * glm::vec4(
                mesh->vertices[face.vertexIndices[0]].position[0],
                mesh->vertices[face.vertexIndices[0]].position[1],
                mesh->vertices[face.vertexIndices[0]].position[2], 1.0f));
            glm::vec3 v1 = glm::vec3(face.transform * glm::vec4(
                mesh->vertices[face.vertexIndices[1]].position[0],
                mesh->vertices[face.vertexIndices[1]].position[1],
                mesh->vertices[face.vertexIndices[1]].position[2], 1.0f));
            glm::vec3 v2 = glm::vec3(face.transform * glm::vec4(
                mesh->vertices[face.vertexIndices[2]].position[0],
                mesh->vertices[face.vertexIndices[2]].position[1],
                mesh->vertices[face.vertexIndices[2]].position[2], 1.0f));
            
            float distance;
            glm::vec3 hitPoint;
            if (RayTriangleIntersect(ray, v0, v1, v2, distance, hitPoint)) {
                if (distance < closestHit.distance && distance > 0.0f) {
                    closestHit.hit = true;
                    closestHit.distance = distance;
                    closestHit.point = hitPoint;
                    closestHit.normal = face.normal;
                    closestHit.meshId = face.meshId;
                    closestHit.faceIndex = face.faceIndex;
                }
            }
        }
        
        if (showDebug) {
            std::cout << "  Mesh " << mesh->meshId << ": " << mesh->faces.size() << " faces checked" << std::endl;
        }
    }
    
    // Always show hit/miss results, but only detailed info occasionally
    if (closestHit.hit) {
        std::cout << "HIT: face " << closestHit.faceIndex << " on mesh " << closestHit.meshId 
                  << " (dist: " << closestHit.distance << ")" << std::endl;
    } else if (showDebug) {
        std::cout << "MISS: No face intersection found" << std::endl;
    }
    
    return closestHit;
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