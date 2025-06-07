// CAD Application with working Vulkan + ImGui setup
// Based on Dear ImGui GLFW+Vulkan example

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "Scene.h"
#include "Camera.h"
#include "CADObject.h"
#include "MeshRenderer.h"
#include "CADMeshConverter.h"
#include "SketchManager.h"
#include <stdio.h>
#include <stdlib.h>
#include <memory>
#include <filesystem>
#include <algorithm>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
#endif

// Vulkan data
static VkAllocationCallbacks*   g_Allocator = nullptr;
static VkInstance               g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily = (uint32_t)-1;
static VkQueue                  g_Queue = VK_NULL_HANDLE;
static VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;
static VkCommandPool            g_CommandPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t                 g_MinImageCount = 2;
static bool                     g_SwapChainRebuild = false;

// Application objects
static std::unique_ptr<Scene> g_Scene;
static std::unique_ptr<Camera> g_Camera;
static std::unique_ptr<MeshRenderer> g_MeshRenderer;
static std::unique_ptr<SketchManager> g_SketchManager;

// File browser state
static bool g_ShowFileBrowser = false;
static std::string g_CurrentPath = "../CAD_files";
static std::vector<std::string> g_DirectoryContents;

// Forward declarations
void LoadSTEPFile(const std::string& filename);

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    if (g_SketchManager && g_SketchManager->IsSketchMode()) {
        // In sketch mode, handle sketching
        if (g_Camera) {
            g_SketchManager->OnMouseMove(xpos, ypos, *g_Camera);
        }
    } else {
        // In 3D mode, handle camera
        if (g_Camera) {
            g_Camera->OnMouseMove(xpos, ypos);
        }
    }
}

static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (g_SketchManager && g_SketchManager->IsSketchMode()) {
        // In sketch mode, handle sketching
        if (action == GLFW_PRESS && g_Camera) {
            double xpos, ypos;
            glfwGetCursorPos(window, &xpos, &ypos);
            g_SketchManager->OnMouseClick(xpos, ypos, *g_Camera);
        }
    } else {
        // In 3D mode, handle camera
        if (g_Camera) {
            g_Camera->OnMouseButton(button, action, mods);
        }
    }
}

static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    if (g_Camera) {
        g_Camera->OnScroll(xoffset, yoffset);
    }
}

static void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS) return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0) abort();
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData) {
    (void)flags; (void)object; (void)location; (void)messageCode; (void)pUserData; (void)pLayerPrefix;
    fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
    return VK_FALSE;
}
#endif

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension) {
    for (const VkExtensionProperties& p : properties)
        if (strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

static void SetupVulkan(ImVector<const char*> instance_extensions) {
    VkResult err;

    // Create Vulkan Instance
    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        // Enumerate available extensions
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        check_vk_result(err);

        // Enable required extensions
        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = debug_report;
        debug_report_ci.pUserData = nullptr;
        err = f_vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
        check_vk_result(err);
#endif
    }

    // Select Physical Device (GPU)
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    // Select graphics queue family
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);

    // Create Logical Device
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
    }

    // Create Descriptor Pool
    {
        VkDescriptorPoolSize pool_sizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
    
    // Create Command Pool
    {
        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = g_QueueFamily;
        err = vkCreateCommandPool(g_Device, &pool_info, g_Allocator, &g_CommandPool);
        check_vk_result(err);
    }
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height) {
    wd->Surface = surface;

    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE) {
        fprintf(stderr, "Error no WSI support on physical device 0\n");
        exit(-1);
    }

    const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));

    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
}

