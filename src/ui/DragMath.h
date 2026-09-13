#pragma once

#include "render/Camera.h"
#include "ui/AppState.h"

#include "ui/RayMath.h"

#include <glm/glm.hpp>

#include <cmath>

// Shared viewport drag math — rays against the axes/planes/arcs that drag
// handles are built from. Hoisted unchanged from the A2.5 occurrence gizmo
// (AssemblyActions.cpp) so the preview handles (docs/ui/direct-manipulation.md)
// use the exact same geometry.
namespace ui {

// Roughly-constant on-screen handle size from the camera distance.
inline float GizmoScale(const AppState& state) {
  return state.camera ? std::max(0.13f * state.camera->GetDistance(), 0.1f)
                      : 1.0f;
}

// A deterministic unit vector perpendicular to `a`.

// Snapping for a drag, shared by every drag handle in the app.
//
// ONE POLARITY, everywhere it is offered: snap is the DEFAULT and Ctrl frees
// it. The preview handles used to mean the opposite by Ctrl (fine by default,
// coarse while held), so "Ctrl frees the snap" was true on an assembly tab and
// backwards on a part tab -- docs/ui/direct-manipulation.md carried that as an
// open question from the day handles shipped.
//
// A CAPTURE WINDOW, not a quantizer: the round value is sticky and everything
// between two of them is still reachable, so 37 degrees is as reachable by
// drag as 45 is. That is what lets snapping be the default without taking a
// value away from anyone.
inline double SnapValue(double v, double step, double capture, bool snap) {
  if (!snap || !(step > 0.0))
    return v;
  const double nearest = std::round(v / step) * step;
  return std::abs(v - nearest) < capture ? nearest : v;
}

// An angle has one round set at every scale and in every unit: 45 degrees and
// its multiples. The mate twist, both assembly rotate drags, the CAM
// placement spin and the revolve-angle handle all use it.
inline constexpr double kAngleSnapStepDeg = 45.0;
inline constexpr double kAngleSnapCaptureDeg = 5.0;

// A LENGTH has no such universal set, so this pair is a literal in document
// units -- 0.5 is round for a 3 mm fillet and arbitrary for a 2 m beam. It is
// the granularity the preview handles already used for their Ctrl-held coarse
// mode; making it the default only changes WHICH modifier state you get it in.
// Deriving it from the part being dragged is the obvious later move.
inline constexpr double kLengthSnapStep = 0.5;
inline constexpr double kLengthSnapCapture = 0.1;

inline double SnapAngleDeg(double deg, bool snap) {
  return SnapValue(deg, kAngleSnapStepDeg, kAngleSnapCaptureDeg, snap);
}

inline double SnapLength(double v, bool snap) {
  return SnapValue(v, kLengthSnapStep, kLengthSnapCapture, snap);
}

// The same, for the callers whose angle is radians (the mate DOFs are).
inline float SnapAngleRad(float rad, bool snap) {
  if (!snap)
    return rad;
  return static_cast<float>(
      glm::radians(SnapAngleDeg(glm::degrees(static_cast<double>(rad)), snap)));
}

} // namespace ui
