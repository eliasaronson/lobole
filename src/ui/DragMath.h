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

} // namespace ui
