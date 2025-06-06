#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <memory>

struct Vertex3D {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
    
    static VkVertexInputBindingDescription getBindingDescription();
    static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions();
};

struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 lightDir;   // Light direction
    glm::vec4 lightColor; // Light color
    glm::vec4 viewPos;    // Camera position
};

class Mesh {
public:
    std::vector<Vertex3D> vertices;
    std::vector<uint32_t> indices;
    
    // Vulkan resources
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
    
    glm::mat4 modelMatrix = glm::mat4(1.0f);
    glm::vec3 color = glm::vec3(0.8f, 0.8f, 0.8f);
    
    bool isDirty = true;
};

class MeshRenderer {
public:
    MeshRenderer();
    ~MeshRenderer();
    
    bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice, 
                   VkRenderPass renderPass, VkQueue graphicsQueue, 
                   uint32_t queueFamily, VkCommandPool commandPool);
    void Shutdown();
    
    // Mesh management
    uint32_t AddMesh(const std::vector<Vertex3D>& vertices, const std::vector<uint32_t>& indices);
    void UpdateMesh(uint32_t meshId, const std::vector<Vertex3D>& vertices, const std::vector<uint32_t>& indices);
    void RemoveMesh(uint32_t meshId);
    void SetMeshTransform(uint32_t meshId, const glm::mat4& transform);
    void SetMeshColor(uint32_t meshId, const glm::vec3& color);
    
    // Rendering
    void UpdateCamera(const glm::mat4& view, const glm::mat4& projection);
    void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame);
    
    // Viewport
    void SetViewportSize(uint32_t width, uint32_t height);

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    
    // Pipeline
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
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
    
    // Meshes
    std::vector<std::unique_ptr<Mesh>> m_meshes;
    uint32_t m_nextMeshId = 1;
    
    // Camera
    glm::mat4 m_viewMatrix = glm::mat4(1.0f);
    glm::mat4 m_projectionMatrix = glm::mat4(1.0f);
    
    // Viewport
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
    
    bool CreateMeshBuffers(Mesh* mesh);
    void UpdateMeshBuffers(Mesh* mesh);
    void DestroyMeshBuffers(Mesh* mesh);
    
    VkShaderModule CreateShaderModule(const std::vector<char>& code);
    std::vector<char> ReadFile(const std::string& filename);
};