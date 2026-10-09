// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <glm/glm.hpp>

// The section clip planes, shared by every pass that has to agree about what
// is cut away: mesh, outline/wireframe, SSAO prepass, field overlay, and the
// shadow map (removed material must not keep casting).
//
// A plane is {xyz = world normal, w = distance}; a fragment is discarded when
// dot(n, p) > w for ANY enabled plane — sections intersect, so two planes
// give a quarter cut. An all-zero slot is disabled.
//
// FOUR, not eight: the shadow pass carries its planes in push constants
// alongside a mat4 (64 bytes), and Vulkan only guarantees 128 — four planes
// fill it exactly. Every pass then has the same limit, which is one fewer
// thing to explain. Commercial CAD typically offers three.
inline constexpr int kMaxClipPlanes = 4;

using ClipPlaneArray = std::array<glm::vec4, kMaxClipPlanes>;

inline ClipPlaneArray NoClipPlanes() {
  return ClipPlaneArray{glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f),
                        glm::vec4(0.0f)};
}
