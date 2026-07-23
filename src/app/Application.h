#pragma once

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include <GLFW/glfw3.h>
#include <memory>

class Scene;
class Camera;
class Viewport;

class Application {
public:
    Application();
    ~Application();

    bool Initialize();
    void Run();
    void Shutdown();

private:
    void SetupVulkan();
    void SetupImGui();
    void SetupWindow();
    void CleanupVulkan();
    void CleanupImGui();
    
    void Update();
    void Render();
    void RenderUI();

    GLFWwindow* m_window;
    
    // Vulkan objects
    VkInstance m_instance;
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
    uint32_t m_queueFamily;
    VkQueue m_queue;
    VkDescriptorPool m_descriptorPool;
    VkAllocationCallbacks* m_allocator;
    
    // ImGui window data
    ImGui_ImplVulkanH_Window m_mainWindowData;
    uint32_t m_minImageCount;
    bool m_swapChainRebuild;

    // Application components
    std::unique_ptr<Scene> m_scene;
    std::unique_ptr<Camera> m_camera;
    std::unique_ptr<Viewport> m_viewport;

    // Application state
    bool m_running;
    float m_deltaTime;
    float m_lastFrameTime;
};