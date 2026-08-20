// CAD Application with working Vulkan + ImGui setup
// Based on Dear ImGui GLFW+Vulkan example

#include "app/CrashHandler.h"
#include "app/GuiTests.h"
#include "app/HeadlessMain.h"
#include "cad/FaceSelector.h"
#include "common/DesignerVersion.h"
#include "common/DevicePreference.h"
#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "core/io/Autosaver.h"
#include "core/io/Settings.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#ifdef HAVE_TEST_ENGINE
#include "imgui_te_engine.h"
#endif
#include "remote/RemoteServer.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/FramesInFlight.h"
#include "render/IBL.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SSAORenderer.h"
#include "render/Scene.h"
#include "render/Screenshot.h"
#include "render/ShadowRenderer.h"
#include "render/SketchRenderer.h"
#include "render/UnderlayRenderer.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/SettingsWindow.h"
#include "ui/Theme.h"
#include "ui/UI.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <time.h>
#include <utility>

namespace {
// Writable per-user cache for precomputed IBL maps: $XDG_CACHE_HOME/designer
// (else ~/.cache/designer, else a temp dir). The executable dir may be
// read-only (Nix store, Flatpak /app), so the cache must live under $HOME.
std::string IblCacheDir() {
  namespace fs = std::filesystem;
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  const char* home = std::getenv("HOME");
  fs::path base = xdg && *xdg     ? fs::path(xdg)
                  : home && *home ? fs::path(home) / ".cache"
                                  : fs::temp_directory_path();
  fs::path dir = base / "designer";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir.string();
}
} // namespace
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#if defined(__linux__)
// Map the window ourselves via XMapWindow instead of glfwShowWindow. GLFW's
// show path calls waitForVisibilityNotify, which blocks until the window is
// unobscured — a deadlock under a tiling WM (i3) when the window opens behind a
// fullscreen window or on a non-focused workspace. XMapWindow just posts the
// map request and returns; the render loop's glfwPollEvents pumps events and
// present targets the mapped X window. Declared extern to avoid <X11/Xlib.h>,
// whose None/Bool/Status/Success macros clash with OCCT/PlaneGCS headers.
extern "C" {
void* glfwGetX11Display(void);
unsigned long glfwGetX11Window(GLFWwindow* handle);
int XMapWindow(void* display, unsigned long window);
int XFlush(void* display);
}
static void ShowWindowNonBlocking(GLFWwindow* window) {
  void* dpy = glfwGetX11Display();
  unsigned long xw = glfwGetX11Window(window);
  if (dpy && xw) {
    XMapWindow(dpy, xw);
    XFlush(dpy);
  } else {
    glfwShowWindow(window); // not X11 (e.g. Wayland): fall back
  }
}
#else
static void ShowWindowNonBlocking(GLFWwindow* window) {
  glfwShowWindow(window);
}
#endif

#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
#endif

// Vulkan data
static VkAllocationCallbacks* g_Allocator = nullptr;
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
static std::unique_ptr<UnderlayRenderer> g_UnderlayRenderer;
static std::unique_ptr<FieldRenderer> g_FieldRenderer;
static std::unique_ptr<RayTracingRenderer> g_RayTracingRenderer;
static std::unique_ptr<ShadowRenderer> g_ShadowRenderer;
static std::unique_ptr<SSAORenderer> g_SSAORenderer;
static std::unique_ptr<FaceSelector> g_FaceSelector;
static std::unique_ptr<IBLTextures> g_IBLTextures;

// All state the UI layer works with (owns the document + undo stack; holds
// non-owning pointers to the objects above once they exist)
static ui::AppState g_App;

static void glfw_error_callback(int error, const char* description) {
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
             const char* pLayerPrefix, const char* pMessage, void* pUserData) {
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
IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties,
                     const char* extension) {
  for (const VkExtensionProperties& p : properties)
    if (strcmp(p.extensionName, extension) == 0)
      return true;
  return false;
}

