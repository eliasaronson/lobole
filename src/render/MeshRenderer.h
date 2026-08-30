#pragma once

#include "render/FramesInFlight.h"

#include "render/ClipPlanes.h"
#include "render/SceneRenderer.h"
#include "render/VulkanCommon.h" // vkutil::StagingBatch (asset upload batching)

#include <algorithm>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

struct Vertex3D {
  glm::vec3 position;
  glm::vec3 normal;

  static VkVertexInputBindingDescription getBindingDescription();
  static std::vector<VkVertexInputAttributeDescription>
  getAttributeDescriptions();
};

// Vertex for unlit colored line/overlay geometry: sketch lines, gizmos, grids,
// datum axes, and translucent plane quads. Carries per-vertex color and no
// normal (the sketch pipeline is unlit). Distinct from Vertex3D, whose color
// moved to per-instance state with the renderer instancing split.
struct LineVertex3D {
  glm::vec3 position;
  glm::vec3 color;
};

struct Light {
  glm::vec4 direction; // xyz = direction, w = intensity
  glm::vec4 color;     // rgb = color, a = unused
};

// std140-clean by construction: mat4/vec4 members only, no scalars (the
// float-then-vec4 padding trap is documented in OutlineRenderer.h).
struct UniformBufferObject {
  glm::mat4 view;
  glm::mat4 proj;
  glm::vec4 viewPos;      // Camera position
  Light primaryLight;     // Main directional light
  Light fillLight;        // Fill light (opposite direction, lower intensity)
  Light rimLight;         // Rim light (side lighting)
  glm::vec4 ambientColor; // rgb = ambient color, a = intensity
  // Section clips (world): xyz = normal, w = dist; an all-zero slot is off.
  glm::vec4 clipPlanes[kMaxClipPlanes];
  glm::mat4 lightViewProj; // world -> key-light clip space (ShadowRenderer)
  // Key-light shadow params (S1): x = strength, y = normal-offset world
  // distance, z = shadow-map texel size (UV), w = enable (0/1).
  glm::vec4 shadowParams;
  // SSAO params (S2): xy = 1 / framebuffer size (gl_FragCoord -> AO uv),
  // z = enable (0/1), w = unused.
  // xy = gl_FragCoord -> AO uv scale, zw = its offset (the viewport rect).
  glm::vec4 aoParams;
  // x = AO enabled, yzw unused.
  glm::vec4 aoParams2;
};

// One opaque instance's draw data for the shadow pass (ShadowRenderer): the
// exact set the main pass's opaque loop draws, ghosts excluded.
struct ShadowCaster {
  VkBuffer vertexBuffer;
  VkBuffer indexBuffer;
  uint32_t indexCount;
  glm::mat4 transform;
};

// One tessellation, uploaded once and shared by any number of instances.
class MeshAsset {
public:
  std::vector<Vertex3D> vertices; // CPU copy kept for updates/picking
  std::vector<uint32_t> indices;

  VkBuffer vertexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
  VkBuffer indexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;

  // Local-space bounds, cached at (re)upload so the per-frame shadow
  // frustum fit only transforms 8 corners per instance.
  glm::vec3 bboxMin = glm::vec3(0.0f);
  glm::vec3 bboxMax = glm::vec3(0.0f);

  uint32_t refCount = 0; // live instances referencing this asset
};

// One drawn occurrence of an asset: everything that may vary between
// occurrences without re-uploading geometry. Rides in push constants per draw.
struct MeshInstance {
  uint32_t assetId = 0;
  glm::mat4 transform = glm::mat4(1.0f);

