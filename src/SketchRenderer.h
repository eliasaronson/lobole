#pragma once

#include "MeshRenderer.h" // Vertex3D

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

// A vertex for the screen-space-expanded primitive pipeline: thick
// anti-aliased lines and constant-size point dots. Each triangle vertex sits on
// `posThis`; the vertex shader offsets it in screen space by `corner` scaled to
// `params.x` pixels (a line offsets along the screen normal of posThis→posOther
// by corner.x; a dot offsets to a square corner). `params.y` is the shape
// (0 = line, 1 = dot); the fragment shader SDF-feathers the edge for AA.
struct PrimVertex {
  glm::vec3 posThis;
  glm::vec3 posOther;
  glm::vec2 corner;
  glm::vec2 params; // x = half-width/radius in px, y = shape (0 line, 1 dot)
  glm::vec4 color;
};

// Draws sketch geometry over the 3D scene: static sketch entity lines,
// a per-frame rubber-band overlay, and translucent plane quads. Uses two
// pipelines (LINE_LIST for lines, TRIANGLE_LIST for quads) sharing one
// view/proj UBO; per-draw opacity rides in a push constant. Vertex data is
// uploaded into per-frame host-visible buffers each frame, so updates never
// race a frame already in flight.
class SketchRenderer {
public:
  SketchRenderer() = default;
  ~SketchRenderer() { Shutdown(); }

  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkRenderPass renderPass, VkQueue graphicsQueue,
                  uint32_t queueFamily, VkCommandPool commandPool,
                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
  void Shutdown();

  // Sketch entity lines (line list: pairs of endpoints). Replaced on
  // document change.
  void SetStaticLines(std::vector<LineVertex3D> lines) {
    m_staticLines = std::move(lines);
  }
  // Rubber-band preview, replaced every frame while drawing.
  void SetOverlayLines(std::vector<LineVertex3D> lines) {
    m_overlayLines = std::move(lines);
  }
  // Translucent default-plane quads (triangle list).
  void SetQuads(std::vector<LineVertex3D> quads) { m_quads = std::move(quads); }
  // Screen-space-expanded primitives (thick lines + point dots), replaced on
  // document/selection change.
  void SetPrimitives(std::vector<PrimVertex> prims) {
    m_primitives = std::move(prims);
  }

  void UpdateCamera(const glm::mat4& view, const glm::mat4& proj) {
    m_view = view;
    m_proj = proj;
  }
  void SetViewportSize(uint32_t w, uint32_t h) { SetViewport(0, 0, w, h); }
  void SetViewport(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    m_viewportX = x;
    m_viewportY = y;
    m_viewportWidth = w;
    m_viewportHeight = h;
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
    glm::vec4 viewport; // xy = viewport size in pixels (for screen expansion)
  };

  bool CreatePipelines();
  bool CreateDescriptors();
  uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props);
  VkShaderModule LoadShader(const std::string& spvName);
  // Grows the buffer if needed and uploads verts; returns the vertex count.
  uint32_t Upload(DynamicBuffer& buf, const std::vector<LineVertex3D>& verts);
  // Raw-bytes variant shared by the LineVertex3D and PrimVertex upload paths.
  uint32_t UploadRaw(DynamicBuffer& buf, const void* data, VkDeviceSize bytes,
                     uint32_t count);
  void DestroyDynamic(DynamicBuffer& buf);
  void DrawBuffer(VkCommandBuffer cb, VkPipeline pipeline, DynamicBuffer& buf,
                  uint32_t count, float alpha);

  VkDevice m_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkRenderPass m_renderPass = VK_NULL_HANDLE;
  VkSampleCountFlagBits m_msaaSamples = VK_SAMPLE_COUNT_1_BIT;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  uint32_t m_queueFamily = 0;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;

  VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
  VkPipeline m_linePipeline = VK_NULL_HANDLE;
  VkPipeline m_quadPipeline = VK_NULL_HANDLE;
  VkPipeline m_primPipeline = VK_NULL_HANDLE;
  VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

  DynamicBuffer m_uniformBuffers[kFramesInFlight];
  VkDescriptorSet m_descriptorSets[kFramesInFlight] = {VK_NULL_HANDLE,
                                                       VK_NULL_HANDLE};
  DynamicBuffer m_lineBuffers[kFramesInFlight];
  DynamicBuffer m_overlayBuffers[kFramesInFlight];
  DynamicBuffer m_quadBuffers[kFramesInFlight];
  DynamicBuffer m_primBuffers[kFramesInFlight];

  std::vector<LineVertex3D> m_staticLines;
  std::vector<LineVertex3D> m_overlayLines;
  std::vector<LineVertex3D> m_quads;
  std::vector<PrimVertex> m_primitives;

  glm::mat4 m_view = glm::mat4(1.0f);
  glm::mat4 m_proj = glm::mat4(1.0f);
  int32_t m_viewportX = 0;
  int32_t m_viewportY = 0;
  uint32_t m_viewportWidth = 1280;
  uint32_t m_viewportHeight = 720;
  bool m_initialized = false;
};
