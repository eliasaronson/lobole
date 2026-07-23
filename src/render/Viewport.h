#pragma once

#include "imgui.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <vector>

class Camera;
class Scene;
class MeshRenderer;
class CADObject;

class Viewport {
public:
    Viewport();
    ~Viewport();

    bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, 
                   uint32_t queueFamily, VkCommandPool commandPool);
    void Shutdown();

    void Update(float deltaTime);
    void Render(Camera* camera, Scene* scene);
    void RenderUI();

    // Viewport properties
    void SetSize(int width, int height);
    ImVec2 GetSize() const { return m_size; }
    
    bool IsMouseOver() const { return m_mouseOver; }
    bool IsFocused() const { return m_focused; }

private:
    void CreateFramebuffer();
    void DestroyFramebuffer();
    void ResizeFramebuffer(int width, int height);

    // Viewport state
    ImVec2 m_size;
    ImVec2 m_position;
    bool m_mouseOver;
    bool m_focused;

    // Vulkan resources for offscreen rendering
    VkDevice m_device;
    VkPhysicalDevice m_physicalDevice;
    VkQueue m_queue;
    uint32_t m_queueFamily;
    VkCommandPool m_commandPool;
    
    VkImage m_colorImage;
    VkDeviceMemory m_colorImageMemory;
    VkImageView m_colorImageView;
    VkImage m_depthImage;
    VkDeviceMemory m_depthImageMemory;
    VkImageView m_depthImageView;
    
    VkRenderPass m_renderPass;
    VkFramebuffer m_framebuffer;
    VkCommandBuffer m_commandBuffer;
    
    // ImGui texture for displaying the viewport
    VkDescriptorSet m_descriptorSet;
    
    // 3D Rendering
    std::unique_ptr<MeshRenderer> m_meshRenderer;
    
    // Object management
    void UpdateSceneObjects(Scene* scene);
    void AddCADObject(CADObject* cadObject);
    void RemoveCADObject(CADObject* cadObject);
    std::vector<uint32_t> m_objectMeshIds;
    
    // Grid rendering
    void RenderGrid(Camera* camera);
    bool m_showGrid;
    float m_gridSize;
    int m_gridSubdivisions;
};