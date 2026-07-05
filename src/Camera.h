#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera {
public:
    Camera();
    ~Camera() = default;

    void Update(float deltaTime);

    // Camera controls: pixel-space deltas, called by the input layer
    void Orbit(double deltaX, double deltaY);
    void Pan(double deltaX, double deltaY);
    void Zoom(double delta); // positive zooms out

    // Camera properties
    void SetPosition(const glm::vec3& position) { m_position = position; UpdateViewMatrix(); }
    void SetTarget(const glm::vec3& target) { m_target = target; UpdateViewMatrix(); }
    void SetUp(const glm::vec3& up) { m_up = up; UpdateViewMatrix(); }
    
    void SetFOV(float fov) { m_fov = fov; UpdateProjectionMatrix(); }
    void SetAspectRatio(float aspect) { m_aspectRatio = aspect; UpdateProjectionMatrix(); }
    void SetNearPlane(float nearPlane) { m_nearPlane = nearPlane; UpdateProjectionMatrix(); }
    void SetFarPlane(float farPlane) { m_farPlane = farPlane; UpdateProjectionMatrix(); }

    // Matrix getters
    const glm::mat4& GetViewMatrix() const { return m_viewMatrix; }
    const glm::mat4& GetProjectionMatrix() const { return m_projectionMatrix; }
    glm::mat4 GetViewProjectionMatrix() const { return m_projectionMatrix * m_viewMatrix; }

    // Camera vectors
    glm::vec3 GetPosition() const { return m_position; }
    glm::vec3 GetTarget() const { return m_target; }
    glm::vec3 GetForward() const { return glm::normalize(m_target - m_position); }
    glm::vec3 GetRight() const { return glm::normalize(glm::cross(GetForward(), m_up)); }
    glm::vec3 GetUp() const { return m_up; }

    // Camera distance from target
    float GetDistance() const { return glm::length(m_position - m_target); }
    void SetDistance(float distance);
    
    // Face alignment for sketching
    void AlignToFace(const glm::vec3& faceCenter, const glm::vec3& faceNormal, float distance = 5.0f);
    void AnimateToPosition(const glm::vec3& position, const glm::vec3& target, float duration = 1.0f);
    void StopAnimation() { m_isAnimating = false; }

private:
    void UpdateViewMatrix();
    void UpdateProjectionMatrix();

    // Camera parameters
    glm::vec3 m_position;
    glm::vec3 m_target;
    glm::vec3 m_up;

    float m_fov;
    float m_aspectRatio;
    float m_nearPlane;
    float m_farPlane;

    // Matrices
    glm::mat4 m_viewMatrix;
    glm::mat4 m_projectionMatrix;

    // Camera settings
    float m_orbitSensitivity;
    float m_panSensitivity;
    float m_zoomSensitivity;
    float m_minDistance;
    float m_maxDistance;
    
    // Animation state
    bool m_isAnimating = false;
    float m_animationTime = 0.0f;
    float m_animationDuration = 1.0f;
    glm::vec3 m_animationStartPos;
    glm::vec3 m_animationStartTarget;
    glm::vec3 m_animationEndPos;
    glm::vec3 m_animationEndTarget;
};