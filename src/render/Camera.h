#pragma once

#include "core/Conventions.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

// Orbit camera. Orientation is stored as a unit quaternion about an orbit
// `pivot` at a fixed `distance`; the world position and the up/forward/right
// axes are all *derived* from it. Keeping orientation (not a separate up
// vector) as the single source of truth means the basis is always orthonormal:
// there is no pole singularity to gimbal-lock at and no stale up vector to
// leave a reset/aligned view degenerate.
class Camera {
public:
  // Vertical field of view, degrees. 45 is a wide, cinematic lens: on a
  // metre-scale part the near end flared out against the far one and the
  // perspective read as distortion rather than depth. CAD sits narrower
  // (SolidWorks and Onshape are near-orthographic); 30 is that look while a
  // solid still gets its depth cues.
  static constexpr float kDefaultFovDegrees = 30.0f;

  // Shared initial/reset isometric view: farther out than the previous
  // millimetre-scale framing while retaining a useful empty-workspace view.
  static glm::vec3 DefaultIsometricPosition() {
    // 14.4 along each horizontal axis, 10.8 along world up — slightly under
    // the diagonal so the default view looks a little down at the part. Those
    // are the numbers a 45-degree lens needed, and what you SEE from them is
    // 1/tan(fov/2), so the standoff carries that factor: narrowing the lens
    // must not silently zoom the empty workspace in.
    const float lens = std::tan(glm::radians(22.5f)) /
                       std::tan(glm::radians(0.5f * kDefaultFovDegrees));
    return (glm::vec3(14.4f) - 3.6f * conv::kWorldUpF) * lens;
  }

  Camera();
  ~Camera() = default;

  void Update(float deltaTime);

  // Camera controls: pixel-space deltas, called by the input layer
  void Orbit(double deltaX, double deltaY);
  void Pan(double deltaX, double deltaY);
  void Zoom(double delta); // positive zooms out
  void Roll(double delta); // rotate around the view axis

  // Camera properties. Position/target/up are a look-at view of the orbit
  // state: SetTarget keeps the camera where it is and re-aims at the target,
  // SetPosition keeps the target and moves the camera, SetUp re-rolls to the
  // given up while keeping the view direction. Each re-derives the
  // orientation through a guarded look-at, so none can produce a degenerate
  // view.
  void SetPosition(const glm::vec3& position);
  void SetTarget(const glm::vec3& target);
  // Move the orbit pivot WITHOUT re-aiming: the orientation is untouched and
  // the camera slides with the pivot, which is what a pan does. SetTarget
  // rotates the camera to look at the new point, so framing something with it
  // also swings the view — fitting a part must not change which way you are
  // looking at it.
  void SetPivot(const glm::vec3& pivot) {
    m_pivot = pivot;
    UpdateViewMatrix();
  }
  void SetUp(const glm::vec3& up);

  void SetFOV(float fov) {
    m_fov = fov;
    UpdateProjectionMatrix();
  }
  void SetAspectRatio(float aspect) {
    m_aspectRatio = aspect;
    UpdateProjectionMatrix();
  }
  void SetNearPlane(float nearPlane) {
    m_nearPlane = nearPlane;
    UpdateProjectionMatrix();
  }
  void SetFarPlane(float farPlane) {
    m_farPlane = farPlane;
    UpdateProjectionMatrix();
  }
  float GetFOV() const { return m_fov; } // vertical, degrees
  float GetAspectRatio() const { return m_aspectRatio; }
  // Zoom-out limit. Fit raises it to track the scene size — a fixed cap
  // silently pinned the camera inside anything bigger than ~40mm.
  void SetMaxDistance(float maxDistance) { m_maxDistance = maxDistance; }

  // Matrix getters
  const glm::mat4& GetViewMatrix() const { return m_viewMatrix; }
  const glm::mat4& GetProjectionMatrix() const { return m_projectionMatrix; }
  glm::mat4 GetViewProjectionMatrix() const {
    return m_projectionMatrix * m_viewMatrix;
  }

  // Projection for an arbitrary output aspect (offline render targets aren't
  // tied to the viewport), with the same Vulkan Y flip as
  // UpdateProjectionMatrix.
  glm::mat4 GetProjectionMatrixForAspect(float aspect) const {
    glm::mat4 proj =
        glm::perspective(glm::radians(m_fov), aspect, m_nearPlane, m_farPlane);
    proj[1][1] *= -1.0f;
    return proj;
  }

  // Camera vectors (derived from the orientation quaternion, always
  // orthonormal)
  glm::vec3 GetPosition() const { return m_pivot - GetForward() * m_distance; }
  glm::vec3 GetTarget() const { return m_pivot; }
  glm::vec3 GetForward() const {
    return m_orientation * glm::vec3(0.0f, 0.0f, -1.0f);
  }
  glm::vec3 GetRight() const {
    return m_orientation * glm::vec3(1.0f, 0.0f, 0.0f);
  }
  glm::vec3 GetUp() const {
    return m_orientation * glm::vec3(0.0f, 1.0f, 0.0f);
  }

  // Camera distance from target
  float GetDistance() const { return m_distance; }
  void SetDistance(float distance);

  // Face alignment for sketching. distance <= 0 keeps the current zoom.
  // `up` is the world direction mapped to screen up (the named views pass
  // their table roll; the default is world up, with a fallback when the view
  // direction is vertical).
  void AlignToFace(const glm::vec3& faceCenter, const glm::vec3& faceNormal,
                   float distance = 0.0f, glm::vec3 up = conv::kWorldUpF);
  // Instantly look at the current pivot from `direction` (unit offset from
  // pivot toward the camera) with `up` as screen-up. Cancels any animation —
  // remote callers need the very next frame to render the final view.
  void SnapToView(const glm::vec3& direction, const glm::vec3& up);
  void AnimateToPosition(const glm::vec3& position, const glm::vec3& target,
                         float duration = 1.0f, glm::vec3 up = conv::kWorldUpF);
  void StopAnimation() { m_isAnimating = false; }
  // Jump straight to the animation's end state. StopAnimation freezes the
  // blend wherever it happens to be, which makes any screen<->world mapping
  // taken next frame-timing dependent — tests use this for determinism.
  void FinishAnimation() {
    if (!m_isAnimating)
      return;
    m_orientation = m_animEndOrient;
    m_pivot = m_animEndPivot;
    m_distance = m_animEndDistance;
    m_isAnimating = false;
    UpdateViewMatrix();
  }
  bool IsAnimating() const { return m_isAnimating; }

private:
  void UpdateViewMatrix();
  void UpdateProjectionMatrix();

  // A unit quaternion whose forward (-Z) points along `dir`, rolled so its up
  // is as close to `up` as possible. Falls back to a non-parallel up when
  // `dir` and `up` are (near) collinear so the result is never degenerate.
  static glm::quat OrientationLookingAlong(const glm::vec3& dir,
                                           const glm::vec3& up);

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
