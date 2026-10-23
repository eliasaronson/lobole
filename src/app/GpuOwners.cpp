// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The owner table (app/GpuOwners.h;
// docs/rendering/README.md#rebuilding-the-device): the instance, the device
// ladder, every row that makes a Vulkan object for the frame, and the rebuild
// that walks them back and forth.

#include "app/GpuOwners.h"

#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "common/Watchdog.h"
#include "core/io/Autosaver.h"
#include "core/io/CrashReports.h"
#include "render/FieldRenderer.h"
#include "render/FramesInFlight.h"
#include "render/IBL.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SSAORenderer.h"
#include "render/SceneRenderer.h"
#include "render/ShadowRenderer.h"
#include "render/SketchRenderer.h"
#include "render/StockRenderer.h"
#include "render/UnderlayRenderer.h"
#include "render/VulkanCommon.h"
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/ProjectActions.h" // AutosaveProject
#include "ui/RendererSync.h"

#include "imgui_impl_glfw.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>

namespace {
// The state the renderers are announced in, and what the last build was
// given: a rebuild builds from the same start.
ui::AppState* s_app = nullptr;
gpuowners::Start s_start;
ui::AppState& App() { return *s_app; }
// Writable per-user cache for precomputed IBL maps. The executable dir may be
// read-only (Nix store, Flatpak /app), so the cache cannot live beside it.
std::string IblCacheDir() {
  const std::filesystem::path dir = RuntimePaths::UserCacheDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}
// A rebuild is running (LOBOLE_FAULT=rebuild fails its device creation).
bool s_rebuilding = false;
// ImGui's backends as the last build made them (gpuowners::KeepImGuiBackends).
void* s_rendererData = nullptr;
void* s_platformData = nullptr;
} // namespace
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
#endif

// Vulkan data
VkAllocationCallbacks* g_Allocator = nullptr;
VkInstance g_Instance = VK_NULL_HANDLE;
VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
VkDevice g_Device = VK_NULL_HANDLE;
uint32_t g_QueueFamily = (uint32_t)-1;
VkQueue g_Queue = VK_NULL_HANDLE;
VkPipelineCache g_PipelineCache = VK_NULL_HANDLE;
VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
VkCommandPool g_CommandPool = VK_NULL_HANDLE;

ImGui_ImplVulkanH_Window g_MainWindowData;
// THREE, not two, and it is worth ten times the frame rate on a PRIME
// laptop: with a 2-image FIFO swapchain the CPU cannot begin a frame until
// the presentation engine releases the image it is scanning out, so a frame
// that overruns the refresh period by a microsecond costs a WHOLE extra
// period — and the GPU, blocked and idle for most of it, clocks down, which
// makes the next frame overrun too. Measured on the RTX 4060 + Intel-driven
// display (docs/rendering/frame-pacing.md#swapchain-and-present): 10-25 fps at
// two images, 48 fps at three, with the same scene and the same binary. Four
// and five are WORSE again (37-38), so this is not "more is better" — it is one
// spare image so the queue never runs dry.
//
// The driver clamps this to what the surface allows, and the renderers hold
// kMaxFramesInFlight = 8 slots indexed by image
// (docs/rendering/frame-pacing.md#frames-in-flight), so a bigger count is
// correct here, only slower.
uint32_t g_MinImageCount = 3;
bool g_DisableVSync = false;
VkRenderPass g_OffscreenPass = VK_NULL_HANDLE;

// Application objects
std::unique_ptr<MeshRenderer> g_MeshRenderer;
std::unique_ptr<OutlineRenderer> g_OutlineRenderer;
std::unique_ptr<SketchRenderer> g_SketchRenderer;
std::unique_ptr<UnderlayRenderer> g_UnderlayRenderer;
std::unique_ptr<FieldRenderer> g_FieldRenderer;
std::unique_ptr<StockRenderer> g_StockRenderer;

// THE list of renderers that draw into the 3D scene, IN DRAW ORDER
// (docs/rendering/README.md#scene-renderers). The order is real information,
// not incidental: the field and the stock stand in front of the mesh they
// replace, the outline goes over the solids it traces, reference images go
// under the sketch lines drawn on top of them.
//
// Every per-frame duty below loops over this instead of naming six renderers
// six times. That is the whole point — the same list written out seven times
// is how the last three renderers each shipped with one of their wires
// missing.
std::vector<SceneRenderer*> g_Scene;

// Filled from the table's scene rows after every build, the start's and each
// rebuild's. Null entries are skipped, so one that failed to initialize is
// simply absent rather than a crash.
static void RegisterSceneRenderers() {
  // ORDER IS THE CONTENT of this function. tests/test_conventions.cpp reads it
  // to check that every SceneRenderer implementation appears here — being
  // absent means drawing nothing at all.
  g_Scene.clear();
  const std::pair<const char*, SceneRenderer*> all[] = {
      {"g_MeshRenderer", g_MeshRenderer.get()},
      {"g_FieldRenderer", g_FieldRenderer.get()},
      {"g_StockRenderer", g_StockRenderer.get()},
      {"g_OutlineRenderer", g_OutlineRenderer.get()},
      {"g_UnderlayRenderer", g_UnderlayRenderer.get()},
      {"g_SketchRenderer", g_SketchRenderer.get()},
  };
  for (const auto& [name, r] : all) {
    // Null: its row's Initialize failed, and InitRenderer named it in
    // renderer.missing. It draws nothing rather than half a picture.
    if (r == nullptr) {
      LOG_DEBUG("scene renderer {} is not drawn: its row failed", name);
      continue;
    }
    g_Scene.push_back(r);
  }
}

// The mirror of RegisterSceneRenderers, deliberately adjacent to it because
// the two lists must agree.
//
// Teardown is ownership, not the frame contract, so the unique_ptrs are still
// reset by name — but ONCE, here, instead of in both shutdown paths. That is
// the list the underlay was missing from (a222bc8): it survived as a static,
// its destructor ran at process exit after the Vulkan device was gone, and
// ~UnderlayRenderer -> Shutdown() -> vkDeviceWaitIdle on a dead device was
// "the long-standing shutdown crash".
std::unique_ptr<RayTracingRenderer> g_RayTracingRenderer;
std::unique_ptr<ShadowRenderer> g_ShadowRenderer;
std::unique_ptr<SSAORenderer> g_SSAORenderer;
render::GpuTimer g_GpuTimer; // per-pass GPU timing (frame-timing.md)
std::unique_ptr<FaceSelector> g_FaceSelector;
std::unique_ptr<IBLTextures> g_IBLTextures;
// The procedural sky's maps (render_still backdrop: sky), for the one sun
// they were built for. Built on first use: most sessions never ask for one.
static std::unique_ptr<IBLTextures> g_SkyIBL;
static std::pair<long, long> g_SkyIBLKey{-1, -1};
// LOBOLE_RENDERER and LOBOLE_FAULT, read once at start (app/StartupReport.h).
startup::RendererChoice g_RendererChoice = startup::RendererChoice::Automatic;
startup::Fault g_Fault;
// device_create and swapchain fail the FIRST device tried, once.
static bool g_FaultSpent = false;
static bool Faulted(startup::Fault::Kind kind) {
  if (g_Fault.kind != kind || g_FaultSpent)
    return false;
  g_FaultSpent = true;
  return true;
}
static bool FaultsRenderer(const char* name) {
  return g_Fault.kind == startup::Fault::Kind::RendererInit &&
         g_Fault.renderer == name;
}

