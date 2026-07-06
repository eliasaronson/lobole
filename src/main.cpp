// CAD Application with working Vulkan + ImGui setup
// Based on Dear ImGui GLFW+Vulkan example

#include "Camera.h"
#include "FaceSelector.h"
#include "GuiTests.h"
#include "Logger.h"
#include "Screenshot.h"
#include "remote/RemoteServer.h"
#include "MeshRenderer.h"
#include "OutlineRenderer.h"
#include "RayTracingRenderer.h"
#include "Scene.h"
#include "SketchRenderer.h"
#include "SketchManager.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/Theme.h"
#include "ui/UI.h"
#include <algorithm>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "imgui_te_engine.h"
#include <filesystem>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
#endif

// Vulkan data
static VkAllocationCallbacks *g_Allocator = nullptr;
static VkInstance g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice g_Device = VK_NULL_HANDLE;
static uint32_t g_QueueFamily = (uint32_t)-1;
static VkQueue g_Queue = VK_NULL_HANDLE;
static VkPipelineCache g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
static VkCommandPool g_CommandPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t g_MinImageCount = 2;
static bool g_SwapChainRebuild = false;
static bool g_DisableVSync = false;

// Application objects
static std::unique_ptr<Scene> g_Scene;
static std::unique_ptr<Camera> g_Camera;
static std::unique_ptr<MeshRenderer> g_MeshRenderer;
static std::unique_ptr<OutlineRenderer> g_OutlineRenderer;
static std::unique_ptr<SketchRenderer> g_SketchRenderer;
static std::unique_ptr<RayTracingRenderer> g_RayTracingRenderer;
static std::unique_ptr<SketchManager> g_SketchManager;
static std::unique_ptr<FaceSelector> g_FaceSelector;

// All state the UI layer works with (owns the document + undo stack; holds
// non-owning pointers to the objects above once they exist)
static ui::AppState g_App;

static void glfw_error_callback(int error, const char *description) {
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static void check_vk_result(VkResult err) {
  if (err == VK_SUCCESS)
    return;
  fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
  if (err < 0)
    abort();
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType,
             uint64_t object, size_t location, int32_t messageCode,
             const char *pLayerPrefix, const char *pMessage, void *pUserData) {
  (void)flags;
  (void)object;
  (void)location;
  (void)messageCode;
  (void)pUserData;
  (void)pLayerPrefix;
  fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n",
          objectType, pMessage);
  return VK_FALSE;
}
#endif

static bool
IsExtensionAvailable(const ImVector<VkExtensionProperties> &properties,
                     const char *extension) {
  for (const VkExtensionProperties &p : properties)
    if (strcmp(p.extensionName, extension) == 0)
      return true;
  return false;
}