  // Albedo. Was baked into vertex colors before the asset/instance split;
  // per-instance so occurrences of one asset can differ in material.
  glm::vec4 baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);

  // PBR params: x = metallic, y = roughness, z = specular, w = dielectric
  // reflectance. Default = neutral dielectric so unassigned bodies read
  // predictably.
  glm::vec4 materialParams = glm::vec4(0.0f, 0.5f, 0.5f, 0.04f);

  // Procedural surface pattern (wood grain / plywood), shaded in mesh.frag
  // from local object-space position: x = pattern id (0 flat, 1 wood,
  // 2 plywood), y = grainScale, z = grainContrast. w = grain-axis index
  // (0/1/2), filled per-draw from the asset bbox — not stored here. Default
  // x = 0 leaves every existing material flat (unchanged appearance).
  glm::vec4 procParams = glm::vec4(0.0f);

  // Face-highlight vertex ranges [start, end). Any number of selected
  // ranges (Ctrl+multi-select, measure pairs — the first rides in the main
  // draw's push constant, the rest are drawn as extra sub-draws); one hover
  // range.
  std::vector<glm::uvec2> selRanges;
  glm::uvec2 hoverRange = glm::uvec2(0);

  bool visible = true;

  // Ghosted context geometry (A4 in-context editing): drawn by the alpha-
  // blended, depth-write-off ghost pipeline in a second pass after all
  // opaque instances. baseColor.a carries the opacity.
  bool ghost = false;
};

// gl_FragCoord -> AO-map uv, as a pure function so it can be tested without a
// GPU. The scene is drawn into a sub-rect of the framebuffer (panels take the
// rest) while every screen-space prepass fills its own target, so a lookup
// that divides by the FRAMEBUFFER size reads the wrong texel — the SSAO
// ghosting bug. Returns .xy = scale, .zw = offset: uv = coord * scale + off.
//
// Any future full-screen effect that consumes a prepass belongs here too; the
// invariant is that viewport origin maps to uv 0 and its far corner to uv 1,
// whatever the framebuffer around it.
inline glm::vec4 ViewportToTargetUV(int32_t viewportX, int32_t viewportY,
                                    uint32_t viewportWidth,
                                    uint32_t viewportHeight) {
  const glm::vec2 size(std::max(viewportWidth, 1u),
                       std::max(viewportHeight, 1u));
  const glm::vec2 scale = 1.0f / size;
  return glm::vec4(scale, -glm::vec2(viewportX, viewportY) * scale);
}

class MeshRenderer : public SceneRenderer {
public:
  MeshRenderer();
  ~MeshRenderer();

  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkRenderPass renderPass, VkQueue graphicsQueue,
                  uint32_t queueFamily, VkCommandPool commandPool,
                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
  void Shutdown() override;

  // Assets: shared tessellations (one GPU upload each)
  uint32_t CreateAsset(const std::vector<Vertex3D>& vertices,
                       const std::vector<uint32_t>& indices);

  // Collect the uploads of many CreateAsset calls behind one submit+wait
  // (vkutil::StagingBatch). Without this each asset drains the queue on its
  // own, which is most of the cost of building a large assembly. Begin before
  // a bulk build, End after; unpaired use is safe — Render() flushes any batch
  // still open, so a draw can never see an asset whose copy has not executed.
  void BeginAssetBatch();
  void EndAssetBatch();
  void UpdateAsset(uint32_t assetId, const std::vector<Vertex3D>& vertices,
                   const std::vector<uint32_t>& indices);

  // Keeps an asset alive with no instances referencing it, so a caller can
  // cache the upload across rebuilds. Without this, an assembly resync — which
  // removes every instance before re-adding them — destroys and re-uploads
  // every tessellation, which was ~4 s of a 5.9 s tab switch on a 299-part
  // import. Retain/Release move the same refCount AddInstance/RemoveInstance
  // do, so the last holder (instance or retainer) frees the buffers.
  void RetainAsset(uint32_t assetId);
  void ReleaseAsset(uint32_t assetId);

