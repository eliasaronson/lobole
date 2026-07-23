#include "render/Camera.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

Camera::Camera()
    : m_pivot(0.0f, 0.0f, 0.0f)
    , m_distance(1.0f)
    , m_orientation(1.0f, 0.0f, 0.0f, 0.0f) // identity
    , m_fov(45.0f)
    , m_aspectRatio(16.0f / 9.0f)
    , m_nearPlane(0.1f)
    , m_farPlane(1000.0f)
    , m_orbitSensitivity(0.005f)
    , m_panSensitivity(0.01f)
    , m_zoomSensitivity(0.1f)
    , m_minDistance(0.5f)
    // Zoom-out clamp must exceed room scale — metre-scale parts fit at
    // several thousand mm of distance.
    , m_maxDistance(100000.0f) {

    // Establish the default iso view (camera at (5,5,5) looking at the origin):
    // sets m_distance and m_orientation from the look-at.
    SetPosition(glm::vec3(5.0f, 5.0f, 5.0f));
    UpdateProjectionMatrix();
}

glm::quat Camera::OrientationLookingAlong(const glm::vec3& dir,
                                          const glm::vec3& up) {
    glm::vec3 f = glm::normalize(dir); // forward: view -Z maps to f
    glm::vec3 u = up;
    // If the requested up is (near) collinear with the view direction the basis
    // would collapse; fall back to a non-parallel reference so the result is
    // always a valid orthonormal frame (world-up normally, world-Z when looking
    // straight up/down).
    if (glm::abs(glm::dot(f, glm::normalize(u))) > 0.999f)
        u = (glm::abs(f.y) < 0.9f) ? glm::vec3(0.0f, 1.0f, 0.0f)
                                   : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 right = glm::normalize(glm::cross(f, u));
    const glm::vec3 trueUp = glm::cross(right, f);
    // Columns are the camera's world axes: +X right, +Y up, +Z backwards
    // (the view looks down -Z), so quat * -Z == forward.
    const glm::mat3 basis(right, trueUp, -f);
    return glm::normalize(glm::quat_cast(basis));
}

void Camera::Update(float deltaTime) {
    if (!m_isAnimating)
        return;

    m_animationTime += deltaTime;
    float t = std::min(m_animationTime / m_animationDuration, 1.0f);
    t = t * t * (3.0f - 2.0f * t); // ease-in-out

    // Constant-angular-velocity orientation blend + linear pivot/zoom blend.
    m_orientation = glm::normalize(glm::slerp(m_animStartOrient, m_animEndOrient, t));
    m_pivot = glm::mix(m_animStartPivot, m_animEndPivot, t);
    m_distance = glm::mix(m_animStartDistance, m_animEndDistance, t);
    UpdateViewMatrix();

    if (t >= 1.0f)
        m_isAnimating = false;
}

void Camera::SetDistance(float distance) {
    m_distance = std::clamp(distance, m_minDistance, m_maxDistance);
    UpdateViewMatrix();
}

void Camera::SetPosition(const glm::vec3& position) {
    // Keep the current target and up; move the camera to `position`.
    const glm::vec3 up = GetUp();
    const glm::vec3 dir = m_pivot - position; // look from position toward pivot
    const float d = glm::length(dir);
    if (d < 1e-6f)
        return;
    m_distance = d;
    m_orientation = OrientationLookingAlong(dir, up);
    UpdateViewMatrix();
}

void Camera::SetTarget(const glm::vec3& target) {
    // Keep the camera where it is; re-aim at `target`.
    const glm::vec3 pos = GetPosition();
    const glm::vec3 up = GetUp();
    const glm::vec3 dir = target - pos;
    const float d = glm::length(dir);
    if (d < 1e-6f)
        return;
    m_pivot = target;
    m_distance = d;
    m_orientation = OrientationLookingAlong(dir, up);
    UpdateViewMatrix();
}

void Camera::SetUp(const glm::vec3& up) {
    // Keep the view direction; re-roll so up is as close to `up` as possible.
    m_orientation = OrientationLookingAlong(GetForward(), up);
    UpdateViewMatrix();
}