static void SetupVulkan(ImVector<const char*> instance_extensions,
                        GLFWwindow* window) {
  VkResult err;

  // Create Vulkan Instance
  {
    // Request Vulkan 1.2: it promotes VK_EXT_descriptor_indexing,
    // VK_KHR_buffer_device_address, VK_KHR_spirv_1_4 and VK_KHR_device_group to
    // core, which satisfies the ray-tracing extensions' dependencies and makes
    // the SPIR-V 1.4 RT shaders legal (both hard validation errors under the
    // implicit 1.0 default). Both discrete and iGPU here report 1.3.
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Designer";
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

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
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
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

  // Select Physical Device (GPU). Prefer a discrete card — on this dual-GPU
  // machine that is the RT-capable RTX 4060, not the Arc iGPU — unless the
  // user named one in Preferences, which is why this no longer just calls
  // ImGui_ImplVulkanH_SelectPhysicalDevice. Log it so a wrong pick (e.g. a
  // headless llvmpipe) is obvious: RT needs the discrete GPU.
  {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_Instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(g_Instance, &count, devices.data());

    // Throwaway surface, purely so each device can be asked whether it can
    // present to THIS window. A device that cannot is ineligible however the
    // user feels about it: choosing one aborts startup with "no WSI support
    // on physical device 0", and the only way back would be hand-editing
    // settings.json. Measured here — the Arc iGPU cannot present under Xvfb,
    // and glfwGetPhysicalDevicePresentationSupport says it CAN (it asks the X
    // server, which answers before Mesa finds no DRI3), so only the real
    // surface query is trustworthy. The app's own surface does not exist
    // until after device selection, hence this one.
    VkSurfaceKHR probe = VK_NULL_HANDLE;
    if (window == nullptr ||
        glfwCreateWindowSurface(g_Instance, window, g_Allocator, &probe) !=
            VK_SUCCESS)
      probe = VK_NULL_HANDLE;

    std::vector<std::string> names;
    std::vector<int> scores;
    for (VkPhysicalDevice pd : devices) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(pd, &props);
      names.emplace_back(props.deviceName);
      bool canPresent = probe == VK_NULL_HANDLE; // unknown: do not exclude
      uint32_t families = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, nullptr);
      std::vector<VkQueueFamilyProperties> qprops(families);
      vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, qprops.data());
      for (uint32_t q = 0; q < families && !canPresent; q++) {
        if (!(qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
          continue;
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, probe, &supported);
        canPresent = supported == VK_TRUE;
      }
      scores.push_back(
          !canPresent                                                  ? -1
          : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 2
          : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                       : 0);
    }
    if (probe != VK_NULL_HANDLE)
      vkDestroySurfaceKHR(g_Instance, probe, g_Allocator);
    // The list is what the Preferences combo offers; the preference itself is
    // matched by the shared rule both device-picking sites use.
    g_App.gpuDevices = names;
    const int pick =
        devicepref::Choose(names, scores, g_App.settings.gpuDevice);
    if (pick >= 0)
      g_PhysicalDevice = devices[static_cast<size_t>(pick)];
    // Say so rather than silently doing something else: a preference that
    // cannot be honoured is exactly the case a user reads the log for.
    if (!g_App.settings.gpuDevice.empty() && pick >= 0 &&
        names[static_cast<size_t>(pick)].find(g_App.settings.gpuDevice) ==
            std::string::npos) {
      g_App.gpuPreferenceNote = "'" + g_App.settings.gpuDevice +
                                "' cannot display a window on this system (or "
                                "is not present), so it was not used.";
      LOG_WARN("Preferred GPU '{}' cannot present here (or is absent); "
               "rendering on {} instead.",
               g_App.settings.gpuDevice, names[static_cast<size_t>(pick)]);
    }
  }
  IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);
  {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
    LOG_INFO("Selected GPU: {} (type {})", props.deviceName,
             static_cast<int>(props.deviceType));
    // Keep it for Help > About: which GPU answered is the first thing worth
    // knowing about a rendering bug, and the log is gone by the time someone
    // writes the report.
    g_App.gpuName = props.deviceName;
    g_App.gpuApiVersion =
        std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
        std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + "." +
        std::to_string(VK_API_VERSION_PATCH(props.apiVersion));
  }

  // Select graphics queue family
  g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
  IM_ASSERT(g_QueueFamily != (uint32_t)-1);

  // Create Logical Device
  {
    ImVector<const char*> device_extensions;
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
      LOG_INFO("Ray Tracing: added acceleration structure extension");
    }
    if (IsExtensionAvailable(properties,
                             VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) {
      device_extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
      LOG_INFO("Ray Tracing: added ray tracing pipeline extension");
    }
    if (IsExtensionAvailable(properties, VK_KHR_RAY_QUERY_EXTENSION_NAME)) {
      device_extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
      LOG_INFO("Ray Tracing: added ray query extension");
    }
    if (IsExtensionAvailable(properties,
                             VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
      device_extensions.push_back(
          VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
      LOG_INFO("Ray Tracing: added deferred host operations extension");
    }
    if (IsExtensionAvailable(properties,
                             VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
      device_extensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
      LOG_INFO("Ray Tracing: added buffer device address extension");
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

    // The OutlineRenderer wireframe pipeline draws with VK_POLYGON_MODE_LINE,
    // which requires the fillModeNonSolid feature. Enable it when supported.
    VkPhysicalDeviceFeatures supportedFeatures{};
    vkGetPhysicalDeviceFeatures(g_PhysicalDevice, &supportedFeatures);
    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.fillModeNonSolid = supportedFeatures.fillModeNonSolid;

    VkDeviceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &asFeatures;
    create_info.pEnabledFeatures = &enabledFeatures;
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
    for (VkDescriptorPoolSize& pool_size : pool_sizes)
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

// Depth buffer. ImGui's swapchain render pass is color-only, so 3D solids
// rendered with only back-face culling look right when convex but wrong when
// non-convex (a cut/hole), and overlays draw through them. We add a depth
// attachment by overriding ImGui's render pass + framebuffers.
static VkImage g_DepthImage = VK_NULL_HANDLE;
static VkDeviceMemory g_DepthMemory = VK_NULL_HANDLE;
static VkImageView g_DepthView = VK_NULL_HANDLE;
static VkRenderPass g_DepthRenderPass = VK_NULL_HANDLE;
static const VkFormat g_DepthFormat = VK_FORMAT_D32_SFLOAT;

// MSAA: the scene + UI render into a multisampled color+depth target that
// resolves to the swapchain backbuffer. Chosen once from device limits (capped
// at 4x); falls back to 1x (no resolve) on hardware that can't multisample.
static VkSampleCountFlagBits g_MSAASamples = VK_SAMPLE_COUNT_1_BIT;
static VkImage g_MsaaColorImage = VK_NULL_HANDLE;
static VkDeviceMemory g_MsaaColorMemory = VK_NULL_HANDLE;
static VkImageView g_MsaaColorView = VK_NULL_HANDLE;

static VkSampleCountFlagBits GetMaxUsableSampleCount() {
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
  // Need the count supported for BOTH color and depth so one pass can use it.
  VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                              props.limits.framebufferDepthSampleCounts;
  if (counts & VK_SAMPLE_COUNT_4_BIT)
    return VK_SAMPLE_COUNT_4_BIT;
  if (counts & VK_SAMPLE_COUNT_2_BIT)
    return VK_SAMPLE_COUNT_2_BIT;
  return VK_SAMPLE_COUNT_1_BIT;
}

static uint32_t FindMemoryTypeIndex(uint32_t typeFilter,
                                    VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((typeFilter & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags & props) == props)
      return i;
  return 0;
}

// Recreates the depth image at the window size and overrides ImGui's
// color-only render pass + framebuffers with color+depth ones. Must run after
// every ImGui_ImplVulkanH_CreateOrResizeWindow (which recreates them
// color-only) and before ImGui_ImplVulkan_Init.
static void SetupDepthBuffer(ImGui_ImplVulkanH_Window* wd) {
  if (g_DepthView)
    vkDestroyImageView(g_Device, g_DepthView, g_Allocator);
  if (g_DepthImage)
    vkDestroyImage(g_Device, g_DepthImage, g_Allocator);
  if (g_DepthMemory)
    vkFreeMemory(g_Device, g_DepthMemory, g_Allocator);
  g_DepthView = VK_NULL_HANDLE;
  g_DepthImage = VK_NULL_HANDLE;
  g_DepthMemory = VK_NULL_HANDLE;
  if (g_MsaaColorView)
    vkDestroyImageView(g_Device, g_MsaaColorView, g_Allocator);
  if (g_MsaaColorImage)
    vkDestroyImage(g_Device, g_MsaaColorImage, g_Allocator);
  if (g_MsaaColorMemory)
    vkFreeMemory(g_Device, g_MsaaColorMemory, g_Allocator);
  g_MsaaColorView = VK_NULL_HANDLE;
  g_MsaaColorImage = VK_NULL_HANDLE;
  g_MsaaColorMemory = VK_NULL_HANDLE;

  const bool msaa = g_MSAASamples != VK_SAMPLE_COUNT_1_BIT;

  // Depth image (multisampled to match the color target).
  VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  img.imageType = VK_IMAGE_TYPE_2D;
  img.format = g_DepthFormat;
  img.extent = {(uint32_t)wd->Width, (uint32_t)wd->Height, 1};
  img.mipLevels = 1;
  img.arrayLayers = 1;
  img.samples = g_MSAASamples;
  img.tiling = VK_IMAGE_TILING_OPTIMAL;
  img.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  vkCreateImage(g_Device, &img, g_Allocator, &g_DepthImage);
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(g_Device, g_DepthImage, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = FindMemoryTypeIndex(
      req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(g_Device, &alloc, g_Allocator, &g_DepthMemory);
  vkBindImageMemory(g_Device, g_DepthImage, g_DepthMemory, 0);
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = g_DepthImage;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = g_DepthFormat;
  view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  vkCreateImageView(g_Device, &view, g_Allocator, &g_DepthView);

  // Multisampled color image that resolves to the backbuffer (MSAA only).
  if (msaa) {
    VkImageCreateInfo cimg{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    cimg.imageType = VK_IMAGE_TYPE_2D;
    cimg.format = wd->SurfaceFormat.format;
    cimg.extent = {(uint32_t)wd->Width, (uint32_t)wd->Height, 1};
    cimg.mipLevels = 1;
    cimg.arrayLayers = 1;
    cimg.samples = g_MSAASamples;
    cimg.tiling = VK_IMAGE_TILING_OPTIMAL;
    cimg.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    vkCreateImage(g_Device, &cimg, g_Allocator, &g_MsaaColorImage);
    VkMemoryRequirements creq;
    vkGetImageMemoryRequirements(g_Device, g_MsaaColorImage, &creq);
    VkMemoryAllocateInfo calloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    calloc.allocationSize = creq.size;
    calloc.memoryTypeIndex = FindMemoryTypeIndex(
        creq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(g_Device, &calloc, g_Allocator, &g_MsaaColorMemory);
    vkBindImageMemory(g_Device, g_MsaaColorImage, g_MsaaColorMemory, 0);
    VkImageViewCreateInfo cview{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    cview.image = g_MsaaColorImage;
    cview.viewType = VK_IMAGE_VIEW_TYPE_2D;
    cview.format = wd->SurfaceFormat.format;
    cview.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(g_Device, &cview, g_Allocator, &g_MsaaColorView);
  }

  // Render pass (created once, reused across resizes so ImGui's pipeline stays
  // render-pass-compatible). Attachment order is kept stable across the MSAA
  // and non-MSAA paths — color=0, depth=1 — so the clear values in FrameRender
  // don't need to branch; the resolve target (backbuffer) is appended at
  // index 2.
  if (g_DepthRenderPass == VK_NULL_HANDLE) {
    VkAttachmentDescription atts[3] = {};
    // 0: color (multisampled when MSAA; the presented backbuffer otherwise)
    atts[0].format = wd->SurfaceFormat.format;
    atts[0].samples = g_MSAASamples;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp =
        msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                               : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    // 1: depth
    atts[1].format = g_DepthFormat;
    atts[1].samples = g_MSAASamples;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    // 2: resolve target = the presented backbuffer (MSAA only)
    atts[2].format = wd->SurfaceFormat.format;
    atts[2].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[2].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{
        1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef{2,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;
    if (msaa)
      sub.pResolveAttachments = &resolveRef;
    // Every framebuffer shares ONE depth image (and one MSAA colour image),
    // which is legal — frames run in order on one queue — only if this
    // dependency says so. It used to leave srcAccessMask at 0 and name only
    // EARLY_FRAGMENT_TESTS as the source stage, so nothing waited for the
    // PREVIOUS frame's depth write, which completes at LATE_FRAGMENT_TESTS.
    // Sync validation flags it as WRITE_AFTER_WRITE the moment the swapchain
    // has more than two images: a 2-image swapchain (this NVIDIA) never
    // overlapped enough to show it, a 3-image one (Mesa/Intel) does.
    // docs/rendering/frames-in-flight.md
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = msaa ? 3 : 2;
    rp.pAttachments = atts;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    vkCreateRenderPass(g_Device, &rp, g_Allocator, &g_DepthRenderPass);
  }

  // Swap ImGui's color-only render pass + framebuffers for our color+depth
  // (+resolve) ones. Framebuffer attachments match the render pass order:
  // MSAA → [msaaColor, depth, backbuffer]; single-sample → [backbuffer, depth].
  if (wd->RenderPass != g_DepthRenderPass && wd->RenderPass != VK_NULL_HANDLE)
    vkDestroyRenderPass(g_Device, wd->RenderPass, g_Allocator);
  wd->RenderPass = g_DepthRenderPass;
  for (uint32_t i = 0; i < wd->ImageCount; i++) {
    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[i];
    if (fd->Framebuffer)
      vkDestroyFramebuffer(g_Device, fd->Framebuffer, g_Allocator);
    VkImageView attach[3];
    uint32_t attachCount;
    if (msaa) {
      attach[0] = g_MsaaColorView;
      attach[1] = g_DepthView;
      attach[2] = fd->BackbufferView;
      attachCount = 3;
    } else {
      attach[0] = fd->BackbufferView;
      attach[1] = g_DepthView;
      attachCount = 2;
    }
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = g_DepthRenderPass;
    fb.attachmentCount = attachCount;
    fb.pAttachments = attach;
    fb.width = wd->Width;
    fb.height = wd->Height;
    fb.layers = 1;
    vkCreateFramebuffer(g_Device, &fb, g_Allocator, &fd->Framebuffer);
  }
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd,
                              VkSurfaceKHR surface, int width, int height) {
  wd->Surface = surface;

  // Decide the MSAA level once, before the render pass / pipelines are built.
  g_MSAASamples = GetMaxUsableSampleCount();

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
    // Test mode: uncapped present (FIFO throttles occluded windows to ~1 fps,
    // stalling the suite). Prefer MAILBOX. A frame-rate cap in the main loop
    // keeps the GPU from a timeout device-lost under the fast test engine.
    VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_MAILBOX_KHR,
                                        VK_PRESENT_MODE_IMMEDIATE_KHR,
                                        VK_PRESENT_MODE_FIFO_KHR};
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_PhysicalDevice, wd->Surface, &present_modes[0],
        IM_ARRAYSIZE(present_modes));
    // Which one we actually got matters: FIFO here means the suite will crawl
    // whenever the window is occluded or the session is locked.
    LOG_INFO("GUI tests: present mode {}",
             wd->PresentMode == VK_PRESENT_MODE_MAILBOX_KHR     ? "MAILBOX"
             : wd->PresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE"
                                                                : "FIFO");
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
  SetupDepthBuffer(wd);
  // Logged because a driver-dependent image count is exactly what made the
  // "camera shakes on Intel" report impossible to guess at: this NVIDIA
  // reports 2, Mesa reports 3+. See docs/rendering/frames-in-flight.md.
  LOG_INFO("Swapchain: {} images, {} per-frame slots", wd->ImageCount,
           render::kMaxFramesInFlight);
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
  if (g_DepthView)
    vkDestroyImageView(g_Device, g_DepthView, g_Allocator);
  if (g_DepthImage)
    vkDestroyImage(g_Device, g_DepthImage, g_Allocator);
  if (g_DepthMemory)
    vkFreeMemory(g_Device, g_DepthMemory, g_Allocator);
  g_DepthView = VK_NULL_HANDLE;
  g_DepthImage = VK_NULL_HANDLE;
  g_DepthMemory = VK_NULL_HANDLE;
  if (g_MsaaColorView)
    vkDestroyImageView(g_Device, g_MsaaColorView, g_Allocator);
  if (g_MsaaColorImage)
    vkDestroyImage(g_Device, g_MsaaColorImage, g_Allocator);
  if (g_MsaaColorMemory)
    vkFreeMemory(g_Device, g_MsaaColorMemory, g_Allocator);
  g_MsaaColorView = VK_NULL_HANDLE;
  g_MsaaColorImage = VK_NULL_HANDLE;
  g_MsaaColorMemory = VK_NULL_HANDLE;
  // wd->RenderPass is g_DepthRenderPass; DestroyWindow frees it.
  ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData,
                                  g_Allocator);
  g_DepthRenderPass = VK_NULL_HANDLE;
}

// Scene prepasses recorded before the main render pass begins: the S1
// key-light shadow map and the S2 SSAO prepass+AO+blur chain. Shared by
// FrameRender and CaptureScreenshot so remote screenshots show identical
// shadows/AO. Each pass's own subpass dependencies order its writes vs.
// fragment sampling, so no explicit barriers are needed here.
static void RecordScenePrepasses(VkCommandBuffer cb, uint32_t currentFrame) {
  if (!g_MeshRenderer)
    return;
  if (g_ShadowRenderer) {
    g_MeshRenderer->SetShadowsEnabled(g_App.showShadows);
    if (g_App.showShadows) {
      g_ShadowRenderer->SetClipPlanes(ui::SectionPlanes(g_App));
      g_ShadowRenderer->RecordShadowPass(cb, *g_MeshRenderer);
      g_MeshRenderer->SetShadowState(g_ShadowRenderer->LightViewProj(),
                                     g_ShadowRenderer->TexelWorldSize(),
                                     g_ShadowRenderer->TexelUVSize());
    }
  }
  if (g_SSAORenderer) {
    g_MeshRenderer->SetAOEnabled(g_App.showAmbientOcclusion);
    if (g_App.showAmbientOcclusion) {
      g_SSAORenderer->SetClipPlanes(ui::SectionPlanes(g_App));
      g_SSAORenderer->RecordSSAOPasses(cb, currentFrame, *g_MeshRenderer);
      g_MeshRenderer->SetAOState(g_SSAORenderer->InvFullResolution());
    }
  }
  // Off-toggles skip the passes entirely; the shaders won't sample the
  // (stale) maps and the image layouts stay read-only.
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
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

  ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
  {
    err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
    check_vk_result(err);

    err = vkResetFences(g_Device, 1, &fd->Fence);
    check_vk_result(err);
  }
  // The acquire and that fence are the frame's GPU/vsync wait; everything
  // after is our recording (docs/rendering/frame-timing.md).
  g_App.frameTimer.Mark(render::FramePhase::Wait, glfwGetTime());

  {
    err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
    check_vk_result(err);
    VkCommandBufferBeginInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
    check_vk_result(err);
  }

  RecordScenePrepasses(fd->CommandBuffer, wd->FrameIndex);

  {
    VkRenderPassBeginInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = wd->RenderPass;
    info.framebuffer = fd->Framebuffer;
    info.renderArea.extent.width = wd->Width;
    info.renderArea.extent.height = wd->Height;
    VkClearValue clears[2];
    clears[0] = wd->ClearValue;
    clears[1].depthStencil = {1.0f, 0};
    info.clearValueCount = 2;
    info.pClearValues = clears;
    vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
  }

  // 3D scene first, UI on top
  if (g_MeshRenderer) {
    g_MeshRenderer->Render(fd->CommandBuffer, wd->FrameIndex);
  }
  if (g_FieldRenderer) { // FEM result overlay (stands in for hidden bodies)
    g_FieldRenderer->Render(fd->CommandBuffer, wd->FrameIndex);
  }
  if (g_OutlineRenderer) {
    g_OutlineRenderer->Render(fd->CommandBuffer, wd->FrameIndex);
  }
  if (g_UnderlayRenderer) {
    // Reference images first, so the sketch lines below draw on top.
    g_UnderlayRenderer->Render(fd->CommandBuffer, wd->FrameIndex);
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
  g_App.frameTimer.Mark(render::FramePhase::Record, glfwGetTime());
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
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

void printUsage(const char* programName) {
  std::cout << "Usage: " << programName << " [OPTIONS]" << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  -f, --file <path>    Load STEP file on startup" << std::endl;
  std::cout << "  -v, --verbose        Enable verbose logging (debug level)"
            << std::endl;
  std::cout
      << "  -vv                  Enable very verbose logging (trace level)"
      << std::endl;
  std::cout << "  -h, --help           Show this help message" << std::endl;
  std::cout << "  --version            Show the Designer version" << std::endl;
  std::cout << "  --gui-tests          Run GUI test suite and exit"
            << std::endl;
  std::cout << "  --gui-test-filter <pattern>  Run only matching GUI tests "
               "(e.g. \"fillet\", \"selection_,tree_\"; implies --gui-tests)"
            << std::endl;
  std::cout << "  --gui-test-list      List GUI tests as category/name and "
               "exit (no GPU needed)"
            << std::endl;
  std::cout << "  --remote <port>      Enable JSON-RPC remote control on "
               "127.0.0.1:<port>"
            << std::endl;
  std::cout << "  --remote-bind-all    Serve --remote on 0.0.0.0 instead of "
               "127.0.0.1 (containers only:\n                       the "
               "interface is unauthenticated and touches files)"
            << std::endl;
  std::cout << "  --headless           No window/GPU: serve --remote with the "
               "document engine only"
            << std::endl;
  std::cout << "  --autosave <sec>     Autosave interval in seconds "
               "(default 60, 0 = off)"
            << std::endl;
  std::cout << "  --record <file>      Record the --remote session's semantic "
               "requests as replayable JSONL"
            << std::endl;
  std::cout << "  --hidden             Never show the window (offscreen "
               "--remote sessions, e.g. automated tests)"
            << std::endl;
  std::cout << "  --perf-hud           Show the frame-time overlay from "
               "launch (also Ctrl+Shift+F, or the\n                       "
               "frame_stats RPC; off by default)"
            << std::endl;
}

struct CommandLineArgs {
  std::string fileToLoad;
  spdlog::level::level_enum logLevel = spdlog::level::info;
  bool runGuiTests = false;
  bool listGuiTests = false;
  std::string guiTestFilter = "all";
  bool headless = false;
  bool hidden = false;
  int remotePort = 0;
  bool remoteBindAll = false;
  double autosaveSeconds = 60.0;
  std::string recordPath;
  bool perfHud = false;
};

CommandLineArgs parseCommandLineArgs(int argc, char** argv) {
  CommandLineArgs args;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      exit(0);
    } else if (arg == "--version") {
      std::cout << "Designer " << DESIGNER_VERSION << std::endl;
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
#ifndef HAVE_TEST_ENGINE
      // Fail on the flag, not four seconds later after Vulkan is up: a
      // configuration error should cost nothing to discover.
      std::cerr << "--gui-tests needs the ImGui Test Engine, and this build "
                   "was configured with -DENABLE_TEST_ENGINE=OFF"
                << std::endl;
      exit(1);
#endif
      args.runGuiTests = true;
    } else if (arg == "--gui-test-filter") {
      if (i + 1 < argc) {
        args.guiTestFilter = argv[++i];
        args.runGuiTests = true;
      } else {
        std::cerr << "--gui-test-filter requires a pattern" << std::endl;
        exit(1);
      }
    } else if (arg == "--gui-test-list") {
      args.listGuiTests = true;
    } else if (arg == "--headless") {
      args.headless = true;
    } else if (arg == "--remote-bind-all") {
      // For containers: the port is useless on loopback when the network is
      // namespaced. Unauthenticated, so it is never the default.
      args.remoteBindAll = true;
    } else if (arg == "--crash-test") {
      // Hidden: exercise the fatal-signal backtrace end-to-end (Phase 0
      // sanity). Installed handler prints the stack, then core-dumps.
      CrashHandlerSelfTest();
    } else if (arg == "--perf-hud") {
      args.perfHud = true;
    } else if (arg == "--hidden") {
      args.hidden = true;
    } else if (arg == "--record") {
      if (i + 1 < argc) {
        args.recordPath = argv[i + 1];
        i++;
      } else {
        std::cerr << "Error: --record requires a file path" << std::endl;
        exit(1);
      }
    } else if (arg == "--autosave") {
      if (i + 1 < argc) {
        args.autosaveSeconds = std::atof(argv[i + 1]);
        i++;
      } else {
        std::cerr << "Error: --autosave requires seconds (0 = off)"
                  << std::endl;
        exit(1);
      }
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

int main(int argc, char** argv) {
  InstallCrashHandler(); // fatal-signal backtraces in dev-watch logs
  CommandLineArgs args = parseCommandLineArgs(argc, argv);

  // --gui-test-list: the suite's own inventory, for the sharding runner.
  // Registration only stores the AppState pointer and fills the engine's
  // table, so this needs neither a window nor Vulkan — and the runner must
  // not need a GPU (or an X display) merely to decide who runs what.
  // Scraping the source for IM_REGISTER_TEST would drift; asking the binary
  // cannot, and a test missing from the list would be silently skipped.
  if (args.listGuiTests) {
#ifdef HAVE_TEST_ENGINE
    ImGui::CreateContext();
    ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
    RegisterGuiTests(engine, &g_App);
    ImVector<ImGuiTest*> tests;
    ImGuiTestEngine_GetTestList(engine, &tests);
    for (const ImGuiTest* test : tests)
      std::cout << test->Category << "/" << test->Name << "\n";
    return 0;
#else
    std::cerr << "--gui-test-list needs the ImGui Test Engine, and this build "
                 "was configured with -DENABLE_TEST_ENGINE=OFF"
              << std::endl;
    return 1;
#endif
  }

  // User preferences, before anything reads them: the GPU choice is consumed
  // by device selection below, and the font scale by the first theme apply.
  // GUI tests deliberately run on DEFAULTS — otherwise the developer's own
  // config decides whether a test passes, and CI and the desk disagree.
  if (!args.runGuiTests) {
    g_App.settings = settings::Load();
    g_App.settingsSaved = g_App.settings;
  }
  // A preference must not silently override an explicit flag, so the command
  // line wins where the two overlap.
  bool autosaveFromFlag = false;
  for (int i = 1; i < argc; i++)
    autosaveFromFlag = autosaveFromFlag || std::string(argv[i]) == "--autosave";
  if (!autosaveFromFlag)
    args.autosaveSeconds = g_App.settings.autosaveInterval;

  g_DisableVSync = args.runGuiTests;

  // Initialize logger with specified level
  Logger::Initialize(args.logLevel);

  // Headless: document engine + remote server, no GLFW/Vulkan/ImGui at all.
  // Must branch before any windowing call — this path runs with no display
  // server and no GPU (CI).
  if (args.headless) {
    if (args.remotePort <= 0) {
      std::cerr << "Error: --headless requires --remote <port> (there is "
                   "nothing else to serve)"
                << std::endl;
      return 2;
    }
    if (args.runGuiTests) {
      std::cerr << "Error: --gui-tests cannot run with --headless" << std::endl;
      return 2;
    }
    return RunHeadless(args.remotePort, args.fileToLoad, args.autosaveSeconds,
                       args.recordPath, args.remoteBindAll);
  }

#ifdef HAVE_NATIVEFILEDIALOG
  LOG_INFO("nativefiledialog available");
#else
  LOG_INFO("Using ImGui file browser (nativefiledialog not available)");
#endif

  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    return 1;

  // Create the window hidden, then map it ourselves (ShowWindowNonBlocking).
  // A *visible* window makes glfwCreateWindow — and glfwShowWindow — block in
  // waitForVisibilityNotify until the window is unobscured; under i3 a window
  // opening behind a fullscreen window or on a non-focused workspace never gets
  // that event, so startup hangs forever (before the remote server binds).
  // The window is shown (non-blocking) unless --hidden asks for an offscreen
  // session — rendering and screenshots work either way, since presents don't
  // need a mapped window. (--remote alone used to imply hidden, which made
  // dev-watch instances invisibly render for nobody.)
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  GLFWwindow* window = glfwCreateWindow(1280, 720, "Designer - 3D CAD Software",
                                        nullptr, nullptr);
  if (window && !args.hidden)
    ShowWindowNonBlocking(window);

  if (!glfwVulkanSupported()) {
    LOG_ERROR("GLFW: Vulkan not supported");
    return 1;
  }

  ImVector<const char*> extensions;
  uint32_t extensions_count = 0;
  const char** glfw_extensions =
      glfwGetRequiredInstanceExtensions(&extensions_count);
  for (uint32_t i = 0; i < extensions_count; i++)
    extensions.push_back(glfw_extensions[i]);
  SetupVulkan(extensions, window);

  VkSurfaceKHR surface;
  VkResult err =
      glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
  check_vk_result(err);

  int w, h;
  glfwGetFramebufferSize(window, &w, &h);
  ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
  SetupVulkanWindow(wd, surface, w, h);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
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
  g_App.perfHudOpen = args.perfHud;

  // Theme, scaled for the display's content scale (HiDPI). A monitor change
  // (window dragged to a display with another scale) re-applies it between
  // frames — the font atlas can't rebuild mid-frame, so the callback only
  // records the new scale (U4).
  float scaleX = 1.0f, scaleY = 1.0f;
  glfwGetWindowContentScale(window, &scaleX, &scaleY);
  // The font-size preference scales the theme exactly as the DPI rescale path
  // does later — one multiplication in both places, or the first frame and
  // every frame after it would disagree.
  ui::ApplySettings(g_App);
  ui::ApplyTheme(std::max(1.0f, std::max(scaleX, scaleY)) * g_App.uiFontScale);
  static float s_pendingContentScale = 0.0f; // 0 = no change pending
  glfwSetWindowContentScaleCallback(
      window, [](GLFWwindow*, float sx, float sy) {
        s_pendingContentScale = std::max(1.0f, std::max(sx, sy));
      });

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
  init_info.MSAASamples = g_MSAASamples;
  init_info.Allocator = g_Allocator;
  init_info.CheckVkResultFn = check_vk_result;
  ImGui_ImplVulkan_Init(&init_info);

  // Initialize application objects
  g_Scene = std::make_unique<Scene>();
  g_Camera = std::make_unique<Camera>();
  g_App.document = std::make_unique<Document>();
  g_App.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(g_App); // one part to start (assemblies A1)
  g_App.scene = g_Scene.get();
  g_App.camera = g_Camera.get();
  g_App.window = window;

  // Set up the shared empty-workspace isometric view.
  g_Camera->SetPosition(Camera::DefaultIsometricPosition());
  g_Camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
  g_Camera->SetAspectRatio(1280.0f / 720.0f);

  // Image-based lighting: precompute (CPU, cached) the irradiance/prefilter/
  // BRDF maps from the bundled studio HDR and upload them. Built before the
  // mesh renderer since its descriptor sets bind these textures at creation.
  g_IBLTextures = std::make_unique<IBLTextures>();
  {
    const std::string hdr =
        (RuntimePaths::DataDir() / "hdri/brown_photostudio_02_1k.hdr").string();
    IBLPrecompute ibl;
    if (ibl.Build(hdr, IblCacheDir())) {
      if (!g_IBLTextures->Upload(g_Device, g_PhysicalDevice, g_Queue,
                                 g_CommandPool, ibl))
        g_IBLTextures->MakeFallback(g_Device, g_PhysicalDevice, g_Queue,
                                    g_CommandPool);
    } else {
      g_IBLTextures->MakeFallback(g_Device, g_PhysicalDevice, g_Queue,
                                  g_CommandPool);
    }
  }

  // Key-light shadow map (S1) + SSAO (S2): initialized before the mesh
  // renderer, whose descriptor sets bind their maps at creation (like IBL).
  g_ShadowRenderer = std::make_unique<ShadowRenderer>();
  if (!g_ShadowRenderer->Initialize(g_Device, g_PhysicalDevice, g_Queue,
                                    g_QueueFamily, g_CommandPool)) {
    LOG_ERROR("ShadowRenderer initialization failed");
  } else {
    LOG_INFO("ShadowRenderer initialized successfully");
  }
  g_SSAORenderer = std::make_unique<SSAORenderer>();
  if (!g_SSAORenderer->Initialize(g_Device, g_PhysicalDevice, g_Queue,
                                  g_QueueFamily, g_CommandPool,
                                  (uint32_t)wd->Width, (uint32_t)wd->Height)) {
    LOG_ERROR("SSAORenderer initialization failed");
  } else {
    LOG_INFO("SSAORenderer initialized successfully");
  }

  // Initialize MeshRenderer with Vulkan context
  g_MeshRenderer = std::make_unique<MeshRenderer>();
  g_MeshRenderer->SetIBL(
      g_IBLTextures->IrradianceView(), g_IBLTextures->PrefilterView(),
      g_IBLTextures->BrdfLutView(), g_IBLTextures->EnvSampler(),
      g_IBLTextures->LutSampler());
  g_MeshRenderer->SetShadowMap(g_ShadowRenderer->ShadowMapView(),
                               g_ShadowRenderer->ShadowSampler());
  g_MeshRenderer->SetAOMap(g_SSAORenderer->AOView(),
                           g_SSAORenderer->AOSampler());
  bool meshRendererInitialized = g_MeshRenderer->Initialize(
      g_Device, g_PhysicalDevice, wd->RenderPass, g_Queue, g_QueueFamily,
      g_CommandPool, g_MSAASamples);
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
  bool outlineInitialized = g_OutlineRenderer->Initialize(
      g_Device, g_PhysicalDevice, wd->RenderPass, g_Queue, g_QueueFamily,
      g_CommandPool, g_MSAASamples);
  if (!outlineInitialized) {
    LOG_WARN("OutlineRenderer initialization failed");
  } else {
    LOG_INFO("OutlineRenderer initialized successfully");
    g_OutlineRenderer->SetViewportSize(1280, 720);

    // Set default outline settings for visibility testing
    OutlineSettings settings = g_OutlineRenderer->GetOutlineSettings();
    LOG_DEBUG("Default outline mode: {}", std::to_underlying(settings.mode));
    LOG_DEBUG("Default outline color: ({}, {}, {}, {})",
              settings.outlineColor.r, settings.outlineColor.g,
              settings.outlineColor.b, settings.outlineColor.a);
    LOG_DEBUG("Default outline width: {}", settings.outlineWidth);

    // Enable wireframe rendering with subtle appearance
    settings.mode = OutlineMode::WIREFRAME;
    g_OutlineRenderer->SetOutlineSettings(settings);
    LOG_DEBUG("Set outline mode to WIREFRAME with subtle styling");
  }

  // Initialize SketchRenderer
  g_SketchRenderer = std::make_unique<SketchRenderer>();
  if (g_SketchRenderer->Initialize(g_Device, g_PhysicalDevice, wd->RenderPass,
                                   g_Queue, g_QueueFamily, g_CommandPool,
                                   g_MSAASamples)) {
    g_SketchRenderer->SetViewportSize(1280, 720);
  } else {
    LOG_WARN("SketchRenderer initialization failed");
  }

  // Initialize UnderlayRenderer (sketch reference images)
  g_UnderlayRenderer = std::make_unique<UnderlayRenderer>();
  if (g_UnderlayRenderer->Initialize(g_Device, g_PhysicalDevice, wd->RenderPass,
                                     g_Queue, g_QueueFamily, g_CommandPool,
                                     g_MSAASamples)) {
    g_UnderlayRenderer->SetViewportSize(1280, 720);
  } else {
    LOG_WARN("UnderlayRenderer initialization failed");
  }

  // Initialize FieldRenderer (FEM result overlay)
  g_FieldRenderer = std::make_unique<FieldRenderer>();
  if (g_FieldRenderer->Initialize(g_Device, g_PhysicalDevice, wd->RenderPass,
                                  g_Queue, g_QueueFamily, g_CommandPool,
                                  g_MSAASamples)) {
    g_FieldRenderer->SetViewport(0, 0, 1280, 720);
  } else {
    LOG_WARN("FieldRenderer initialization failed");
  }

  // Initialize RayTracingRenderer
  g_RayTracingRenderer = std::make_unique<RayTracingRenderer>();
  bool rtInitialized = g_RayTracingRenderer->Initialize(
      g_Device, g_PhysicalDevice, g_Queue, g_QueueFamily, g_CommandPool);
  if (!rtInitialized) {
    LOG_WARN(
        "Ray Tracing: initialization failed, falling back to rasterization");
  } else {
    LOG_INFO("Ray Tracing: initialization successful");
  }

  // Hand the UI layer its pointers now that everything exists
  g_App.meshRenderer = g_MeshRenderer.get();
  g_App.outlineRenderer = g_OutlineRenderer.get();
  g_App.sketchRenderer = g_SketchRenderer.get();
  g_App.underlayRenderer = g_UnderlayRenderer.get();
  g_App.fieldRenderer = g_FieldRenderer.get();
  g_App.rayTracer = g_RayTracingRenderer.get();
  g_App.faceSelector = g_FaceSelector.get();
  g_App.ibl = g_IBLTextures.get();

  // Load startup file if specified
  if (!args.fileToLoad.empty()) {
    LOG_INFO("Loading startup file: {}", args.fileToLoad);
    ui::ImportAsFeature(g_App, args.fileToLoad);
  }

  LOG_INFO("=== APPLICATION READY - click selects edges/faces (again: cycle, "
           "Ctrl: multi-select, Shift: align camera) ===");

  // GUI test engine: always started so tests can also be run interactively;
  // --gui-tests queues the whole suite and exits with a pass/fail code.
  // Absent in a -DENABLE_TEST_ENGINE=OFF build (see CMakeLists) — the app is
  // whole without it, minus its own GUI tests and the remote ui_action tier.
#ifdef HAVE_TEST_ENGINE
  ImGuiTestEngine* testEngine = ImGuiTestEngine_CreateContext();
  ImGuiTestEngineIO& testEngineIO = ImGuiTestEngine_GetIO(testEngine);
  testEngineIO.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
  testEngineIO.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
  testEngineIO.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
  testEngineIO.ConfigLogToTTY = args.runGuiTests;
  testEngineIO.ConfigWatchdogWarning = 60.0f;
  testEngineIO.ConfigWatchdogKillTest = 300.0f;
  ImGuiTestEngine_Start(testEngine, ImGui::GetCurrentContext());
  RegisterGuiTests(testEngine, &g_App);
  if (args.runGuiTests) {
    // The filter matches the test engine's syntax: substrings against
    // "category/name", comma-separated, "-" to exclude ("all" = everything).
    ImGuiTestEngine_QueueTests(testEngine, ImGuiTestGroup_Tests,
                               args.guiTestFilter.c_str());
  }
#endif

  // Remote control (opt-in)
  RemoteServer remoteServer;
  if (args.remotePort > 0) {
    // Fail fast on a taken port, as the headless path does (HeadlessMain.cpp).
    // This path ignored the result, so `--hidden --remote <taken>` — how every
    // preview instance and dev-watch starts — came up looking healthy and
    // served nothing, and the client just timed out with no reason given.
    // This path always has a renderer (that is what distinguishes it from
    // HeadlessMain), so screenshots work; whether the GUI can be DRIVEN is
    // the separate question SetTestEngine answers.
    if (!remoteServer.Start(static_cast<uint16_t>(args.remotePort),
                            /*hasRenderer=*/true, args.remoteBindAll)) {
      LOG_ERROR("Cannot listen on port {} - is another instance using it?",
                args.remotePort);
      return 1;
    }
#ifdef HAVE_TEST_ENGINE
    remoteServer.SetTestEngine(testEngine);
#endif
    if (!args.recordPath.empty()) {
      remoteServer.StartRecording(args.recordPath);
    }
  }

  // Autosave (F2). GUI tests force it off: a mid-test autosave would break
  // their determinism (and there is nothing worth recovering).
  Autosaver autosaver(args.runGuiTests ? 0.0 : args.autosaveSeconds);
  autosaver.Prime(glfwGetTime());

  const ui::ClearColor vp = ui::ViewportClearColor();
  ImVec4 clear_color = ImVec4(vp.r, vp.g, vp.b, 1.00f);

  // Main loop
  while (!glfwWindowShouldClose(window)) {
    g_App.frameTimer.BeginFrame(glfwGetTime());
    glfwPollEvents();

    // Monitor content scale changed (window moved to another display) or the
    // user picked a font size (U5): re-apply the theme and rebuild the font
    // atlas — both must happen outside a frame, hence only flags here.
    if (s_pendingContentScale > 0.0f || g_App.fontRebuildRequested) {
      static float s_baseContentScale = 0.0f;
      if (s_baseContentScale <= 0.0f) {
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(window, &sx, &sy);
        s_baseContentScale = std::max(1.0f, std::max(sx, sy));
      }
      if (s_pendingContentScale > 0.0f)
        s_baseContentScale = s_pendingContentScale;
      s_pendingContentScale = 0.0f;
      g_App.fontRebuildRequested = false;
      vkDeviceWaitIdle(g_Device); // the old font texture may be in flight
      ImGui_ImplVulkan_DestroyFontsTexture();
      ui::ApplyTheme(s_baseContentScale * g_App.uiFontScale);
      ImGui_ImplVulkan_CreateFontsTexture();
    }

    // Resize swap chain?
    int fb_width, fb_height;
    glfwGetFramebufferSize(window, &fb_width, &fb_height);
    if (fb_width > 0 && fb_height > 0 &&
        (g_SwapChainRebuild || g_MainWindowData.Width != fb_width ||
         g_MainWindowData.Height != fb_height)) {
      ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
      // ImGui's swapchain recreate destroys wd->RenderPass at its top. Since we
      // point that at our shared g_DepthRenderPass, null it first so ImGui
      // frees only its own (throwaway) pass and ours survives; SetupDepthBuffer
      // reattaches it.
      g_MainWindowData.RenderPass = VK_NULL_HANDLE;
      ImGui_ImplVulkanH_CreateOrResizeWindow(
          g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData,
          g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount);
      SetupDepthBuffer(&g_MainWindowData);
      g_MainWindowData.FrameIndex = 0;
      g_SwapChainRebuild = false;

      // Update viewport sizes for renderers
      if (g_MeshRenderer) {
        g_MeshRenderer->SetViewportSize(fb_width, fb_height);
      }
      // SSAO targets are window-sized: recreate (waits for idle) and rebind
      // the new AO view into the mesh renderer's descriptor sets.
      if (g_SSAORenderer) {
        g_SSAORenderer->Resize((uint32_t)fb_width, (uint32_t)fb_height);
        if (g_MeshRenderer)
          g_MeshRenderer->RebindAOMap(g_SSAORenderer->AOView());
      }
      if (g_OutlineRenderer) {
        g_OutlineRenderer->SetViewportSize(fb_width, fb_height);
      }
      if (g_SketchRenderer) {
        g_SketchRenderer->SetViewportSize(fb_width, fb_height);
      }
      if (g_UnderlayRenderer) {
        g_UnderlayRenderer->SetViewportSize(fb_width, fb_height);
      }
      if (g_FieldRenderer) {
        g_FieldRenderer->SetViewport(0, 0, fb_width, fb_height);
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

    if (g_App.document) {
      autosaver.MaybeSave(*g_App.document, g_App.projectPath,
                          g_App.lastSavedVersion, glfwGetTime());
    }
    // Preferences: debounced so dragging a control is one write, and never
    // during GUI tests (they run on defaults and must not touch the
    // developer's config).
    if (!args.runGuiTests) {
      if (autosaver.Interval() != g_App.settings.autosaveInterval)
        autosaver.SetInterval(g_App.settings.autosaveInterval);
      ui::FlushSettings(g_App, glfwGetTime());
    }

    // Window title tracks the project name + a dirty marker.
    {
      static std::string lastTitle;
      const std::string title = ui::WindowTitle(g_App);
      if (title != lastTitle) {
        glfwSetWindowTitle(window, title.c_str());
        lastTitle = title;
      }
    }

    // Mirror document changes (edits, undo, redo) into the renderers
    ui::SyncDocumentRenderers(g_App);
    // Rebuild the FEM field overlay (and hide/show bodies) when the study or
    // result changes; cheap no-op otherwise.
    ui::SyncFieldRenderer(g_App);

    // S3 render-quality settings (View > Render Quality / set_render_options
    // RPC): recreate the GPU targets when they change. Both helpers wait for
    // idle internally; the mesh renderer's descriptor sets must then re-point
    // at the recreated views.
    if (g_ShadowRenderer && g_MeshRenderer &&
        g_ShadowRenderer->SetResolution((uint32_t)g_App.shadowMapSize)) {
      g_MeshRenderer->RebindShadowMap(g_ShadowRenderer->ShadowMapView());
    }
    if (g_SSAORenderer && g_MeshRenderer &&
        g_SSAORenderer->SetFullResolution(g_App.ssaoFullRes)) {
      g_MeshRenderer->RebindAOMap(g_SSAORenderer->AOView());
    }

    // Section view (I3): push the clip plane to the renderers each frame.
    if (g_MeshRenderer)
      g_MeshRenderer->SetClipPlanes(ui::SectionPlanes(g_App));
    if (g_OutlineRenderer)
      g_OutlineRenderer->SetClipPlanes(ui::SectionPlanes(g_App));
    if (g_FieldRenderer) {
      g_FieldRenderer->SetClipPlanes(ui::SectionPlanes(g_App));
      g_FieldRenderer->SetDeformScale(g_App.simulation.deformScale);
    }

    // Advance an in-flight offline render by one sample batch (the Render modal
    // shows progress; runs on the main thread with its own one-shot submits).
    ui::PumpRenderJob(g_App);

    // Everything above is per-frame CPU work on the document and renderers;
    // the ImGui build is measured on its own (docs/rendering/frame-timing.md).
    g_App.frameTimer.Mark(render::FramePhase::Update, glfwGetTime());

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

    if (g_UnderlayRenderer && g_Camera) {
      g_UnderlayRenderer->UpdateCamera(g_Camera->GetViewMatrix(),
                                       g_Camera->GetProjectionMatrix());
    }

    if (g_FieldRenderer && g_Camera) {
      g_FieldRenderer->UpdateCamera(g_Camera->GetViewMatrix(),
                                    g_Camera->GetProjectionMatrix());
    }

    // Rendering
    ImGui::Render();
    g_App.frameTimer.Mark(render::FramePhase::BuildUI, glfwGetTime());
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool is_minimized =
        (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
    if (!is_minimized) {
      wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
      wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
      wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
      wd->ClearValue.color.float32[3] = clear_color.w;
      FrameRender(wd, draw_data);
      FramePresent(wd);
      // Before the uncapped-mode sleep below: the frame is over when the
      // image is presented, not when we have finished idling.
      g_App.frameTimer.EndFrame(glfwGetTime());
#ifdef HAVE_TEST_ENGINE
      ImGuiTestEngine_PostSwap(testEngine);
#endif

      // In uncapped test mode, cap the frame rate so the GPU isn't driven to
      // a timeout device-lost by the fast test-engine loop. 60 fps keeps the
      // suite well under the ctest timeout while giving the GPU headroom.
      if (g_DisableVSync) {
        static double lastFrame = 0.0;
        const double now = glfwGetTime();
        const double minFrame = 1.0 / 60.0;
        const double elapsed = now - lastFrame;
        if (elapsed < minFrame && elapsed > 0.0) {
          std::this_thread::sleep_for(
              std::chrono::duration<double>(minFrame - elapsed));
        }
        lastFrame = glfwGetTime();
      }

      // Remote screenshot: re-render this frame into a readable image
      std::string shotPath;
      if (remoteServer.WantsScreenshot(shotPath)) {
        vkQueueWaitIdle(g_Queue); // frame resources must be reusable
        std::string shotError;
        FrameStats shotStats;
        // Postconditions judge the 3D viewport, not the panel chrome.
        shotStats.x = static_cast<int>(g_App.viewportPos.x);
        shotStats.y = static_cast<int>(g_App.viewportPos.y);
        shotStats.w = static_cast<int>(g_App.viewportSize.x);
        shotStats.h = static_cast<int>(g_App.viewportSize.y);
        const bool ok = CaptureScreenshot(
            g_Device, g_PhysicalDevice, g_Queue, g_CommandPool, wd->RenderPass,
            wd->SurfaceFormat.format, static_cast<uint32_t>(wd->Width),
            static_cast<uint32_t>(wd->Height), wd->ClearValue,
            [&](VkCommandBuffer cb) {
              if (g_MeshRenderer)
                g_MeshRenderer->Render(cb, wd->FrameIndex);
              if (g_FieldRenderer)
                g_FieldRenderer->Render(cb, wd->FrameIndex);
              if (g_OutlineRenderer)
                g_OutlineRenderer->Render(cb, wd->FrameIndex);
              if (g_UnderlayRenderer)
                g_UnderlayRenderer->Render(cb, wd->FrameIndex);
              if (g_SketchRenderer)
                g_SketchRenderer->Render(cb, wd->FrameIndex);
              ImGui_ImplVulkan_RenderDrawData(draw_data, cb);
            },
            shotPath, &shotError, g_MSAASamples,
            [&](VkCommandBuffer cb) {
              RecordScenePrepasses(cb, wd->FrameIndex);
            },
            &shotStats);
        remoteServer.ScreenshotDone(ok, shotError, shotStats.dominantFraction,
                                    shotStats.hash);
      }
    }

#ifdef HAVE_TEST_ENGINE
    if (args.runGuiTests && ImGuiTestEngine_IsTestQueueEmpty(testEngine)) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
#endif
  }

  remoteServer.Stop();

  // Clean exit with nothing unsaved: the sidecar has no recovery value.
  // A dirty exit deliberately KEEPS it (safety over tidiness).
  if (g_App.document && g_App.document->Version() == g_App.lastSavedVersion) {
    Autosaver::Clear(g_App.projectPath);
  }

  // Collect GUI test results before teardown
  int exitCode = 0;
#ifdef HAVE_TEST_ENGINE
  if (args.runGuiTests) {
    int countTested = 0, countSuccess = 0;
    ImGuiTestEngine_GetResult(testEngine, countTested, countSuccess);
    LOG_INFO("GUI tests: {}/{} passed", countSuccess, countTested);
    exitCode = (countTested > 0 && countTested == countSuccess) ? 0 : 1;
  }
  ImGuiTestEngine_Stop(testEngine);
#endif

  // Cleanup
  err = vkDeviceWaitIdle(g_Device);
  check_vk_result(err);

  g_App.document.reset();
  g_App.commands.reset();
  g_Scene.reset();
  g_Camera.reset();
  g_MeshRenderer.reset();
  g_SketchRenderer.reset();
  // Must reset here (not at static-destruction time): ~UnderlayRenderer ->
  // Shutdown() calls vkDeviceWaitIdle, so it has to run while g_Device is still
  // alive — before CleanupVulkan below. Omitting it left the global to destruct
  // at exit on an already-freed device -> SIGSEGV at shutdown.
  g_UnderlayRenderer.reset();
  g_FieldRenderer.reset();
  g_OutlineRenderer.reset();
  g_RayTracingRenderer.reset();
  g_ShadowRenderer.reset();
  g_SSAORenderer.reset();
  g_FaceSelector.reset();
  if (g_IBLTextures) {
    g_IBLTextures->Shutdown(g_Device);
    g_IBLTextures.reset();
  }

  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
#ifdef HAVE_TEST_ENGINE
  // Test engine context must outlive the ImGui context it hooks
  ImGuiTestEngine_DestroyContext(testEngine);
#endif

  CleanupVulkanWindow();
  CleanupVulkan();

  glfwDestroyWindow(window);
  glfwTerminate();

  return exitCode;
}
