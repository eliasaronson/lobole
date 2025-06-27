#include "Camera.h"
#include <GLFW/glfw3.h>
#include <algorithm>

Camera::Camera() 
    : m_position(5.0f, 5.0f, 5.0f)
    , m_target(0.0f, 0.0f, 0.0f)
    , m_up(0.0f, 1.0f, 0.0f)
    , m_fov(45.0f)
    , m_aspectRatio(16.0f / 9.0f)
    , m_nearPlane(0.1f)
    , m_farPlane(1000.0f)
    , m_lastMouseX(0.0)
    , m_lastMouseY(0.0)
    , m_currentMode(CameraMode::Orbit)
    , m_orbitSensitivity(0.005f)
    , m_panSensitivity(0.01f)
    , m_zoomSensitivity(0.1f)
    , m_minDistance(0.5f)
    , m_maxDistance(100.0f) {
    
    for (int i = 0; i < 3; i++) {
        m_mousePressed[i] = false;
    }
    
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

void Camera::OnMouseMove(double xpos, double ypos) {
    if (m_mousePressed[0] || m_mousePressed[1] || m_mousePressed[2]) {
        double deltaX = xpos - m_lastMouseX;
        double deltaY = ypos - m_lastMouseY;
        
        if (m_mousePressed[0]) {
            HandleOrbitMode(deltaX, deltaY);
        } else if (m_mousePressed[1]) {
            HandlePanMode(deltaX, deltaY);
        } else if (m_mousePressed[2]) {
            HandleZoomMode(deltaY);
        }
    }
    
    m_lastMouseX = xpos;
    m_lastMouseY = ypos;
}

void Camera::OnMouseButton(int button, int action, int mods) {
    if (button >= 0 && button < 3) {
        m_mousePressed[button] = (action == GLFW_PRESS);
    }
}

void Camera::OnScroll(double xoffset, double yoffset) {
    HandleZoomMode(-yoffset * 5.0); // Scale scroll for better feel
}

void Camera::OnKey(int key, int scancode, int action, int mods) {
    // Handle keyboard shortcuts for camera controls
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
}

void Camera::HandleOrbitMode(double deltaX, double deltaY) {
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

void Camera::HandlePanMode(double deltaX, double deltaY) {
    glm::vec3 right = GetRight();
    glm::vec3 up = GetUp();
    
    // Use a fixed pan scale that's proportional to the viewport, not distance
    // This makes panning feel more direct and mouse-like
    float panScale = 0.001f; // Much smaller base scale
    
    // Apply distance scaling more moderately
    float distance = GetDistance();
    panScale *= (distance * 0.1f); // Reduced distance influence
    
    // Fix Y inversion: when mouse moves up (positive deltaY), camera should move down (negative up)
    glm::vec3 offset = right * (float)(-deltaX * panScale) + up * (float)(-deltaY * panScale);
    
    m_position += offset;
    m_target += offset;
    
    UpdateViewMatrix();
}

void Camera::HandleZoomMode(double deltaY) {
    float currentDistance = GetDistance();
    float newDistance = currentDistance * (1.0f + deltaY * m_zoomSensitivity);
    SetDistance(newDistance);
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