  // Instances: drawn occurrences of an asset. Removing the last instance of
  // an asset destroys the asset's buffers too, unless a RetainAsset holds it.
  uint32_t AddInstance(uint32_t assetId,
                       const glm::mat4& transform = glm::mat4(1.0f));
  void RemoveInstance(uint32_t instanceId);
  void SetInstanceTransform(uint32_t instanceId, const glm::mat4& transform);
  // What an instance is actually rendering at. Exists so a test can assert
  // the RENDERER's state rather than recomputing the caller's arithmetic —
  // the difference between testing the path and testing the test.
  std::optional<glm::mat4> GetInstanceTransform(uint32_t instanceId) const;
  void SetInstanceColor(uint32_t instanceId, const glm::vec3& albedo);
  // Albedo + PBR params in one shot. params is (metallic, roughness,
  // specular, reflectance).
  void SetInstanceMaterial(uint32_t instanceId, const glm::vec3& albedo,
                           const glm::vec4& params);
  // Procedural surface pattern for wood/plywood: pattern (0 flat, 1 wood,
  // 2 plywood), grainScale, grainContrast. pattern 0 restores the default
  // flat look. The grain axis is derived per-draw from the asset's bbox.
  void SetInstanceProcedural(uint32_t instanceId, int pattern, float grainScale,
                             float grainContrast);
  void SetInstanceVisible(uint32_t instanceId, bool visible);
  // Marks an instance as ghosted context geometry (A4): translucent
  // (`alpha`), no depth write, drawn after all opaque instances. Unsorted
  // blending — acceptable at reference-visual alphas (~0.25).
  void SetInstanceGhost(uint32_t instanceId, float alpha = 0.25f);

  // Transitional one-body-one-mesh API (asset + instance in one shot);
  // call sites migrate to the split API with assemblies A2.
  uint32_t AddMesh(const std::vector<Vertex3D>& vertices,
                   const std::vector<uint32_t>& indices);
  void UpdateMesh(uint32_t instanceId, const std::vector<Vertex3D>& vertices,
                  const std::vector<uint32_t>& indices);
  void RemoveMesh(uint32_t instanceId) { RemoveInstance(instanceId); }
  void SetMeshTransform(uint32_t instanceId, const glm::mat4& transform) {
    SetInstanceTransform(instanceId, transform);
  }
  void SetMeshColor(uint32_t instanceId, const glm::vec3& color) {
    SetInstanceColor(instanceId, color);
  }
  void SetMeshMaterial(uint32_t instanceId, const glm::vec3& albedo,
                       const glm::vec4& params) {
    SetInstanceMaterial(instanceId, albedo, params);
  }
  void SetMeshProcedural(uint32_t instanceId, int pattern, float grainScale,
                         float grainContrast) {
    SetInstanceProcedural(instanceId, pattern, grainScale, grainContrast);
  }

  // Face highlighting (vertex ranges within the instance's asset).
  // ADDITIVE per call — the per-frame selection projection clears first
  // (SyncSelectionHighlights), so several faces highlight together.
  void SetFaceHighlight(uint32_t instanceId, uint32_t firstVertex,
                        uint32_t count);
  void SetFaceHover(uint32_t instanceId, uint32_t firstVertex, uint32_t count);
  void ClearFaceHighlight();
  void ClearFaceHover();

  // Highlights a whole instance (its full vertex range) WITHOUT clearing
  // others, so all instances of a selected occurrence highlight together
  // (assemblies A2). Pair with ClearFaceHighlight to deselect.
  void HighlightInstance(uint32_t instanceId);