static void SetupVulkan(ImVector<const char *> instance_extensions) {
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
    err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count,
                                                 properties.Data);
    check_vk_result(err);

    // Enable required extensions
    if (IsExtensionAvailable(
            properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
      instance_extensions.push_back(
          VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    if (IsExtensionAvailable(properties,
                             VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
      instance_extensions.push_back(
          VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
      create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    create_info.enabledLayerCount = 1;
    create_info.ppEnabledLayerNames = layers;
    instance_extensions.push_back("VK_EXT_debug_report");
#endif

    create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
    create_info.ppEnabledExtensionNames = instance_extensions.Data;
    err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
    check_vk_result(err);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    auto f_vkCreateDebugReportCallbackEXT =
        (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(
            g_Instance, "vkCreateDebugReportCallbackEXT");
    IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
    VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
    debug_report_ci.sType =
        VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
    debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT |
                            VK_DEBUG_REPORT_WARNING_BIT_EXT |
                            VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
    debug_report_ci.pfnCallback = debug_report;
    debug_report_ci.pUserData = nullptr;
    err = f_vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci,
                                           g_Allocator, &g_DebugReport);
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
    ImVector<const char *> device_extensions;
    device_extensions.push_back("VK_KHR_swapchain");

    uint32_t properties_count;
    ImVector<VkExtensionProperties> properties;
    vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr,
                                         &properties_count, nullptr);
    properties.resize(properties_count);
    vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr,
                                         &properties_count, properties.Data);

    // Add ray tracing extensions if available
    if (IsExtensionAvailable(properties,
                             VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
      device_extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
      std::cout << "Ray Tracing: Added acceleration structure extension"
                << std::endl;
    }
    if (IsExtensionAvailable(properties,
                             VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) {
      device_extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
      std::cout << "Ray Tracing: Added ray tracing pipeline extension"
                << std::endl;
    }
    if (IsExtensionAvailable(properties, VK_KHR_RAY_QUERY_EXTENSION_NAME)) {
      device_extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
      std::cout << "Ray Tracing: Added ray query extension" << std::endl;
    }
    if (IsExtensionAvailable(properties,
                             VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
      device_extensions.push_back(
          VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
      std::cout << "Ray Tracing: Added deferred host operations extension"
                << std::endl;
    }
    if (IsExtensionAvailable(properties,
                             VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
      device_extensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
      std::cout << "Ray Tracing: Added buffer device address extension"
                << std::endl;
    }

#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    if (IsExtensionAvailable(properties,
                             VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
      device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

    const float queue_priority[] = {1.0f};
    VkDeviceQueueCreateInfo queue_info[1] = {};
    queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info[0].queueFamilyIndex = g_QueueFamily;
    queue_info[0].queueCount = 1;
    queue_info[0].pQueuePriorities = queue_priority;

    // Enable ray tracing features if available
    VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{};
    bufferDeviceAddressFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    bufferDeviceAddressFeatures.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};
    rtPipelineFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtPipelineFeatures.rayTracingPipeline = VK_TRUE;
    rtPipelineFeatures.pNext = &bufferDeviceAddressFeatures;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    asFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.accelerationStructure = VK_TRUE;
    asFeatures.pNext = &rtPipelineFeatures;

    VkDeviceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &asFeatures;
    create_info.queueCreateInfoCount =
        sizeof(queue_info) / sizeof(queue_info[0]);
    create_info.pQueueCreateInfos = queue_info;
    create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
    create_info.ppEnabledExtensionNames = device_extensions.Data;
    err =
        vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
    check_vk_result(err);
    vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
  }

  // Create Descriptor Pool
  {
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE},
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 0;
    for (VkDescriptorPoolSize &pool_size : pool_sizes)
      pool_info.maxSets += pool_size.descriptorCount;
    pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;
    err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator,
                                 &g_DescriptorPool);
    check_vk_result(err);
  }

  // Create Command Pool
  {
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = g_QueueFamily;
    err =
        vkCreateCommandPool(g_Device, &pool_info, g_Allocator, &g_CommandPool);
    check_vk_result(err);
  }
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window *wd,
                              VkSurfaceKHR surface, int width, int height) {
  wd->Surface = surface;

  VkBool32 res;
  vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily,
                                       wd->Surface, &res);
  if (res != VK_TRUE) {
    fprintf(stderr, "Error no WSI support on physical device 0\n");
    exit(-1);
  }

  const VkFormat requestSurfaceImageFormat[] = {
      VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
  const VkColorSpaceKHR requestSurfaceColorSpace =
      VK_COLORSPACE_SRGB_NONLINEAR_KHR;
  wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
      g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat,
      (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat),
      requestSurfaceColorSpace);

  if (g_DisableVSync) {
    // Uncapped presents: vsynced (FIFO) presents of an occluded window get
    // throttled to ~1 fps by the compositor, which stalls GUI tests.
    VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_IMMEDIATE_KHR,
                                        VK_PRESENT_MODE_MAILBOX_KHR,
                                        VK_PRESENT_MODE_FIFO_KHR};
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_PhysicalDevice, wd->Surface, &present_modes[0],
        IM_ARRAYSIZE(present_modes));
  } else {
    VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_PhysicalDevice, wd->Surface, &present_modes[0],
        IM_ARRAYSIZE(present_modes));
  }

  IM_ASSERT(g_MinImageCount >= 2);
  ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device,
                                         wd, g_QueueFamily, g_Allocator, width,
                                         height, g_MinImageCount);
}

