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
    // Update camera state if needed
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
    phi += deltaY * m_orbitSensitivity;
    
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
    
    float distance = GetDistance();
    float panScale = distance * m_panSensitivity;
    
    glm::vec3 offset = right * (float)(-deltaX * panScale) + up * (float)(deltaY * panScale);
    
    m_position += offset;
    m_target += offset;
    
    UpdateViewMatrix();
}

void Camera::HandleZoomMode(double deltaY) {
    float currentDistance = GetDistance();
    float newDistance = currentDistance * (1.0f + deltaY * m_zoomSensitivity);
    SetDistance(newDistance);
}