  // Rendering
  void UpdateCamera(const glm::mat4& view,
                    const glm::mat4& projection) override;
  void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame) override;

  // Viewport
  void SetViewportSize(uint32_t width, uint32_t height) {
    SetViewport(0, 0, width, height);
  }

  // Section clip plane (world space): xyz = normal, w = distance; fragments
  // past it are discarded. Zero vector = no clipping (the default).
  void SetClipPlanes(const ClipPlaneArray& planes) override {
    m_clipPlanes = planes;
  }
  void SetViewport(int32_t x, int32_t y, uint32_t width,
                   uint32_t height) override;

  // IBL textures for the fragment shader. Must be set BEFORE Initialize()
  // (descriptor sets bind them at creation).
  void SetIBL(VkImageView irradiance, VkImageView prefilter, VkImageView brdf,
              VkSampler envSampler, VkSampler lutSampler) {
    m_iblIrradiance = irradiance;
    m_iblPrefilter = prefilter;
    m_iblBrdf = brdf;
    m_iblEnvSampler = envSampler;
    m_iblLutSampler = lutSampler;
  }

  // Key-light shadow map (S1, ShadowRenderer). Like SetIBL, must be set
  // BEFORE Initialize() — the descriptor sets bind it at creation.
  void SetShadowMap(VkImageView shadowMap, VkSampler shadowSampler) {
    m_shadowMapView = shadowMap;
    m_shadowSampler = shadowSampler;
  }
  // View > Shadows toggle; off zeroes shadowParams.w so the shader skips
  // the lookup (the map itself may then hold stale contents — harmless).
  void SetShadowsEnabled(bool enabled) { m_shadowsEnabled = enabled; }
  // Per-frame light fit from ShadowRenderer::RecordShadowPass: the world ->
  // light-clip matrix plus the map's texel size in world units (drives the
  // normal-offset bias) and in UV (drives the PCF kernel step).
  void SetShadowState(const glm::mat4& lightViewProj, float texelWorldSize,
                      float texelUVSize) {
    m_lightViewProj = lightViewProj;
    m_shadowTexelWorld = texelWorldSize;
    m_shadowTexelUV = texelUVSize;
  }

  // Blurred SSAO map (S2, SSAORenderer). Like SetIBL/SetShadowMap, must be
  // set BEFORE Initialize() — the descriptor sets bind it at creation.
  void SetAOMap(VkImageView aoMap, VkSampler aoSampler) {
    m_aoMapView = aoMap;
    m_aoSampler = aoSampler;
  }
  // Rebinds the AO map after the SSAO targets were recreated on resize.
  // Call behind wait-idle only (the descriptor sets must not be in flight).
  void RebindAOMap(VkImageView aoMap);
  // Same for the shadow map after ShadowRenderer::SetResolution.
  void RebindShadowMap(VkImageView shadowMap);
  // View > Ambient Occlusion toggle; off zeroes aoParams2.x so the shader
  // skips the lookup (the map may then hold stale contents — harmless).
  void SetAOEnabled(bool enabled) { m_aoEnabled = enabled; }
  // Per-frame from SSAORenderer: 1 / full framebuffer size. The AO lookup
  // does NOT use it directly — see UpdateUniformBuffer, which maps
  // gl_FragCoord through the 3D VIEWPORT rect instead. The scene is drawn
  // into that sub-rect of the framebuffer (panels take the rest) while the
  // SSAO prepass draws the same projection across its whole target, so
  // dividing gl_FragCoord by the framebuffer size read the wrong texel and
  // painted displaced ghosts of nearby geometry onto flat faces.
  void SetAOState(const glm::vec2& invFullResolution) {
    m_aoInvSize = invFullResolution;
  }

  // Camera matrices as last set by UpdateCamera — the prepasses read them
  // here so they are guaranteed identical to what the main pass uses.
  const glm::mat4& ViewMatrix() const { return m_viewMatrix; }
  const glm::mat4& ProjectionMatrix() const { return m_projectionMatrix; }

  // Shadow-pass draw list: every visible, non-ghost instance (ghosts don't
  // write depth in the main pass and must not cast), plus the combined
  // world-space bbox for the light frustum fit. Returns false when empty.
  // Also serves the SSAO depth+normal prepass, which draws the same set.
  bool GetShadowCasters(std::vector<ShadowCaster>& out, glm::vec3& bbMin,
                        glm::vec3& bbMax);