static void CleanupVulkan() {
  vkDestroyCommandPool(g_Device, g_CommandPool, g_Allocator);
  vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
  auto f_vkDestroyDebugReportCallbackEXT =
      (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(
          g_Instance, "vkDestroyDebugReportCallbackEXT");
  f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif

  vkDestroyDevice(g_Device, g_Allocator);
  vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow() {
  ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData,
                                  g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window *wd, ImDrawData *draw_data) {
  VkSemaphore image_acquired_semaphore =
      wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore render_complete_semaphore =
      wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
  VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX,
                                       image_acquired_semaphore, VK_NULL_HANDLE,
                                       &wd->FrameIndex);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    g_SwapChainRebuild = true;
    return;
  }
  check_vk_result(err);

  ImGui_ImplVulkanH_Frame *fd = &wd->Frames[wd->FrameIndex];
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

  // 3D scene first, UI on top
  if (g_MeshRenderer) {
    g_MeshRenderer->Render(fd->CommandBuffer, wd->FrameIndex);
  }
  if (g_OutlineRenderer) {
    g_OutlineRenderer->Render(fd->CommandBuffer, wd->FrameIndex);
  }
  if (g_SketchRenderer) {
    g_SketchRenderer->Render(fd->CommandBuffer, wd->FrameIndex);
  }

  ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

  vkCmdEndRenderPass(fd->CommandBuffer);
  {
    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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

static void FramePresent(ImGui_ImplVulkanH_Window *wd) {
  if (g_SwapChainRebuild)
    return;
  VkSemaphore render_complete_semaphore =
      wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
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


void printUsage(const char *programName) {
  std::cout << "Usage: " << programName << " [OPTIONS]" << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  -f, --file <path>    Load STEP file on startup" << std::endl;
  std::cout << "  -v, --verbose        Enable verbose logging (debug level)"
            << std::endl;
  std::cout
      << "  -vv                  Enable very verbose logging (trace level)"
      << std::endl;
  std::cout << "  -h, --help           Show this help message" << std::endl;
  std::cout << "  --gui-tests          Run GUI test suite and exit"
            << std::endl;
  std::cout << "  --remote <port>      Enable JSON-RPC remote control on "
               "127.0.0.1:<port>"
            << std::endl;
}

struct CommandLineArgs {
  std::string fileToLoad;
  spdlog::level::level_enum logLevel = spdlog::level::info;
  bool runGuiTests = false;
  int remotePort = 0;
};

CommandLineArgs parseCommandLineArgs(int argc, char **argv) {
  CommandLineArgs args;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      exit(0);
    } else if (arg == "-f" || arg == "--file") {
      if (i + 1 < argc) {
        args.fileToLoad = argv[i + 1];
        i++; // Skip the next argument since we consumed it
      } else {
        std::cerr << "Error: --file option requires a file path" << std::endl;
        printUsage(argv[0]);
        exit(1);
      }
    } else if (arg == "--gui-tests") {
      args.runGuiTests = true;
    } else if (arg == "--remote") {
      if (i + 1 < argc) {
        args.remotePort = std::atoi(argv[i + 1]);
        i++;
      }
      if (args.remotePort <= 0 || args.remotePort > 65535) {
        std::cerr << "Error: --remote requires a port (1-65535)" << std::endl;
        exit(1);
      }
    } else if (arg == "-v" || arg == "--verbose") {
      args.logLevel = spdlog::level::debug;
    } else if (arg == "-vv") {
      args.logLevel = spdlog::level::trace;
    } else {
      std::cerr << "Error: Unknown option '" << arg << "'" << std::endl;
      printUsage(argv[0]);
      exit(1);
    }
  }

  return args;
}

int main(int argc, char **argv) {
  CommandLineArgs args = parseCommandLineArgs(argc, argv);
  g_DisableVSync = args.runGuiTests;

  // Initialize logger with specified level
  Logger::Initialize(args.logLevel);

#ifdef HAVE_NATIVEFILEDIALOG
  LOG_INFO("nativefiledialog available");
#else
  LOG_INFO("Using ImGui file browser (nativefiledialog not available)");
#endif

  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    return 1;

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow *window = glfwCreateWindow(1280, 720, "Designer - 3D CAD Software",
                                        nullptr, nullptr);

  if (!glfwVulkanSupported()) {
    printf("GLFW: Vulkan Not Supported\n");
    return 1;
  }

  ImVector<const char *> extensions;
  uint32_t extensions_count = 0;
  const char **glfw_extensions =
      glfwGetRequiredInstanceExtensions(&extensions_count);
  for (uint32_t i = 0; i < extensions_count; i++)
    extensions.push_back(glfw_extensions[i]);
  SetupVulkan(extensions);

  VkSurfaceKHR surface;
  VkResult err =
      glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
  check_vk_result(err);

  int w, h;
  glfwGetFramebufferSize(window, &w, &h);
  ImGui_ImplVulkanH_Window *wd = &g_MainWindowData;
  SetupVulkanWindow(wd, surface, w, h);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  // Build the default dock layout when there is no saved one. GUI tests
  // always use the default layout and never persist, for determinism.
  if (args.runGuiTests) {
    io.IniFilename = nullptr;
    g_App.buildDefaultDockLayout = true;
  } else {
    g_App.buildDefaultDockLayout = !std::filesystem::exists("imgui.ini");
  }
  g_App.runGuiTests = args.runGuiTests;

  // Theme, scaled for the display's content scale (HiDPI)
  float scaleX = 1.0f, scaleY = 1.0f;
  glfwGetWindowContentScale(window, &scaleX, &scaleY);
  ui::ApplyTheme(std::max(1.0f, std::max(scaleX, scaleY)));

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
  g_App.document = std::make_unique<Document>();
  g_App.commands = std::make_unique<CommandStack>();
  g_App.scene = g_Scene.get();
  g_App.camera = g_Camera.get();
  g_App.sketchManager = g_SketchManager.get();
  g_App.window = window;

  // Set up camera for good 3D view
  g_Camera->SetPosition(glm::vec3(8.0f, 6.0f, 8.0f));
  g_Camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
  g_Camera->SetAspectRatio(1280.0f / 720.0f);

  // Set viewport size for sketch manager
  g_SketchManager->SetViewportSize(1280, 720);

  // Initialize MeshRenderer with Vulkan context
  g_MeshRenderer = std::make_unique<MeshRenderer>();
  bool meshRendererInitialized =
      g_MeshRenderer->Initialize(g_Device, g_PhysicalDevice, wd->RenderPass,
                                 g_Queue, g_QueueFamily, g_CommandPool);
  if (!meshRendererInitialized) {
    LOG_ERROR("MeshRenderer initialization failed");
  } else {
    LOG_INFO("MeshRenderer initialized successfully");
    g_MeshRenderer->SetViewportSize(1280, 720);
  }

  // Initialize FaceSelector
  g_FaceSelector = std::make_unique<FaceSelector>();
  LOG_INFO("FaceSelector initialized successfully");

  // Initialize OutlineRenderer
  g_OutlineRenderer = std::make_unique<OutlineRenderer>();
  bool outlineInitialized =
      g_OutlineRenderer->Initialize(g_Device, g_PhysicalDevice, wd->RenderPass,
                                    g_Queue, g_QueueFamily, g_CommandPool);
  if (!outlineInitialized) {
    std::cout << "Warning: OutlineRenderer initialization failed" << std::endl;
  } else {
    std::cout << "OutlineRenderer initialized successfully" << std::endl;
    g_OutlineRenderer->SetViewportSize(1280, 720);

    // Set default outline settings for visibility testing
    OutlineSettings settings = g_OutlineRenderer->GetOutlineSettings();
    std::cout << "Default outline mode: " << static_cast<int>(settings.mode)
              << std::endl;
    std::cout << "Default outline color: (" << settings.outlineColor.r << ", "
              << settings.outlineColor.g << ", " << settings.outlineColor.b
              << ", " << settings.outlineColor.a << ")" << std::endl;
    std::cout << "Default outline width: " << settings.outlineWidth
              << std::endl;

    // Enable wireframe rendering with subtle appearance
    settings.mode = OutlineMode::WIREFRAME;
    g_OutlineRenderer->SetOutlineSettings(settings);
    std::cout << "Set outline mode to WIREFRAME with subtle styling"
              << std::endl;
  }

  // Initialize SketchRenderer
  g_SketchRenderer = std::make_unique<SketchRenderer>();
  if (g_SketchRenderer->Initialize(g_Device, g_PhysicalDevice, wd->RenderPass,
                                   g_Queue, g_QueueFamily, g_CommandPool)) {
    g_SketchRenderer->SetViewportSize(1280, 720);
  } else {
    std::cout << "Warning: SketchRenderer initialization failed" << std::endl;
  }

  // Initialize RayTracingRenderer
  g_RayTracingRenderer = std::make_unique<RayTracingRenderer>();
  bool rtInitialized = g_RayTracingRenderer->Initialize(
      g_Device, g_PhysicalDevice, g_Queue, g_QueueFamily, g_CommandPool);
  if (!rtInitialized) {
    std::cout
        << "Ray Tracing: Initialization failed, falling back to rasterization"
        << std::endl;
  } else {
    std::cout << "Ray Tracing: Initialization successful!" << std::endl;
  }

  // Hand the UI layer its pointers now that everything exists
  g_App.meshRenderer = g_MeshRenderer.get();
  g_App.outlineRenderer = g_OutlineRenderer.get();
  g_App.sketchRenderer = g_SketchRenderer.get();
  g_App.rayTracer = g_RayTracingRenderer.get();
  g_App.faceSelector = g_FaceSelector.get();

  // Load startup file if specified
  if (!args.fileToLoad.empty()) {
    LOG_INFO("Loading startup file: {}", args.fileToLoad);
    ui::LoadSTEPFile(g_App, args.fileToLoad);
  }

  LOG_INFO("=== APPLICATION READY - Left-click to select faces, "
           "Ctrl+Left-click to align camera ===");

  // GUI test engine: always started so tests can also be run interactively;
  // --gui-tests queues the whole suite and exits with a pass/fail code.
  ImGuiTestEngine *testEngine = ImGuiTestEngine_CreateContext();
  ImGuiTestEngineIO &testEngineIO = ImGuiTestEngine_GetIO(testEngine);
  testEngineIO.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
  testEngineIO.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
  testEngineIO.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
  testEngineIO.ConfigLogToTTY = args.runGuiTests;
  testEngineIO.ConfigWatchdogWarning = 60.0f;
  testEngineIO.ConfigWatchdogKillTest = 300.0f;
  ImGuiTestEngine_Start(testEngine, ImGui::GetCurrentContext());
  RegisterGuiTests(testEngine, &g_App);
  if (args.runGuiTests) {
    ImGuiTestEngine_QueueTests(testEngine, ImGuiTestGroup_Tests, "all");
  }

  // Remote control (opt-in)
  RemoteServer remoteServer;
  if (args.remotePort > 0) {
    remoteServer.Start(static_cast<uint16_t>(args.remotePort), testEngine);
  }

  const ui::ClearColor vp = ui::ViewportClearColor();
  ImVec4 clear_color = ImVec4(vp.r, vp.g, vp.b, 1.00f);

  // Main loop
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    // Resize swap chain?
    int fb_width, fb_height;
    glfwGetFramebufferSize(window, &fb_width, &fb_height);
    if (fb_width > 0 && fb_height > 0 &&
        (g_SwapChainRebuild || g_MainWindowData.Width != fb_width ||
         g_MainWindowData.Height != fb_height)) {
      ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
      ImGui_ImplVulkanH_CreateOrResizeWindow(
          g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData,
          g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount);
      g_MainWindowData.FrameIndex = 0;
      g_SwapChainRebuild = false;

      // Update viewport sizes for renderers
      if (g_MeshRenderer) {
        g_MeshRenderer->SetViewportSize(fb_width, fb_height);
      }
      if (g_OutlineRenderer) {
        g_OutlineRenderer->SetViewportSize(fb_width, fb_height);
      }
      if (g_SketchRenderer) {
        g_SketchRenderer->SetViewportSize(fb_width, fb_height);
      }
      if (g_Camera) {
        g_Camera->SetAspectRatio(static_cast<float>(fb_width) /
                                 static_cast<float>(fb_height));
      }
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

    if (g_Camera)
      g_Camera->Update(deltaTime);
    if (g_Scene)
      g_Scene->Update(deltaTime);

    // Remote-control requests act on the document/UI before this frame
    remoteServer.ProcessPending(g_App);

    // Mirror document changes (edits, undo, redo) into the renderers
    ui::SyncDocumentRenderers(g_App);

    // Start ImGui frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ui::DrawUI(g_App);

    // Update camera matrices for 3D rendering
    if (g_MeshRenderer && g_Camera) {
      g_MeshRenderer->UpdateCamera(g_Camera->GetViewMatrix(),
                                   g_Camera->GetProjectionMatrix());
    }

    if (g_OutlineRenderer && g_Camera) {
      g_OutlineRenderer->UpdateCamera(g_Camera->GetViewMatrix(),
                                      g_Camera->GetProjectionMatrix());
    }

    if (g_SketchRenderer && g_Camera) {
      g_SketchRenderer->UpdateCamera(g_Camera->GetViewMatrix(),
                                     g_Camera->GetProjectionMatrix());
    }

    // Rendering
    ImGui::Render();
    ImDrawData *draw_data = ImGui::GetDrawData();
    const bool is_minimized =
        (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
    if (!is_minimized) {
      wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
      wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
      wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
      wd->ClearValue.color.float32[3] = clear_color.w;
      FrameRender(wd, draw_data);
      FramePresent(wd);
      ImGuiTestEngine_PostSwap(testEngine);

      // Remote screenshot: re-render this frame into a readable image
      std::string shotPath;
      if (remoteServer.WantsScreenshot(shotPath)) {
        vkQueueWaitIdle(g_Queue); // frame resources must be reusable
        std::string shotError;
        const bool ok = CaptureScreenshot(
            g_Device, g_PhysicalDevice, g_Queue, g_CommandPool,
            wd->RenderPass, wd->SurfaceFormat.format,
            static_cast<uint32_t>(wd->Width), static_cast<uint32_t>(wd->Height),
            wd->ClearValue,
            [&](VkCommandBuffer cb) {
              if (g_MeshRenderer) g_MeshRenderer->Render(cb, wd->FrameIndex);
              if (g_OutlineRenderer)
                g_OutlineRenderer->Render(cb, wd->FrameIndex);
              if (g_SketchRenderer)
                g_SketchRenderer->Render(cb, wd->FrameIndex);
              ImGui_ImplVulkan_RenderDrawData(draw_data, cb);
            },
            shotPath, &shotError);
        remoteServer.ScreenshotDone(ok, shotError);
      }
    }

    if (args.runGuiTests && ImGuiTestEngine_IsTestQueueEmpty(testEngine)) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
  }

  remoteServer.Stop();

  // Collect GUI test results before teardown
  int exitCode = 0;
  if (args.runGuiTests) {
    int countTested = 0, countSuccess = 0;
    ImGuiTestEngine_GetResult(testEngine, countTested, countSuccess);
    LOG_INFO("GUI tests: {}/{} passed", countSuccess, countTested);
    exitCode = (countTested > 0 && countTested == countSuccess) ? 0 : 1;
  }
  ImGuiTestEngine_Stop(testEngine);

  // Cleanup
  err = vkDeviceWaitIdle(g_Device);
  check_vk_result(err);

  g_App.document.reset();
  g_App.commands.reset();
  g_Scene.reset();
  g_Camera.reset();
  g_MeshRenderer.reset();
  g_SketchRenderer.reset();
  g_OutlineRenderer.reset();
  g_RayTracingRenderer.reset();
  g_SketchManager.reset();
  g_FaceSelector.reset();

  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  // Test engine context must outlive the ImGui context it hooks
  ImGuiTestEngine_DestroyContext(testEngine);

  CleanupVulkanWindow();
  CleanupVulkan();

  glfwDestroyWindow(window);
  glfwTerminate();

  return exitCode;
}
