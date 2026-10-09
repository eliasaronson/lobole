// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>

#include <cmath>

// Pure ray/axis/plane/arc geometry for viewport drag handles. Split out of
// DragMath.h, which also carries GizmoScale and therefore depends on AppState
// — ViewportHandle.h needs the geometry and is included BY AppState.h, so the
// two cannot live in one header.
//
// In either precision: a preview handle lives in float beside the renderer,
// while the occurrence gizmo drags a placement, which is double
// (docs/conventions.md#the-model-is-double), and widens the screen ray once.
namespace ui {

template <typename T> using RayVec = glm::vec<3, T, glm::defaultp>;

template <typename T> RayVec<T> PerpTo(const RayVec<T>& a) {
  const RayVec<T> ref =
      std::abs(a.y) < T(0.9) ? RayVec<T>(0, 1, 0) : RayVec<T>(1, 0, 0);
  return glm::normalize(glm::cross(a, ref));
}

// Coordinate `t` along the axis line (C + A·t) closest to the ray (O + D·s).
template <typename T>
T RayAxisParam(const RayVec<T>& O, const RayVec<T>& D, const RayVec<T>& C,
               const RayVec<T>& A) {
  const T b = glm::dot(D, A);
  const RayVec<T> w0 = O - C;
  const T d = glm::dot(D, w0), e = glm::dot(A, w0);
  const T denom = T(1) - b * b; // a=c=1 (D,A unit)
  if (std::abs(denom) < T(1e-6))
    return e;                 // near-parallel: project O
  return (e - b * d) / denom; // axis coordinate tc
}

// Ray vs plane (point C, unit normal N). false if near-parallel / behind.
template <typename T>
bool RayPlane(const RayVec<T>& O, const RayVec<T>& D, const RayVec<T>& C,
              const RayVec<T>& N, RayVec<T>& hit) {
  const T dn = glm::dot(D, N);
  if (std::abs(dn) < T(1e-6))
    return false;
  const T t = glm::dot(C - O, N) / dn;
  if (t < T(0))
    return false;
  hit = O + D * t;
  return true;
}

// Perpendicular distance from point Q to the ray (O + D·s).
template <typename T>
T PointRayDist(const RayVec<T>& Q, const RayVec<T>& O, const RayVec<T>& D) {
  const RayVec<T> w = Q - O;
  return glm::length(w - glm::dot(w, D) * D);
}

// The angle of the ray's hit on the plane ⟂ A through C, in a fixed in-plane
// basis. Used for rotate handles.
template <typename T>
bool RotateAngle(const RayVec<T>& O, const RayVec<T>& D, const RayVec<T>& C,
                 const RayVec<T>& A, T& angle) {
  RayVec<T> hit;
  if (!RayPlane(O, D, C, A, hit))
    return false;
  const RayVec<T> b1 = PerpTo(A), b2 = glm::cross(A, b1);
  const RayVec<T> v = hit - C;
  angle = std::atan2(glm::dot(v, b2), glm::dot(v, b1));
  return true;
}

} // namespace ui