private:
  ClipPlaneArray m_clipPlanes = NoClipPlanes(); // section view
  VkDevice m_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  uint32_t m_queueFamily = 0;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;

  // Pipeline
  VkRenderPass m_renderPass = VK_NULL_HANDLE;
  VkSampleCountFlagBits m_msaaSamples = VK_SAMPLE_COUNT_1_BIT;
  VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
  VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
  // The renderer's first transparency (A4 ghosts): same shaders/layout,
  // alpha blend on, depth test on / depth write off.
  VkPipeline m_ghostPipeline = VK_NULL_HANDLE;

  // Uniform buffers
  std::vector<VkBuffer> m_uniformBuffers;
  std::vector<VkDeviceMemory> m_uniformBuffersMemory;
  std::vector<void*> m_uniformBuffersMapped;

  // Descriptor sets
  VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> m_descriptorSets;

  // Assets + instances. Ids are 1-based slot indices; removal leaves a null
  // hole so later ids stay stable.
  std::vector<std::unique_ptr<MeshAsset>> m_assets;
  std::vector<std::unique_ptr<MeshInstance>> m_instances;

  // IBL texture handles (owned by IBLTextures; bound into descriptor sets).
  VkImageView m_iblIrradiance = VK_NULL_HANDLE;
  VkImageView m_iblPrefilter = VK_NULL_HANDLE;
  VkImageView m_iblBrdf = VK_NULL_HANDLE;
  VkSampler m_iblEnvSampler = VK_NULL_HANDLE;
  VkSampler m_iblLutSampler = VK_NULL_HANDLE;

  // Key-light shadow map (owned by ShadowRenderer; bound like the IBL maps).
  VkImageView m_shadowMapView = VK_NULL_HANDLE;
  VkSampler m_shadowSampler = VK_NULL_HANDLE;
  bool m_shadowsEnabled = true;
  glm::mat4 m_lightViewProj = glm::mat4(1.0f);
  float m_shadowTexelWorld = 0.0f;
  float m_shadowTexelUV = 0.0f;

  // Blurred SSAO map (owned by SSAORenderer; rebound on resize).
  VkImageView m_aoMapView = VK_NULL_HANDLE;
  VkSampler m_aoSampler = VK_NULL_HANDLE;
  bool m_aoEnabled = true;
  glm::vec2 m_aoInvSize = glm::vec2(0.0f);

  // Camera
  glm::mat4 m_viewMatrix = glm::mat4(1.0f);
  glm::mat4 m_projectionMatrix = glm::mat4(1.0f);

  // Viewport
  int32_t m_viewportX = 0;
  int32_t m_viewportY = 0;
  uint32_t m_viewportWidth = 800;
  uint32_t m_viewportHeight = 600;

  // Constants
  // One slot per swapchain image, not per "frame in flight" — the caller
  // passes the image index. See render/FramesInFlight.h.
  static const int MAX_FRAMES_IN_FLIGHT = render::kMaxFramesInFlight;

  // Helper functions
  bool CreateDescriptorSetLayout();
  bool CreateGraphicsPipeline();
  bool CreateUniformBuffers();
  bool CreateDescriptorPool();
  bool CreateDescriptorSets();

  MeshAsset* GetAsset(uint32_t assetId);
  MeshInstance* GetInstance(uint32_t instanceId);
  bool CreateAssetBuffers(MeshAsset* asset);
  // Open only between BeginAssetBatch/EndAssetBatch; CreateAssetBuffers uses
  // it when set and a one-shot batch of its own otherwise.
  std::unique_ptr<vkutil::StagingBatch> m_assetBatch;
  // See OutlineRenderer: one device wait per batch of removals, not one per
  // asset destroyed. A resync of an assembly drops every instance in a row.
  bool m_deviceIdle = false;
  void WaitDeviceIdleOnce();
  void DestroyAssetBuffers(MeshAsset* asset);
};
