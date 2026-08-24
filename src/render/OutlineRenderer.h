#pragma once

#include "render/FramesInFlight.h"

#include "render/ClipPlanes.h"

#include "render/OutlineEdges.h"
#include <chrono>
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

// Forward declarations
struct Vertex3D;

enum class OutlineMode {
  NONE,
  EDGES,     // Sharp edge detection
  WIREFRAME, // Full wireframe
  SILHOUETTE // Silhouette outline only
};

struct OutlineSettings {
  OutlineMode mode = OutlineMode::EDGES;
  glm::vec4 outlineColor =
      glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); // Bright red for testing
  glm::vec4 wireframeColor =
      glm::vec4(0.2f, 0.2f, 0.2f, 1.0f); // Subtle dark gray
  float outlineWidth = 5.0f;             // Larger for visibility
  float lineWidth = 1.5f;
  bool enableDepthTest = true;
  bool enableBlending = true;
  // Wireframe silhouettes are recomputed once the camera has been still for
  // this long; while it moves, only the static feature/boundary edges draw.
  float silhouetteSettleMs = 120.0f;
  // Escape hatch: reclassify silhouettes on every camera change instead of
  // waiting for it to settle (costs CPU on large assemblies).
  bool liveSilhouettes = false;
};

class OutlineRenderer {
public:
  OutlineRenderer();
  ~OutlineRenderer();

  // Initialization and cleanup
  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkRenderPass renderPass, VkQueue graphicsQueue,
                  uint32_t queueFamily, VkCommandPool commandPool,
                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
  void Shutdown();

  // Mesh management
  uint32_t AddMesh(const std::vector<Vertex3D>& vertices,
                   const std::vector<uint32_t>& indices);
  void UpdateMesh(uint32_t meshId, const std::vector<Vertex3D>& vertices,
                  const std::vector<uint32_t>& indices);
  void RemoveMesh(uint32_t meshId);
  void SetMeshTransform(uint32_t meshId, const glm::mat4& transform);

  // Camera and rendering
  void UpdateCamera(const glm::mat4& view, const glm::mat4& projection);
  void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame);

  // Settings
  void SetOutlineSettings(const OutlineSettings& settings) {
    m_settings = settings;
  }
  const OutlineSettings& GetOutlineSettings() const { return m_settings; }
  void SetViewportSize(uint32_t width, uint32_t height) {
    SetViewport(0, 0, width, height);
  }

  // Section clip plane (world): xyz = normal, w = distance; edges past it
  // are discarded. Zero = disabled (default).
  void SetClipPlanes(const ClipPlaneArray& planes) { m_clipPlanes = planes; }
  void SetViewport(int32_t x, int32_t y, uint32_t width, uint32_t height);

  bool IsInitialized() const { return m_isInitialized; }

