#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>

// Forward declarations
struct Vertex3D;

enum class OutlineMode {
    NONE,
    EDGES,      // Sharp edge detection
    WIREFRAME,  // Full wireframe
    SILHOUETTE  // Silhouette outline only
};

struct OutlineSettings {
    OutlineMode mode = OutlineMode::EDGES;
    glm::vec4 outlineColor = glm::vec4(0.2f, 0.2f, 0.2f, 1.0f);
    glm::vec4 wireframeColor = glm::vec4(0.4f, 0.4f, 0.4f, 1.0f);
    float outlineWidth = 1.0f;
    float lineWidth = 1.0f;
    bool enableDepthTest = true;
    bool enableBlending = true;
};

class OutlineRenderer {
public:
    OutlineRenderer();
    ~OutlineRenderer();

    // Initialization and cleanup
    bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice, 
                   VkRenderPass renderPass, VkQueue graphicsQueue, 
                   uint32_t queueFamily, VkCommandPool commandPool);
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
    void SetOutlineSettings(const OutlineSettings& settings) { m_settings = settings; }
    const OutlineSettings& GetOutlineSettings() const { return m_settings; }
    void SetViewportSize(uint32_t width, uint32_t height);
    
    bool IsInitialized() const { return m_isInitialized; }

private:
    struct OutlineMesh {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
        VkBuffer edgeIndexBuffer = VK_NULL_HANDLE;  // For edge-only rendering
        VkDeviceMemory edgeIndexBufferMemory = VK_NULL_HANDLE;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t edgeIndexCount = 0;
        glm::mat4 transform = glm::mat4(1.0f);
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
    };

    // Initialization helpers
    bool CreateOutlinePipeline();
    bool CreateWireframePipeline();
    bool CreateDescriptorSetLayout();
    bool CreateUniformBuffers();
    bool CreateDescriptorPool();
    bool CreateDescriptorSets();

    // Edge detection
    std::vector<uint32_t> ExtractEdges(const std::vector<Vertex3D>& vertices,
                                      const std::vector<uint32_t>& indices);
    
    // Rendering helpers
    void UpdateUniformBuffers(uint32_t currentFrame);
    void RenderOutlines(VkCommandBuffer commandBuffer, uint32_t currentFrame);
    void RenderWireframes(VkCommandBuffer commandBuffer, uint32_t currentFrame);

    // Utility functions
    VkBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, 
                         VkMemoryPropertyFlags properties, VkDeviceMemory& bufferMemory);
    void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    std::vector<char> ReadShaderFile(const std::string& filename);
    VkShaderModule CreateShaderModule(const std::vector<char>& code);
    std::string GetExecutableDirectory();

    // Vulkan objects
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
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

    // Viewport
    uint32_t m_viewportWidth = 1280;
    uint32_t m_viewportHeight = 720;

    // Settings
    OutlineSettings m_settings;
    
    // State
    bool m_isInitialized = false;
    static const int MAX_FRAMES_IN_FLIGHT = 2;
};