// ImGui's swapchain recreate reports through CheckVkResultFn, which used to
// abort on any error. While a recreate runs, the first error is kept here
// instead and the frame loop decides: retry next frame, rebuild the surface,
// or treat the device as lost.
bool g_CollectVkErrors = false;
VkResult g_CollectedVkError = VK_SUCCESS;
void check_vk_result(VkResult err) {
  if (err == VK_SUCCESS)
    return;
  if (g_CollectVkErrors && err < 0) {
    if (g_CollectedVkError == VK_SUCCESS)
      g_CollectedVkError = err;
    return;
  }
  fprintf(stderr, "[vulkan] Error: %s\n", startup::VkResultName(err).c_str());
  if (err == VK_ERROR_DEVICE_LOST) {
    ui::MarkDeviceLost(App(), "in the interface renderer");
    return;
  }
  if (err < 0)
    abort(); // still a crash report: the result's name is on stderr above
}

// What start-up has seen, for the failure's message.
static std::vector<std::string> g_SeenDevices;

// A startup Vulkan call: a failure becomes a StartupError naming the step
// and the result, which main() turns into the report and exit code 3
// (docs/architecture.md#when-the-renderer-cannot-start).
void CheckStartup(const char* step, VkResult r, const std::string& detail) {
  if (r >= 0)
    return;
  startup::Failure f;
  f.step = step;
  f.result = startup::VkResultName(r);
  f.detail = detail;
  f.devices = g_SeenDevices;
  throw startup::StartupError(std::move(f));
}
[[noreturn]] void FailStartup(const char* step, const std::string& detail,
                              const std::string& result) {
  startup::Failure f;
  f.step = step;
  f.result = result;
  f.detail = detail;
  f.devices = g_SeenDevices;
  throw startup::StartupError(std::move(f));
}

static const char* DeviceTypeName(VkPhysicalDeviceType t) {
  switch (t) {
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    return "discrete";
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    return "integrated";
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    return "virtual";
  case VK_PHYSICAL_DEVICE_TYPE_CPU:
    return "software";
  default:
    return "other";
  }
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

// The Vulkan instance. Throws StartupError: a machine with no Vulkan driver
// at all fails HERE, with VK_ERROR_INCOMPATIBLE_DRIVER.
static void CreateInstance(std::vector<const char*> instance_extensions) {
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
    app_info.pApplicationName = "Lobole";
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
    CheckStartup(startup::step::kInstance, err);

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

    create_info.enabledExtensionCount = (uint32_t)instance_extensions.size();
    create_info.ppEnabledExtensionNames = instance_extensions.data();
    err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
    CheckStartup(startup::step::kInstance, err,
                 err == VK_ERROR_INCOMPATIBLE_DRIVER
                     ? "No Vulkan driver loaded: the loader found no driver "
                       "(ICD) for this machine's GPU, or none that works."
                     : "");

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
}

// Every device the loader offers, scored as devicepref expects (< 0: cannot
// present to `window`). Fills the Preferences combo and g_SeenDevices.
struct DeviceCandidate {
  VkPhysicalDevice device = VK_NULL_HANDLE;
  std::string name;
  VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
  int score = 0;
};
// `listed` is vkEnumeratePhysicalDevices' own result: a rebuild remakes the
// instance when it fails.
static std::vector<DeviceCandidate> EnumerateDevices(GLFWwindow* window,
                                                     VkResult* listed) {
  uint32_t count = 0;
  *listed = vkEnumeratePhysicalDevices(g_Instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  if (count > 0 && *listed >= 0)
    *listed = vkEnumeratePhysicalDevices(g_Instance, &count, devices.data());
  if (*listed < 0)
    count = 0;
  devices.resize(count);

  // Throwaway surface, purely so each device can be asked whether it can
  // present to THIS window. A device that cannot is ineligible however the
  // user feels about it: choosing one aborted startup with "no WSI support
  // on physical device 0", and the only way back would be hand-editing
  // settings.json. Measured here — the Arc iGPU cannot present under Xvfb,
  // and glfwGetPhysicalDevicePresentationSupport says it CAN (it asks the X
  // server, which answers before Mesa finds no DRI3), so only the real
  // surface query is trustworthy. The app's own surface is made per rung of
  // the ladder, hence this one.
  VkSurfaceKHR probe = VK_NULL_HANDLE;
  if (window == nullptr ||
      glfwCreateWindowSurface(g_Instance, window, g_Allocator, &probe) !=
          VK_SUCCESS)
    probe = VK_NULL_HANDLE;

  std::vector<DeviceCandidate> out;
  g_SeenDevices.clear();
  for (VkPhysicalDevice pd : devices) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
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
    DeviceCandidate c;
    c.device = pd;
    c.name = props.deviceName;
    c.type = props.deviceType;
    c.score = !canPresent                                                  ? -1
              : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 2
              : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                           : 0;
    g_SeenDevices.push_back(c.name + " (" + DeviceTypeName(c.type) +
                            (canPresent ? "" : ", cannot show this window") +
                            ")");
    out.push_back(std::move(c));
  }
  if (probe != VK_NULL_HANDLE)
    vkDestroySurfaceKHR(g_Instance, probe, g_Allocator);
  App().gpuDevices.clear();
  for (const DeviceCandidate& c : out)
    App().gpuDevices.push_back(c.name);
  return out;
}

// The logical device on g_PhysicalDevice, its queue and pools. Throws
// StartupError (step kDevice); whatever it made is left for DestroyDevice.
static void CreateDevice(bool wsi) {
  VkResult err;
  g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
  if (g_QueueFamily == (uint32_t)-1)
    FailStartup(startup::step::kDevice, "it has no graphics queue");

  // Create Logical Device
  {
    ImVector<const char*> device_extensions;
    if (wsi)
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
    // A quick still draws its lines k times wider at k times the size
    // (OutlineRenderer reads the same feature to know it may).
    enabledFeatures.wideLines = supportedFeatures.wideLines;

    VkDeviceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &asFeatures;
    create_info.pEnabledFeatures = &enabledFeatures;
    create_info.queueCreateInfoCount =
        sizeof(queue_info) / sizeof(queue_info[0]);
    create_info.pQueueCreateInfos = queue_info;
    create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
    create_info.ppEnabledExtensionNames = device_extensions.Data;
    // LOBOLE_FAULT=rebuild: every rung of a rebuild fails here, so the
    // backstop is reached (tests/device_loss_smoke.py).
    const bool rebuildFault =
        s_rebuilding && g_Fault.kind == startup::Fault::Kind::Rebuild;
    err = rebuildFault || Faulted(startup::Fault::Kind::DeviceCreate)
              ? VK_ERROR_INITIALIZATION_FAILED
              : vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator,
                               &g_Device);
    if (err != VK_SUCCESS)
      g_Device = VK_NULL_HANDLE;
    CheckStartup(startup::step::kDevice, err);
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
    CheckStartup(startup::step::kDevice, err);
  }

  // Create Command Pool
  {
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = g_QueueFamily;
    err =
        vkCreateCommandPool(g_Device, &pool_info, g_Allocator, &g_CommandPool);
    CheckStartup(startup::step::kDevice, err);
  }
}

