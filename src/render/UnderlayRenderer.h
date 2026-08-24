#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <map>
#include <string>
#include <vector>

// One reference image to draw this frame: the four world-space corners of its
// quad in pixel order (0,0),(W,0),(W,H),(0,H); UVs are implied; `opacity` is
// the blend fade; `bytes` are the raw file bytes, read ONLY when `asset` is not
// yet cached as a GPU texture.
struct UnderlayDraw {
  std::string asset;
  const std::vector<unsigned char>* bytes = nullptr;
  std::array<glm::vec3, 4> corners;
  float opacity = 0.5f;
};

// Draws sketch reference-image underlays
// (docs/sketching/reference-image-plan.md): textured quads on the active sketch
// plane, drawn before the sketch lines so geometry sits on top. Kept separate
// from SketchRenderer because it owns PERSISTENT device-local textures (decoded
// once per asset) rather than SketchRenderer's per-frame host-visible line
// buffers. Depth test on / write off, like the rest of the sketch overlay.
class UnderlayRenderer {
public:
  UnderlayRenderer() = default;
  ~UnderlayRenderer() { Shutdown(); }

  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkRenderPass renderPass, VkQueue graphicsQueue,
                  uint32_t queueFamily, VkCommandPool commandPool,
                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
  void Shutdown();

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

  // Replace this frame's draw list. Decodes + uploads any asset not yet cached
  // (a one-time synchronous upload); cached assets are reused.
  void SetImages(const std::vector<UnderlayDraw>& images);

  // Destroy every cached texture (wait-idle first) and reset the texture
  // descriptor pool. Call on sketch exit / document close / version change so a
  // texture is never freed while a frame in flight may still sample it.
  void ReleaseTextures();

  void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame);
  bool IsInitialized() const { return m_initialized; }

private:
  static constexpr int kFramesInFlight = 2;
  static constexpr uint32_t kMaxTextures = 64; // reference images per sketch

  struct Ubo {
    glm::mat4 view;
    glm::mat4 proj;
  };
  struct Vertex {
    glm::vec3 pos;
    glm::vec2 uv;
  };
  struct DynamicBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
  };
  struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE; // combined image sampler (set 1)
    int w = 0, h = 0;
  };
  // A quad to record this frame: where its verts start and which texture set.
  struct DrawRec {
    uint32_t firstVertex;
    VkDescriptorSet texSet;
    float opacity;
  };

  bool CreateDescriptors();
  bool CreatePipeline();
  // Returns the cached texture for `asset`, creating it from `bytes` on first
  // sight. Null if decode/upload fails or the descriptor pool is exhausted.
  Texture* GetOrCreateTexture(const std::string& asset,
                              const std::vector<unsigned char>* bytes);
  bool UploadTexture(const unsigned char* rgba, int w, int h, Texture& out);
  uint32_t UploadVerts(DynamicBuffer& buf, const std::vector<Vertex>& verts);
  void DestroyDynamic(DynamicBuffer& buf);
  void DestroyTexture(Texture& tex);

  VkDevice m_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkRenderPass m_renderPass = VK_NULL_HANDLE;
  VkSampleCountFlagBits m_msaaSamples = VK_SAMPLE_COUNT_1_BIT;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  uint32_t m_queueFamily = 0;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;

  VkDescriptorSetLayout m_uboSetLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_texSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
  VkPipeline m_pipeline = VK_NULL_HANDLE;
  VkDescriptorPool m_uboPool = VK_NULL_HANDLE;
  VkDescriptorPool m_texPool = VK_NULL_HANDLE;
  VkSampler m_sampler = VK_NULL_HANDLE;

  DynamicBuffer m_uniformBuffers[kFramesInFlight];
  VkDescriptorSet m_uboSets[kFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  DynamicBuffer m_vertexBuffers[kFramesInFlight];

  std::map<std::string, Texture> m_textures;
  std::vector<Vertex> m_verts;  // this frame's quad vertices
  std::vector<DrawRec> m_draws; // this frame's per-image draws

  glm::mat4 m_view = glm::mat4(1.0f);
  glm::mat4 m_proj = glm::mat4(1.0f);
  int32_t m_viewportX = 0;
  int32_t m_viewportY = 0;
  uint32_t m_viewportWidth = 1280;
  uint32_t m_viewportHeight = 720;
  bool m_initialized = false;
};
