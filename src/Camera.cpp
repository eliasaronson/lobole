#include "Camera.h"
#include <algorithm>

Camera::Camera() 
    : m_position(5.0f, 5.0f, 5.0f)
    , m_target(0.0f, 0.0f, 0.0f)
    , m_up(0.0f, 1.0f, 0.0f)
    , m_fov(45.0f)
    , m_aspectRatio(16.0f / 9.0f)
    , m_nearPlane(0.1f)
    , m_farPlane(1000.0f)
    , m_orbitSensitivity(0.005f)
    , m_panSensitivity(0.01f)
    , m_zoomSensitivity(0.1f)
    , m_minDistance(0.5f)
    , m_maxDistance(100.0f) {

    UpdateViewMatrix();
    UpdateProjectionMatrix();
}

void Camera::Update(float deltaTime) {
    // Handle animation
    if (m_isAnimating) {
        m_animationTime += deltaTime;
        float t = std::min(m_animationTime / m_animationDuration, 1.0f);
        
        // Smooth interpolation (ease-in-out)
        t = t * t * (3.0f - 2.0f * t);
        
        m_position = glm::mix(m_animationStartPos, m_animationEndPos, t);
        m_target = glm::mix(m_animationStartTarget, m_animationEndTarget, t);
        
        UpdateViewMatrix();
        
        if (t >= 1.0f) {
            m_isAnimating = false;
        }
    }
}

void Camera::SetDistance(float distance) {
    distance = std::clamp(distance, m_minDistance, m_maxDistance);
    glm::vec3 direction = glm::normalize(m_position - m_target);
    m_position = m_target + direction * distance;
    UpdateViewMatrix();
}

void Camera::UpdateViewMatrix() {
    m_viewMatrix = glm::lookAt(m_position, m_target, m_up);
}

void Camera::UpdateProjectionMatrix() {
    m_projectionMatrix = glm::perspective(glm::radians(m_fov), m_aspectRatio, m_nearPlane, m_farPlane);
    // glm produces OpenGL clip space; Vulkan's NDC y points down. Flipping
    // here keeps every consumer (renderers AND the picking ray) consistent.
    m_projectionMatrix[1][1] *= -1.0f;
}

void Camera::Orbit(double deltaX, double deltaY) {
    glm::vec3 toCamera = m_position - m_target;
    float distance = glm::length(toCamera);
    
    // Convert to spherical coordinates
    float theta = atan2(toCamera.z, toCamera.x);
    float phi = acos(toCamera.y / distance);
    
    // Apply rotation
    theta -= deltaX * m_orbitSensitivity;
    phi -= deltaY * m_orbitSensitivity;
    
    // Clamp phi to avoid flipping
    phi = std::clamp(phi, 0.01f, 3.14f - 0.01f);
    
    // Convert back to cartesian
    m_position.x = m_target.x + distance * sin(phi) * cos(theta);
    m_position.y = m_target.y + distance * cos(phi);
    m_position.z = m_target.z + distance * sin(phi) * sin(theta);
    
    UpdateViewMatrix();
}

void Camera::Pan(double deltaX, double deltaY) {
    glm::vec3 right = GetRight();
    glm::vec3 up = GetUp();
    
    // Use a more responsive pan scale
    float panScale = 0.005f; // Increased from 0.001f for better responsiveness
    
    // Apply distance scaling more moderately
    float distance = GetDistance();
    panScale *= (distance * 0.2f); // Slightly increased distance influence
    
    // Fix Y inversion: when mouse moves up (positive deltaY), camera should move down (negative up)
    glm::vec3 offset = right * (float)(-deltaX * panScale) + up * (float)(-deltaY * panScale);
    
    m_position += offset;
    m_target += offset;
    
    UpdateViewMatrix();
}

void Camera::Zoom(double deltaY) {
    float currentDistance = GetDistance();
    float newDistance = currentDistance * (1.0f + deltaY * m_zoomSensitivity);
    SetDistance(newDistance);
}

void Camera::Roll(double delta) {
    const float angle = static_cast<float>(delta) * 0.01f;
    glm::mat4 rot = glm::rotate(glm::mat4(1.0f), angle, GetForward());
    m_up = glm::normalize(glm::vec3(rot * glm::vec4(m_up, 0.0f)));
    UpdateViewMatrix();
}

void Camera::AlignToFace(const glm::vec3& faceCenter, const glm::vec3& faceNormal, float distance) {
    // Calculate the camera position to look at the face perpendicularly
    glm::vec3 newTarget = faceCenter;
    glm::vec3 newPosition = faceCenter + glm::normalize(faceNormal) * distance;
    
    // Use world up as our up vector, but adjust if face normal is too close to up
    glm::vec3 worldUp = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 newUp = worldUp;
    
    // If face normal is nearly parallel to world up, use a different up vector
    if (glm::abs(glm::dot(faceNormal, worldUp)) > 0.95f) {
        newUp = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    
    AnimateToPosition(newPosition, newTarget, 1.0f);
}

void Camera::AnimateToPosition(const glm::vec3& position, const glm::vec3& target, float duration) {
    m_animationStartPos = m_position;
    m_animationStartTarget = m_target;
    m_animationEndPos = position;
    m_animationEndTarget = target;
    m_animationTime = 0.0f;
    m_animationDuration = duration;
    m_isAnimating = true;
}