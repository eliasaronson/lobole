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

struct Light {
    glm::vec4 direction;  // xyz = direction, w = intensity
    glm::vec4 color;      // rgb = color, a = unused
};

struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 viewPos;     // Camera position
    Light primaryLight;    // Main directional light
    Light fillLight;       // Fill light (opposite direction, lower intensity)
    Light rimLight;        // Rim light (side lighting)
    glm::vec4 ambientColor; // rgb = ambient color, a = intensity
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

    // PBR params passed to the fragment shader via push constants:
    // x = metallic, y = roughness, z = specular, w = dielectric reflectance.
    // Default = neutral dielectric (matte, non-metal) so unassigned bodies read
    // predictably instead of being guessed from albedo.
    glm::vec4 materialParams = glm::vec4(0.0f, 0.5f, 0.5f, 0.04f);

    // Face-highlight vertex ranges [start, end), passed via push constants
    glm::uvec4 highlight = glm::uvec4(0); // x,y = selected; z,w = hovered

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
    // Sets albedo (as vertex color) + PBR params in one shot. params is
    // (metallic, roughness, specular, reflectance).
    void SetMeshMaterial(uint32_t meshId, const glm::vec3& albedo,
                         const glm::vec4& params);

    // Face highlighting (vertex ranges within the mesh's vertex buffer)
    void SetFaceHighlight(uint32_t meshId, uint32_t firstVertex, uint32_t count);
    void SetFaceHover(uint32_t meshId, uint32_t firstVertex, uint32_t count);
    void ClearFaceHighlight();
    void ClearFaceHover();
    
    // Rendering
    void UpdateCamera(const glm::mat4& view, const glm::mat4& projection);
    void Render(VkCommandBuffer commandBuffer, uint32_t currentFrame);
    
    // Viewport
    void SetViewportSize(uint32_t width, uint32_t height) { SetViewport(0, 0, width, height); }
    void SetViewport(int32_t x, int32_t y, uint32_t width, uint32_t height);

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
    
    bool CreateMeshBuffers(Mesh* mesh);
    void UpdateMeshBuffers(Mesh* mesh);
    void DestroyMeshBuffers(Mesh* mesh);
    
    VkShaderModule CreateShaderModule(const std::vector<char>& code);
    std::vector<char> ReadFile(const std::string& filename);
};