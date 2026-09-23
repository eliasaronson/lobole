#pragma once

#include "render/ClipPlanes.h"

#include "render/MeshRenderer.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

// Key-light shadow mapping (docs/rendering/README.md#shadows): a
// depth-only pass into a 2048^2 D32 map from the primary light's direction,
// recorded before the main render pass each frame. The ortho frustum is re-fit
// to the combined scene bounds every frame (CAD scenes are compact, so no
// cascades and no slack far planes). The map is window-resolution-independent
// and survives swapchain rebuilds untouched. The render pass's subpass
// dependencies order depth-write against fragment sampling, so no explicit
// barriers are needed at the call site.
class ShadowRenderer {
public:
  ShadowRenderer();
  ~ShadowRenderer();

  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkQueue graphicsQueue, uint32_t queueFamily,
                  VkCommandPool commandPool);
  void Shutdown();

  // Section clip plane (world): xyz = normal, w = distance; 0 = off. Same
  // convention as MeshRenderer::SetClipPlane — sectioned-away material must
  // not cast.
  void SetClipPlanes(const ClipPlaneArray& planes) { m_clipPlanes = planes; }

  // Records the depth-only shadow pass: fits the light frustum to `scene`'s
  // combined bounds and draws every opaque (visible, non-ghost) instance.
  // Call before the main render pass begins, then hand LightViewProj() and
  // the texel sizes to MeshRenderer::SetShadowState.
  void RecordShadowPass(VkCommandBuffer cb, MeshRenderer& scene);

  // For MeshRenderer::SetShadowMap — must be wired BEFORE the mesh
  // renderer's Initialize (its descriptor sets bind these at creation).
  VkImageView ShadowMapView() const { return m_shadowView; }
  VkSampler ShadowSampler() const { return m_sampler; }

  const glm::mat4& LightViewProj() const { return m_lightViewProj; }
  float TexelWorldSize() const { return m_texelWorld; }
  float TexelUVSize() const { return 1.0f / m_resolution; }

  // Shadow-map size (View > Render Quality / set_render_options RPC,
  // 1024 or 2048): recreates the depth target behind wait-idle when it
  // changes. Returns true when recreated — the caller must rebind the new
  // ShadowMapView() into the mesh renderer's descriptor sets.
  bool SetResolution(uint32_t resolution);
  uint32_t Resolution() const { return m_resolution; }

  static constexpr uint32_t kDefaultResolution = 2048;

private:
  bool CreateShadowTarget();
  bool CreateFramebuffer();
  bool CreateRenderPass();
  bool CreatePipeline();

  VkDevice m_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  uint32_t m_queueFamily = 0;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;

  VkImage m_shadowImage = VK_NULL_HANDLE;
  VkDeviceMemory m_shadowMemory = VK_NULL_HANDLE;
  VkImageView m_shadowView = VK_NULL_HANDLE;
  VkSampler m_sampler = VK_NULL_HANDLE;
  VkRenderPass m_renderPass = VK_NULL_HANDLE;
  VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
  VkPipeline m_pipeline = VK_NULL_HANDLE;

  ClipPlaneArray m_clipPlanes = NoClipPlanes(); // section view
  glm::mat4 m_lightViewProj = glm::mat4(1.0f);
  float m_texelWorld = 0.0f; // world size of one shadow-map texel
  uint32_t m_resolution = kDefaultResolution;

  // Scratch reused across frames (avoids per-frame allocations).
  std::vector<ShadowCaster> m_casters;
};
