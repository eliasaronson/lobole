#include "Application.h"
#include "Scene.h"
#include "Camera.h"
#include "Viewport.h"
#include <stdio.h>
#include <stdlib.h>
#include <iostream>

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
#endif

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS) return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0) abort();
}

Application::Application()
    : m_window(nullptr)
    , m_instance(VK_NULL_HANDLE)
    , m_physicalDevice(VK_NULL_HANDLE)
    , m_device(VK_NULL_HANDLE)
    , m_queueFamily((uint32_t)-1)
    , m_queue(VK_NULL_HANDLE)
    , m_descriptorPool(VK_NULL_HANDLE)
    , m_allocator(nullptr)
    , m_minImageCount(2)
    , m_swapChainRebuild(false)
    , m_running(false)
    , m_deltaTime(0.0f)
    , m_lastFrameTime(0.0f) {
}

Application::~Application() {
    Shutdown();
}

bool Application::Initialize() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return false;
    }

    SetupWindow();
    SetupVulkan();
    SetupImGui();

    // Initialize application components
    m_scene = std::make_unique<Scene>();
    m_camera = std::make_unique<Camera>();
    m_viewport = std::make_unique<Viewport>();

    if (!m_viewport->Initialize(m_device, m_physicalDevice, m_queue, m_queueFamily)) {
        std::cerr << "Failed to initialize viewport" << std::endl;
        return false;
    }

    m_running = true;
    return true;
}

void Application::Run() {
    while (m_running && !glfwWindowShouldClose(m_window)) {
        float currentTime = (float)glfwGetTime();
        m_deltaTime = currentTime - m_lastFrameTime;
        m_lastFrameTime = currentTime;

        glfwPollEvents();

        Update();
        Render();
    }
}

void Application::Shutdown() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }

    m_viewport.reset();
    m_camera.reset();
    m_scene.reset();

    CleanupImGui();
    CleanupVulkan();

    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }

    glfwTerminate();
}

void Application::SetupWindow() {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(1280, 720, "Designer - 3D CAD Software", nullptr, nullptr);
    
    if (!glfwVulkanSupported()) {
        fprintf(stderr, "GLFW: Vulkan Not Supported\n");
        exit(1);
    }
}

void Application::SetupVulkan() {
    // This is a simplified version - full implementation would include all Vulkan setup
    // For now, we'll initialize basic structures
}

void Application::SetupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    // This would need proper Vulkan initialization
    // ImGui_ImplGlfw_InitForVulkan(m_window, true);
    // ImGui_ImplVulkan_Init(&init_info);
}

void Application::CleanupVulkan() {
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, m_allocator);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, m_allocator);
        m_device = VK_NULL_HANDLE;
    }

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, m_allocator);
        m_instance = VK_NULL_HANDLE;
    }
}

void Application::CleanupImGui() {
    // ImGui_ImplVulkan_Shutdown();
    // ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void Application::Update() {
    if (m_camera) {
        m_camera->Update(m_deltaTime);
    }

    if (m_scene) {
        m_scene->Update(m_deltaTime);
    }

    if (m_viewport) {
        m_viewport->Update(m_deltaTime);
    }
}

void Application::Render() {
    // Handle window resize
    int fb_width, fb_height;
    glfwGetFramebufferSize(m_window, &fb_width, &fb_height);
    if (fb_width > 0 && fb_height > 0 && (m_swapChainRebuild || m_mainWindowData.Width != fb_width || m_mainWindowData.Height != fb_height)) {
        // Recreate swapchain
        m_swapChainRebuild = false;
    }

    if (glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) != 0) {
        return;
    }

    // Start ImGui frame
    // ImGui_ImplVulkan_NewFrame();
    // ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    RenderUI();

    // Render viewport
    if (m_viewport && m_camera && m_scene) {
        m_viewport->Render(m_camera.get(), m_scene.get());
    }

    // Render ImGui
    ImGui::Render();
    
    // Present frame would happen here
}

void Application::RenderUI() {
    // Main menu bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New")) {}
            if (ImGui::MenuItem("Open")) {}
            if (ImGui::MenuItem("Save")) {}
            if (ImGui::MenuItem("Exit")) {
                m_running = false;
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo")) {}
            if (ImGui::MenuItem("Redo")) {}
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset Camera")) {
                if (m_camera) {
                    m_camera->SetPosition(glm::vec3(5.0f, 5.0f, 5.0f));
                    m_camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
                }
            }
            ImGui::EndMenu();
        }
        
        ImGui::EndMainMenuBar();
    }

    // Render viewport UI
    if (m_viewport) {
        m_viewport->RenderUI();
    }

    // Properties panel
    ImGui::Begin("Properties");
    ImGui::Text("Scene Objects: %zu", m_scene ? m_scene->GetObjects().size() : 0);
    if (m_camera) {
        glm::vec3 pos = m_camera->GetPosition();
        ImGui::Text("Camera Position: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
        ImGui::Text("Camera Distance: %.2f", m_camera->GetDistance());
    }
    ImGui::End();

    // Scene hierarchy
    ImGui::Begin("Scene");
    if (m_scene) {
        for (const auto& object : m_scene->GetObjects()) {
            ImGui::Text("%s (ID: %u)", object->GetName().c_str(), object->GetId());
        }
    }
    ImGui::End();
}