#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <memory>

struct Vertex3D {
    glm::vec3 position;
    glm::vec3 normal;

    static VkVertexInputBindingDescription getBindingDescription();
    static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions();
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
    glm::vec4 direction;  // xyz = direction, w = intensity
    glm::vec4 color;      // rgb = color, a = unused
};

// std140-clean by construction: mat4/vec4 members only, no scalars (the
// float-then-vec4 padding trap is documented in OutlineRenderer.h).
struct UniformBufferObject {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 viewPos;     // Camera position
    Light primaryLight;    // Main directional light
    Light fillLight;       // Fill light (opposite direction, lower intensity)
    Light rimLight;        // Rim light (side lighting)
    glm::vec4 ambientColor; // rgb = ambient color, a = intensity
    glm::vec4 clipPlane;   // section clip (world): xyz = normal, w = dist; 0 = off
};

// One tessellation, uploaded once and shared by any number of instances.
class MeshAsset {
public:
    std::vector<Vertex3D> vertices;   // CPU copy kept for updates/picking
    std::vector<uint32_t> indices;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;

    uint32_t refCount = 0;  // live instances referencing this asset
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

    // Face-highlight vertex ranges [start, end): x,y = selected; z,w = hovered
    glm::uvec4 highlight = glm::uvec4(0);

    bool visible = true;
};

class MeshRenderer {
public:
    MeshRenderer();
    ~MeshRenderer();

    bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                   VkRenderPass renderPass, VkQueue graphicsQueue,
                   uint32_t queueFamily, VkCommandPool commandPool,
                   VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void Shutdown();

    // Assets: shared tessellations (one GPU upload each)
    uint32_t CreateAsset(const std::vector<Vertex3D>& vertices, const std::vector<uint32_t>& indices);
    void UpdateAsset(uint32_t assetId, const std::vector<Vertex3D>& vertices, const std::vector<uint32_t>& indices);

    // Instances: drawn occurrences of an asset. Removing the last instance of
    // an asset destroys the asset's buffers too.
    uint32_t AddInstance(uint32_t assetId, const glm::mat4& transform = glm::mat4(1.0f));
    void RemoveInstance(uint32_t instanceId);
    void SetInstanceTransform(uint32_t instanceId, const glm::mat4& transform);
    void SetInstanceColor(uint32_t instanceId, const glm::vec3& albedo);
    // Albedo + PBR params in one shot. params is (metallic, roughness,
    // specular, reflectance).
    void SetInstanceMaterial(uint32_t instanceId, const glm::vec3& albedo,
                             const glm::vec4& params);
    void SetInstanceVisible(uint32_t instanceId, bool visible);

    // Transitional one-body-one-mesh API (asset + instance in one shot);
    // call sites migrate to the split API with assemblies A2.
    uint32_t AddMesh(const std::vector<Vertex3D>& vertices, const std::vector<uint32_t>& indices);
    void UpdateMesh(uint32_t instanceId, const std::vector<Vertex3D>& vertices, const std::vector<uint32_t>& indices);
    void RemoveMesh(uint32_t instanceId) { RemoveInstance(instanceId); }
    void SetMeshTransform(uint32_t instanceId, const glm::mat4& transform) { SetInstanceTransform(instanceId, transform); }
    void SetMeshColor(uint32_t instanceId, const glm::vec3& color) { SetInstanceColor(instanceId, color); }
    void SetMeshMaterial(uint32_t instanceId, const glm::vec3& albedo,
                         const glm::vec4& params) { SetInstanceMaterial(instanceId, albedo, params); }

    // Face highlighting (vertex ranges within the instance's asset)
    void SetFaceHighlight(uint32_t instanceId, uint32_t firstVertex, uint32_t count);
    void SetFaceHover(uint32_t instanceId, uint32_t firstVertex, uint32_t count);
    void ClearFaceHighlight();
    void ClearFaceHover();

    // Rendering
    void UpdateCamera(const glm::mat4& view, const glm::mat4& projection);
    void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame);

    // Viewport
    void SetViewportSize(uint32_t width, uint32_t height) { SetViewport(0, 0, width, height); }

    // Section clip plane (world space): xyz = normal, w = distance; fragments
    // past it are discarded. Zero vector = no clipping (the default).
    void SetClipPlane(const glm::vec4& plane) { m_clipPlane = plane; }
    void SetViewport(int32_t x, int32_t y, uint32_t width, uint32_t height);

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

private:
    glm::vec4 m_clipPlane = glm::vec4(0.0f); // section view; 0 = disabled
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

    // Camera
    glm::mat4 m_viewMatrix = glm::mat4(1.0f);
    glm::mat4 m_projectionMatrix = glm::mat4(1.0f);

    // Viewport
    int32_t m_viewportX = 0;
    int32_t m_viewportY = 0;
    uint32_t m_viewportWidth = 800;
    uint32_t m_viewportHeight = 600;

    // Constants
    static const int MAX_FRAMES_IN_FLIGHT = 2;

    // Helper functions
    bool CreateDescriptorSetLayout();
    bool CreateGraphicsPipeline();
    bool CreateUniformBuffers();
    bool CreateDescriptorPool();
    bool CreateDescriptorSets();

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
    void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    MeshAsset* GetAsset(uint32_t assetId);
    MeshInstance* GetInstance(uint32_t instanceId);
    bool CreateAssetBuffers(MeshAsset* asset);
    void DestroyAssetBuffers(MeshAsset* asset);

    VkShaderModule CreateShaderModule(const std::vector<char>& code);
    std::vector<char> ReadFile(const std::string& filename);
};
