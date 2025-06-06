#include "SketchManager.h"
#include <glm/gtc/matrix_transform.hpp>

SketchManager::SketchManager() : m_mode(SketchManagerMode::ThreeD) {
    InitializeDefaultPlanes();
}

void SketchManager::InitializeDefaultPlanes() {
    m_planes.clear();
    
    // Create the three default planes
    m_planes.push_back(DefaultPlanes::CreateFrontPlane());
    m_planes.push_back(DefaultPlanes::CreateTopPlane());
    m_planes.push_back(DefaultPlanes::CreateRightPlane());
}

bool SketchManager::StartSketch(std::shared_ptr<SketchPlane> plane) {
    if (!plane) return false;
    
    // Set mode to sketching
    m_mode = SketchManagerMode::Sketch;
    m_activePlane = plane;
    m_activePlane->SetActive(true);
    
    // Create new sketch on this plane
    m_activeSketch = std::make_shared<Sketch>(plane, "Sketch " + std::to_string(m_planes.size()));
    m_activeSketch->SetActive(true);
    
    return true;
}

bool SketchManager::StartSketch(PlaneType planeType) {
    // Find the plane of the requested type
    for (auto& plane : m_planes) {
        if (plane->GetType() == planeType) {
            return StartSketch(plane);
        }
    }
    return false;
}

void SketchManager::ExitSketch() {
    m_mode = SketchManagerMode::ThreeD;
    
    if (m_activePlane) {
        m_activePlane->SetActive(false);
        m_activePlane.reset();
    }
    
    if (m_activeSketch) {
        m_activeSketch->SetActive(false);
        // Note: Keep the sketch in memory for potential extrude/cut operations
        // In a full implementation, you'd store completed sketches somewhere
    }
}

void SketchManager::SetSketchMode(SketchMode mode) {
    if (m_activeSketch) {
        m_activeSketch->SetMode(mode);
    }
}

SketchMode SketchManager::GetSketchMode() const {
    if (m_activeSketch) {
        return m_activeSketch->GetMode();
    }
    return SketchMode::Select;
}

void SketchManager::OnMouseClick(double x, double y, const Camera& camera) {
    if (!IsSketchMode() || !m_activeSketch || !m_activePlane) return;
    
    // Convert screen coordinates to world ray
    glm::vec3 rayOrigin, rayDirection;
    ScreenToWorldRay(x, y, camera, rayOrigin, rayDirection);
    
    // Find intersection with active plane
    glm::vec3 worldPos = RayPlaneIntersection(rayOrigin, rayDirection, m_activePlane);
    
    // Convert world position to plane coordinates
    glm::vec2 planePos = m_activePlane->WorldToPlane(worldPos);
    
    // Forward to sketch
    m_activeSketch->OnMouseClick(planePos);
}

void SketchManager::OnMouseMove(double x, double y, const Camera& camera) {
    if (!IsSketchMode() || !m_activeSketch || !m_activePlane) return;
    
    // Convert screen coordinates to world ray
    glm::vec3 rayOrigin, rayDirection;
    ScreenToWorldRay(x, y, camera, rayOrigin, rayDirection);
    
    // Find intersection with active plane
    glm::vec3 worldPos = RayPlaneIntersection(rayOrigin, rayDirection, m_activePlane);
    
    // Convert world position to plane coordinates
    glm::vec2 planePos = m_activePlane->WorldToPlane(worldPos);
    
    // Forward to sketch
    m_activeSketch->OnMouseMove(planePos);
}

void SketchManager::OnMouseRelease(double x, double y, const Camera& camera) {
    if (!IsSketchMode() || !m_activeSketch || !m_activePlane) return;
    
    // Convert screen coordinates to world ray
    glm::vec3 rayOrigin, rayDirection;
    ScreenToWorldRay(x, y, camera, rayOrigin, rayDirection);
    
    // Find intersection with active plane
    glm::vec3 worldPos = RayPlaneIntersection(rayOrigin, rayDirection, m_activePlane);
    
    // Convert world position to plane coordinates
    glm::vec2 planePos = m_activePlane->WorldToPlane(worldPos);
    
    // Forward to sketch
    m_activeSketch->OnMouseRelease(planePos);
}

glm::vec3 SketchManager::RayPlaneIntersection(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, 
                                            std::shared_ptr<SketchPlane> plane) const {
    // Ray-plane intersection
    // P = O + t * D, where P is on the plane
    // dot(P - PlaneOrigin, PlaneNormal) = 0
    // Solve for t: t = dot(PlaneOrigin - O, PlaneNormal) / dot(D, PlaneNormal)
    
    glm::vec3 planeOrigin = plane->GetOrigin();
    glm::vec3 planeNormal = plane->GetNormal();
    
    float denominator = glm::dot(rayDirection, planeNormal);
    
    // Check if ray is parallel to plane
    if (abs(denominator) < 1e-6f) {
        // Return a default point on the plane
        return planeOrigin;
    }
    
    float t = glm::dot(planeOrigin - rayOrigin, planeNormal) / denominator;
    
    return rayOrigin + t * rayDirection;
}

void SketchManager::ScreenToWorldRay(double screenX, double screenY, const Camera& camera,
                                    glm::vec3& rayOrigin, glm::vec3& rayDirection) const {
    // Convert screen coordinates to normalized device coordinates
    float x = (2.0f * screenX) / m_viewportWidth - 1.0f;
    float y = 1.0f - (2.0f * screenY) / m_viewportHeight;
    
    // Get camera matrices
    glm::mat4 viewMatrix = camera.GetViewMatrix();
    glm::mat4 projMatrix = camera.GetProjectionMatrix();
    
    // Calculate ray in world space
    glm::mat4 invViewProj = glm::inverse(projMatrix * viewMatrix);
    
    glm::vec4 rayStart = invViewProj * glm::vec4(x, y, -1.0f, 1.0f);
    glm::vec4 rayEnd = invViewProj * glm::vec4(x, y, 1.0f, 1.0f);
    
    rayStart /= rayStart.w;
    rayEnd /= rayEnd.w;
    
    rayOrigin = glm::vec3(rayStart);
    rayDirection = glm::normalize(glm::vec3(rayEnd - rayStart));
}

bool SketchManager::CanExtrude() const {
    return m_activeSketch && m_activeSketch->IsClosed();
}

bool SketchManager::CanCut() const {
    return m_activeSketch && m_activeSketch->IsClosed();
}