// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "render/ClipPlanes.h"
#include "render/FramesInFlight.h"
#include "render/SceneRenderer.h"

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
  // The active field over the legend's range: 0 at its low end, 1 at its
  // high; below 0 draws the under colour and above 1 the over colour.
  float scalar;
  // The triangle's barycentric coordinate opposite each ELEMENT edge: 0 on
  // the edge, growing away from it. The fragment shader draws the edge
  // where one nears zero (FieldStyle::edges); a triangle's three vertices
  // carry (1,0,0)-like corners, a tri6's midside nodes the halves.
  glm::vec3 edge{1.0f};
};

// How the field is drawn. The result contour is the default; the mesh view
// (docs/simulation/README.md#the-mesh-you-look-at) turns the others on.
struct FieldStyle {
  bool edges = false;     // element edges, from FieldVertex::edge
  bool plain = false;     // no colour map: a grey body under the edges
  bool lit = false;       // shade by the facet's normal (a headlight)
  bool unclipped = false; // ignore the section planes (the crinkle cut is
                          // already made on the CPU)
  bool reversed = false;  // the colour map runs the other way (a factor's
                          // LOW end is the hot one)
  // The colours past the legend's ends, shared with the legend: kOver past
  // the hot end, kUnder past the other (a reversed map's hot end is low).
  static constexpr glm::vec3 kOver{1.0f, 0.35f, 0.9f};
  static constexpr glm::vec3 kUnder{0.42f, 0.44f, 0.48f};
};

class FieldRenderer : public SceneRenderer {
public:
  FieldRenderer() = default;
  ~FieldRenderer() { Shutdown(); }

  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkRenderPass renderPass, VkQueue graphicsQueue,
                  uint32_t queueFamily, VkCommandPool commandPool,
                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
  void Shutdown() override;

  // Replace the field geometry (indexed triangle list). Empty clears it.
  void SetMesh(std::vector<FieldVertex> vertices,
               std::vector<uint32_t> indices) {
    m_vertices = std::move(vertices);
    m_indices = std::move(indices);
    m_dirty = true;
  }
  bool HasMesh() const { return !m_indices.empty(); }

  void SetDeformScale(float scale) { m_deformScale = scale; }
  void SetStyle(const FieldStyle& style) { m_style = style; }
  const FieldStyle& Style() const { return m_style; }

  void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame,
              const SceneView& view) override;
  bool IsInitialized() const { return m_initialized; }

private:
  // One slot per swapchain image (render/FramesInFlight.h): with two, a
  // three-image swapchain rewrote a buffer the GPU was still reading.
  static constexpr int kFramesInFlight =
      static_cast<int>(render::kMaxFramesInFlight);
  struct DynamicBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
  };
  struct Ubo {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 clipPlanes[kMaxClipPlanes]; // section clips (world)
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
  VkDescriptorSet m_descriptorSets[kFramesInFlight] = {};
  DynamicBuffer m_vertexBuffers[kFramesInFlight];
  DynamicBuffer m_indexBuffers[kFramesInFlight];

  std::vector<FieldVertex> m_vertices;
  std::vector<uint32_t> m_indices;
  bool m_dirty = false;

  float m_deformScale = 0.0f;
  FieldStyle m_style;
  bool m_initialized = false;
};
