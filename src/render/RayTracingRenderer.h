#pragma once

#include "core/LightRig.h"
#include "render/ClipPlanes.h"

#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

// Forward declarations
struct Vertex3D;
class Mesh;

struct RTAccelerationStructure {
  VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceAddress deviceAddress = 0;
};

// Per-geometry behavior flags (must match closesthit.rchit).
enum RTGeometryFlags : uint32_t {
  // Shadow catcher: shows the environment darkened by shadow/AO instead of a
  // shaded surface — the ground plane under offline renders.
  RT_GEOM_SHADOW_CATCHER = 1u << 0,
  // The material comes per TRIANGLE from the geometry's triangle-material
  // buffer, not from materialIndex: a body whose faces carry their own
  // appearance (v164). Set by AddGeometry when it is handed one.
  RT_GEOM_PER_TRIANGLE_MATERIAL = 1u << 1,
};

struct RTGeometry {
  VkBuffer vertexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
  VkDeviceAddress vertexAddress = 0;
  VkBuffer indexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory indexMemory = VK_NULL_HANDLE;
  VkDeviceAddress indexAddress = 0;
  uint32_t vertexCount = 0;
  uint32_t indexCount = 0;
  uint32_t materialIndex = 0;
  uint32_t flags = 0; // RTGeometryFlags, forwarded to the closest-hit shader
  // One material index per triangle (RT_GEOM_PER_TRIANGLE_MATERIAL only).
  VkBuffer triMaterialBuffer = VK_NULL_HANDLE;
  VkDeviceMemory triMaterialMemory = VK_NULL_HANDLE;
  VkDeviceAddress triMaterialAddress = 0;
  // Per-geometry bottom-level AS (built in R4.2).
  RTAccelerationStructure blas;
};

struct RTMaterial {
  glm::vec3 baseColor;
  float metallic;
  glm::vec3 emissive;
  float roughness;
  glm::vec3 specular;
  float transmission;
  // Procedural surface pattern (matches the closest-hit `Material`):
  // x = CADMaterials::SurfacePattern, y = patternScale, z = patternContrast,
  // w = the pattern axis. std430: lands at offset 48.
  glm::vec4 procParams{0.0f};
  // Glass (transmission > 0): x = index of refraction; yzw unused. Offset 64,
  // making the struct 80 bytes — the SSBO upload is still a blind memcpy of
  // sizeof(RTMaterial), mirrored by `Material` in closesthit.rchit and
  // section.rahit.
  glm::vec4 optics{0.0f};
};
static_assert(sizeof(RTMaterial) == 80,
              "RTMaterial must match the shaders' std430 Material");

// Everything an offline still render needs from the app: output size, camera,
// and the shared IBL maps (same environment as the raster viewport, so both
// agree on lighting).
struct RTStillDesc {
  uint32_t width = 0;
  uint32_t height = 0;
  glm::mat4 view{1.0f};
  glm::mat4 proj{1.0f};
  glm::vec3 cameraPos{0.0f};
  VkImageView environment = VK_NULL_HANDLE; // full-res HDR (background)
  VkImageView irradiance = VK_NULL_HANDLE;
  VkImageView prefilter = VK_NULL_HANDLE;
  VkImageView brdfLut = VK_NULL_HANDLE;
  VkSampler envSampler = VK_NULL_HANDLE;
  VkSampler lutSampler = VK_NULL_HANDLE;
  uint32_t prefilterMips = 1;
  // How far a ray may travel before it counts as a miss, and how far an
  // ambient-occlusion ray reaches. Both were literals in the shaders — 10000
  // and 2.0 — and both carry a UNIT: CAD works in millimetres, so nothing
  // more than 10 m from the camera existed to the ray tracer, and AO on a
  // 3 m dock reached 2 mm. MakeStillDesc derives them from the scene.
  float rayFar = 10000.0f;
  float aoRadius = 2.0f;
  // A neutral sweep behind the model instead of the HDRI photograph. The
  // HDRI still lights it and still fills its reflections — it just stops
  // being the background.
  bool studioBackdrop = true;
  // Where the lights stand and which way the environment faces — the
  // viewport's own rig for the same camera (core/LightRig.h), so a still is
  // lit the way the screen it came from is. MakeStillDesc fills it.
  conv::LightRig rig = conv::ModelLightRig();
  uint32_t seed = 0; // RNG seed; fixed seed -> deterministic image
  // Edges over the traced image, the way the viewport draws them. Without
  // them a row of butted boards renders as one continuous slab -- the single
  // biggest reason a still read as softer than the screen it came from.
  bool outlines = true;
  glm::vec3 outlineColor{0.2f, 0.2f, 0.2f};
  float outlineWidth = 1.5f; // pixels, at the still's own resolution
  // Section cuts, from ui::SectionPlanes -- the same array main.cpp hands
  // every raster renderer, so a still and the viewport cannot disagree about
  // what has been cut away. A ray tracer cannot discard in closest-hit, so
  // these are applied by an any-hit shader (section.rahit).
  ClipPlaneArray clipPlanes = NoClipPlanes();
};

