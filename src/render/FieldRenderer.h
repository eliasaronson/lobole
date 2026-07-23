#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

// Draws a FEM result field: the boundary triangles of a solved study's mesh,
// warped by displacement (shader-side, push-constant scale) and colormapped by
// a per-vertex scalar (von Mises or |displacement|, pre-normalized to [0,1])
// through an analytic Turbo polynomial. Its own pipeline + vertex format (the
// SketchRenderer precedent); shares the section clip plane so a stress plot
// clips like the rest of the scene. Depth-tested and depth-writing — it stands
// in for the body's normal render while a field is displayed.
struct FieldVertex {
  glm::vec3 position;     // undeformed node position (mm)
  glm::vec3 displacement; // solved u (mm)
  float scalar;           // active field, pre-normalized to [0,1]
};

class FieldRenderer {
public:
  FieldRenderer() = default;
  ~FieldRenderer() { Shutdown(); }

  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkRenderPass renderPass, VkQueue graphicsQueue,
                  uint32_t queueFamily, VkCommandPool commandPool,
                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
  void Shutdown();

  // Replace the field geometry (indexed triangle list). Empty clears it.
  void SetMesh(std::vector<FieldVertex> vertices, std::vector<uint32_t> indices) {
    m_vertices = std::move(vertices);
    m_indices = std::move(indices);
    m_dirty = true;
  }
  bool HasMesh() const { return !m_indices.empty(); }

  void UpdateCamera(const glm::mat4& view, const glm::mat4& proj) {
    m_view = view;
    m_proj = proj;
  }
  void SetClipPlane(const glm::vec4& plane) { m_clipPlane = plane; }
  void SetDeformScale(float scale) { m_deformScale = scale; }
  void SetViewport(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    m_vx = x; m_vy = y; m_vw = w; m_vh = h;
  }

  void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame);
  bool IsInitialized() const { return m_initialized; }

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
    glm::vec4 clipPlane; // xyz = normal, w = dist; 0 = off
  };

  bool CreateDescriptors();
  bool CreatePipeline();
  uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props);
  VkShaderModule LoadShader(const std::string& spvName);
  void EnsureCapacity(DynamicBuffer& buf, VkDeviceSize bytes, VkBufferUsageFlags usage);
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
  VkDescriptorSet m_descriptorSets[kFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  DynamicBuffer m_vertexBuffers[kFramesInFlight];
  DynamicBuffer m_indexBuffers[kFramesInFlight];

  std::vector<FieldVertex> m_vertices;
  std::vector<uint32_t> m_indices;
  bool m_dirty = false;

  glm::mat4 m_view = glm::mat4(1.0f);
  glm::mat4 m_proj = glm::mat4(1.0f);
  glm::vec4 m_clipPlane = glm::vec4(0.0f);
  float m_deformScale = 0.0f;
  int32_t m_vx = 0, m_vy = 0;
  uint32_t m_vw = 1280, m_vh = 720;
  bool m_initialized = false;
};
