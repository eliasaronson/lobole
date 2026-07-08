#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

// Orbit camera. Orientation is stored as a unit quaternion about an orbit
// `pivot` at a fixed `distance`; the world position and the up/forward/right
// axes are all *derived* from it. Keeping orientation (not a separate up
// vector) as the single source of truth means the basis is always orthonormal:
// there is no pole singularity to gimbal-lock at and no stale up vector to
// leave a reset/aligned view degenerate.
class Camera {
public:
    Camera();
    ~Camera() = default;

    void Update(float deltaTime);

    // Camera controls: pixel-space deltas, called by the input layer
    void Orbit(double deltaX, double deltaY);
    void Pan(double deltaX, double deltaY);
    void Zoom(double delta);        // positive zooms out
    void Roll(double delta);        // rotate around the view axis

    // Camera properties. Position/target/up are a look-at view of the orbit
    // state: SetTarget keeps the camera where it is and re-aims at the target,
    // SetPosition keeps the target and moves the camera, SetUp re-rolls to the
    // given up while keeping the view direction. Each re-derives the
    // orientation through a guarded look-at, so none can produce a degenerate
    // view.
    void SetPosition(const glm::vec3& position);
    void SetTarget(const glm::vec3& target);
    void SetUp(const glm::vec3& up);

    void SetFOV(float fov) { m_fov = fov; UpdateProjectionMatrix(); }
    void SetAspectRatio(float aspect) { m_aspectRatio = aspect; UpdateProjectionMatrix(); }
    void SetNearPlane(float nearPlane) { m_nearPlane = nearPlane; UpdateProjectionMatrix(); }
    void SetFarPlane(float farPlane) { m_farPlane = farPlane; UpdateProjectionMatrix(); }

    // Matrix getters
    const glm::mat4& GetViewMatrix() const { return m_viewMatrix; }
    const glm::mat4& GetProjectionMatrix() const { return m_projectionMatrix; }
    glm::mat4 GetViewProjectionMatrix() const { return m_projectionMatrix * m_viewMatrix; }

    // Camera vectors (derived from the orientation quaternion, always orthonormal)
    glm::vec3 GetPosition() const { return m_pivot - GetForward() * m_distance; }
    glm::vec3 GetTarget() const { return m_pivot; }
    glm::vec3 GetForward() const { return m_orientation * glm::vec3(0.0f, 0.0f, -1.0f); }
    glm::vec3 GetRight() const { return m_orientation * glm::vec3(1.0f, 0.0f, 0.0f); }
    glm::vec3 GetUp() const { return m_orientation * glm::vec3(0.0f, 1.0f, 0.0f); }

    // Camera distance from target
    float GetDistance() const { return m_distance; }
    void SetDistance(float distance);

    // Face alignment for sketching. distance <= 0 keeps the current zoom.
    void AlignToFace(const glm::vec3& faceCenter, const glm::vec3& faceNormal, float distance = 0.0f);
    void AnimateToPosition(const glm::vec3& position, const glm::vec3& target, float duration = 1.0f);
    void StopAnimation() { m_isAnimating = false; }

private:
    void UpdateViewMatrix();
    void UpdateProjectionMatrix();

    // A unit quaternion whose forward (-Z) points along `dir`, rolled so its up
    // is as close to `up` as possible. Falls back to a non-parallel up when
    // `dir` and `up` are (near) collinear so the result is never degenerate.
    static glm::quat OrientationLookingAlong(const glm::vec3& dir, const glm::vec3& up);

    // Orbit parameters (authoritative state)
    glm::vec3 m_pivot;       // orbit target
    float m_distance;        // pivot -> camera distance
    glm::quat m_orientation; // camera orientation; forward = orientation * -Z

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

    // Animation state (slerp orientation, lerp pivot + distance)
    bool m_isAnimating = false;
    float m_animationTime = 0.0f;
    float m_animationDuration = 1.0f;
    glm::quat m_animStartOrient;
    glm::quat m_animEndOrient;
    glm::vec3 m_animStartPivot;
    glm::vec3 m_animEndPivot;
    float m_animStartDistance = 0.0f;
    float m_animEndDistance = 0.0f;
};