class RayTracingRenderer {
public:
  RayTracingRenderer();
  ~RayTracingRenderer();

  // Initialization and cleanup
  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkQueue graphicsQueue, uint32_t queueFamily,
                  VkCommandPool commandPool);
  void Shutdown();

  // RT capability checking
  static bool IsRayTracingSupported(VkPhysicalDevice physicalDevice);
  bool IsInitialized() const { return m_isInitialized; }

  // Geometry management
  uint32_t
  AddGeometry(const std::vector<Vertex3D>& vertices,
              const std::vector<uint32_t>& indices, uint32_t materialIndex = 0,
              uint32_t flags = 0,
              const std::vector<uint32_t>* triangleMaterials = nullptr);
  void UpdateGeometry(uint32_t geometryId,
                      const std::vector<Vertex3D>& vertices,
                      const std::vector<uint32_t>& indices);
  void RemoveGeometry(uint32_t geometryId);
  // Drops all geometry + acceleration structures (offline render rebuilds the
  // scene from scratch each time).
  void ClearGeometry();
  void ClearMaterials() {
    m_materials.clear();
    m_nextMaterialId = 0;
  }
  size_t GeometryCount() const { return m_geometries.size(); }
  bool HasTLAS() const { return m_topLevelAS.handle != VK_NULL_HANDLE; }

  // Material management
  uint32_t AddMaterial(const RTMaterial& material);
  void UpdateMaterial(uint32_t materialId, const RTMaterial& material);

  // Acceleration structure management
  bool BuildAccelerationStructures();

  // Rendering
  void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame,
              const glm::mat4& view, const glm::mat4& projection,
              const glm::vec3& cameraPos);

  // ---- Offline still render (R4.3-R4.5) ----
  // On-demand path-traced still into an offscreen rgba32f image, accumulated
  // over N samples and read back to a PNG. Independent of the raster viewport.
  // Requires acceleration structures already built
  // (BuildAccelerationStructures).
  //   BeginStill  -> create the output image, UBO, per-body SSBOs, descriptor
  //                  set, and clear the accumulation buffer.
  //   RenderBatch -> dispatch `samplesInBatch` traces (one command buffer,
  //                  submit + wait); call repeatedly until firstSample reaches
  //                  the total. Cheap to batch for progress/cancel.
  //   ReadbackStill -> divide by totalSamples, ACES tonemap + gamma, write PNG.
  //   EndStill    -> free the per-render resources.
  bool BeginStill(const RTStillDesc& desc);
  bool RenderBatch(uint32_t firstSample, uint32_t samplesInBatch);
  bool ReadbackStill(uint32_t totalSamples, const std::string& pngPath,
                     std::string* error = nullptr);
  void EndStill();
  bool IsStillActive() const { return m_stillActive; }

  // Settings
  void SetRenderMode(bool useRayTracing) { m_useRayTracing = useRayTracing; }
  bool GetRenderMode() const { return m_useRayTracing; }

  void SetMaxRayDepth(uint32_t depth) { m_maxRayDepth = depth; }
  void SetSamplesPerPixel(uint32_t samples) { m_samplesPerPixel = samples; }

