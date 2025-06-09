#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>

struct Vertex3D;
struct Face;

class FaceHighlighter {
public:
    FaceHighlighter();
    ~FaceHighlighter();

    // Initialization
    bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice, 
                   VkRenderPass renderPass, VkQueue graphicsQueue, 
                   uint32_t queueFamily, VkCommandPool commandPool);
    void Shutdown();

    // Highlighting
    void SetHoveredFace(const Face* face, const std::vector<Vertex3D>& vertices);
    void SetSelectedFace(const Face* face, const std::vector<Vertex3D>& vertices);
    void ClearHover();
    void ClearSelection();

    // Camera and rendering
    void UpdateCamera(const glm::mat4& view, const glm::mat4& projection);
    void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame);
    void SetViewportSize(uint32_t width, uint32_t height);

    bool IsInitialized() const { return m_isInitialized; }

private:
    struct HighlightMesh {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        glm::mat4 transform = glm::mat4(1.0f);
        glm::vec4 color = glm::vec4(1.0f);
        bool active = false;
    };

    struct HighlightUniformBufferObject {
        glm::mat4 model;
        glm::mat4 view;
        glm::mat4 proj;
        glm::vec4 color;
    };

    // Pipeline creation
    bool CreatePipeline();
    bool CreateDescriptorSetLayout();
    bool CreateUniformBuffers();
    bool CreateDescriptorPool();
    bool CreateDescriptorSets();

    // Mesh utilities
    void UpdateHighlightMesh(HighlightMesh& mesh, const Face* face, 
                            const std::vector<Vertex3D>& vertices, const glm::vec4& color);
    
    // Vulkan utilities
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
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;

    // Descriptor objects
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;

    // Uniform buffers
    std::vector<VkBuffer> m_uniformBuffers;
    std::vector<VkDeviceMemory> m_uniformBuffersMemory;

    // Highlight meshes
    HighlightMesh m_hoverMesh;
    HighlightMesh m_selectionMesh;

    // Camera data
    glm::mat4 m_viewMatrix = glm::mat4(1.0f);
    glm::mat4 m_projectionMatrix = glm::mat4(1.0f);

    // Viewport
    uint32_t m_viewportWidth = 1280;
    uint32_t m_viewportHeight = 720;

    // State
    bool m_isInitialized = false;
    static const int MAX_FRAMES_IN_FLIGHT = 2;
};