private:
  // One slot per swapchain image, not per "frame in flight" — the caller
  // passes the image index. See render/FramesInFlight.h.
  static const int MAX_FRAMES_IN_FLIGHT = render::kMaxFramesInFlight;

  struct OutlineMesh {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    glm::mat4 transform = glm::mat4(1.0f);

    // Wireframe edge indices. CPU side: precomputed adjacency plus the
    // current line list, laid out [static feature/boundary prefix |
    // per-view silhouette suffix]. GPU side: one persistently mapped
    // host-visible buffer per frame in flight, allocated once at worst case
    // (MaxIndexCount) — updates are a memcpy into the slot whose fence the
    // frame loop already waited on, so no reallocation, no staging copy and
    // no mid-frame vkDeviceWaitIdle, and an in-flight frame can never see
    // its buffer freed under it.
    outline::EdgeAdjacency adjacency;
    std::vector<uint32_t> edgeIndices;
    uint32_t staticIndexCount = 0;
    uint64_t edgeVersion = 1; // bumped whenever edgeIndices changes
    VkBuffer edgeIndexBuffers[MAX_FRAMES_IN_FLIGHT] = {};
    VkDeviceMemory edgeIndexBufferMemory[MAX_FRAMES_IN_FLIGHT] = {};
    void* edgeIndexMapped[MAX_FRAMES_IN_FLIGHT] = {};
    uint32_t uploadedIndexCount[MAX_FRAMES_IN_FLIGHT] = {};
    uint64_t uploadedVersion[MAX_FRAMES_IN_FLIGHT] = {};
  };

  struct OutlineUniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 viewPos;
    glm::vec4 outlineColor;
    float outlineWidth;
    float _padding[3];
  };

  struct WireframeUniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 viewPos;
    glm::vec4 wireframeColor;
    float lineWidth;
    float _padding[3];
    glm::vec4 clipPlanes[kMaxClipPlanes]; // section clips (world)
  };

  // Initialization helpers
  bool CreateOutlinePipeline();
  bool CreateWireframePipeline();
  bool CreateDescriptorSetLayout();
  bool CreateUniformBuffers();
  bool CreateDescriptorPool();
  bool CreateDescriptorSets();

  // Creates and maps this slot's edge index buffer the first time the slot is
  // used. Slots exist for capacity (kMaxFramesInFlight); a real swapchain
  // touches 2-3, and this is per MESH — so they are filled on demand.
  void EnsureEdgeBuffer(OutlineMesh& mesh, uint32_t slot);

  // Edge classification state (see OutlineEdges.h for the algorithm)
  void RefreshSilhouettes();
  void DropToStaticEdges();

  // Rendering helpers
  void UpdateUniformBuffers(uint32_t currentFrame);
  void RenderOutlines(VkCommandBuffer commandBuffer, uint32_t currentFrame);
  void RenderWireframes(VkCommandBuffer commandBuffer, uint32_t currentFrame);

  // Utility functions

  // Vulkan objects
  VkDevice m_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkRenderPass m_renderPass = VK_NULL_HANDLE;
  VkSampleCountFlagBits m_msaaSamples = VK_SAMPLE_COUNT_1_BIT;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  uint32_t m_queueFamily = 0;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;

  // Pipeline objects
  VkPipeline m_outlinePipeline = VK_NULL_HANDLE;
  VkPipeline m_wireframePipeline = VK_NULL_HANDLE;
  VkPipelineLayout m_outlinePipelineLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_wireframePipelineLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_outlineDescriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_wireframeDescriptorSetLayout = VK_NULL_HANDLE;

  // Descriptor objects
  VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> m_outlineDescriptorSets;
  std::vector<VkDescriptorSet> m_wireframeDescriptorSets;

  // Uniform buffers
  std::vector<VkBuffer> m_outlineUniformBuffers;
  std::vector<VkDeviceMemory> m_outlineUniformBuffersMemory;
  std::vector<VkBuffer> m_wireframeUniformBuffers;
  std::vector<VkDeviceMemory> m_wireframeUniformBuffersMemory;

  // Mesh data
  std::vector<std::unique_ptr<OutlineMesh>> m_meshes;
  uint32_t m_nextMeshId = 1;

  // Camera data
  glm::mat4 m_viewMatrix = glm::mat4(1.0f);
  glm::mat4 m_projectionMatrix = glm::mat4(1.0f);
  glm::vec3 m_cameraPosition = glm::vec3(0.0f);

  // Silhouette settle state: while the camera moves only static edges draw;
  // one classification pass runs after it has been still for the configured
  // delay (or immediately, with liveSilhouettes).
  std::chrono::steady_clock::time_point m_lastCameraMove{};
  bool m_silhouettesShown = false;
  bool m_silhouettesDirty = true;
  outline::SilhouetteScratch m_silhouetteScratch;

  // Viewport
  int32_t m_viewportX = 0;
  int32_t m_viewportY = 0;
  uint32_t m_viewportWidth = 1280;
  uint32_t m_viewportHeight = 720;

  // Settings
  OutlineSettings m_settings;
  ClipPlaneArray m_clipPlanes = NoClipPlanes(); // section view

  // State
  bool m_isInitialized = false;
};