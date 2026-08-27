#pragma once

#include "render/ClipPlanes.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

// Draws the CAM cutting simulation's stock (docs/cam/simulation.md, S3).
//
// Its own pipeline and vertex format, for one reason: the mesh changes EVERY
// FRAME during playback, and `MeshRenderer::UpdateAsset` cannot do that — it
// vkDeviceWaitIdles, destroys and recreates device-local buffers through a
// staging upload on every call, which is correct for a regen and a stutter
// machine at 15 Hz. The pattern here is FieldRenderer's: host-visible,
// persistently mapped vertex and index buffers PER FRAME IN FLIGHT, grown on
// demand, written with a memcpy and no device wait anywhere.
//
// The cost of the split is that the stock does not take part in the shadow or
// SSAO passes. That is the same trade the FEM field makes, and it is the
// reason not to teach MeshRenderer a dynamic-asset kind for this: that would
// touch the shadow-caster collection and every draw path that binds an
// asset's vertex buffer, which is a viewport-wide regression risk for a
// cosmetic gain.
struct StockVertex {
  glm::vec3 position;
  glm::vec3 normal;
};

class StockRenderer {
public:
  StockRenderer() = default;
  ~StockRenderer() { Shutdown(); }

  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkRenderPass renderPass, VkQueue graphicsQueue,
                  uint32_t queueFamily, VkCommandPool commandPool,
                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
  void Shutdown();

  // Replace the stock geometry. Empty clears it.
  void SetMesh(std::vector<StockVertex> vertices,
               std::vector<uint32_t> indices) {
    m_vertices = std::move(vertices);
    m_indices = std::move(indices);
  }
  void Clear() {
    m_vertices.clear();
    m_indices.clear();
  }
  [[nodiscard]] bool HasMesh() const { return !m_indices.empty(); }

  // Setup space -> world. An indexed setup (C9) leans the block with the
  // table, and the stock has to lean with it or it floats beside the part.
  void SetTransform(const glm::mat4& m) { m_model = m; }
  void SetColor(const glm::vec4& c) { m_color = c; }

  void UpdateCamera(const glm::mat4& view, const glm::mat4& proj) {
    m_view = view;
    m_proj = proj;
  }
  void SetClipPlanes(const ClipPlaneArray& planes) { m_clipPlanes = planes; }
  void SetViewport(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    m_vx = x;
    m_vy = y;
    m_vw = w;
    m_vh = h;
  }

  void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame);
  [[nodiscard]] bool IsInitialized() const { return m_initialized; }

private:
  static constexpr int kFramesInFlight = 2;
  struct DynamicBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
  };
  struct Ubo {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 clipPlanes[kMaxClipPlanes];
  };
  // 80 bytes, inside the 128-byte floor every Vulkan implementation
  // guarantees (the wood-grain push constant already sits at that ceiling —
  // see MeshRenderer).
  struct Push {
    glm::mat4 model;
    glm::vec4 baseColor;
  };

  bool CreateDescriptors();
  bool CreatePipeline();
  bool EnsureCapacity(DynamicBuffer& buf, VkDeviceSize bytes,
                      VkBufferUsageFlags usage);
  void DestroyDynamic(DynamicBuffer& buf);

  VkDevice m_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkRenderPass m_renderPass = VK_NULL_HANDLE;
  VkSampleCountFlagBits m_msaaSamples = VK_SAMPLE_COUNT_1_BIT;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  uint32_t m_queueFamily = 0;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;

  VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
  VkPipeline m_pipeline = VK_NULL_HANDLE;
  VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

  DynamicBuffer m_uniformBuffers[kFramesInFlight];
  VkDescriptorSet m_descriptorSets[kFramesInFlight] = {VK_NULL_HANDLE,
                                                       VK_NULL_HANDLE};
  DynamicBuffer m_vertexBuffers[kFramesInFlight];
  DynamicBuffer m_indexBuffers[kFramesInFlight];

  std::vector<StockVertex> m_vertices;
  std::vector<uint32_t> m_indices;

  glm::mat4 m_model = glm::mat4(1.0f);
  glm::vec4 m_color = glm::vec4(0.62f, 0.64f, 0.68f, 1.0f);
  glm::mat4 m_view = glm::mat4(1.0f);
  glm::mat4 m_proj = glm::mat4(1.0f);
  ClipPlaneArray m_clipPlanes = NoClipPlanes();
  int32_t m_vx = 0, m_vy = 0;
  uint32_t m_vw = 1280, m_vh = 720;
  bool m_initialized = false;
};
