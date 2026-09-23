#pragma once

#include "render/ClipPlanes.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>

// The per-frame contract every renderer that draws into the 3D scene owes the
// frame loop (docs/rendering/README.md#scene-renderers).
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
class SceneRenderer {
public:
  virtual ~SceneRenderer() = default;

  // The 3D viewport is a sub-region between the docked panels, not the
  // framebuffer. A renderer that misses this draws full-window while the
  // camera aspect is the sub-region, which is the bug this file exists for.
  virtual void SetViewport(int32_t x, int32_t y, uint32_t width,
                           uint32_t height) = 0;
  virtual void UpdateCamera(const glm::mat4& view, const glm::mat4& proj) = 0;
  // Not every renderer sections — the sketch lines and the underlay images do
  // not — so this one defaults to doing nothing rather than forcing an empty
  // override.
  virtual void SetClipPlanes(const ClipPlaneArray&) {}
  virtual void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame) = 0;
  virtual void Shutdown() = 0;
};