static void DestroyDevice() {
  if (g_Device != VK_NULL_HANDLE) {
    // On a lost device this returns VK_ERROR_DEVICE_LOST in finite time
    // (the spec's Lost Device section), and destroying is still valid.
    (void)vkDeviceWaitIdle(g_Device);
    if (g_CommandPool != VK_NULL_HANDLE)
      vkDestroyCommandPool(g_Device, g_CommandPool, g_Allocator);
    if (g_DescriptorPool != VK_NULL_HANDLE)
      vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
    vkDestroyDevice(g_Device, g_Allocator);
  }
  g_CommandPool = VK_NULL_HANDLE;
  g_DescriptorPool = VK_NULL_HANDLE;
  g_Device = VK_NULL_HANDLE;
  g_Queue = VK_NULL_HANDLE;
  g_QueueFamily = (uint32_t)-1;
}

static std::string AtStep(const std::string& step) {
  if (step == startup::step::kDevice)
    return "at device creation";
  if (step == startup::step::kSwapchain)
    return "at swapchain setup";
  if (step == startup::step::kSurface)
    return "at the window surface";
  return "when it tried to " + step;
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
VkSampleCountFlagBits g_MSAASamples = VK_SAMPLE_COUNT_1_BIT;
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

// Recreates the depth image at the window size and overrides ImGui's
// color-only render pass + framebuffers with color+depth ones. Must run after
// every ImGui_ImplVulkanH_CreateOrResizeWindow (which recreates them
// color-only) and before ImGui_ImplVulkan_Init.
void SetupDepthBuffer(ImGui_ImplVulkanH_Window* wd) {
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
  vkutil::AllocateImageMemory(g_Device, g_PhysicalDevice, g_DepthImage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              g_DepthMemory);
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
    vkutil::AllocateImageMemory(g_Device, g_PhysicalDevice, g_MsaaColorImage,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                g_MsaaColorMemory);
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
    // docs/rendering/frame-pacing.md#frames-in-flight
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

// The --offscreen twin of SetupDepthBuffer's render pass (above): identical
// attachment shape, formats, samples and subpass dependency — the renderers'
// pipelines are built against THIS pass, and CaptureScreenshot builds its
// framebuffer against it, so the two definitions never have to be compatible
// with each other, only self-consistent. The one deliberate difference: the
// readback attachment ends at COLOR_ATTACHMENT_OPTIMAL, not PRESENT_SRC_KHR —
// there is no swapchain, and PRESENT_SRC is a VK_KHR_swapchain layout the
// offscreen device does not even enable. Keep the dependency in sync with
// SetupDepthBuffer's (the WRITE_AFTER_WRITE story documented there).
static VkRenderPass CreateOffscreenRenderPass(VkFormat format,
                                              VkSampleCountFlagBits samples) {
  const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;
  VkAttachmentDescription atts[3] = {};
  // 0: color (multisampled when MSAA; the readback image otherwise)
  atts[0].format = format;
  atts[0].samples = samples;
  atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[0].storeOp =
      msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
  atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  // 1: depth
  atts[1].format = g_DepthFormat;
  atts[1].samples = samples;
  atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  // 2: resolve target = the readback image (MSAA only)
  atts[2].format = format;
  atts[2].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  atts[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[2].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depthRef{
      1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colorRef;
  sub.pDepthStencilAttachment = &depthRef;
  if (msaa)
    sub.pResolveAttachments = &resolveRef;
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
  VkRenderPass pass = VK_NULL_HANDLE;
  vkCreateRenderPass(g_Device, &rp, g_Allocator, &pass);
  return pass;
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd,
                              VkSurfaceKHR surface, int width, int height) {
  wd->Surface = surface;

  // Decide the MSAA level once, before the render pass / pipelines are built.
  g_MSAASamples = GetMaxUsableSampleCount();

  // Each of these used to be an exit(-1) or an abort; now it is a rung of
  // the device ladder that failed, and the next device is tried.
  VkBool32 res = VK_FALSE;
  CheckStartup(startup::step::kSwapchain,
               vkGetPhysicalDeviceSurfaceSupportKHR(
                   g_PhysicalDevice, g_QueueFamily, wd->Surface, &res));
  if (res != VK_TRUE)
    FailStartup(startup::step::kSwapchain,
                "its graphics queue cannot present to this window");
  {
    VkSurfaceCapabilitiesKHR probe{};
    CheckStartup(startup::step::kSwapchain,
                 vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                     g_PhysicalDevice, wd->Surface, &probe));
    uint32_t formats = 0;
    CheckStartup(startup::step::kSwapchain,
                 vkGetPhysicalDeviceSurfaceFormatsKHR(
                     g_PhysicalDevice, wd->Surface, &formats, nullptr));
    if (formats == 0)
      FailStartup(startup::step::kSwapchain,
                  "the window surface offers no image format");
  }
  if (Faulted(startup::Fault::Kind::Swapchain))
    FailStartup(startup::step::kSwapchain, "LOBOLE_FAULT=swapchain",
                startup::VkResultName(VK_ERROR_INITIALIZATION_FAILED));

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
  // ImGui reports through CheckVkResultFn only once it is initialised, which
  // is later: before that its errors vanish. Kept here instead, and the
  // swapchain it built is checked for itself.
  g_CollectVkErrors = true;
  g_CollectedVkError = VK_SUCCESS;
  ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device,
                                         wd, g_QueueFamily, g_Allocator, width,
                                         height, g_MinImageCount);
  g_CollectVkErrors = false;
  CheckStartup(startup::step::kSwapchain, g_CollectedVkError);
  if (wd->Swapchain == VK_NULL_HANDLE || wd->ImageCount == 0)
    FailStartup(startup::step::kSwapchain, "no swapchain images");
  SetupDepthBuffer(wd);
  // Logged because a driver-dependent image count is exactly what made the
  // "camera shakes on Intel" report impossible to guess at: this NVIDIA
  // reports 2, Mesa reports 3+. See
  // docs/rendering/frame-pacing.md#frames-in-flight.
  App().swapchainImages = wd->ImageCount;
  App().presentMode =
      wd->PresentMode == VK_PRESENT_MODE_MAILBOX_KHR        ? "mailbox"
      : wd->PresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR    ? "immediate"
      : wd->PresentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "fifo_relaxed"
                                                            : "fifo";
  // ...and logged WITH THE SURFACE'S OWN LIMITS, because the request is only
  // a request: the driver clamps it into [minImageCount, maxImageCount]. That
  // is what makes this portable to report rather than to guess — measured
  // here, NVIDIA allows 2..8 and hands back exactly what we ask for, while
  // Mesa on the Intel Arc answers 3 whether asked for 2, 3 or 4. A machine
  // that lands on 2 anyway (a surface with maxImageCount 2) is on the FIFO
  // cliff with nothing the app can do, and this line is how anyone would
  // know (docs/rendering/frame-pacing.md#swapchain-and-present).
  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_PhysicalDevice, wd->Surface,
                                            &caps);
  LOG_INFO("Swapchain: {} images (asked {}, surface allows {}..{}), {} "
           "per-frame slots",
           wd->ImageCount, g_MinImageCount, caps.minImageCount,
           caps.maxImageCount == 0 ? std::string("no limit")
                                   : std::to_string(caps.maxImageCount),
           render::kMaxFramesInFlight);
}

// The window's surface again, after VK_ERROR_SURFACE_LOST_KHR: the old
// swapchain goes with the old surface (a new one may not name an old one
// from another surface), and the frame loop's recreate builds on the new.
// False when it could not be made; tried again next frame.
bool RebuildSurface(GLFWwindow* window) {
  ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
  vkDeviceWaitIdle(g_Device);
  if (wd->Swapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(g_Device, wd->Swapchain, g_Allocator);
    wd->Swapchain = VK_NULL_HANDLE;
  }
  if (wd->Surface != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(g_Instance, wd->Surface, g_Allocator);
    wd->Surface = VK_NULL_HANDLE;
  }
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  const VkResult r =
      glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
  if (r != VK_SUCCESS) {
    LOG_WARN("Rendering: could not make the window surface again ({})",
             startup::VkResultName(r));
    return false;
  }
  wd->Surface = surface;
  LOG_INFO("Rendering: the window surface was made again");
  return true;
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
// The sky's maps for a sun at (azimuth, elevation) degrees
// (docs/rendering/still-render.md#the-sky): the image generated, then
// convolved exactly as the HDRI is (about 0.2 s, so not cached on disk). The
// sun is rounded to a tenth of a degree first, so the key and the pixels
// agree. One sky is held at a time; a different sun replaces it behind a
// wait-idle, since a still that used the old one has ended by the time
// another can ask.
IBLTextures* SkyEnvironment(double azimuthDeg, double elevationDeg) {
  const std::pair<long, long> key{std::lround(azimuthDeg * 10.0),
                                  std::lround(elevationDeg * 10.0)};
  if (g_SkyIBL && key == g_SkyIBLKey)
    return g_SkyIBL.get();
  if (g_SkyIBL) {
    vkDeviceWaitIdle(g_Device);
    g_SkyIBL->Shutdown(g_Device);
    g_SkyIBL.reset();
  }
  const double az = glm::radians(key.first / 10.0);
  const double el = glm::radians(key.second / 10.0);
  const glm::vec3 sun(static_cast<float>(std::cos(el) * std::cos(az)),
                      static_cast<float>(std::cos(el) * std::sin(az)),
                      static_cast<float>(std::sin(el)));
  const auto t0 = std::chrono::steady_clock::now();
  IBLPrecompute ibl;
  ibl.BuildFromImage(IBLPrecompute::GenerateSky(sun));
  auto tex = std::make_unique<IBLTextures>();
  if (!tex->Upload(g_Device, g_PhysicalDevice, g_Queue, g_CommandPool, ibl)) {
    tex->Shutdown(g_Device);
    LOG_ERROR("IBL: the sky's maps could not be uploaded");
    return nullptr;
  }
  LOG_INFO("IBL: sky for sun azimuth {:.1f}, elevation {:.1f} deg in {:.2f} s",
           key.first / 10.0, key.second / 10.0,
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
               .count());
  g_SkyIBL = std::move(tex);
  g_SkyIBLKey = key;
  return g_SkyIBL.get();
}

static void ShutdownSkyEnvironment() {
  if (g_SkyIBL)
    g_SkyIBL->Shutdown(g_Device);
  g_SkyIBL.reset();
  g_SkyIBLKey = {-1, -1};
}
// A renderer whose Initialize failed — or that LOBOLE_FAULT=renderer_init:
// <name> says failed — is NAMED, in App().renderer.missing (the menu bar's
// notice, About, ping), and dropped, so nothing draws through a half-built
// one: every user of these pointers checks for null, and
// RegisterSceneRenderers skips it. `init` is Initialize plus whatever must
// follow a success.
template <class R, class Init>
static void InitRenderer(std::unique_ptr<R>& r, const char* name, Init&& init) {
  const bool ok = !FaultsRenderer(name) && init(*r);
  if (ok) {
    LOG_INFO("Renderer {}: initialized", name);
    return;
  }
  LOG_WARN("Renderer {}: initialization failed{} — the picture is missing "
           "what it draws",
           name, FaultsRenderer(name) ? " (LOBOLE_FAULT)" : "");
  App().renderer.missing.push_back(name);
  r.reset();
}

// The mesh renderer is the picture: without it there is nothing to show,
// so its failure ends the start like a device that cannot be created.
static void InitMeshRenderer(const std::function<bool()>& init) {
  if (!FaultsRenderer("mesh") && init()) {
    LOG_INFO("Renderer mesh: initialized");
    return;
  }
  FailStartup(startup::step::kMeshRenderer,
              FaultsRenderer("mesh")
                  ? "LOBOLE_FAULT=renderer_init:mesh"
                  : "Its pipelines could not be built on " + App().gpuName +
                        " (the log says which); its shaders may be missing "
                        "from the install.");
}

// The path tracer, where the device has one. Off in safe mode, and never on
// a software device: Mesa's lavapipe does offer ray tracing now, but a
// path-traced still on the CPU is hours of a frozen-looking app, so it is
// refused by name (ui::PathTracerRefusal) and the quick still serves.
static void InitRayTracing(bool safeMode) {
  App().renderer.rayTracing = false;
  if (safeMode) {
    LOG_INFO("Ray tracing: off (safe mode)");
    return;
  }
  if (App().renderer.software) {
    LOG_INFO("Ray tracing: off on {}, a software device; the quick still "
             "renders",
             App().gpuName);
    return;
  }
  g_RayTracingRenderer = std::make_unique<RayTracingRenderer>();
  if (!FaultsRenderer("raytracing") &&
      g_RayTracingRenderer->Initialize(g_Device, g_PhysicalDevice, g_Queue,
                                       g_QueueFamily, g_CommandPool)) {
    App().renderer.rayTracing = true;
    LOG_INFO("Ray tracing: initialization successful");
    return;
  }
  // Not fatal: modeling, measurement and the quick still all serve, and
  // render_still says why in its own response.
  LOG_WARN("Ray tracing: unavailable on {}{} — the path-traced still will "
           "refuse",
           App().gpuName,
           FaultsRenderer("raytracing") ? " (LOBOLE_FAULT)" : "");
  g_RayTracingRenderer.reset();
}

// ---------------------------------------------------------------------------
// The owner table (docs/rendering/README.md#rebuilding-the-device)

namespace {

// The window's swapchain, and ImGui's backend built against it.
ImGui_ImplVulkan_InitInfo ImGuiInitInfo() {
  ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
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
  // From the swapchain just made: a rebuilt one may have another count.
  init_info.ImageCount = wd->ImageCount;
  init_info.MSAASamples = g_MSAASamples;
  init_info.Allocator = g_Allocator;
  init_info.CheckVkResultFn = check_vk_result;
  return init_info;
}

// What the scene renderers are built against: the window's render pass, or
// --offscreen's; and the size they start at.
VkRenderPass ScenePass() {
  return s_start.window != nullptr ? g_MainWindowData.RenderPass
                                   : g_OffscreenPass;
}
uint32_t SceneWidth() {
  return s_start.window != nullptr ? 1280u : kOffscreenWidth;
}
uint32_t SceneHeight() {
  return s_start.window != nullptr ? 720u : kOffscreenHeight;
}

// The four that start alike: a pipeline on the scene pass, a viewport size.
template <class R> bool InitOnScenePass(R& r) {
  if (!r.Initialize(g_Device, g_PhysicalDevice, ScenePass(), g_Queue,
                    g_QueueFamily, g_CommandPool, g_MSAASamples))
    return false;
  if constexpr (requires { r.SetViewportSize(0u, 0u); })
    r.SetViewportSize(SceneWidth(), SceneHeight());
  else
    r.SetViewport(0, 0, SceneWidth(), SceneHeight());
  return true;
}

void CreateWindowRow() {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  CheckStartup(startup::step::kSurface,
               glfwCreateWindowSurface(g_Instance, s_start.window, g_Allocator,
                                       &surface));
  int w = 0, h = 0;
  glfwGetFramebufferSize(s_start.window, &w, &h);
  SetupVulkanWindow(&g_MainWindowData, surface, w, h);
}
void DestroyWindowRow() {
  if (g_MainWindowData.Surface != VK_NULL_HANDLE ||
      g_MainWindowData.Swapchain != VK_NULL_HANDLE)
    CleanupVulkanWindow(); // destroys the surface too
  // A fresh struct: the next swapchain must not name this one as its old.
  g_MainWindowData = ImGui_ImplVulkanH_Window();
}

void CreateIbl() {
  // Image-based lighting: precompute (CPU, cached) the irradiance/prefilter/
  // BRDF maps from the bundled studio HDR and upload them. Built before the
  // mesh renderer since its descriptor sets bind these textures at creation;
  // the RT still lights with them too (MakeStillDesc refuses without).
  g_IBLTextures = std::make_unique<IBLTextures>();
  const std::string hdr = RuntimePaths::EnvironmentHdri().string();
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

// Key-light shadow map (S1) + SSAO (S2): before the mesh renderer, whose
// descriptor sets bind their maps at creation (like IBL). A failure is
// named, and the object kept: the mesh renderer binds its (empty) views.
void CreateShadow() {
  g_ShadowRenderer = std::make_unique<ShadowRenderer>();
  if (!g_ShadowRenderer->Initialize(g_Device, g_PhysicalDevice, g_Queue,
                                    g_QueueFamily, g_CommandPool)) {
    LOG_ERROR("ShadowRenderer initialization failed");
    App().renderer.missing.push_back("shadow");
  }
}
void CreateSsao() {
  const bool windowed = s_start.window != nullptr;
  g_SSAORenderer = std::make_unique<SSAORenderer>();
  if (!g_SSAORenderer->Initialize(
          g_Device, g_PhysicalDevice, g_Queue, g_QueueFamily, g_CommandPool,
          windowed ? (uint32_t)g_MainWindowData.Width : kOffscreenWidth,
          windowed ? (uint32_t)g_MainWindowData.Height : kOffscreenHeight)) {
    LOG_ERROR("SSAORenderer initialization failed");
    App().renderer.missing.push_back("ssao");
  }
}

void CreateMesh() {
  g_MeshRenderer = std::make_unique<MeshRenderer>();
  g_MeshRenderer->SetIBL(
      g_IBLTextures->IrradianceView(), g_IBLTextures->PrefilterView(),
      g_IBLTextures->BrdfLutView(), g_IBLTextures->EnvSampler(),
      g_IBLTextures->LutSampler());
  g_MeshRenderer->SetShadowMap(g_ShadowRenderer->ShadowMapView(),
                               g_ShadowRenderer->ShadowSampler());
  g_MeshRenderer->SetAOMap(g_SSAORenderer->AOView(),
                           g_SSAORenderer->AOSampler());
  InitMeshRenderer([] { return InitOnScenePass(*g_MeshRenderer); });
}
void CreateOutline() {
  g_OutlineRenderer = std::make_unique<OutlineRenderer>();
  InitRenderer(g_OutlineRenderer, "outline", [](OutlineRenderer& o) {
    if (!InitOnScenePass(o))
      return false;
    // The struct's default is EDGES in bright red ("for testing");
    // WIREFRAME with the subtle styling is the product look.
    OutlineSettings settings = o.GetOutlineSettings();
    settings.mode = OutlineMode::WIREFRAME;
    o.SetOutlineSettings(settings);
    return true;
  });
}
template <class R> void CreateScene(std::unique_ptr<R>& r, const char* name) {
  r = std::make_unique<R>();
  InitRenderer(r, name, [](R& x) { return InitOnScenePass(x); });
}

void DestroyIbl() {
  if (g_IBLTextures)
    g_IBLTextures->Shutdown(g_Device);
  g_IBLTextures.reset();
}

struct Row {
  const char* name;
  enum Mode { Both, Windowed, Offscreen } mode;
  // Part of a rung of the device ladder: made on each device tried, and a
  // failure tries the next device.
  bool rung;
  void (*create)();
  void (*destroy)();
};

// IN CREATION ORDER: built forward, torn down backward, by the start, the
// exit and every rebuild alike. The scene renderers' draw order is
// RegisterSceneRenderers', filled from their rows.
const Row kRows[] = {
    {"device", Row::Both, true, [] { CreateDevice(s_start.window != nullptr); },
     DestroyDevice},
    {"window surface and swapchain", Row::Windowed, true, CreateWindowRow,
     DestroyWindowRow},
    {"offscreen render pass", Row::Offscreen, false,
     [] {
       // The GUI decides this in SetupVulkanWindow, which never runs here.
       g_MSAASamples = GetMaxUsableSampleCount();
       g_OffscreenPass =
           CreateOffscreenRenderPass(kOffscreenFormat, g_MSAASamples);
     },
     [] {
       if (g_OffscreenPass != VK_NULL_HANDLE)
         vkDestroyRenderPass(g_Device, g_OffscreenPass, g_Allocator);
       g_OffscreenPass = VK_NULL_HANDLE;
     }},
    // ImGui's Vulkan backend owns its pipeline, its font texture and the
    // descriptor sets it takes from our pool. Its Shutdown also destroys
    // every viewport's PLATFORM data -- the main viewport's GLFW window
    // handle among them (ImGui_ImplVulkan_ShutdownMultiViewportSupport ->
    // DestroyPlatformWindows) -- after which ImGui_ImplGlfw_NewFrame reads a
    // null window. So the GLFW backend is in this row too, made first and
    // destroyed last, which re-chains the window's callbacks exactly as the
    // start did. The ImGui context, its windows, docking, the font atlas's
    // CPU data and the test engine are untouched.
    {"ImGui's backends", Row::Windowed, false,
     [] {
       ImGui_ImplGlfw_InitForVulkan(s_start.window, true);
       ImGui_ImplVulkan_InitInfo init_info = ImGuiInitInfo();
       ImGui_ImplVulkan_Init(&init_info);
       s_rendererData = ImGui::GetIO().BackendRendererUserData;
       s_platformData = ImGui::GetIO().BackendPlatformUserData;
     },
     [] {
       if (ImGui::GetIO().BackendRendererUserData != nullptr)
         ImGui_ImplVulkan_Shutdown();
       if (ImGui::GetIO().BackendPlatformUserData != nullptr)
         ImGui_ImplGlfw_Shutdown();
     }},
    {"g_IBLTextures", Row::Both, false, CreateIbl, DestroyIbl},
    {"g_ShadowRenderer", Row::Both, false, CreateShadow,
     [] { g_ShadowRenderer.reset(); }},
    {"g_GpuTimer", Row::Both, false,
     [] { g_GpuTimer.Initialize(g_Device, g_PhysicalDevice, g_QueueFamily); },
     [] { g_GpuTimer.Shutdown(g_Device); }},
    {"g_SSAORenderer", Row::Both, false, CreateSsao,
     [] { g_SSAORenderer.reset(); }},
    {"g_MeshRenderer", Row::Both, false, CreateMesh,
     [] { g_MeshRenderer.reset(); }},
    {"g_OutlineRenderer", Row::Both, false, CreateOutline,
     [] { g_OutlineRenderer.reset(); }},
    {"g_SketchRenderer", Row::Both, false,
     [] { CreateScene(g_SketchRenderer, "sketch"); },
     [] { g_SketchRenderer.reset(); }},
    // Sketch reference images
    {"g_UnderlayRenderer", Row::Both, false,
     [] { CreateScene(g_UnderlayRenderer, "underlay"); },
     [] { g_UnderlayRenderer.reset(); }},
    // FEM result overlay
    {"g_FieldRenderer", Row::Both, false,
     [] { CreateScene(g_FieldRenderer, "field"); },
     [] { g_FieldRenderer.reset(); }},
    // CAM cutting simulation
    {"g_StockRenderer", Row::Both, false,
     [] { CreateScene(g_StockRenderer, "stock"); },
     [] { g_StockRenderer.reset(); }},
    // No Vulkan handle, but its pick data is keyed on mesh ids.
    {"g_FaceSelector", Row::Both, false,
     [] { g_FaceSelector = std::make_unique<FaceSelector>(); },
     [] { g_FaceSelector.reset(); }},
    {"g_RayTracingRenderer", Row::Both, false,
     [] { InitRayTracing(s_start.safeMode); },
     [] { g_RayTracingRenderer.reset(); }},
    // The sky's maps are made on first use (most sessions never ask); the
    // row forgets which sun they were for.
    {"g_SkyIBL", Row::Both, false, [] { g_SkyIBLKey = {-1, -1}; },
     ShutdownSkyEnvironment},
};
constexpr size_t kRowCount = std::size(kRows);
bool s_made[kRowCount] = {};

bool Applies(const Row& r) {
  const bool windowed = s_start.window != nullptr;
  return r.mode == Row::Both || (r.mode == Row::Windowed) == windowed;
}

// Every row made at or after `from`, backward.
void TearDownRows(size_t from = 0) {
  for (size_t i = kRowCount; i-- > from;) {
    if (!s_made[i])
      continue;
    kRows[i].destroy();
    s_made[i] = false;
  }
}

ui::RendererSet CurrentSet() {
  ui::RendererSet set;
  set.mesh = g_MeshRenderer.get();
  set.outline = g_OutlineRenderer.get();
  set.sketch = g_SketchRenderer.get();
  set.underlay = g_UnderlayRenderer.get();
  set.field = g_FieldRenderer.get();
  set.stock = g_StockRenderer.get();
  set.scene = g_Scene;
  set.rayTracer = g_RayTracingRenderer.get();
  set.faceSelector = g_FaceSelector.get();
  set.ibl = g_IBLTextures.get();
  if (g_MeshRenderer)
    set.skyEnvironment = SkyEnvironment;
  return set;
}

void DestroyInstance() {
  if (g_Instance == VK_NULL_HANDLE)
    return;
#ifdef APP_USE_VULKAN_DEBUG_REPORT
  auto f_vkDestroyDebugReportCallbackEXT =
      (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(
          g_Instance, "vkDestroyDebugReportCallbackEXT");
  f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
  g_DebugReport = VK_NULL_HANDLE;
#endif
  vkDestroyInstance(g_Instance, g_Allocator);
  g_Instance = VK_NULL_HANDLE;
}

// The device ladder (docs/rendering/README.md#the-device-ladder), then every
// other row. The rung rows are tried on each eligible device in
// startup::DeviceLadder's order until they all hold on one; a rung that
// fails is logged with its result and taken down before the next is tried.
// Landing below the first rung is recorded in renderer for the modal, the
// menu bar, About and ping. `firstName`, when it names a device, goes first
// (a rebuild after a loss). Throws StartupError when no rung holds, or a
// later row (the mesh renderer) fails.
void BuildRows(const std::string& firstName) {
  ui::AppState& app = App();
  app.renderer.missing.clear();

  VkResult listed = VK_SUCCESS;
  std::vector<DeviceCandidate> candidates =
      EnumerateDevices(s_start.window, &listed);
  // The instance stays across a rebuild unless it can no longer list a
  // device: then it is made again, once.
  if (s_rebuilding && candidates.empty()) {
    LOG_WARN("Rendering: the instance lists no device ({}); making it again",
             startup::VkResultName(listed));
    DestroyInstance();
    CreateInstance(s_start.instanceExtensions);
    candidates = EnumerateDevices(s_start.window, &listed);
  }
  if (candidates.empty())
    FailStartup(startup::step::kNoDevice,
                "The Vulkan loader runs, but it offered no device: there is "
                "no Vulkan driver for this machine's GPU, or no GPU it can "
                "use.");
  std::vector<startup::Device> devices;
  int first = -1;
  for (size_t i = 0; i < candidates.size(); i++) {
    const DeviceCandidate& c = candidates[i];
    devices.push_back({c.name, c.type == VK_PHYSICAL_DEVICE_TYPE_CPU, c.score});
    if (!firstName.empty() && c.name == firstName && first < 0)
      first = static_cast<int>(i);
  }
  const std::vector<int> ladder = startup::DeviceLadder(
      devices, app.settings.gpuDevice, g_RendererChoice, first);
  if (ladder.empty()) {
    const bool anyCpu =
        std::any_of(devices.begin(), devices.end(),
                    [](const startup::Device& d) { return d.cpu; });
    FailStartup(
        startup::step::kNoDevice,
        g_RendererChoice == startup::RendererChoice::Software
            ? (anyCpu ? "LOBOLE_RENDERER=software, and the software device "
                        "cannot show this window."
                      : "LOBOLE_RENDERER=software, and there is no software "
                        "device: install Mesa's Vulkan drivers (lavapipe).")
        : g_RendererChoice == startup::RendererChoice::Hardware
            ? "LOBOLE_RENDERER=hardware, and no hardware device can show "
              "this window."
            : "None of the devices can show this window.");
  }

  // Say so rather than silently doing something else: a preference that
  // cannot be honoured is exactly the case a user reads the log for. The
  // rung the PREFERENCE picks, not a lost device put first.
  const std::vector<int> preferred =
      startup::DeviceLadder(devices, app.settings.gpuDevice, g_RendererChoice);
  const std::string& usual =
      candidates[static_cast<size_t>(preferred.empty() ? ladder[0]
                                                       : preferred[0])]
          .name;
  app.gpuPreferenceNote.clear();
  if (!app.settings.gpuDevice.empty() &&
      usual.find(app.settings.gpuDevice) == std::string::npos) {
    app.gpuPreferenceNote = "'" + app.settings.gpuDevice +
                            "' cannot display a window on this system (or "
                            "is not present), so it was not used.";
    LOG_WARN("Preferred GPU '{}' cannot present here (or is absent); "
             "rendering on {} instead.",
             app.settings.gpuDevice, usual);
  }

  std::vector<std::string> attempts;
  std::optional<startup::Failure> last;
  int landed = -1;
  for (size_t rung = 0; rung < ladder.size() && landed < 0; rung++) {
    const DeviceCandidate& c = candidates[static_cast<size_t>(ladder[rung])];
    g_PhysicalDevice = c.device;
    try {
      for (size_t i = 0; i < kRowCount; i++) {
        if (!kRows[i].rung || !Applies(kRows[i]))
          continue;
        // Marked before it is made: a row that fails half way is torn down
        // with the rest, and every destroy copes with what was not made.
        s_made[i] = true;
        kRows[i].create();
      }
      landed = static_cast<int>(rung);
    } catch (const startup::StartupError& e) {
      const startup::Failure& f = e.failure();
      const std::string why =
          (f.result.empty() ? f.detail : f.result) + " " + AtStep(f.step);
      attempts.push_back(c.name + ": " + why);
      LOG_WARN("Rendering: {} failed ({}){}", c.name, why,
               rung + 1 < ladder.size() ? "; trying the next device" : "");
      last = f;
      TearDownRows();
      g_PhysicalDevice = VK_NULL_HANDLE;
    }
  }
  if (landed < 0) {
    startup::Failure f = *last;
    f.devices = g_SeenDevices;
    f.attempts = attempts;
    throw startup::StartupError(std::move(f));
  }

  const DeviceCandidate& used =
      candidates[static_cast<size_t>(ladder[static_cast<size_t>(landed)])];
  {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
    // maxMemoryAllocationCount beside the device name, because every buffer
    // and image takes its own vkAllocateMemory (no sub-allocator) and a
    // 299-part assembly makes 1196 of them. Every device this project targets
    // reports 4294967295 — no ceiling (measured 2026-09-01: NVIDIA, Intel Arc,
    // llvmpipe). The Vulkan FLOOR is 4096 though, so a driver that imposes a
    // real limit is a port away, and running out of handles looks nothing like
    // running out of memory: the 51 MB those 1196 hold is unremarkable, and an
    // external GPU monitor would show nothing wrong. Logging it means such a
    // machine says so on its first line rather than at the allocation that
    // fails.
    LOG_INFO("Selected GPU: {} (type {}), max device allocations {}",
             props.deviceName, static_cast<int>(props.deviceType),
             props.limits.maxMemoryAllocationCount);
    // Keep it for Help > About: which GPU answered is the first thing worth
    // knowing about a rendering bug, and the log is gone by the time someone
    // writes the report.
    app.gpuName = props.deviceName;
    app.gpuApiVersion =
        std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
        std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + "." +
        std::to_string(VK_API_VERSION_PATCH(props.apiVersion));
  }
  ui::AppState::RendererStatus& status = app.renderer;
  status.device = used.name;
  status.type = DeviceTypeName(used.type);
  status.software = used.type == VK_PHYSICAL_DEVICE_TYPE_CPU;
  // Below the rung the preference would have picked: a lost device put
  // first and holding again is not a fallback.
  status.fallback =
      !preferred.empty() && ladder[static_cast<size_t>(landed)] != preferred[0];
  status.reason.clear();
  for (const std::string& a : attempts)
    status.reason += (status.reason.empty() ? "" : "; ") + a;
  status.forced =
      g_RendererChoice == startup::RendererChoice::Software   ? "software"
      : g_RendererChoice == startup::RendererChoice::Hardware ? "hardware"
                                                              : "";
  status.appliedPreference = app.settings.gpuDevice;
  if (status.fallback)
    LOG_WARN("Rendering on {} ({}), lower than the best device here: {}",
             used.name, status.type, status.reason);
  else if (status.software)
    LOG_INFO("Rendering in software on {}{}", used.name,
             status.forced.empty() ? "" : " (LOBOLE_RENDERER=software)");

  // Every other row, forward. A failure here (the mesh renderer) is the
  // caller's: the start ends, a rebuild reaches the backstop.
  for (size_t i = 0; i < kRowCount; i++) {
    if (kRows[i].rung || !Applies(kRows[i]))
      continue;
    s_made[i] = true;
    kRows[i].create();
  }
  RegisterSceneRenderers();
  ui::ForgetGpuProjection(app, CurrentSet());
}

// --- the rebuild -------------------------------------------------------------

// The one trigger set: a lost device, and a preference that moves the first
// rung. tests/test_conventions.cpp fails on a third call.
std::optional<startup::RebuildReason> s_pending;
void RequestDeviceRebuild(startup::RebuildReason reason) {
  if (!s_pending)
    s_pending = reason;
}

// Does the preference put another device on the first rung than the one in
// use? Asks the loader, without making anything but a probe surface -- once
// per preference value, since a switch that waits for a render asks again
// every frame until it may run.
std::string s_askedPreference, s_askedOn;
bool s_askedMoves = false;
bool PreferenceMovesFirstRungUncached(ui::AppState& app);
bool PreferenceMovesFirstRung(ui::AppState& app) {
  if (app.settings.gpuDevice == s_askedPreference &&
      app.renderer.device == s_askedOn && !s_askedOn.empty())
    return s_askedMoves;
  s_askedPreference = app.settings.gpuDevice;
  s_askedOn = app.renderer.device;
  s_askedMoves = PreferenceMovesFirstRungUncached(app);
  return s_askedMoves;
}
bool PreferenceMovesFirstRungUncached(ui::AppState& app) {
  VkResult listed = VK_SUCCESS;
  const std::vector<DeviceCandidate> candidates =
      EnumerateDevices(s_start.window, &listed);
  std::vector<startup::Device> devices;
  for (const DeviceCandidate& c : candidates)
    devices.push_back({c.name, c.type == VK_PHYSICAL_DEVICE_TYPE_CPU, c.score});
  const std::vector<int> ladder =
      startup::DeviceLadder(devices, app.settings.gpuDevice, g_RendererChoice);
  if (ladder.empty())
    return false; // nothing could be used: stay where we are
  const std::string& first = candidates[static_cast<size_t>(ladder[0])].name;
  app.gpuPreferenceNote.clear();
  if (!app.settings.gpuDevice.empty() &&
      first.find(app.settings.gpuDevice) == std::string::npos)
    app.gpuPreferenceNote = "'" + app.settings.gpuDevice +
                            "' cannot display a window on this system (or "
                            "is not present), so it was not used.";
  return first != app.renderer.device;
}

// The table backward and forward again, on the device the ladder picks.
// False with `why` when it could not: every row is down then.
bool RebuildDevice(startup::RebuildReason reason, std::string* why) {
  const std::string lost = reason == startup::RebuildReason::DeviceLost
                               ? App().renderer.device
                               : std::string();
  // The view menu's outline mode lives in the renderer: carried over.
  std::optional<OutlineSettings> outline;
  if (g_OutlineRenderer)
    outline = g_OutlineRenderer->GetOutlineSettings();
  // ImGui's and our own waits on a lost device answer VK_ERROR_DEVICE_LOST;
  // collected, neither aborts nor marks a second loss.
  g_CollectVkErrors = true;
  g_CollectedVkError = VK_SUCCESS;
  if (g_Device != VK_NULL_HANDLE)
    (void)vkDeviceWaitIdle(g_Device);
  TearDownRows();
  g_CollectedVkError = VK_SUCCESS;
  s_rebuilding = true;
  bool ok = true;
  try {
    BuildRows(lost);
  } catch (const startup::StartupError& e) {
    const startup::Failure& f = e.failure();
    *why = f.step + ": " + (f.result.empty() ? f.detail : f.result);
    for (const std::string& a : f.attempts)
      *why += "; " + a;
    ok = false;
  } catch (const std::exception& e) {
    *why = std::string("it threw: ") + e.what();
    ok = false;
  }
  s_rebuilding = false;
  g_CollectVkErrors = false;
  if (!ok) {
    TearDownRows();
    RegisterSceneRenderers(); // empty: nothing draws
    ui::ForgetGpuProjection(App(), {});
    return false;
  }
  if (outline && g_OutlineRenderer)
    g_OutlineRenderer->SetOutlineSettings(*outline);
  return true;
}

// The backstop: the lost state of docs/rendering/README.md#a-lost-device.
void Backstop(ui::AppState& app, const std::string& why) {
  app.renderer.lost = true;
  app.renderer.backstop = true;
  if (app.renderer.lostReason.empty())
    app.renderer.lostReason = "the graphics device could not be rebuilt";
  app.renderer.lostReason += "; " + why;
  LOG_ERROR("Rendering: {} — nothing renders until Loböle restarts", why);
}

gpuowners::Outcome RunPendingRebuild(ui::AppState& app,
                                     const gpuowners::Boundary& b, double now) {
  const startup::RebuildReason reason = *s_pending;
  s_pending.reset();
  const auto t0 = std::chrono::steady_clock::now();
  const watchdog::Label label("rebuilding the graphics device");
  const std::string from = app.renderer.device;
  LOG_WARN("Rendering: rebuilding the graphics device ({}, on {})",
           startup::RebuildReasonName(reason), from);
  const int autosaved = gpuowners::FlushAutosaveNow(app);
  if (autosaved > 0)
    LOG_INFO("Rendering: {} document(s) autosaved before the rebuild",
             autosaved);
  ui::EndRendersForRebuild(app);
  ui::ReleaseGpuProjection(app);
  ui::AppState::RendererStatus& r = app.renderer;
  // The loss being rebuilt is over from here: one seen while the new device
  // is built is the NEW device's, and the next boundary judges it.
  const std::string lostReason = r.lostReason;
  r.lost = false;
  r.lostReason.clear();
  vkutil::ClearDeviceLost();
  std::string why;
  if (!RebuildDevice(reason, &why)) {
    r.lostReason = lostReason;
    Backstop(app, "rebuilding the device failed: " + why);
    return gpuowners::Outcome::Backstop;
  }
  if (b.afterBuild)
    b.afterBuild();
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  r.rebuilds++;
  r.lastRebuildReason = startup::RebuildReasonName(reason);
  r.lastRebuildFrom = from;
  r.lastRebuildTo = r.device;
  r.lastRebuildMs = ms;
  r.lastRebuildAt = now;
  r.preferenceWaiting = false;
  // Landing below the first rung is announced as the start announces it.
  if (r.fallback)
    app.rendererFallbackNoticeShown = false;
  app.rendererRestartedNoticeUntil = -2.0; // the menu bar arms it
  LOG_WARN("Rendering: the graphics device was rebuilt in {:.0f} ms ({} -> "
           "{})",
           ms, from, r.device);
  if (reason == startup::RebuildReason::DeviceLost) {
    // Evidence, never a recovery notice: nothing was lost.
    crash::ReportThisSession(std::string(crash::kDeviceRecoveredReason) + ": " +
                                 lostReason,
                             /*recovered=*/true);
  }
  return gpuowners::Outcome::Rebuilt;
}

} // namespace

namespace gpuowners {

int FlushAutosaveNow(ui::AppState& app) {
  Autosaver now(1.0);
  now.Prime(-1.0e9);
  return ui::AutosaveProject(app, now, 0.0);
}

void Build(const Start& start) {
  s_start = start;
  s_app = start.app;
  CreateInstance(s_start.instanceExtensions);
  BuildRows("");
}

void Teardown() {
  g_CollectVkErrors = true;
  if (g_Device != VK_NULL_HANDLE)
    (void)vkDeviceWaitIdle(g_Device);
  TearDownRows();
  g_CollectVkErrors = false;
  g_Scene.clear();
  if (s_app)
    ui::ForgetGpuProjection(*s_app, {});
  DestroyInstance();
}

void KeepImGuiBackends() {
  if (s_rendererData == nullptr || ImGui::GetCurrentContext() == nullptr)
    return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.BackendRendererUserData == s_rendererData &&
      io.BackendPlatformUserData == s_platformData)
    return;
  LOG_INFO("ImGui: the backends' data was put back as it was before the "
           "last device rebuild (a test-engine test ended); pointing it at "
           "the current backends");
  io.BackendRendererUserData = s_rendererData;
  io.BackendPlatformUserData = s_platformData;
}

Outcome AtFrameBoundary(ui::AppState& app, const Boundary& b, double now) {
  ui::AppState::RendererStatus& r = app.renderer;
  if (r.backstop)
    return Outcome::Nothing;
  if (r.lost) {
    // A device that dies again on its first frames is not one another
    // rebuild will save.
    const double since = r.lastRebuildAt < 0.0 ? -1.0 : now - r.lastRebuildAt;
    if (startup::JudgeLoss(since) == startup::LossVerdict::Backstop) {
      Backstop(app, "it was lost again " +
                        std::to_string(static_cast<int>(since)) +
                        " s after the last rebuild");
      return Outcome::Backstop;
    }
    if (!b.canBuild)
      return Outcome::Nothing; // a minimised window: when it is shown
    RequestDeviceRebuild(startup::RebuildReason::DeviceLost);
  } else if (app.settings.gpuDevice != r.appliedPreference && b.canBuild) {
    if (!PreferenceMovesFirstRung(app)) {
      r.appliedPreference = app.settings.gpuDevice; // the same device
      r.preferenceWaiting = false;
      return Outcome::Nothing;
    }
    // A switch waits for what holds the device instead of ending it.
    const bool busy = (app.renderJob.active && !app.renderJob.quick) ||
                      ui::BackgroundJobRunning(app, "motion-video") ||
                      b.capturing;
    r.preferenceWaiting = busy;
    if (busy)
      return Outcome::Nothing;
    RequestDeviceRebuild(startup::RebuildReason::GpuPreference);
  }
  if (!s_pending)
    return Outcome::Nothing;
  return RunPendingRebuild(app, b, now);
}

} // namespace gpuowners