private:
  // RT extension function pointers
  struct RTFunctions {
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR =
        nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR =
        nullptr;
    PFN_vkBuildAccelerationStructuresKHR vkBuildAccelerationStructuresKHR =
        nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR
        vkCmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR
        vkGetAccelerationStructureDeviceAddressKHR = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR
        vkGetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR
        vkGetRayTracingShaderGroupHandlesKHR = nullptr;
    PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = nullptr;
  };

  // Initialization helpers
  bool LoadRTFunctions();
  bool CheckRTProperties();
  bool CreateRTPipeline();
  bool CreateRTDescriptorSets();
  bool CreateShaderBindingTable();

  // Still-render helpers
  void DestroyStillResources();
  void TransitionImage(VkCommandBuffer cb, VkImage image,
                       VkImageLayout oldLayout, VkImageLayout newLayout,
                       VkPipelineStageFlags srcStage,
                       VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                       VkAccessFlags dstAccess);

  // Acceleration structure helpers
  bool CreateBottomLevelAS();
  bool CreateTopLevelAS();

  // Utility functions
  VkBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags properties,
                        VkDeviceMemory& bufferMemory);
  VkDeviceAddress GetBufferDeviceAddress(VkBuffer buffer);
  void DestroyAccelerationStructure(RTAccelerationStructure& as);

  // Vulkan objects
  VkDevice m_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkQueue m_graphicsQueue = VK_NULL_HANDLE;
  uint32_t m_queueFamily = 0;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;

  // RT-specific objects
  RTFunctions m_rtFunctions;
  VkPipeline m_rtPipeline = VK_NULL_HANDLE;
  VkPipelineLayout m_rtPipelineLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_rtDescriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_rtDescriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet m_rtDescriptorSet = VK_NULL_HANDLE;

  // Shader binding table
  VkBuffer m_shaderBindingTableBuffer = VK_NULL_HANDLE;
  VkDeviceMemory m_shaderBindingTableMemory = VK_NULL_HANDLE;
  VkStridedDeviceAddressRegionKHR m_raygenSBT{};
  VkStridedDeviceAddressRegionKHR m_missSBT{};
  VkStridedDeviceAddressRegionKHR m_hitSBT{};
  VkStridedDeviceAddressRegionKHR m_callableSBT{};

  // Acceleration structures
  RTAccelerationStructure m_bottomLevelAS;
  RTAccelerationStructure m_topLevelAS;

  // Geometry and materials
  std::vector<std::unique_ptr<RTGeometry>> m_geometries;
  std::vector<RTMaterial> m_materials;
  uint32_t m_nextGeometryId = 1;
  uint32_t m_nextMaterialId = 0;

  // RT properties
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_rtProperties{};

  // ---- Offline still-render resources (live between BeginStill/EndStill) ----
  bool m_stillActive = false;
  uint32_t m_stillWidth = 0;
  uint32_t m_stillHeight = 0;
  uint32_t m_stillSeed = 0;
  VkImage m_stillImage = VK_NULL_HANDLE; // rgba32f accumulation
  VkDeviceMemory m_stillImageMemory = VK_NULL_HANDLE;
  VkImageView m_stillImageView = VK_NULL_HANDLE;
  // Outline G-buffer (rgba32ui: packed face normal, hit distance, body id),
  // traced by primary-ray launches one strip of rows at a time, at
  // m_auxScale x the still so the edge mask can be averaged into coverage --
  // an aliased 1 px line is exactly the "cheap render" look this pass exists
  // to remove.
  VkImage m_auxImage = VK_NULL_HANDLE;
  VkDeviceMemory m_auxImageMemory = VK_NULL_HANDLE;
  VkImageView m_auxImageView = VK_NULL_HANDLE;
  uint32_t m_auxScale = 1;
  uint32_t m_auxStripRows = 1; // G-buffer rows per launch (the image's height)
  bool m_stillOutlines = true;
  bool m_stillHasClip = false; // any section plane live this render
  glm::vec3 m_stillOutlineColor{0.2f};
  float m_stillOutlineWidth = 1.5f;
  // Traces the outline G-buffer and returns the per-output-pixel edge
  // coverage in [0,1], or an empty vector when outlines are off.
  std::vector<float> TraceOutlineCoverage();
  VkBuffer m_stillUBO = VK_NULL_HANDLE; // camera + light UBO
  VkDeviceMemory m_stillUBOMemory = VK_NULL_HANDLE;
  VkBuffer m_stillGeomInfo = VK_NULL_HANDLE; // GeometryInfo[] SSBO
  VkDeviceMemory m_stillGeomInfoMemory = VK_NULL_HANDLE;
  VkBuffer m_stillMaterials = VK_NULL_HANDLE; // RTMaterial[] SSBO
  VkDeviceMemory m_stillMaterialsMemory = VK_NULL_HANDLE;

  // Settings
  bool m_isInitialized = false;
  bool m_useRayTracing = true;
  uint32_t m_maxRayDepth = 4;
  uint32_t m_samplesPerPixel = 1;
};