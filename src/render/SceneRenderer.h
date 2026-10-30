// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "render/ClipPlanes.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>

// The per-frame contract every renderer that draws into the 3D scene owes the
// frame loop (docs/rendering/README.md#scene-renderers): it draws a
// SceneView, handed to it with the call.
//
// It exists because that contract used to be seven hand-maintained lists in
// main.cpp and UI.cpp, and the compiler checked none of them. Every one of the
// last three renderers added arrived with a wire missing from one of those
// lists: the underlay from shutdown (a SIGSEGV at process exit that went
// unexplained long enough to be called "the long-standing shutdown crash",
// a222bc8), the FEM field from the viewport list (every field silently
// offset, 0346a8e), the CAM stock from the same list (4a3083d, reported from
// the field as a workpiece that was not parallel to the part).
//
// Deliberately NARROW. This is the frame contract and nothing else — the
// renderers keep their own typed APIs for content, and the ui/ layer keeps
// calling them through typed pointers in AppState (`AddMesh`,
// `SetInstanceGhost`, `fieldRenderer->SetMesh`, and some forty more). This is
// not an attempt to hide the renderers behind one type; it is an attempt to
// have ONE list where there were seven.
//
// The prepass renderers (shadow, SSAO, ray tracing) are NOT scene renderers:
// they own their own targets and their own passes, take no scene viewport,
// and are correctly absent here.
// What one draw of the scene is FOR: where it lands and what it looks at.
// Passed to every Render, never stored between them, so a renderer cannot draw
// with a viewport, a camera or a section that belongs to some other draw.
struct SceneView {
  // x, y, w, h in framebuffer pixels: the 3D sub-region between the docked
  // panels, not the framebuffer. A renderer drawing the whole framebuffer
  // while the camera's aspect is this rectangle is the bug this file exists
  // for.
  glm::ivec4 viewport{0, 0, 1, 1};
  glm::mat4 view{1.0f};
  glm::mat4 proj{1.0f};
  // World section planes; the sketch lines and the underlay images ignore
  // them.
  ClipPlaneArray clips = NoClipPlanes();
};

// The Vulkan viewport and scissor of a SceneView: its rectangle, depth 0..1.
inline VkViewport ViewportOf(const SceneView& v) {
  VkViewport vp{};
  vp.x = static_cast<float>(v.viewport.x);
  vp.y = static_cast<float>(v.viewport.y);
  vp.width = static_cast<float>(v.viewport.z);
  vp.height = static_cast<float>(v.viewport.w);
  vp.minDepth = 0.0f;
  vp.maxDepth = 1.0f;
  return vp;
}
inline VkRect2D ScissorOf(const SceneView& v) {
  return {{v.viewport.x, v.viewport.y},
          {static_cast<uint32_t>(v.viewport.z),
           static_cast<uint32_t>(v.viewport.w)}};
}

class SceneRenderer {
public:
  virtual ~SceneRenderer() = default;

  virtual void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame,
                      const SceneView& view) = 0;
  virtual void Shutdown() = 0;
};
