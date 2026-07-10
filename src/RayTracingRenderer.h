#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>

// Forward declarations
struct Vertex3D;
class Mesh;

struct RTAccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress deviceAddress = 0;
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
    uint32_t AddGeometry(const std::vector<Vertex3D>& vertices, 
                        const std::vector<uint32_t>& indices,
                        uint32_t materialIndex = 0);
    void UpdateGeometry(uint32_t geometryId, const std::vector<Vertex3D>& vertices,
                       const std::vector<uint32_t>& indices);
    void RemoveGeometry(uint32_t geometryId);
    // Drops all geometry + acceleration structures (offline render rebuilds the
    // scene from scratch each time).
    void ClearGeometry();
    void ClearMaterials() { m_materials.clear(); m_nextMaterialId = 0; }
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

    // Settings
    void SetRenderMode(bool useRayTracing) { m_useRayTracing = useRayTracing; }
    bool GetRenderMode() const { return m_useRayTracing; }
    
    void SetMaxRayDepth(uint32_t depth) { m_maxRayDepth = depth; }
    void SetSamplesPerPixel(uint32_t samples) { m_samplesPerPixel = samples; }

private:
    // RT extension function pointers
    struct RTFunctions {
        PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
        PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
        PFN_vkBuildAccelerationStructuresKHR vkBuildAccelerationStructuresKHR = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
        PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR = nullptr;
        PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR = nullptr;
        PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = nullptr;
    };

    // Initialization helpers
    bool LoadRTFunctions();
    bool CheckRTProperties();
    bool CreateRTPipeline();
    bool CreateRTDescriptorSets();
    bool CreateShaderBindingTable();

    // Acceleration structure helpers
    bool CreateBottomLevelAS();
    bool CreateTopLevelAS();
    
    // Utility functions
    VkBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags properties, VkDeviceMemory& bufferMemory);
    VkDeviceAddress GetBufferDeviceAddress(VkBuffer buffer);
    void DestroyAccelerationStructure(RTAccelerationStructure& as);
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer cb);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    std::vector<char> ReadShaderFile(const std::string& filename);
    VkShaderModule CreateShaderModule(const std::vector<char>& code);
    std::string GetExecutableDirectory();

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
    
    // Settings
    bool m_isInitialized = false;
    bool m_useRayTracing = true;
    uint32_t m_maxRayDepth = 4;
    uint32_t m_samplesPerPixel = 1;
};