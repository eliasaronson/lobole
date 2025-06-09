#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>

struct Vertex3D;
class Camera;

struct Face {
    uint32_t meshId;
    uint32_t faceIndex;
    std::vector<uint32_t> vertexIndices; // Triangle indices (3 vertices)
    glm::vec3 normal;
    glm::vec3 center;
    glm::mat4 transform = glm::mat4(1.0f);
};

struct Ray {
    glm::vec3 origin;
    glm::vec3 direction;
};

struct RayHit {
    bool hit = false;
    float distance = 0.0f;
    glm::vec3 point;
    glm::vec3 normal;
    uint32_t meshId = 0;
    uint32_t faceIndex = 0;
};

class FaceSelector {
public:
    FaceSelector();
    ~FaceSelector();

    // Mesh management
    void AddMesh(uint32_t meshId, const std::vector<Vertex3D>& vertices, 
                 const std::vector<uint32_t>& indices, const glm::mat4& transform = glm::mat4(1.0f));
    void UpdateMeshTransform(uint32_t meshId, const glm::mat4& transform);
    void RemoveMesh(uint32_t meshId);
    void Clear();

    // Selection
    RayHit PickFace(const Ray& ray) const;
    Ray ScreenToWorldRay(float screenX, float screenY, int screenWidth, int screenHeight, 
                        const Camera& camera) const;
    
    // Face access
    const Face* GetSelectedFace() const;
    const Face* GetHoveredFace() const;
    void SetSelectedFace(uint32_t meshId, uint32_t faceIndex);
    void SetHoveredFace(uint32_t meshId, uint32_t faceIndex);
    void ClearSelection();
    void ClearHover();

    // Utility
    glm::vec3 GetFaceNormal(uint32_t meshId, uint32_t faceIndex) const;
    glm::vec3 GetFaceCenter(uint32_t meshId, uint32_t faceIndex) const;

private:
    struct MeshData {
        uint32_t meshId;
        std::vector<Vertex3D> vertices;
        std::vector<uint32_t> indices;
        std::vector<Face> faces;
        glm::mat4 transform = glm::mat4(1.0f);
    };

    // Ray-triangle intersection
    bool RayTriangleIntersect(const Ray& ray, const glm::vec3& v0, const glm::vec3& v1, 
                             const glm::vec3& v2, float& distance, glm::vec3& hitPoint) const;
    
    // Face extraction
    void ExtractFaces(MeshData& meshData);
    
    std::vector<std::unique_ptr<MeshData>> m_meshes;
    
    // Current selection state
    uint32_t m_selectedMeshId = 0;
    uint32_t m_selectedFaceIndex = 0;
    bool m_hasSelection = false;
    
    uint32_t m_hoveredMeshId = 0;
    uint32_t m_hoveredFaceIndex = 0;
    bool m_hasHover = false;
};