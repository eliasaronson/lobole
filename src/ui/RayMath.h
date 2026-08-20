#pragma once

#include <glm/glm.hpp>

#include <cmath>

// Pure ray/axis/plane/arc geometry for viewport drag handles. Split out of
// DragMath.h, which also carries GizmoScale and therefore depends on AppState
// — ViewportHandle.h needs the geometry and is included BY AppState.h, so the
// two cannot live in one header.
namespace ui {

inline glm::vec3 PerpTo(const glm::vec3& a) {
  const glm::vec3 ref =
      std::abs(a.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  return glm::normalize(glm::cross(a, ref));
}

// Coordinate `t` along the axis line (C + A·t) closest to the ray (O + D·s).
inline float RayAxisParam(const glm::vec3& O, const glm::vec3& D,
                          const glm::vec3& C, const glm::vec3& A) {
  const float b = glm::dot(D, A);
  const glm::vec3 w0 = O - C;
  const float d = glm::dot(D, w0), e = glm::dot(A, w0);
  const float denom = 1.0f - b * b; // a=c=1 (D,A unit)
  if (std::abs(denom) < 1e-6f)
    return e;                 // near-parallel: project O
  return (e - b * d) / denom; // axis coordinate tc
}

// Ray vs plane (point C, unit normal N). false if near-parallel / behind.
inline bool RayPlane(const glm::vec3& O, const glm::vec3& D, const glm::vec3& C,
                     const glm::vec3& N, glm::vec3& hit) {
  const float dn = glm::dot(D, N);
  if (std::abs(dn) < 1e-6f)
    return false;
  const float t = glm::dot(C - O, N) / dn;
  if (t < 0.0f)
    return false;
  hit = O + D * t;
  return true;
}

// Perpendicular distance from point Q to the ray (O + D·s).
inline float PointRayDist(const glm::vec3& Q, const glm::vec3& O,
                          const glm::vec3& D) {
  const glm::vec3 w = Q - O;
  return glm::length(w - glm::dot(w, D) * D);
}

// The angle of the ray's hit on the plane ⟂ A through C, in a fixed in-plane
// basis. Used for rotate handles.
inline bool RotateAngle(const glm::vec3& O, const glm::vec3& D,
                        const glm::vec3& C, const glm::vec3& A, float& angle) {
  glm::vec3 hit;
  if (!RayPlane(O, D, C, A, hit))
    return false;
  const glm::vec3 b1 = PerpTo(A), b2 = glm::cross(A, b1);
  const glm::vec3 v = hit - C;
  angle = std::atan2(glm::dot(v, b2), glm::dot(v, b1));
  return true;
}

} // namespace ui