void Camera::UpdateViewMatrix() {
    // Derived position/up are always orthonormal to the forward axis, so lookAt
    // is never degenerate here.
    m_viewMatrix = glm::lookAt(GetPosition(), m_pivot, GetUp());

    // Clip planes track the viewing distance: the old fixed 0.1..1000 range
    // clipped metre-scale parts entirely (an imported 1 m speaker wall
    // vanished at its own fit distance). Scaling near with distance keeps
    // depth precision at both desk and room scale.
    const float wantFar = std::max(2000.0f, m_distance * 50.0f);
    const float wantNear = std::clamp(m_distance * 0.001f, 0.01f, 5.0f);
    if (wantFar != m_farPlane || wantNear != m_nearPlane) {
        m_farPlane = wantFar;
        m_nearPlane = wantNear;
        UpdateProjectionMatrix();
    }
}

void Camera::UpdateProjectionMatrix() {
    m_projectionMatrix = glm::perspective(glm::radians(m_fov), m_aspectRatio, m_nearPlane, m_farPlane);
    // glm produces OpenGL clip space; Vulkan's NDC y points down. Flipping
    // here keeps every consumer (renderers AND the picking ray) consistent.
    m_projectionMatrix[1][1] *= -1.0f;
}

void Camera::Orbit(double deltaX, double deltaY) {
    // Trackball orbit: yaw about the camera's current up, pitch about its right.
    // Composing quaternions about the live basis has no pole singularity, so the
    // view never gimbal-locks, and the orientation stays a clean unit rotation
    // (no drifting up vector to re-orthonormalize).
    const float yaw = -static_cast<float>(deltaX) * m_orbitSensitivity;
    const float pitch = -static_cast<float>(deltaY) * m_orbitSensitivity;
    const glm::quat qYaw = glm::angleAxis(yaw, GetUp());
    const glm::quat qPitch = glm::angleAxis(pitch, GetRight());
    m_orientation = glm::normalize(qYaw * qPitch * m_orientation);
    UpdateViewMatrix();
}

void Camera::Pan(double deltaX, double deltaY) {
    const glm::vec3 right = GetRight();
    const glm::vec3 up = GetUp();

    float panScale = 0.005f;
    panScale *= (m_distance * 0.2f); // pan speed tracks zoom

    // Content follows the cursor: drag right moves the model right (camera
    // left), drag down moves the model down (camera up). Moving the pivot moves
    // the derived position with it.
    m_pivot += right * static_cast<float>(-deltaX * panScale) +
               up * static_cast<float>(deltaY * panScale);
    UpdateViewMatrix();
}

void Camera::Zoom(double deltaY) {
    SetDistance(m_distance * (1.0f + static_cast<float>(deltaY) * m_zoomSensitivity));
}

void Camera::Roll(double delta) {
    const float angle = static_cast<float>(delta) * 0.01f;
    m_orientation = glm::normalize(glm::angleAxis(angle, GetForward()) * m_orientation);
    UpdateViewMatrix();
}

void Camera::AlignToFace(const glm::vec3& faceCenter, const glm::vec3& faceNormal, float distance) {
    // Keep the current zoom unless the caller asks for a specific distance.
    if (distance <= 0.0f)
        distance = m_distance;

    const glm::vec3 n = glm::normalize(faceNormal);
    const glm::vec3 newPosition = faceCenter + n * distance;
    // Upright handling (world-up, or world-Z for a horizontal face) is applied
    // when the target orientation is built in AnimateToPosition.
    AnimateToPosition(newPosition, faceCenter, 1.0f);
}

void Camera::SnapToView(const glm::vec3& direction, const glm::vec3& up) {
    m_isAnimating = false;
    m_orientation = OrientationLookingAlong(-glm::normalize(direction), up);
    UpdateViewMatrix();
}

void Camera::AnimateToPosition(const glm::vec3& position, const glm::vec3& target, float duration) {
    m_animStartOrient = m_orientation;
    m_animStartPivot = m_pivot;
    m_animStartDistance = m_distance;

    const glm::vec3 dir = target - position;
    m_animEndPivot = target;
    m_animEndDistance = glm::length(dir);
    m_animEndOrient = (m_animEndDistance > 1e-6f)
                          ? OrientationLookingAlong(dir, glm::vec3(0.0f, 1.0f, 0.0f))
                          : m_orientation;

    m_animationTime = 0.0f;
    m_animationDuration = duration;
    m_isAnimating = true;
}
