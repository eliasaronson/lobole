#pragma once

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include <GLFW/glfw3.h>
#include <memory>

class Scene;
class Camera;

class SimpleApp {
public:
    SimpleApp();
    ~SimpleApp();

    bool Initialize();
    void Run();
    void Shutdown();

private:
    void SetupVulkan();
    void SetupImGui();
    void SetupWindow();
    void CleanupVulkan();
    void CleanupImGui();
    void CleanupVulkanWindow();
    
    void Update();
    void Render();
    void RenderUI();
    void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data);
    void FramePresent(ImGui_ImplVulkanH_Window* wd);

    GLFWwindow* m_window;
    
    // Vulkan objects (copied from working example)
    VkAllocationCallbacks* m_allocator;
    VkInstance m_instance;
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
    uint32_t m_queueFamily;
    VkQueue m_queue;
    VkPipelineCache m_pipelineCache;
    VkDescriptorPool m_descriptorPool;
    
    // ImGui window data
    ImGui_ImplVulkanH_Window m_mainWindowData;
    uint32_t m_minImageCount;
    bool m_swapChainRebuild;

    // Application components
    std::unique_ptr<Scene> m_scene;
    std::unique_ptr<Camera> m_camera;

    // Application state
    bool m_running;
    float m_deltaTime;
    float m_lastFrameTime;
};