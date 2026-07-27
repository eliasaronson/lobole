#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

struct Vertex3D;

// Simple color-based picking system
class ColorPicker {
public:
  ColorPicker();
  ~ColorPicker();

  // Initialization
  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkQueue graphicsQueue, uint32_t queueFamily,
                  VkCommandPool commandPool, uint32_t width, uint32_t height);
  void Shutdown();

  // Picking
  uint32_t PickObject(float screenX, float screenY, const glm::mat4& view,
                      const glm::mat4& proj);
  void AddMesh(uint32_t meshId, const std::vector<Vertex3D>& vertices,
               const std::vector<uint32_t>& indices,
               const glm::mat4& transform = glm::mat4(1.0f));
  void UpdateMeshTransform(uint32_t meshId, const glm::mat4& transform);
  void Clear();

  bool IsInitialized() const { return m_isInitialized; }

private:
  struct PickingMesh {
    uint32_t meshId;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    glm::mat4 transform = glm::mat4(1.0f);
    glm::vec3 pickingColor; // Unique color for this mesh
  };

  struct PickingUniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 pickingColor;
    float _padding;
  };

  // Render to offscreen buffer with unique colors
  void RenderPickingBuffer(const glm::mat4& view, const glm::mat4& proj);

  // Convert mesh ID to unique color
  glm::vec3 MeshIdToColor(uint32_t meshId);
  uint32_t ColorToMeshId(const glm::vec3& color);

  // Vulkan utilities
  VkBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags properties,
                        VkDeviceMemory& bufferMemory);
  void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
  uint32_t FindMemoryType(uint32_t typeFilter,
                          VkMemoryPropertyFlags properties);
  std::vector<char> ReadShaderFile(const std::string& filename);
  VkShaderModule CreateShaderModule(const std::vector<char>& code);

  // Vulkan objects
  VkDevice m_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  uint32_t m_queueFamily = 0;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;

  // Offscreen rendering
  VkImage m_pickingImage = VK_NULL_HANDLE;
  VkDeviceMemory m_pickingImageMemory = VK_NULL_HANDLE;
  VkImageView m_pickingImageView = VK_NULL_HANDLE;
  VkFramebuffer m_pickingFramebuffer = VK_NULL_HANDLE;
  VkRenderPass m_pickingRenderPass = VK_NULL_HANDLE;

  // Pipeline
  VkPipeline m_pickingPipeline = VK_NULL_HANDLE;
  VkPipelineLayout m_pickingPipelineLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> m_descriptorSets;

  // Uniform buffers
  std::vector<VkBuffer> m_uniformBuffers;
  std::vector<VkDeviceMemory> m_uniformBuffersMemory;

  // Meshes
  std::vector<std::unique_ptr<PickingMesh>> m_meshes;

  // State
  bool m_isInitialized = false;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  static const int MAX_FRAMES_IN_FLIGHT = 2;
};