static void CleanupVulkan() {
    vkDestroyCommandPool(g_Device, g_CommandPool, g_Allocator);
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif

    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow() {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        check_vk_result(err);

        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    
    // Render 3D meshes
    if (g_MeshRenderer) {
        g_MeshRenderer->Render(fd->CommandBuffer, wd->FrameIndex);
    }

    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild) return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

void UpdateDirectoryContents() {
    g_DirectoryContents.clear();
    
    try {
        if (std::filesystem::exists(g_CurrentPath) && std::filesystem::is_directory(g_CurrentPath)) {
            // Add parent directory entry if not at root
            if (g_CurrentPath != "." && g_CurrentPath != "/" && g_CurrentPath.find("..") == std::string::npos) {
                g_DirectoryContents.push_back("..");
            }
            
            for (const auto& entry : std::filesystem::directory_iterator(g_CurrentPath)) {
                std::string filename = entry.path().filename().string();
                
                if (entry.is_directory()) {
                    g_DirectoryContents.push_back("[DIR] " + filename);
                } else {
                    // Show all files, highlight CAD files
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    
                    if (ext == ".step" || ext == ".stp" || ext == ".iges" || ext == ".igs") {
                        g_DirectoryContents.push_back("[CAD] " + filename);
                    } else {
                        g_DirectoryContents.push_back(filename);
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error reading directory '" << g_CurrentPath << "': " << e.what() << std::endl;
        g_DirectoryContents.push_back("Error reading directory");
    }
    
    // Sort contents
    std::sort(g_DirectoryContents.begin(), g_DirectoryContents.end());
}

void RenderFileBrowser() {
    if (!g_ShowFileBrowser) return;
    
    ImGui::Begin("Open STEP File", &g_ShowFileBrowser, ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::Text("Current Path: %s", g_CurrentPath.c_str());
    ImGui::Separator();
    
    if (ImGui::Button("Refresh")) {
        UpdateDirectoryContents();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        g_ShowFileBrowser = false;
    }
    
    ImGui::Separator();
    
    // File list
    if (ImGui::BeginListBox("##files", ImVec2(400, 300))) {
        for (const auto& item : g_DirectoryContents) {
            bool isSelected = false;
            
            if (ImGui::Selectable(item.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (item == "..") {
                        // Go to parent directory
                        try {
                            std::filesystem::path currentPath(g_CurrentPath);
                            std::filesystem::path parentPath = currentPath.parent_path();
                            if (!parentPath.empty()) {
                                g_CurrentPath = parentPath.string();
                                // Normalize the path to avoid issues
                                g_CurrentPath = std::filesystem::weakly_canonical(g_CurrentPath).string();
                            } else {
                                g_CurrentPath = ".";
                            }
                            UpdateDirectoryContents();
                        } catch (const std::exception& e) {
                            std::cout << "Error navigating to parent directory: " << e.what() << std::endl;
                        }
                    } else if (item.starts_with("[DIR]")) {
                        // Enter directory
                        try {
                            std::string dirName = item.substr(6); // Remove "[DIR] " prefix
                            std::filesystem::path newPath = std::filesystem::path(g_CurrentPath) / dirName;
                            g_CurrentPath = std::filesystem::weakly_canonical(newPath).string();
                            UpdateDirectoryContents();
                        } catch (const std::exception& e) {
                            std::cout << "Error entering directory: " << e.what() << std::endl;
                        }
                    } else if (item.starts_with("[CAD]")) {
                        // Load CAD file
                        std::string fileName = item.substr(6); // Remove "[CAD] " prefix
                        std::string fullPath = (std::filesystem::path(g_CurrentPath) / fileName).string();
                        std::cout << "Loading selected file: " << fullPath << std::endl;
                        LoadSTEPFile(fullPath);
                        g_ShowFileBrowser = false;
                    }
                }
            }
        }
        ImGui::EndListBox();
    }
    
    ImGui::Text("Double-click directories to enter, CAD files to load");
    
    ImGui::End();
}

void LoadSTEPFile(const std::string& filename) {
    if (!g_Scene || !g_MeshRenderer) return;
    
    std::cout << "Loading STEP file: " << filename << std::endl;
    
    // Use OpenCASCADE to load STEP file
    std::vector<CADShape> shapes;
    bool success = CADKernel::Instance().LoadSTEP(filename, shapes);
    
    if (!success) {
        std::cout << "Failed to load STEP file: " << filename << std::endl;
        return;
    }
    
    std::cout << "Loaded " << shapes.size() << " shapes from STEP file" << std::endl;
    
    // Convert each shape to a mesh and add to renderer (limit to first 20 shapes for performance)
    int shapeIndex = 0;
    int maxShapes = std::min((int)shapes.size(), 20);
    std::cout << "Processing first " << maxShapes << " shapes..." << std::endl;
    
    for (int i = 0; i < maxShapes; i++) {
        const auto& shape = shapes[i];
        std::cout << "Processing shape " << (i + 1) << "/" << maxShapes << "..." << std::endl;
        if (shape.IsValid() && shape.Volume() > 1e-6) {
            // Generate mesh from the shape
            CADShape::Mesh cadMesh = shape.GenerateMesh(0.5); // Good balance of quality and performance
            
            if (!cadMesh.vertices.empty()) {
                // Convert to renderer format with aluminum-like colors
                glm::vec3 color;
                if (shapeIndex == 0) {
                    color = glm::vec3(0.8f, 0.85f, 0.9f); // Light aluminum
                } else {
                    color = glm::vec3(0.7f + 0.2f * (shapeIndex % 3) / 3.0f, 
                                    0.7f + 0.2f * ((shapeIndex + 1) % 3) / 3.0f, 
                                    0.8f + 0.2f * ((shapeIndex + 2) % 3) / 3.0f); // Varied metallic colors
                }
                std::vector<Vertex3D> vertices = CADMeshConverter::ConvertCADMeshToVertices(cadMesh, color);
                uint32_t meshId = g_MeshRenderer->AddMesh(vertices, cadMesh.indices);
                
                // Create a CAD object for the shape
                auto cadObject = std::make_unique<CADObject>("STEP Shape " + std::to_string(shapeIndex + 1), shape);
                g_Scene->AddObject(std::move(cadObject));
                
                std::cout << "Added shape " << shapeIndex + 1 << " with " << cadMesh.vertices.size() 
                         << " vertices and " << cadMesh.indices.size() / 3 << " triangles (Mesh ID: " << meshId << ")" << std::endl;
                shapeIndex++;
            }
        }
    }
    
    if (shapeIndex > 0) {
        std::cout << "Successfully loaded " << shapeIndex << " shapes from STEP file" << std::endl;
    } else {
        std::cout << "No valid shapes found in STEP file" << std::endl;
    }
}

void RenderCADUI() {
    // Main menu bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New")) {}
            
            if (ImGui::MenuItem("Open STEP File...")) {
                g_ShowFileBrowser = true;
                UpdateDirectoryContents();
            }
            
            if (ImGui::MenuItem("Save")) {}
            if (ImGui::MenuItem("Exit")) {
                glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo")) {}
            if (ImGui::MenuItem("Redo")) {}
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Create")) {
            // Sketch options
            if (ImGui::BeginMenu("Sketch")) {
                if (ImGui::MenuItem("Sketch on Front Plane")) {
                    if (g_SketchManager) {
                        g_SketchManager->StartSketch(PlaneType::XY);
                        std::cout << "Started sketch on Front Plane" << std::endl;
                    }
                }
                if (ImGui::MenuItem("Sketch on Top Plane")) {
                    if (g_SketchManager) {
                        g_SketchManager->StartSketch(PlaneType::XZ);
                        std::cout << "Started sketch on Top Plane" << std::endl;
                    }
                }
                if (ImGui::MenuItem("Sketch on Right Plane")) {
                    if (g_SketchManager) {
                        g_SketchManager->StartSketch(PlaneType::YZ);
                        std::cout << "Started sketch on Right Plane" << std::endl;
                    }
                }
                
                ImGui::Separator();
                
                if (ImGui::MenuItem("Exit Sketch", nullptr, false, g_SketchManager && g_SketchManager->IsSketchMode())) {
                    if (g_SketchManager) {
                        g_SketchManager->ExitSketch();
                        std::cout << "Exited sketch mode" << std::endl;
                    }
                }
                
                ImGui::EndMenu();
            }
            
            ImGui::Separator();
            
            // Legacy primitive creation (for comparison)
            if (ImGui::BeginMenu("Primitives (Legacy)")) {
            if (ImGui::MenuItem("Box")) {
                if (g_Scene && g_MeshRenderer) {
                    int objectCount = g_Scene->GetObjects().size();
                    auto box = CADObject::CreateBox("Box " + std::to_string(objectCount + 1), 2.0, 2.0, 2.0);
                    
                    // Convert CAD shape to mesh and add to renderer
                    if (box->GetVolume() > 0.0) {
                        CADShape::Mesh cadMesh = box->GetShape().GenerateMesh(0.1);
                        if (!cadMesh.vertices.empty()) {
                            std::vector<Vertex3D> vertices = CADMeshConverter::ConvertCADMeshToVertices(cadMesh, glm::vec3(0.6f, 0.8f, 1.0f));
                            uint32_t meshId = g_MeshRenderer->AddMesh(vertices, cadMesh.indices);
                            
                            // Position objects at different locations
                            glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(-4.0f + objectCount * 2.0f, 0.0f, 0.0f));
                            g_MeshRenderer->SetMeshTransform(meshId, transform);
                            
                            std::cout << "Added box mesh with ID: " << meshId << std::endl;
                        }
                    }
                    
                    g_Scene->AddObject(std::move(box));
                }
            }
            if (ImGui::MenuItem("Sphere")) {
                if (g_Scene && g_MeshRenderer) {
                    int objectCount = g_Scene->GetObjects().size();
                    auto sphere = CADObject::CreateSphere("Sphere " + std::to_string(objectCount + 1), 
                                                         glm::vec3(0, 0, 0), 1.0);
                    
                    // Convert CAD shape to mesh and add to renderer
                    if (sphere->GetVolume() > 0.0) {
                        CADShape::Mesh cadMesh = sphere->GetShape().GenerateMesh(0.1);
                        if (!cadMesh.vertices.empty()) {
                            std::vector<Vertex3D> vertices = CADMeshConverter::ConvertCADMeshToVertices(cadMesh, glm::vec3(1.0f, 0.6f, 0.6f));
                            uint32_t meshId = g_MeshRenderer->AddMesh(vertices, cadMesh.indices);
                            
                            // Position objects at different locations
                            glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(-4.0f + objectCount * 2.0f, 0.0f, 2.0f));
                            g_MeshRenderer->SetMeshTransform(meshId, transform);
                            
                            std::cout << "Added sphere mesh with ID: " << meshId << std::endl;
                        }
                    }
                    
                    g_Scene->AddObject(std::move(sphere));
                }
            }
            if (ImGui::MenuItem("Cylinder")) {
                if (g_Scene && g_MeshRenderer) {
                    int objectCount = g_Scene->GetObjects().size();
                    auto cylinder = CADObject::CreateCylinder("Cylinder " + std::to_string(objectCount + 1),
                                                            glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 1.0, 2.0);
                    
                    // Convert CAD shape to mesh and add to renderer
                    if (cylinder->GetVolume() > 0.0) {
                        CADShape::Mesh cadMesh = cylinder->GetShape().GenerateMesh(0.1);
                        if (!cadMesh.vertices.empty()) {
                            std::vector<Vertex3D> vertices = CADMeshConverter::ConvertCADMeshToVertices(cadMesh, glm::vec3(0.6f, 1.0f, 0.6f));
                            uint32_t meshId = g_MeshRenderer->AddMesh(vertices, cadMesh.indices);
                            
                            // Position objects at different locations
                            glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(-4.0f + objectCount * 2.0f, 0.0f, -2.0f));
                            g_MeshRenderer->SetMeshTransform(meshId, transform);
                            
                            std::cout << "Added cylinder mesh with ID: " << meshId << std::endl;
                        }
                    }
                    
                    g_Scene->AddObject(std::move(cylinder));
                }
            }
            ImGui::EndMenu();  // End Primitives (Legacy)
            }
            ImGui::EndMenu();  // End Create
        }
        
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset Camera")) {
                if (g_Camera) {
                    g_Camera->SetPosition(glm::vec3(5.0f, 5.0f, 5.0f));
                    g_Camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
                }
            }
            ImGui::EndMenu();
        }
        
        ImGui::EndMainMenuBar();
    }
    
    // Sketch toolbar (only shown in sketch mode)
    if (g_SketchManager && g_SketchManager->IsSketchMode()) {
        ImGui::Begin("Sketch Tools");
        
        ImGui::Text("Sketch Mode Active");
        ImGui::Separator();
        
        // Drawing tools
        if (ImGui::Button("Select")) {
            g_SketchManager->SetSketchMode(SketchMode::Select);
        }
        ImGui::SameLine();
        if (ImGui::Button("Point")) {
            g_SketchManager->SetSketchMode(SketchMode::Point);
        }
        ImGui::SameLine();
        if (ImGui::Button("Line")) {
            g_SketchManager->SetSketchMode(SketchMode::Line);
        }
        ImGui::SameLine();
        if (ImGui::Button("Circle")) {
            g_SketchManager->SetSketchMode(SketchMode::Circle);
        }
        ImGui::SameLine();
        if (ImGui::Button("Rectangle")) {
            g_SketchManager->SetSketchMode(SketchMode::Rectangle);
        }
        
        ImGui::Separator();
        
        // Current mode display
        SketchMode currentMode = g_SketchManager->GetSketchMode();
        const char* modeNames[] = {"Select", "Point", "Line", "Circle", "Rectangle"};
        ImGui::Text("Current Tool: %s", modeNames[(int)currentMode]);
        
        // Sketch info
        if (auto sketch = g_SketchManager->GetActiveSketch()) {
            ImGui::Text("Elements: %zu", sketch->GetElementCount());
            ImGui::Text("Can Extrude: %s", g_SketchManager->CanExtrude() ? "Yes" : "No");
        }
        
        ImGui::Separator();
        
        // Exit sketch button
        if (ImGui::Button("Exit Sketch")) {
            g_SketchManager->ExitSketch();
            std::cout << "Exited sketch mode" << std::endl;
        }
        
        ImGui::End();
    }
    
    // File browser
    RenderFileBrowser();

    // 3D Viewport placeholder
    ImGui::Begin("3D Viewport");
    ImVec2 contentRegion = ImGui::GetContentRegionAvail();
    
    // Update camera aspect ratio
    if (g_Camera && contentRegion.x > 0 && contentRegion.y > 0) {
        g_Camera->SetAspectRatio(contentRegion.x / contentRegion.y);
    }
    
    // Placeholder for 3D content
    ImGui::Text("3D Viewport (%dx%d)", (int)contentRegion.x, (int)contentRegion.y);
    ImGui::Text("Camera controls will be implemented here");
    
    ImGui::End();

    // Properties panel
    ImGui::Begin("Properties");
    ImGui::Text("Scene Objects: %zu", g_Scene ? g_Scene->GetObjects().size() : 0);
    if (g_Camera) {
        glm::vec3 pos = g_Camera->GetPosition();
        ImGui::Text("Camera Position: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
        ImGui::Text("Camera Distance: %.2f", g_Camera->GetDistance());
    }
    ImGui::End();

    // Scene hierarchy
    ImGui::Begin("Scene");
    if (g_Scene) {
        for (const auto& object : g_Scene->GetObjects()) {
            // Check if it's a CAD object
            if (auto cadObject = dynamic_cast<CADObject*>(object.get())) {
                if (ImGui::TreeNode(object->GetName().c_str())) {
                    ImGui::Text("ID: %u", object->GetId());
                    ImGui::Text("Type: CAD Object");
                    ImGui::Text("Volume: %.3f", cadObject->GetVolume());
                    
                    glm::vec3 min, max;
                    cadObject->GetBoundingBox(min, max);
                    ImGui::Text("Bounds: (%.2f,%.2f,%.2f) - (%.2f,%.2f,%.2f)", 
                               min.x, min.y, min.z, max.x, max.y, max.z);
                    
                    ImGui::TreePop();
                }
            } else {
                ImGui::Text("%s (ID: %u)", object->GetName().c_str(), object->GetId());
            }
        }
        
        ImGui::Separator();
        if (ImGui::Button("Add Test Object")) {
            g_Scene->AddObject(std::make_unique<SceneObject>("Test Object " + std::to_string(g_Scene->GetObjects().size() + 1)));
        }
    }
    ImGui::End();
}

int main(int, char**) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Designer - 3D CAD Software", nullptr, nullptr);
    
    // Set up input callbacks
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetScrollCallback(window, scroll_callback);
    if (!glfwVulkanSupported()) {
        printf("GLFW: Vulkan Not Supported\n");
        return 1;
    }

    ImVector<const char*> extensions;
    uint32_t extensions_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    for (uint32_t i = 0; i < extensions_count; i++)
        extensions.push_back(glfw_extensions[i]);
    SetupVulkan(extensions);

    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
    check_vk_result(err);

    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, w, h);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.RenderPass = wd->RenderPass;
    init_info.Subpass = 0;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = g_Allocator;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    // Initialize application objects
    g_Scene = std::make_unique<Scene>();
    g_Camera = std::make_unique<Camera>();
    g_SketchManager = std::make_unique<SketchManager>();
    
    // Set up camera for good 3D view
    g_Camera->SetPosition(glm::vec3(8.0f, 6.0f, 8.0f));
    g_Camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
    g_Camera->SetAspectRatio(1280.0f / 720.0f);
    
    // Set viewport size for sketch manager
    g_SketchManager->SetViewportSize(1280, 720);
    
    // Initialize MeshRenderer with Vulkan context
    g_MeshRenderer = std::make_unique<MeshRenderer>();
    bool meshRendererInitialized = g_MeshRenderer->Initialize(g_Device, g_PhysicalDevice, 
                                                             wd->RenderPass, g_Queue, 
                                                             g_QueueFamily, g_CommandPool);
    if (!meshRendererInitialized) {
        std::cout << "Warning: MeshRenderer initialization failed" << std::endl;
    } else {
        std::cout << "MeshRenderer initialized successfully" << std::endl;
    }
    
    // Test OpenCASCADE directly
    std::cout << "=== Testing OpenCASCADE directly ===" << std::endl;
    CADShape testShape = CADKernel::Instance().CreateBox(1.0, 1.0, 1.0);
    std::cout << "Test shape valid: " << (testShape.IsValid() ? "YES" : "NO") << std::endl;
    std::cout << "Test shape volume: " << testShape.Volume() << std::endl;
    
    // Test mesh generation
    if (testShape.IsValid()) {
        std::cout << "=== Testing mesh generation ===" << std::endl;
        CADShape::Mesh mesh = testShape.GenerateMesh(0.1);
        std::cout << "Generated mesh - Vertices: " << mesh.vertices.size() 
                  << ", Triangles: " << mesh.indices.size() / 3 << std::endl;
    }

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Resize swap chain?
        int fb_width, fb_height;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        if (fb_width > 0 && fb_height > 0 && (g_SwapChainRebuild || g_MainWindowData.Width != fb_width || g_MainWindowData.Height != fb_height)) {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        // Update application
        static float lastTime = 0.0f;
        float currentTime = (float)glfwGetTime();
        float deltaTime = currentTime - lastTime;
        lastTime = currentTime;

        if (g_Camera) g_Camera->Update(deltaTime);
        if (g_Scene) g_Scene->Update(deltaTime);

        // Start ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Render CAD UI
        RenderCADUI();

        // Update camera matrices for 3D rendering
        if (g_MeshRenderer && g_Camera) {
            g_MeshRenderer->UpdateCamera(g_Camera->GetViewMatrix(), g_Camera->GetProjectionMatrix());
        }

        // Rendering
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized) {
            wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
            wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
            wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
            wd->ClearValue.color.float32[3] = clear_color.w;
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }
    }

    // Cleanup
    err = vkDeviceWaitIdle(g_Device);
    check_vk_result(err);
    
    g_Scene.reset();
    g_Camera.reset();
    g_MeshRenderer.reset();
    g_SketchManager.reset();
    
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    CleanupVulkanWindow();
    CleanupVulkan();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}