// CAD Application with working Vulkan + ImGui setup
// Based on Dear ImGui GLFW+Vulkan example

#include "app/CrashHandler.h"
#include "app/FramePacer.h"
#include "app/GuiTests.h"
#include "app/HeadlessMain.h"
#include "app/ParentWatch.h"
#include "cad/FaceSelector.h"
#include "common/DesignerVersion.h"
#include "common/DevicePreference.h"
#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "common/Watchdog.h"
#include "core/io/Autosaver.h"
#include "core/io/Settings.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#ifdef HAVE_TEST_ENGINE
#include "imgui_te_engine.h"
#include "imgui_te_internal.h" // TestsAll, for --gui-test-shuffle
#endif
#include "remote/RemoteServer.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/FrameStream.h"
#include "render/FramesInFlight.h"
#include "render/GpuTimer.h"
#include "render/IBL.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SSAORenderer.h"
#include "render/SceneRenderer.h"
#include "render/Screenshot.h"
#include "render/ShadowRenderer.h"
#include "render/SketchRenderer.h"
#include "render/StockRenderer.h"
#include "render/UnderlayRenderer.h"
#include "render/VulkanCommon.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/BearingTool.h"
#include "ui/CamJobs.h" // OpenCamSurfaceStore
#include "ui/CamPlayback.h"
#include "ui/DocumentSlots.h"
#include "ui/FastenerTool.h"
#include "ui/ImportActions.h"
#include "ui/ImportJob.h"
#include "ui/MachineActions.h"
#include "ui/MotionActions.h"
#include "ui/ProjectActions.h"
#include "ui/RendererSync.h"
#include "ui/SelectionPipeline.h"
#include "ui/SettingsWindow.h"
#include "ui/SimulationActions.h"
#include "ui/TerminalPanel.h"
#include "ui/Theme.h"
#include "ui/UI.h"
#include "ui/ViewActions.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <time.h>
#include <utility>

namespace {
// Writable per-user cache for precomputed IBL maps. The executable dir may be
// read-only (Nix store, Flatpak /app), so the cache cannot live beside it.
std::string IblCacheDir() {
  const std::filesystem::path dir = RuntimePaths::UserCacheDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}
} // namespace
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#if defined(__linux__)
// Which display protocol this session actually runs. Wayland deliberately
// provides no window POSITION (a client may not know or choose where it sits)
// and has no X11 handles, so three things below have to ask before they act.
// glfwGetPlatform is GLFW 3.4; the stock distros CI builds against ship 3.3,
// where a build is X11 or Wayland at compile time and the X11 display getter
// answers null on anything but X11.
extern "C" void* glfwGetX11Display(void);
static bool OnWayland() {
#if GLFW_VERSION_MAJOR > 3 ||                                                  \
    (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
  return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
#else
  return glfwGetX11Display() == nullptr;
#endif
}

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
  // Ask the PLATFORM, not for a handle. This used to call the X11 getters and
  // fall back when they returned null, which works but logs two "X11: Platform
  // not initialized" errors on the way to the right answer.
  if (OnWayland()) {
    glfwShowWindow(window);
    return;
  }
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
// Wayland is a Linux display protocol; everywhere else windows have a position.
static bool OnWayland() { return false; }
static void ShowWindowNonBlocking(GLFWwindow* window) {
  glfwShowWindow(window);
}
#endif

// This thread's CPU time, ms: what a frame cost rather than how long it took
// (FrameTimer::SetWorkCpuMs). Wall time where there is no thread CPU clock.
static double ThreadCpuMs() {
#if defined(__linux__)
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
  return glfwGetTime() * 1e3;
}

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
static uint32_t g_MinImageCount = 3;
static bool g_SwapChainRebuild = false;
static bool g_DisableVSync = false;

// Application objects
static std::unique_ptr<Camera> g_Camera;
static std::unique_ptr<MeshRenderer> g_MeshRenderer;
static std::unique_ptr<OutlineRenderer> g_OutlineRenderer;
static std::unique_ptr<SketchRenderer> g_SketchRenderer;
static std::unique_ptr<UnderlayRenderer> g_UnderlayRenderer;
static std::unique_ptr<FieldRenderer> g_FieldRenderer;
static std::unique_ptr<StockRenderer> g_StockRenderer;

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
static std::vector<SceneRenderer*> g_Scene;

// Registered once a construction path has built its renderers. Null entries
// are skipped, so one that failed to initialize is simply absent rather than a
// crash.
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
    // The renderers are constructed in TWO places — the offscreen path and
    // the windowed one — so a new one can be built in one and forgotten in
    // the other, and then it silently draws nothing in that mode only. Say
    // which, at startup, rather than leaving it to be noticed in a
    // screenshot.
    if (r == nullptr) {
      LOG_WARN("scene renderer {} was never constructed in this mode — it "
               "will draw nothing here",
               name);
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
static void ShutdownSceneRenderers() {
  g_Scene.clear();
  g_MeshRenderer.reset();
  g_FieldRenderer.reset();
  g_StockRenderer.reset();
  g_OutlineRenderer.reset();
  g_UnderlayRenderer.reset();
  g_SketchRenderer.reset();
}
static std::unique_ptr<RayTracingRenderer> g_RayTracingRenderer;
static std::unique_ptr<ShadowRenderer> g_ShadowRenderer;
static std::unique_ptr<SSAORenderer> g_SSAORenderer;
static render::GpuTimer g_GpuTimer; // per-pass GPU timing (frame-timing.md)
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

// `wsi` = this process will present to a window (swapchain device extension,
// present-support probe). --offscreen passes false: it renders to images it
// reads back, and enabling VK_KHR_swapchain without VK_KHR_surface on the
// instance is a spec violation — WSI is all-or-nothing across the two levels.
static void SetupVulkan(ImVector<const char*> instance_extensions,
                        GLFWwindow* window, bool wsi) {
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
  // reports 2, Mesa reports 3+. See
  // docs/rendering/frame-pacing.md#frames-in-flight.
  g_App.swapchainImages = wd->ImageCount;
  g_App.presentMode =
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
static void RecordScenePrepasses(VkCommandBuffer cb, uint32_t currentFrame,
                                 render::GpuTimer* timer = nullptr) {
  // `timer` only from the live frame loop: the screenshot path re-records
  // these passes into its own one-shot buffer, and timestamps there would
  // double-write the frame's query slot.
  if (!g_MeshRenderer)
    return;
  // The light rig follows the camera unless locked to the model
  // (core/LightRig.h). Set here because the frame and every capture path
  // come through, and before the shadow pass, which renders from the key.
  g_MeshRenderer->SetLightsLockedToModel(g_App.lightsLockedToModel);
  if (g_StockRenderer)
    g_StockRenderer->SetLightsLockedToModel(g_App.lightsLockedToModel);
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
  if (timer)
    timer->Write(cb, currentFrame, render::GpuTimer::Point::AfterShadow);
  if (g_SSAORenderer) {
    g_MeshRenderer->SetAOEnabled(g_App.showAmbientOcclusion);
    if (g_App.showAmbientOcclusion) {
      g_SSAORenderer->SetClipPlanes(ui::SectionPlanes(g_App));
      g_SSAORenderer->RecordSSAOPasses(cb, currentFrame, *g_MeshRenderer);
      g_MeshRenderer->SetAOState(g_SSAORenderer->InvFullResolution());
    }
  }
  if (timer)
    timer->Write(cb, currentFrame, render::GpuTimer::Point::AfterSSAO);
  // Off-toggles skip the passes entirely; the shaders won't sample the
  // (stale) maps and the image layouts stay read-only.
}

// Where a quick render (CaptureRasterStill) draws and what it must put back:
// the render pass and format of the loop it runs in, an idle frame slot, the
// SSAO targets' size and the viewport rect the next frame expects.
struct RasterTarget {
  VkRenderPass pass = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
  VkImageLayout finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  uint32_t frame = 0;
  VkClearValue clear{};
  uint32_t aoW = 0, aoH = 0; // the SSAO targets, restored
  int32_t vpX = 0, vpY = 0;  // the scene viewport, restored
  uint32_t vpW = 0, vpH = 0;
};

// The model's renderers only: the sketch, underlay and CAM stock renderers
// carry the overlays (sketches, datums, gizmos, handles). What a quick render
// and a motion video draw.
static void RecordModel(VkCommandBuffer cb, uint32_t frame) {
  g_MeshRenderer->Render(cb, frame);
  if (g_FieldRenderer)
    g_FieldRenderer->Render(cb, frame);
  if (g_OutlineRenderer)
    g_OutlineRenderer->Render(cb, frame);
}

// How a capture draws its edges: whether at all, and the wireframe's width
// in the capture's own pixels (k x the output's line weight).
struct CaptureLines {
  bool outlines = true;
  float rasterWidth = 0.0f; // <= 0: the viewport's, unscaled
};

// Point every renderer at a W x H capture of the current camera, drawn `k`
// times larger than it will be shown (lines widened to match), after waiting
// for the GPU: the SSAO targets are reallocated. Returns the outline settings
// AimRenderersBack restores.
static OutlineSettings AimRenderersAt(uint32_t W, uint32_t H, uint32_t k,
                                      const CaptureLines& lines = {}) {
  vkDeviceWaitIdle(g_Device);
  // Only the model: no selection or hover tint on it (the GUI re-applies
  // both next frame), and none of the overlay renderers.
  g_MeshRenderer->ClearFaceHighlight();
  g_MeshRenderer->ClearFaceHover();
  // Everything that is sized in pixels, pointed at the capture.
  for (SceneRenderer* r : g_Scene)
    r->SetViewport(0, 0, W, H);
  if (g_SSAORenderer) {
    g_SSAORenderer->Resize(W, H);
    g_MeshRenderer->RebindAOMap(g_SSAORenderer->AOView());
  }
  OutlineSettings outline{};
  if (g_OutlineRenderer) {
    outline = g_OutlineRenderer->GetOutlineSettings();
    OutlineSettings wide = outline;
    // The wireframe at the width asked for; the EDGES mode's outline at the
    // viewport's once averaged down.
    if (lines.rasterWidth > 0.0f)
      wide.rasterLineWidth = lines.rasterWidth;
    wide.outlineWidth *= float(k);
    if (!lines.outlines)
      wide.mode = OutlineMode::NONE;
    g_OutlineRenderer->SetOutlineSettings(wide);
  }
  const glm::mat4 view = g_Camera->GetViewMatrix();
  const glm::mat4 proj = g_Camera->GetProjectionMatrixForAspect(
      static_cast<float>(W) / static_cast<float>(H));
  for (SceneRenderer* r : g_Scene)
    r->UpdateCamera(view, proj);
  if (g_OutlineRenderer)
    g_OutlineRenderer->SettleSilhouettes();
  return outline;
}

// Put back everything the next frame reads.
static void AimRenderersBack(const RasterTarget& t,
                             const OutlineSettings& outline) {
  vkDeviceWaitIdle(g_Device);
  if (g_OutlineRenderer)
    g_OutlineRenderer->SetOutlineSettings(outline);
  if (g_SSAORenderer) {
    g_SSAORenderer->Resize(t.aoW, t.aoH);
    g_MeshRenderer->RebindAOMap(g_SSAORenderer->AOView());
  }
  for (SceneRenderer* r : g_Scene)
    r->SetViewport(t.vpX, t.vpY, t.vpW, t.vpH);
  const glm::mat4 view = g_Camera->GetViewMatrix();
  const glm::mat4 liveProj = g_Camera->GetProjectionMatrix();
  for (SceneRenderer* r : g_Scene)
    r->UpdateCamera(view, liveProj);
}

// QUICK render (docs/rendering/still-render.md#quick-mode): the model as the
// viewport rasterizes it, captured supersampled at k x the requested size
// into `out` (DownsampleToPng averages it down and writes it). Synchronous;
// every size it changes is put back before it returns, so the next frame is
// the frame it would have been.
static bool CaptureRasterStill(const RasterTarget& t,
                               const ui::AppState::RasterStill& req,
                               ui::RasterFrame* out, std::string* error,
                               std::string* warning) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (!g_MeshRenderer || !g_Camera)
    return fail("no renderer: a quick render needs a GPU (not --headless)");
  if (req.width == 0 || req.height == 0)
    return fail("width and height must be positive");
  // The supersampled image has to fit the device.
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
  const uint32_t limit = std::min(props.limits.maxImageDimension2D,
                                  props.limits.maxFramebufferWidth);
  uint32_t k = std::clamp(req.supersample, 1u, 4u);
  while (k > 1 && (req.width * k > limit || req.height * k > limit))
    --k;
  if (req.width > limit || req.height > limit)
    return fail("the device's largest image is " + std::to_string(limit) +
                " px on a side");
  if (k != std::clamp(req.supersample, 1u, 4u) && warning)
    *warning = "supersampled " + std::to_string(k) + "x, not " +
               std::to_string(req.supersample) +
               "x: the device's largest "
               "image is " +
               std::to_string(limit) + " px on a side";
  const uint32_t W = req.width * k, H = req.height * k;

  // Lines are geometry, drawn k times wider at k times the size, so the
  // average down leaves them the line weight wide and as dark as the
  // viewport's (docs/rendering/still-render.md#lines-in-a-quick-still).
  // Drawn at the capture's 1 px instead, they averaged down to 1/k of a
  // pixel: a quarter of their darkness at 4x.
  CaptureLines lines;
  lines.outlines = req.outlines;
  float weight = req.lineWeight;
  if (g_OutlineRenderer) {
    if (weight <= 0.0f)
      weight = g_OutlineRenderer->GetOutlineSettings().rasterLineWidth;
    lines.rasterWidth = weight * float(k);
    const float widest = g_OutlineRenderer->MaxLineWidth();
    if (req.outlines && lines.rasterWidth > widest) {
      lines.rasterWidth = widest;
      if (warning) {
        if (!warning->empty())
          *warning += "; ";
        *warning += std::format("lines drawn {:.2f} px wide, not {:.2f}: "
                                "this device's widest line is {:.0f} px at "
                                "{}x",
                                widest / float(k), weight, widest, k);
      }
    }
  }
  const OutlineSettings outline = AimRenderersAt(W, H, k, lines);

  out->rgb.clear();
  std::string captureError;
  FrameStats stats; // whole frame: there is no chrome in it
  const bool captured = CaptureScreenshot(
      g_Device, g_PhysicalDevice, g_Queue, g_CommandPool, t.pass, t.format, W,
      H, t.clear, [&](VkCommandBuffer cb) { RecordModel(cb, t.frame); },
      /*outputPath=*/"", &captureError, g_MSAASamples,
      [&](VkCommandBuffer cb) { RecordScenePrepasses(cb, t.frame); }, &stats,
      t.finalLayout, 1, {}, &out->rgb);

  AimRenderersBack(t, outline);

  if (!captured)
    return fail(captureError.empty() ? "the capture failed" : captureError);
  out->width = W;
  out->height = H;
  out->factor = k;
  out->lineWeight = lines.rasterWidth / float(k);
  // Saved is not the same as good, as for a traced still: a picture that is
  // all one colour framed nothing.
  if (stats.dominantFraction > 0.995f && warning) {
    if (!warning->empty())
      *warning += "; ";
    *warning += "the picture is one colour: the camera frames nothing "
                "(fit the view first, or pass fit: true)";
  }
  return true;
}

// A motion video's frames (ui::FrameCapture,
// docs/assemblies/README.md#motion-video): the viewport's own render — the
// same renderers, settings, MSAA and line widths — at the video's size, not
// supersampled, streamed through a render::FrameStream. Lives for one slice
// of the frame loop.
class ViewportFrameCapture final : public ui::FrameCapture {
public:
  explicit ViewportFrameCapture(const RasterTarget& t) : m_t(t) {}
  ~ViewportFrameCapture() override {
    if (!m_aimed)
      return;
    m_stream.reset(); // waits for the frames in flight
    AimRenderersBack(m_t, m_outline);
  }
  bool Begin(uint32_t w, uint32_t h, std::string* error) {
    if (!g_MeshRenderer || !g_Camera) {
      if (error)
        *error = "no renderer: a video needs a GPU (not --headless)";
      return false;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
    const uint32_t limit = std::min(props.limits.maxImageDimension2D,
                                    props.limits.maxFramebufferWidth);
    if (w == 0 || h == 0 || w > limit || h > limit) {
      if (error)
        *error = "a frame must be 1.." + std::to_string(limit) +
                 " px on a side (the device's largest image)";
      return false;
    }
    m_outline = AimRenderersAt(w, h, 1);
    m_aimed = true;
    render::FrameStream::Config c;
    c.device = g_Device;
    c.physicalDevice = g_PhysicalDevice;
    c.queue = g_Queue;
    c.commandPool = g_CommandPool;
    c.renderPass = m_t.pass;
    c.format = m_t.format;
    c.samples = g_MSAASamples;
    c.colorFinalLayout = m_t.finalLayout;
    c.width = w;
    c.height = h;
    c.clear = m_t.clear;
    m_stream = std::make_unique<render::FrameStream>();
    return m_stream->Init(c, error);
  }
  bool Submit(std::string* error) override {
    return m_stream->Submit(
        [](VkCommandBuffer cb, uint32_t frame) {
          RecordScenePrepasses(cb, frame);
        },
        [](VkCommandBuffer cb, uint32_t frame) { RecordModel(cb, frame); },
        error);
  }
  bool Collect(std::vector<uint8_t>* pixels, std::string* error) override {
    return m_stream->Collect(pixels, error);
  }
  bool Full() const override { return m_stream->Full(); }
  size_t InFlight() const override { return m_stream->InFlight(); }
  bool Bgra() const override { return m_stream->Bgra(); }

private:
  RasterTarget m_t;
  OutlineSettings m_outline{};
  bool m_aimed = false;
  std::unique_ptr<render::FrameStream> m_stream;
};

static std::unique_ptr<ui::FrameCapture>
BeginFrameCapture(const RasterTarget& t, uint32_t w, uint32_t h,
                  std::string* error) {
  auto c = std::make_unique<ViewportFrameCapture>(t);
  if (!c->Begin(w, h, error))
    return nullptr;
  return c;
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
  // The fence above completed this slot's PREVIOUS submission, so its query
  // results are ready to read without waiting — the GpuTimer contract.
  g_GpuTimer.Harvest(wd->FrameIndex);
  // The acquire and that fence are the frame's GPU/vsync wait; everything
  // after is our recording
  // (docs/rendering/frame-pacing.md#the-frame-time-instrument).
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
  g_GpuTimer.BeginFrame(fd->CommandBuffer, wd->FrameIndex);

  RecordScenePrepasses(fd->CommandBuffer, wd->FrameIndex, &g_GpuTimer);

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
  for (SceneRenderer* r : g_Scene)
    r->Render(fd->CommandBuffer, wd->FrameIndex);

  ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

  vkCmdEndRenderPass(fd->CommandBuffer);
  g_GpuTimer.Write(fd->CommandBuffer, wd->FrameIndex,
                   render::GpuTimer::Point::AfterMain);
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
  std::cout << "  --gui-test-shuffle [seed]    Run GUI tests in a random "
               "order (prints the seed)\n";
  std::cout << "  --gui-test-filter <pattern>  Run only matching GUI tests "
               "(e.g. \"fillet\", \"selection_,tree_\"; implies --gui-tests)"
            << std::endl;
  std::cout << "  --gui-test-sequence <cat/name,...>  Run exactly these GUI "
               "tests, in this order (replays a failed shard)"
            << std::endl;
  std::cout << "  --gui-test-min-frame-ms <ms>  Make every GUI-test frame take "
               "at least this long (a loaded machine, without loading it)"
            << std::endl;
  std::cout
      << "  --gui-test-job-latency-ms <ms>  Reap no background job sooner "
         "than this after it started (starved workers)"
      << std::endl;
  std::cout << "  --gui-test-list      List GUI tests as category/name and "
               "exit (no GPU needed)"
            << std::endl;
  std::cout << "  --remote <port>      Enable JSON-RPC remote control on "
               "127.0.0.1:<port>"
            << std::endl;
  std::cout << "                       (0 = let the OS choose; the port is "
               "logged and announced)"
            << std::endl;
  std::cout << "  --remote-bind-all    Serve --remote on 0.0.0.0 instead of "
               "127.0.0.1 (containers only:\n                       the "
               "interface is unauthenticated and touches files)"
            << std::endl;
  std::cout << "  --headless           No window/GPU: serve --remote with the "
               "document engine only"
            << std::endl;
  std::cout << "  --offscreen          No window/display, real renderer: "
               "serve --remote with renders\n                       "
               "(needs a Vulkan device; ui_action needs a window)"
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
  std::cout << "  --exit-with-parent   Exit (SIGTERM) when the launching "
               "process dies; for test launchers.\n                       "
               "DESIGNER_EXIT_WITH_PARENT=<launcher pid> does the same"
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
  // --gui-test-sequence: exactly these tests ("category/name", comma-
  // separated), in exactly this order. Empty = not asked for.
  std::string guiTestSequence;
  // -1 = run in registration order. >= 0 = shuffle with this seed; 0 means
  // "pick one and tell me". See --gui-test-shuffle.
  long guiTestShuffle = -1;
  // --gui-test-min-frame-ms: the test-mode frame cap's floor, raised. Frames
  // as slow as someone else's build makes them, on a quiet machine
  // (docs/ui/testing.md#wait-on-the-condition-not-a-frame-count).
  double guiTestMinFrameMs = 0.0;
  // --gui-test-job-latency-ms: no background job is reaped sooner than this
  // after it started — its workers starved, the frames not (SetTestJobLatency).
  double guiTestJobLatencyMs = 0.0;
  bool headless = false;
  bool offscreen = false;
  bool hidden = false;
  bool exitWithParent = false; // app/ParentWatch.h
  // 0 is BOTH "not asked for" and, once asked for, "let the OS choose" —
  // so the request is a separate flag rather than a magic value.
  int remotePort = 0;
  bool remoteRequested = false;
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
    } else if (arg == "--gui-test-shuffle") {
      // Order-independence is a property you find out you have lost, not one
      // you can assert once. Running the suite in a random order surfaces a
      // dependency LOUDLY and reproducibly, instead of waiting for the shard
      // partition to reshuffle and turn something unrelated red.
      args.guiTestShuffle = 0;
      if (i + 1 < argc && argv[i + 1][0] != '-')
        args.guiTestShuffle = std::strtol(argv[++i], nullptr, 10);
    } else if (arg == "--gui-test-filter") {
      if (i + 1 < argc) {
        args.guiTestFilter = argv[++i];
        args.runGuiTests = true;
      } else {
        std::cerr << "--gui-test-filter requires a pattern" << std::endl;
        exit(1);
      }
    } else if (arg == "--gui-test-sequence") {
      if (i + 1 < argc) {
        args.guiTestSequence = argv[++i];
        args.runGuiTests = true;
      } else {
        std::cerr << "--gui-test-sequence requires a list of category/name"
                  << std::endl;
        exit(1);
      }
    } else if (arg == "--gui-test-min-frame-ms" ||
               arg == "--gui-test-job-latency-ms") {
      char* end = nullptr;
      const double ms = i + 1 < argc ? std::strtod(argv[i + 1], &end) : -1.0;
      if (end == nullptr || *end != '\0' || !(ms >= 0.0 && ms <= 60000.0)) {
        std::cerr << arg << " takes milliseconds, 0..60000" << std::endl;
        exit(1);
      }
      (arg == "--gui-test-min-frame-ms" ? args.guiTestMinFrameMs
                                        : args.guiTestJobLatencyMs) = ms;
      ++i;
    } else if (arg == "--gui-test-list") {
      args.listGuiTests = true;
    } else if (arg == "--headless") {
      args.headless = true;
    } else if (arg == "--offscreen") {
      args.offscreen = true;
    } else if (arg == "--remote-bind-all") {
      // For containers: the port is useless on loopback when the network is
      // namespaced. Unauthenticated, so it is never the default.
      args.remoteBindAll = true;
    } else if (arg == "--stall-test") {
      // Hidden: exercise the frame watchdog end-to-end — one 4 s stall a few
      // iterations in, reported with the main thread's stack.
      watchdog::RequestSelfTest();
    } else if (arg == "--crash-test") {
      // Hidden: exercise the fatal-signal backtrace end-to-end (Phase 0
      // sanity). Installed handler prints the stack, then core-dumps.
      CrashHandlerSelfTest();
    } else if (arg == "--perf-hud") {
      args.perfHud = true;
    } else if (arg == "--hidden") {
      args.hidden = true;
    } else if (arg == "--exit-with-parent") {
      args.exitWithParent = true;
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
      // Strict parse, not atoi: atoi("--headless") is 0, and 0 now MEANS
      // something ("choose one"), so a fat-fingered flag would silently start
      // a server on a port the caller never asked for. Refuse, don't
      // substitute.
      const std::string_view val = i + 1 < argc ? argv[i + 1] : "";
      const bool digits = !val.empty() && val.find_first_not_of("0123456789") ==
                                              std::string_view::npos;
      const long parsed = digits ? std::stol(std::string(val)) : -1;
      if (!digits || parsed > 65535) {
        std::cerr << "Error: --remote requires a port (1-65535), or 0 to let "
                     "the OS choose one"
                  << std::endl;
        exit(1);
      }
      args.remotePort = static_cast<int>(parsed);
      args.remoteRequested = true;
      i++;
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

// Where the window is now, for the next launch. Not MarkSettingsChanged():
// that re-applies every preference and asks whether the font moved, neither
// of which a window drag has done. Only the debounce flag is wanted here, so
// dragging an edge is one write instead of a hundred (ui::FlushSettings).
static void RecordWindowGeometry(GLFWwindow* w) {
  settings::WindowGeometry g = g_App.settings.window;
  g.maximized = glfwGetWindowAttrib(w, GLFW_MAXIMIZED) != 0;
  // A maximised window's rect IS the screen, which is useless to restore to —
  // un-maximise and you get a full-screen window with no memory of what it
  // was. Keep the last un-maximised rect and the flag beside it.
  if (!g.maximized) {
    // Wayland has no window position. Asking anyway is not just an error per
    // window event: glfwGetWindowPos ZEROES its out-params before failing, so
    // the rect would record as 0,0 -- which differs from what is stored, marks
    // the settings dirty, and writes 0,0 over the position an X11 session
    // saved. Keep whatever is stored and record only what this session knows.
    if (!OnWayland())
      glfwGetWindowPos(w, &g.x, &g.y);
    glfwGetWindowSize(w, &g.w, &g.h);
  }
  if (g == g_App.settings.window)
    return;
  g_App.settings.window = g;
  g_App.settingsDirtyAt = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
}

// --offscreen: RunHeadless's document engine and remote server, plus a real
// Vulkan renderer with no window, no surface and no display server — Vulkan
// only needs a display to PRESENT, and this mode never presents. It serves
// the path-traced still (render_still), the quick raster still, and the
// raster capture handshake (screenshot, and render_sheet's tiles through it),
// which the offscreen tick fulfils into an offscreen framebuffer.
//
// Lives here rather than beside RunHeadless because everything it drives —
// SetupVulkan, the renderer globals, CleanupVulkan — is file-static in this
// translation unit.
// Everything that has to be true of the renderers before --offscreen draws:
// the document->renderer projection, plus the render-quality targets, the
// section planes and the deform scale, which only matter to a frame.
//
// Called from the capture path rather than the tick, which is the whole point
// of the mode: it draws only when asked, so an edit nobody looks at costs no
// tessellation and no upload. On a 200-body part that is ~2-5 s of renderer
// work per edit that used to be spent unconditionally. The RPCs that read the
// projection (select_face and friends) call ui::SyncRenderersToDocument for
// themselves; the target maintenance below is a frame's business alone.
static void SyncOffscreenScene() {
  ui::SyncRenderersToDocument(g_App);
  // Skipping the rebinds would leave the mesh renderer sampling destroyed
  // views on the first quality change.
  if (g_ShadowRenderer && g_MeshRenderer &&
      g_ShadowRenderer->SetResolution((uint32_t)g_App.shadowMapSize)) {
    g_MeshRenderer->RebindShadowMap(g_ShadowRenderer->ShadowMapView());
  }
  if (g_SSAORenderer && g_MeshRenderer &&
      g_SSAORenderer->SetFullResolution(g_App.ssaoFullRes)) {
    g_MeshRenderer->RebindAOMap(g_SSAORenderer->AOView());
  }
  for (SceneRenderer* r : g_Scene)
    r->SetClipPlanes(ui::SectionPlanes(g_App));
  g_FieldRenderer->SetDeformScale(g_App.simulation.deformScale);
}

static std::atomic<bool> g_OffscreenStopRequested{false};
static void HandleOffscreenStop(int) { g_OffscreenStopRequested = true; }

static int RunOffscreen(const CommandLineArgs& args) {
  LOG_INFO("Offscreen mode: real renderer, no window or display server");

  SetupVulkan({}, nullptr, /*wsi=*/false);
  // The GUI sets these in SetupVulkanWindow, which never runs here. The
  // timer is why this mode is worth measuring in: with no swapchain there is
  // no image to wait for, so the main pass reports shading and nothing else
  // (docs/rendering/frame-pacing.md#measuring-a-pass-offscreen).
  g_MSAASamples = GetMaxUsableSampleCount();
  g_GpuTimer.Initialize(g_Device, g_PhysicalDevice, g_QueueFamily);

  // Same defaults as the GUI and RunHeadless, so view/fit_view/get_state
  // agree across modes and a GUI session opening the same project sees the
  // same framing.
  g_Camera = std::make_unique<Camera>();
  g_Camera->SetPosition(Camera::DefaultIsometricPosition());
  g_Camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
  g_Camera->SetAspectRatio(1280.0f / 720.0f);

  g_App.document = std::make_unique<Document>();
  g_App.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(g_App); // one part to start (assemblies A1)
  g_App.camera = g_Camera.get();

  // Image-based lighting, same block as the GUI init: the RT still lights
  // with these maps (MakeStillDesc refuses without them).
  g_IBLTextures = std::make_unique<IBLTextures>();
  {
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

  g_RayTracingRenderer = std::make_unique<RayTracingRenderer>();
  if (!g_RayTracingRenderer->Initialize(g_Device, g_PhysicalDevice, g_Queue,
                                        g_QueueFamily, g_CommandPool)) {
    // Not fatal: modeling and measurement still serve; render_still reports
    // "ray tracing not available" in its own response.
    LOG_WARN("Offscreen: ray tracing unavailable on this device — "
             "render_still will refuse");
  } else {
    LOG_INFO("Offscreen: ray tracing initialized");
  }

  // The raster stack, mirrored from the GUI init at the fixed 1280x720 the
  // hidden/gui-test carve-out also uses (a host-derived size would make
  // captures depend on whose machine rendered them). Shadow and SSAO come
  // before the mesh renderer, whose descriptor sets bind their maps at
  // creation — same order constraint as the GUI.
  constexpr uint32_t kOffW = 1280, kOffH = 720;
  constexpr VkFormat kOffFormat = VK_FORMAT_B8G8R8A8_UNORM;
  const VkRenderPass offscreenPass =
      CreateOffscreenRenderPass(kOffFormat, g_MSAASamples);
  g_ShadowRenderer = std::make_unique<ShadowRenderer>();
  if (!g_ShadowRenderer->Initialize(g_Device, g_PhysicalDevice, g_Queue,
                                    g_QueueFamily, g_CommandPool))
    LOG_ERROR("Offscreen: ShadowRenderer initialization failed");
  g_SSAORenderer = std::make_unique<SSAORenderer>();
  if (!g_SSAORenderer->Initialize(g_Device, g_PhysicalDevice, g_Queue,
                                  g_QueueFamily, g_CommandPool, kOffW, kOffH))
    LOG_ERROR("Offscreen: SSAORenderer initialization failed");
  g_MeshRenderer = std::make_unique<MeshRenderer>();
  g_MeshRenderer->SetIBL(
      g_IBLTextures->IrradianceView(), g_IBLTextures->PrefilterView(),
      g_IBLTextures->BrdfLutView(), g_IBLTextures->EnvSampler(),
      g_IBLTextures->LutSampler());
  g_MeshRenderer->SetShadowMap(g_ShadowRenderer->ShadowMapView(),
                               g_ShadowRenderer->ShadowSampler());
  g_MeshRenderer->SetAOMap(g_SSAORenderer->AOView(),
                           g_SSAORenderer->AOSampler());
  if (g_MeshRenderer->Initialize(g_Device, g_PhysicalDevice, offscreenPass,
                                 g_Queue, g_QueueFamily, g_CommandPool,
                                 g_MSAASamples))
    g_MeshRenderer->SetViewportSize(kOffW, kOffH);
  else
    LOG_ERROR("Offscreen: MeshRenderer initialization failed");
  g_OutlineRenderer = std::make_unique<OutlineRenderer>();
  if (g_OutlineRenderer->Initialize(g_Device, g_PhysicalDevice, offscreenPass,
                                    g_Queue, g_QueueFamily, g_CommandPool,
                                    g_MSAASamples)) {
    g_OutlineRenderer->SetViewportSize(kOffW, kOffH);
    // Same override as the GUI init: the struct's default is EDGES in bright
    // red at width 5 ("for testing"), which paints the whole part red at
    // this scale. WIREFRAME with the subtle styling is the product look.
    OutlineSettings outline = g_OutlineRenderer->GetOutlineSettings();
    outline.mode = OutlineMode::WIREFRAME;
    g_OutlineRenderer->SetOutlineSettings(outline);
  }
  g_SketchRenderer = std::make_unique<SketchRenderer>();
  if (g_SketchRenderer->Initialize(g_Device, g_PhysicalDevice, offscreenPass,
                                   g_Queue, g_QueueFamily, g_CommandPool,
                                   g_MSAASamples))
    g_SketchRenderer->SetViewportSize(kOffW, kOffH);
  g_UnderlayRenderer = std::make_unique<UnderlayRenderer>();
  if (g_UnderlayRenderer->Initialize(g_Device, g_PhysicalDevice, offscreenPass,
                                     g_Queue, g_QueueFamily, g_CommandPool,
                                     g_MSAASamples))
    g_UnderlayRenderer->SetViewportSize(kOffW, kOffH);
  g_FieldRenderer = std::make_unique<FieldRenderer>();
  if (g_FieldRenderer->Initialize(g_Device, g_PhysicalDevice, offscreenPass,
                                  g_Queue, g_QueueFamily, g_CommandPool,
                                  g_MSAASamples))
    g_FieldRenderer->SetViewport(0, 0, kOffW, kOffH);
  g_StockRenderer = std::make_unique<StockRenderer>();
  if (g_StockRenderer->Initialize(g_Device, g_PhysicalDevice, offscreenPass,
                                  g_Queue, g_QueueFamily, g_CommandPool,
                                  g_MSAASamples))
    g_StockRenderer->SetViewport(0, 0, kOffW, kOffH);
  g_FaceSelector = std::make_unique<FaceSelector>();

  g_App.meshRenderer = g_MeshRenderer.get();
  g_App.outlineRenderer = g_OutlineRenderer.get();
  g_App.sketchRenderer = g_SketchRenderer.get();
  g_App.underlayRenderer = g_UnderlayRenderer.get();
  g_App.fieldRenderer = g_FieldRenderer.get();
  g_App.stockRenderer = g_StockRenderer.get();
  RegisterSceneRenderers();
  g_App.sceneRenderers = g_Scene;
  g_App.rayTracer = g_RayTracingRenderer.get();
  g_App.faceSelector = g_FaceSelector.get();
  g_App.ibl = g_IBLTextures.get();
  // Shadow/SSAO/shadow-map-size preferences flow into the same fields
  // set_render_options writes, so the toggles below read coherent state.
  ui::ApplySettings(g_App);

  if (!args.fileToLoad.empty()) {
    LOG_INFO("Loading startup file: {}", args.fileToLoad);
    ui::ImportAsFeature(g_App, args.fileToLoad);
  }

  RemoteServer server;
  // No wake callback: there is no GLFW event loop to wake (and
  // glfwPostEmptyEvent without glfwInit would be the bug). The 2 ms poll
  // below is the pacing.
  if (!server.Start(static_cast<uint16_t>(args.remotePort),
                    RemoteMode::Offscreen, args.remoteBindAll)) {
    LOG_ERROR("Offscreen: cannot listen on port {} — exiting", args.remotePort);
    return 1;
  }
  if (args.remotePort == 0)
    LOG_INFO("Offscreen: remote port {}", server.Port());
  if (!args.recordPath.empty() && !server.StartRecording(args.recordPath)) {
    return 1;
  }

  // The measuring verb (frame_stats {renders: N}). Synchronous: this runs on
  // the main thread inside ProcessPending, which is the same thread the tick
  // renders on, so there is no handshake to stage — unlike a screenshot,
  // nothing here has to wait for a camera animation or produce a file.
  g_App.renderBurst = [&](size_t count) -> size_t {
    if (count == 0)
      return 0;
    SyncOffscreenScene();
    const glm::mat4 view = g_Camera->GetViewMatrix();
    const glm::mat4 proj = g_Camera->GetProjectionMatrix();
    for (SceneRenderer* r : g_Scene)
      r->UpdateCamera(view, proj);
    vkQueueWaitIdle(g_Queue);
    const ui::ClearColor vp = ui::ViewportClearColor();
    VkClearValue clear{};
    clear.color.float32[0] = vp.r;
    clear.color.float32[1] = vp.g;
    clear.color.float32[2] = vp.b;
    clear.color.float32[3] = 1.0f;
    constexpr uint32_t kTimerSlot = 0;
    // Warm up first, measuring nothing. Two things are cold at the start of a
    // burst and neither belongs in the answer: pipelines are created on first
    // use, and the GPU is at its idle clock — a laptop ramps over the first
    // tenth of a second, so an unwarmed burst of 300 had its median taken
    // over frames that were still climbing (a bare box read 0.237 ms against
    // 0.033 measured warm).
    constexpr uint32_t kWarmup = 32;
    const auto render = [&](uint32_t times,
                            const std::function<void()>& after) {
      return CaptureScreenshot(
          g_Device, g_PhysicalDevice, g_Queue, g_CommandPool, offscreenPass,
          kOffFormat, kOffW, kOffH, clear,
          [&](VkCommandBuffer cb) {
            for (SceneRenderer* r : g_Scene)
              r->Render(cb, 0);
            g_GpuTimer.Write(cb, kTimerSlot,
                             render::GpuTimer::Point::AfterMain);
          },
          /*outputPath=*/"", /*error=*/nullptr, g_MSAASamples,
          [&](VkCommandBuffer cb) {
            g_GpuTimer.BeginFrame(cb, kTimerSlot);
            RecordScenePrepasses(cb, kTimerSlot, &g_GpuTimer);
          },
          /*stats=*/nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, times,
          after);
    };
    render(kWarmup, {});
    g_GpuTimer.ResetSamples(); // a burst is its own measurement window
    g_App.gpuStats = {};
    const bool ok = render(static_cast<uint32_t>(count),
                           [&] { g_GpuTimer.Harvest(kTimerSlot); });
    const render::GpuTimer::Snapshot gs = g_GpuTimer.Average();
    g_App.gpuStats.supported = gs.supported;
    g_App.gpuStats.samples = gs.samples;
    g_App.gpuStats.shadowMs = gs.shadowMs;
    g_App.gpuStats.ssaoMs = gs.ssaoMs;
    g_App.gpuStats.mainMs = gs.mainMs;
    g_App.gpuStats.totalMs = gs.totalMs;
    g_App.gpuStats.shadowP50Ms = gs.shadowP50Ms;
    g_App.gpuStats.ssaoP50Ms = gs.ssaoP50Ms;
    g_App.gpuStats.mainP50Ms = gs.mainP50Ms;
    g_App.gpuStats.totalP50Ms = gs.totalP50Ms;
    return ok ? count : 0;
  };

  // Quick render (docs/rendering/still-render.md#quick-mode). Synchronous like
  // the burst; the lazy projection is brought up to date first, as a capture
  // is.
  const auto offscreenTarget = [&] {
    RasterTarget t;
    t.pass = offscreenPass;
    t.format = kOffFormat;
    t.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    t.frame = 0;
    const ui::ClearColor vp = ui::ViewportClearColor();
    t.clear.color = {{vp.r, vp.g, vp.b, 1.0f}};
    t.aoW = t.vpW = kOffW;
    t.aoH = t.vpH = kOffH;
    return t;
  };
  g_App.captureRaster = [&](const ui::AppState::RasterStill& req,
                            ui::RasterFrame* out, std::string* error,
                            std::string* warning) {
    SyncOffscreenScene();
    return CaptureRasterStill(offscreenTarget(), req, out, error, warning);
  };
  // A motion video's frames: the same target, streamed.
  g_App.beginFrameCapture = [&](uint32_t w, uint32_t h, std::string* error) {
    SyncOffscreenScene();
    return BeginFrameCapture(offscreenTarget(), w, h, error);
  };

  std::signal(SIGINT, HandleOffscreenStop);
  std::signal(SIGTERM, HandleOffscreenStop);

  const auto start = std::chrono::steady_clock::now();
  auto seconds = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
  };
  Autosaver autosaver(args.autosaveSeconds);
  autosaver.Prime(seconds());

  double last = seconds();
  double animatingSince = -1.0;
  while (!g_OffscreenStopRequested) {
    watchdog::Beat();
    server.ProcessPending(g_App);
    if (g_App.gpuStatsReset) {
      g_App.gpuStatsReset = false;
      g_GpuTimer.ResetSamples();
      g_App.gpuStats = {};
    }
    const double now = seconds();
    // Real dt so orient_to_face's animation completes at wall-clock speed.
    g_Camera->Update(static_cast<float>(now - last));
    last = now;

    // Playback is WALL-CLOCK: it has to advance whether or not anybody is
    // looking, so it ticks here while the projection it feeds does not.
    ui::TickCamPlayback(g_App, now);
    ui::TickMotionPlayback(g_App, now);
    // Every long-running operation, from one place (ui/BackgroundJobs.h):
    // whether a background job can finish must not depend on which panel is
    // open, which is what two of them used to depend on.
    ui::PollBackgroundJobs(g_App);

    // Captures hold until the camera lands: orient_to_face animates ~1 s,
    // and WantsScreenshot's own 300-call cap was tuned for 60 fps frames —
    // at this tick rate it elapses in 0.6 s, mid-flight. Bounded by wall
    // clock so a stuck animation degrades to a mid-flight capture rather
    // than a wedged queue.
    if (g_Camera->IsAnimating()) {
      if (animatingSince < 0)
        animatingSince = now;
    } else {
      animatingSince = -1.0;
    }
    std::string shotPath;
    if ((animatingSince < 0 || now - animatingSince > 5.0) &&
        server.WantsScreenshot(shotPath)) {
      SyncOffscreenScene();
      const glm::mat4 view = g_Camera->GetViewMatrix();
      const glm::mat4 proj = g_Camera->GetProjectionMatrix();
      for (SceneRenderer* r : g_Scene)
        r->UpdateCamera(view, proj);
      vkQueueWaitIdle(g_Queue);
      const ui::ClearColor vp = ui::ViewportClearColor();
      VkClearValue clear{};
      clear.color.float32[0] = vp.r;
      clear.color.float32[1] = vp.g;
      clear.color.float32[2] = vp.b;
      clear.color.float32[3] = 1.0f;
      std::string shotError;
      FrameStats shotStats; // rect unset = judge the whole frame, which IS
                            // the viewport here (no panel chrome exists)
      // ONE query slot, index 0: there are no swapchain images here and no
      // frame loop to collide with. (The windowed screenshot path stays
      // untimed — there it WOULD collide with the live frame's slot.)
      constexpr uint32_t kTimerSlot = 0;
      if (g_OutlineRenderer)
        g_OutlineRenderer->SettleSilhouettes();
      const bool ok = CaptureScreenshot(
          g_Device, g_PhysicalDevice, g_Queue, g_CommandPool, offscreenPass,
          kOffFormat, kOffW, kOffH, clear,
          [&](VkCommandBuffer cb) {
            // The scene only — no ImGui: nothing draws an interface in this
            // mode, so a capture is the viewport, full frame. Which also
            // makes `main` here the SCENE's cost, with no interface over it.
            for (SceneRenderer* r : g_Scene)
              r->Render(cb, 0);
            g_GpuTimer.Write(cb, kTimerSlot,
                             render::GpuTimer::Point::AfterMain);
          },
          shotPath, &shotError, g_MSAASamples,
          [&](VkCommandBuffer cb) {
            g_GpuTimer.BeginFrame(cb, kTimerSlot);
            RecordScenePrepasses(cb, kTimerSlot, &g_GpuTimer);
          },
          &shotStats, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      // CaptureScreenshot waits on the queue, so this slot's results are
      // already there: harvesting never stalls.
      g_GpuTimer.Harvest(kTimerSlot);
      {
        const render::GpuTimer::Snapshot gs = g_GpuTimer.Average();
        g_App.gpuStats.supported = gs.supported;
        g_App.gpuStats.samples = gs.samples;
        g_App.gpuStats.shadowMs = gs.shadowMs;
        g_App.gpuStats.ssaoMs = gs.ssaoMs;
        g_App.gpuStats.mainMs = gs.mainMs;
        g_App.gpuStats.totalMs = gs.totalMs;
        g_App.gpuStats.shadowP50Ms = gs.shadowP50Ms;
        g_App.gpuStats.ssaoP50Ms = gs.ssaoP50Ms;
        g_App.gpuStats.mainP50Ms = gs.mainP50Ms;
        g_App.gpuStats.totalP50Ms = gs.totalP50Ms;
      }
      server.ScreenshotDone(ok, shotError, shotStats.dominantFraction,
                            shotStats.hash);
    }

    ui::AutosaveProject(g_App, autosaver, now);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  LOG_INFO("Offscreen: stop requested, shutting down");
  server.Stop();
  // Clean exit with nothing unsaved: drop the sidecar. Dirty exits keep it.
  if (g_App.document && !ui::ProjectDirty(g_App)) {
    Autosaver::Clear(g_App.projectPath);
  }

  vkDeviceWaitIdle(g_Device);
  g_App.document.reset();
  g_App.commands.reset();
  g_Camera.reset();
  // Renderer teardown order mirrors the GUI exit: UnderlayRenderer's
  // Shutdown waits on the device, so it must run before CleanupVulkan.
  // The per-part render cache holds RETAINED asset ids in the mesh and
  // outline renderers; releasing them after the renderers are gone would free
  // nothing and leave the buffers to a dead device. Before, not after.
  ui::ReleasePartRenderCache(g_App);
  ShutdownSceneRenderers();
  g_GpuTimer.Shutdown(g_Device);
  g_RayTracingRenderer.reset();
  g_ShadowRenderer.reset();
  g_SSAORenderer.reset();
  g_FaceSelector.reset();
  g_IBLTextures->Shutdown(g_Device);
  g_IBLTextures.reset();
  vkDestroyRenderPass(g_Device, offscreenPass, g_Allocator);
  CleanupVulkan();
  return 0;
}

int main(int argc, char** argv) {
  InstallCrashHandler(); // fatal-signal backtraces in dev-watch logs
  CommandLineArgs args = parseCommandLineArgs(argc, argv);
  // Before any thread: a test's instance dies with the test
  // (app/ParentWatch.h).
  if (const int status = ArmExitWithParent(args.exitWithParent); status >= 0)
    return status;

  // The frame watchdog (common/Watchdog.h): a stalled main thread is logged
  // with what it was doing and its stack. Every loop below beats it.
  double watchdogSeconds = 0.0;
  std::string watchdogError;
  if (!watchdog::ThresholdFromEnvironment(&watchdogSeconds, &watchdogError)) {
    std::cerr << watchdogError << std::endl;
    return 2;
  }
  InstallStallCapture(); // here: it records THIS thread as the one to ask
  watchdog::Start(watchdogSeconds, [] { return CaptureMainThreadStack(1.0); });
  struct WatchdogStop {
    ~WatchdogStop() { watchdog::Stop(); }
  } watchdogStop;

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
  // The machine store lives beside the settings and loads with them. Under
  // the GUI tests too: the runner points XDG_CONFIG_HOME at a scratch
  // directory, so the tests see an empty store rather than the desk's.
  ui::ReloadMachines(g_App);
#ifdef HAVE_CAM
  // Surfacing passes kept between runs
  // (docs/ui/long-operations.md#passes-kept-on-disk). Not under the GUI
  // tests, which run on defaults for the same reason as the settings: what
  // an earlier run left must not decide what a test sees.
  if (!args.runGuiTests)
    ui::OpenCamSurfaceStore(g_App);
#endif
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

  // Mode flags conflict loudly, never by precedence: --headless silently
  // overriding --hidden once left a windowed test running headless for weeks
  // (CLAUDE.md), so a flag pair that cannot both hold is an error, not a
  // ranking. Checked before either mode branch can win.
  if (args.offscreen) {
    if (args.headless) {
      std::cerr << "Error: --offscreen cannot run with --headless "
                   "(offscreen IS headless plus a renderer — pick one)"
                << std::endl;
      return 2;
    }
    if (args.hidden) {
      std::cerr << "Error: --offscreen cannot run with --hidden "
                   "(offscreen has no window to hide)"
                << std::endl;
      return 2;
    }
    if (args.runGuiTests) {
      std::cerr << "Error: --gui-tests cannot run with --offscreen "
                   "(the tests drive a windowed GUI)"
                << std::endl;
      return 2;
    }
    if (!args.remoteRequested) {
      std::cerr << "Error: --offscreen requires --remote <port> (there is "
                   "nothing else to serve; --remote 0 picks one)"
                << std::endl;
      return 2;
    }
    return RunOffscreen(args);
  }

  // Headless: document engine + remote server, no GLFW/Vulkan/ImGui at all.
  // Must branch before any windowing call — this path runs with no display
  // server and no GPU (CI).
  if (args.headless) {
    if (!args.remoteRequested) {
      std::cerr << "Error: --headless requires --remote <port> (there is "
                   "nothing else to serve; --remote 0 picks one)"
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

  // Where to open. An INTERACTIVE run restores what it had last time, or —
  // on a first run — takes 80% of the monitor's work area; a literal 1280x720
  // is a third of a 1440p panel and most of a laptop's.
  //
  // Offscreen and GUI-test runs keep the fixed 1280x720, and that carve-out
  // is load-bearing rather than tidiness: toolbar/every_tool_is_reachable and
  // tabs/overflow_scrolls_and_plus_stays are assertions about what fits AT
  // THAT SIZE, and gui-tests-xvfb.sh / headless-preview.sh --gui create that
  // screen. A host-derived size here would make layout tests depend on whose
  // monitor they ran on.
  const bool rememberGeometry = !args.hidden && !args.runGuiTests;
  settings::WindowGeometry geom;
  geom.w = settings::kDefaultWidth;
  geom.h = settings::kDefaultHeight;
  if (rememberGeometry) {
    std::vector<settings::MonitorRect> monitors;
    int monitorCount = 0;
    GLFWmonitor** found = glfwGetMonitors(&monitorCount);
    for (int i = 0; i < monitorCount; i++) {
      settings::MonitorRect m;
      // The WORK area, not the video mode: a window sized to the mode opens
      // underneath the taskbar.
      glfwGetMonitorWorkarea(found[i], &m.x, &m.y, &m.w, &m.h);
      if (m.w > 0 && m.h > 0)
        monitors.push_back(m);
    }
    geom = settings::FitToMonitors(g_App.settings.window, monitors);
  }

  GLFWwindow* window = glfwCreateWindow(
      geom.w, geom.h, "Designer - 3D CAD Software", nullptr, nullptr);
  if (window && rememberGeometry) {
    // Position after creation: the hint-based route needs a monitor handle
    // and would make this fullscreen. Wayland refuses positioning outright --
    // the compositor places windows -- so there it is not attempted.
    if (!OnWayland())
      glfwSetWindowPos(window, geom.x, geom.y);
    if (g_App.settings.window.maximized)
      glfwMaximizeWindow(window);
  }
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
  SetupVulkan(extensions, window, /*wsi=*/true);

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
  //
  // The layout is a PREFERENCE, so it belongs beside settings.json in the
  // config directory — not in `imgui.ini` in whatever directory the app was
  // launched from, which is where ImGui puts it by default. That default
  // punched a hole in the isolation headless-preview.sh exists to provide:
  // it gives a scratch instance its own XDG_CONFIG_HOME so an agent cannot
  // touch the human's preferences, and then the agent rewrote the human's
  // window layout anyway, because both instances run from the repo root.
  // A layout already sitting in the launch directory is adopted once, so
  // nobody loses the arrangement they had.
  static std::string s_iniPath;
  if (args.runGuiTests) {
    io.IniFilename = nullptr;
    g_App.buildDefaultDockLayout = true;
  } else {
    const std::filesystem::path dir(::settings::ConfigDir());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path ini = dir / "imgui.ini";
    if (!std::filesystem::exists(ini) && std::filesystem::exists("imgui.ini"))
      std::filesystem::copy_file(
          "imgui.ini", ini, std::filesystem::copy_options::overwrite_existing,
          ec);
    s_iniPath = ini.string();
    io.IniFilename = s_iniPath.c_str();
    g_App.buildDefaultDockLayout = !std::filesystem::exists(ini);
  }
  g_App.runGuiTests = args.runGuiTests;
  if (args.runGuiTests)
    ui::SetTestJobLatency(args.guiTestJobLatencyMs / 1000.0);
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
  // The two factors separately, because they fail differently: a content
  // scale of 1.00 on a HiDPI display means the display's scaling never
  // reached us, while a preference of 1.00 just means nobody changed it.
  LOG_INFO("Display content scale {:.2f} x font preference {:.2f}",
           std::max(1.0f, std::max(scaleX, scaleY)), g_App.uiFontScale);
  // The scale the theme is currently built at, and a pending one when the
  // display's changes. Both live here so the callback can tell a real change
  // from a repeat: Wayland's wp_fractional_scale_v1 re-sends the preferred
  // scale on every configure, and treating "the callback fired" as "the scale
  // changed" rebuilt the whole font atlas each time the window was dragged.
  static float s_contentScale = std::max(1.0f, std::max(scaleX, scaleY));
  static float s_pendingContentScale = 0.0f; // 0 = no change pending
  // How much the backend already magnifies ImGui's coordinates: the ratio of
  // the framebuffer GLFW draws into to the window's logical size. 1.0 on X11;
  // the display scale on Wayland, where GLFW scales the framebuffer itself.
  const auto framebufferScale = [](GLFWwindow* w) {
    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(w, &ww, &wh);
    glfwGetFramebufferSize(w, &fw, &fh);
    return (ww > 0 && fw > 0) ? static_cast<float>(fw) / static_cast<float>(ww)
                              : 1.0f;
  };
  ui::ApplyTheme(s_contentScale * g_App.uiFontScale, framebufferScale(window));
  glfwSetWindowContentScaleCallback(
      window, [](GLFWwindow*, float sx, float sy) {
        const float s = std::max(1.0f, std::max(sx, sy));
        if (s != s_contentScale)
          s_pendingContentScale = s;
      });

  // Remember where the window ends up. Recorded on move/resize/maximise and
  // written by the SAME debounced path as every other setting
  // (ui::FlushSettings), so dragging an edge is one write rather than a
  // hundred — and merge-on-save means the other instance running here keeps
  // its own rect.
  //
  // A MAXIMISED window's position and size are the maximised ones, which are
  // useless to restore to: un-maximise and you get a window filling the
  // screen with no memory of what it was. So the rect is only recorded while
  // the window is not maximised, and `maximized` is recorded beside it.
  if (rememberGeometry) {
    glfwSetWindowPosCallback(
        window, [](GLFWwindow* w, int, int) { RecordWindowGeometry(w); });
    glfwSetWindowSizeCallback(
        window, [](GLFWwindow* w, int, int) { RecordWindowGeometry(w); });
    glfwSetWindowMaximizeCallback(
        window, [](GLFWwindow* w, int) { RecordWindowGeometry(w); });
  }

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
  g_Camera = std::make_unique<Camera>();
  g_App.document = std::make_unique<Document>();
  g_App.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(g_App); // one part to start (assemblies A1)
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

  // Key-light shadow map (S1) + SSAO (S2): initialized before the mesh
  // renderer, whose descriptor sets bind their maps at creation (like IBL).
  g_ShadowRenderer = std::make_unique<ShadowRenderer>();
  if (!g_ShadowRenderer->Initialize(g_Device, g_PhysicalDevice, g_Queue,
                                    g_QueueFamily, g_CommandPool)) {
    LOG_ERROR("ShadowRenderer initialization failed");
  } else {
    LOG_INFO("ShadowRenderer initialized successfully");
  }
  g_GpuTimer.Initialize(g_Device, g_PhysicalDevice, g_QueueFamily);
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

  // Initialize StockRenderer (CAM cutting simulation)
  g_StockRenderer = std::make_unique<StockRenderer>();
  if (g_StockRenderer->Initialize(g_Device, g_PhysicalDevice, wd->RenderPass,
                                  g_Queue, g_QueueFamily, g_CommandPool,
                                  g_MSAASamples)) {
    g_StockRenderer->SetViewport(0, 0, 1280, 720);
  } else {
    LOG_WARN("StockRenderer initialization failed");
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
  g_App.stockRenderer = g_StockRenderer.get();
  RegisterSceneRenderers();
  g_App.sceneRenderers = g_Scene;
  g_App.rayTracer = g_RayTracingRenderer.get();
  g_App.faceSelector = g_FaceSelector.get();
  g_App.ibl = g_IBLTextures.get();

  // Quick render (docs/rendering/still-render.md#quick-mode), from the dialog
  // or render_still. Both run on this thread between frames, so after a wait
  // the current frame slot is idle; the viewport rect it restores is the one
  // UI.cpp sets every frame (the dock between the panels, in framebuffer px).
  static const auto windowTarget = [] {
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    RasterTarget t;
    t.pass = wd->RenderPass;
    t.format = wd->SurfaceFormat.format;
    t.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    t.frame = wd->FrameIndex;
    const ui::ClearColor vp = ui::ViewportClearColor();
    t.clear.color = {{vp.r, vp.g, vp.b, 1.0f}};
    t.aoW = static_cast<uint32_t>(wd->Width);
    t.aoH = static_cast<uint32_t>(wd->Height);
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    if (g_App.viewportSize.x > 0 && g_App.viewportSize.y > 0) {
      t.vpX = static_cast<int32_t>(g_App.viewportPos.x * scale.x);
      t.vpY = static_cast<int32_t>(g_App.viewportPos.y * scale.y);
      t.vpW = static_cast<uint32_t>(g_App.viewportSize.x * scale.x);
      t.vpH = static_cast<uint32_t>(g_App.viewportSize.y * scale.y);
    } else {
      t.vpW = t.aoW;
      t.vpH = t.aoH;
    }
    return t;
  };
  g_App.captureRaster = [](const ui::AppState::RasterStill& req,
                           ui::RasterFrame* out, std::string* error,
                           std::string* warning) {
    return CaptureRasterStill(windowTarget(), req, out, error, warning);
  };
  // A motion video's frames: the viewport's render, streamed.
  g_App.beginFrameCapture = [](uint32_t w, uint32_t h, std::string* error) {
    return BeginFrameCapture(windowTarget(), w, h, error);
  };
  // The live viewport, re-rendered as the frame loop records it (the remote
  // screenshot's path without the interface) and cropped to the viewport.
  g_App.captureViewport = [](std::vector<uint8_t>* rgb, uint32_t* w,
                             uint32_t* h) {
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    const RasterTarget t = windowTarget();
    vkQueueWaitIdle(g_Queue);
    if (g_OutlineRenderer)
      g_OutlineRenderer->SettleSilhouettes();
    std::vector<uint8_t> full;
    const uint32_t W = static_cast<uint32_t>(wd->Width),
                   H = static_cast<uint32_t>(wd->Height);
    if (!CaptureScreenshot(
            g_Device, g_PhysicalDevice, g_Queue, g_CommandPool, wd->RenderPass,
            wd->SurfaceFormat.format, W, H, wd->ClearValue,
            [&](VkCommandBuffer cb) {
              for (SceneRenderer* r : g_Scene)
                r->Render(cb, wd->FrameIndex);
            },
            /*outputPath=*/"", nullptr, g_MSAASamples,
            [&](VkCommandBuffer cb) {
              RecordScenePrepasses(cb, wd->FrameIndex);
            },
            nullptr, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1, {}, &full))
      return false;
    const uint32_t x0 = static_cast<uint32_t>(std::max(t.vpX, 0)),
                   y0 = static_cast<uint32_t>(std::max(t.vpY, 0));
    if (t.vpW == 0 || t.vpH == 0 || x0 + t.vpW > W || y0 + t.vpH > H)
      return false;
    rgb->resize(size_t(t.vpW) * t.vpH * 3);
    for (uint32_t y = 0; y < t.vpH; y++)
      std::copy_n(&full[(size_t(y0 + y) * W + x0) * 3], size_t(t.vpW) * 3,
                  &(*rgb)[size_t(y) * t.vpW * 3]);
    *w = t.vpW;
    *h = t.vpH;
    return true;
  };

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
  // A suite run counts ImGui's clock in FRAMES, one 60th of a second each
  // (the frame cap's own rate, so a quiet machine sees no change). The engine
  // injects input a frame at a time, but ImGui judges it by the clock: its
  // double-click is two presses two frames apart that land within 0.3 s, so
  // with 150 ms frames under someone else's build every double-click in the
  // suite was two single clicks and the editor it opens never appeared
  // (docs/ui/testing.md#wait-on-the-condition-not-a-frame-count). Not for an
  // interactive app the engine merely serves (remote ui_action): its clock is
  // the user's.
  if (args.runGuiTests)
    testEngineIO.ConfigFixedDeltaTime = 1.0f / 60.0f;
  ImGuiTestEngine_Start(testEngine, ImGui::GetCurrentContext());
  RegisterGuiTests(testEngine, &g_App);
  if (args.runGuiTests && !args.guiTestSequence.empty()) {
    // A failed shard's order, replayed as it ran. The seed alone cannot do
    // that for long: it shuffles the WHOLE registered list, so a test added
    // since the failure deals every shard a different order, and the replay
    // passes for no reason (docs/ui/testing.md#replaying-a-failed-shard).
    if (args.guiTestShuffle >= 0 || args.guiTestFilter != "all") {
      std::cerr << "--gui-test-sequence fixes both the tests and their order; "
                   "it takes no --gui-test-filter or --gui-test-shuffle"
                << std::endl;
      return 1;
    }
    std::vector<ImGuiTest*> sequence;
    std::string unknown;
    std::stringstream entries(args.guiTestSequence);
    for (std::string entry; std::getline(entries, entry, ',');) {
      if (entry.empty())
        continue;
      ImGuiTest* found = nullptr;
      for (ImGuiTest* test : testEngine->TestsAll)
        if (entry == std::string(test->Category) + "/" + test->Name)
          found = test;
      if (found == nullptr)
        unknown += (unknown.empty() ? "" : ", ") + entry;
      else
        sequence.push_back(found);
    }
    // Refused, not skipped: a replay without one of its tests is another
    // order.
    if (!unknown.empty()) {
      std::cerr << "--gui-test-sequence: no such test: " << unknown
                << " (--gui-test-list prints every category/name)" << std::endl;
      return 1;
    }
    for (ImGuiTest* test : sequence)
      ImGuiTestEngine_QueueTest(testEngine, test);
  } else if (args.runGuiTests) {
    if (args.guiTestShuffle >= 0) {
      // Shuffle the engine's own test list rather than the queue: QueueTests
      // walks TestsAll in order, so reordering it here leaves the filter
      // semantics entirely to the engine. (This is why imgui_te_internal.h is
      // included — the list is not in the public header.)
      unsigned long seed = static_cast<unsigned long>(args.guiTestShuffle);
      if (seed == 0)
        seed = static_cast<unsigned long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
      // Printed unconditionally: a shuffled failure is worthless if it cannot
      // be replayed with --gui-test-shuffle <seed>.
      std::cout << "GUI tests: shuffled order, seed " << seed << std::endl;
      std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
      std::shuffle(testEngine->TestsAll.begin(), testEngine->TestsAll.end(),
                   rng);
    }
    // The filter matches the test engine's syntax: substrings against
    // "category/name", comma-separated, "-" to exclude ("all" = everything).
    ImGuiTestEngine_QueueTests(testEngine, ImGuiTestGroup_Tests,
                               args.guiTestFilter.c_str());
  }
#endif

  // Remote control (opt-in)
  RemoteServer remoteServer;
  // Before EITHER Start path (here and ensureRemotePort below): the reader
  // threads call this unsynchronized. An enqueued request wakes a throttled
  // loop out of WaitEventsTimeout, so RPC latency does not inherit the idle
  // floor (docs/rendering/frame-pacing.md#idle-throttle). glfwPostEmptyEvent is
  // one of the few GLFW calls documented thread-safe.
  remoteServer.SetWakeCallback([] { glfwPostEmptyEvent(); });
  if (args.remoteRequested) {
    // Fail fast on a taken port, as the headless path does (HeadlessMain.cpp).
    // This path ignored the result, so `--hidden --remote <taken>` — how every
    // preview instance and dev-watch starts — came up looking healthy and
    // served nothing, and the client just timed out with no reason given.
    // This path always has a renderer (that is what distinguishes it from
    // HeadlessMain), so screenshots work; whether the GUI can be DRIVEN is
    // the separate question SetTestEngine answers.
    if (!remoteServer.Start(static_cast<uint16_t>(args.remotePort),
                            RemoteMode::Gui, args.remoteBindAll)) {
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
    // From the SERVER, not the argument: with `--remote 0` the argument is 0
    // and the real port is whatever the kernel gave us. The terminal panel
    // and the instance registry both key off this.
    g_App.remotePort = remoteServer.Port();
  }

  // The terminal panel binds a Claude Code session to THIS window by port
  // (docs/ui/terminal.md), which is useless if the app was started
  // without --remote. main.cpp owns the server, so it hands the panel a way
  // to open one on demand.
  //
  // The documented default FIRST, so a client configured for 8437 finds a
  // lone instance — then straight to the kernel. This used to walk 8437..8456
  // one bind at a time, which made a fixed 20-port range a resource shared
  // between every process on the machine. Measured with four concurrent GUI
  // shards: they collided, walked to 8438 and 8440, and logged an `error` per
  // failed bind — up to 19 socket/bind/close syscalls on the UI THREAD, mid
  // frame, in a suite whose tests wait fixed frame counts. Asking the kernel
  // is one syscall and cannot collide with anyone.
  g_App.ensureRemotePort = [&remoteServer]() -> int {
    if (remoteServer.Port() != 0)
      return remoteServer.Port();
    for (const uint16_t want : {uint16_t{8437}, uint16_t{0}}) {
      if (remoteServer.Start(want, RemoteMode::Gui, /*bindAll=*/false)) {
        const int got = remoteServer.Port();
        LOG_INFO("Terminal panel opened the remote interface on port {}", got);
        g_App.remotePort = got;
        return got;
      }
    }
    LOG_WARN("Could not open a remote port for the terminal panel");
    return 0;
  };

  // Autosave (F2). GUI tests force it off: a mid-test autosave would break
  // their determinism (and there is nothing worth recovering).
  Autosaver autosaver(args.runGuiTests ? 0.0 : args.autosaveSeconds);
  autosaver.Prime(glfwGetTime());

  const ui::ClearColor vp = ui::ViewportClearColor();
  ImVec4 clear_color = ImVec4(vp.r, vp.g, vp.b, 1.00f);

  // Main loop
  static app::FramePacer s_pacer;
  bool paceIdle = false; // last iteration's verdict paces this one
  while (!glfwWindowShouldClose(window)) {
    // Idle throttle (docs/rendering/frame-pacing.md#idle-throttle): with
    // nothing happening, wait for events at a ~10 Hz floor instead of
    // re-rendering the unchanged scene at vsync rate. BEFORE BeginFrame on
    // purpose — the wait is not part of the frame, so frame_stats keeps reading
    // "10 fps of cheap frames", not "100 ms frames". Any OS event or a posted
    // wake (a remote request) ends the wait immediately. Every iteration that
    // runs still renders exactly as before.
    if (paceIdle)
      glfwWaitEventsTimeout(app::FramePacer::kIdleTimeoutSeconds);
    watchdog::Beat(); // per iteration: a minimized window finishes no frame
    g_App.frameTimer.BeginFrame(glfwGetTime());
    g_App.frameTimer.SetIdlePaced(paceIdle);
    const double frameCpuStartMs = ThreadCpuMs();
    glfwPollEvents();

    // Monitor content scale changed (window moved to another display) or the
    // user picked a font size (U5): re-apply the theme and rebuild the font
    // atlas — both must happen outside a frame, hence only flags here.
    if (s_pendingContentScale > 0.0f || g_App.fontRebuildRequested) {
      if (s_pendingContentScale > 0.0f)
        s_contentScale = s_pendingContentScale;
      s_pendingContentScale = 0.0f;
      g_App.fontRebuildRequested = false;
      vkDeviceWaitIdle(g_Device); // the old font texture may be in flight
      ImGui_ImplVulkan_DestroyFontsTexture();
      ui::ApplyTheme(s_contentScale * g_App.uiFontScale,
                     framebufferScale(window));
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
      g_App.swapchainImages = g_MainWindowData.ImageCount;
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
      // The framebuffer changed size. SetViewportSize is a DIFFERENT question
      // from the scene viewport rect (which UI.cpp sets from the sub-region
      // between the panels every frame) and only some renderers have it.
      if (g_OutlineRenderer)
        g_OutlineRenderer->SetViewportSize(fb_width, fb_height);
      if (g_SketchRenderer)
        g_SketchRenderer->SetViewportSize(fb_width, fb_height);
      if (g_UnderlayRenderer)
        g_UnderlayRenderer->SetViewportSize(fb_width, fb_height);
      for (SceneRenderer* r : g_Scene)
        r->SetViewport(0, 0, fb_width, fb_height);
      if (g_Camera) {
        g_Camera->SetAspectRatio(static_cast<float>(fb_width) /
                                 static_cast<float>(fb_height));
      }
    }
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
      ImGui_ImplGlfw_Sleep(10);
      continue;
    }
    // F8: focus coming back is the moment to ask whether the project moved.
    // It is the shape of the workflow — switch to a terminal, run a git
    // command, switch back — and it costs nothing while you are working.
    g_App.windowFocused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;

    // Update application
    static float lastTime = 0.0f;
    float currentTime = (float)glfwGetTime();
    float deltaTime = currentTime - lastTime;
    lastTime = currentTime;

    if (g_Camera)
      g_Camera->Update(deltaTime);

    // Remote-control requests act on the document/UI before this frame
    const size_t remoteActivity = remoteServer.ProcessPending(g_App);

    // The PROJECT, not the open tab (F10): this used to be handed
    // g_App.document, so a multi-document project autosaved one part and an
    // assembly or drawing tab — where that pointer is null — autosaved
    // nothing at all.
    ui::AutosaveProject(g_App, autosaver, glfwGetTime());
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
    // Sampled here, immediately before the sync that consumes it, so the
    // value is exactly what SyncDocumentRenderers used to read inline.
    g_App.anyWidgetActive = ImGui::IsAnyItemActive();
    // Before the sync: it can regenerate, and the projection must see the
    // bodies it settled on rather than last frame's.
    ui::SyncSketchViewRollback(g_App);
    ui::SyncDocumentRenderers(g_App);
    // Rebuild the FEM field overlay (and hide/show bodies) when the study or
    // result changes; cheap no-op otherwise.
    ui::SyncFieldRenderer(g_App);
    ui::TickCamPlayback(g_App, glfwGetTime());
    ui::SyncCamPlayback(g_App);
    ui::TickMotionPlayback(g_App, glfwGetTime());
    // Per frame on purpose: the resyncs are version-gated and
    // picking a rim or ticking a washer bumps no version.
    ui::SyncFastenerGhost(g_App);
    ui::SyncBearingGhost(g_App);

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
    for (SceneRenderer* r : g_Scene)
      r->SetClipPlanes(ui::SectionPlanes(g_App));
    if (g_FieldRenderer)
      g_FieldRenderer->SetDeformScale(g_App.simulation.deformScale);

    // Every long-running operation, from one place (ui/BackgroundJobs.h).
    ui::PollBackgroundJobs(g_App);

    // Mirror the GPU pass averages where frame_stats and the HUD read them.
    {
      if (g_App.gpuStatsReset) {
        g_App.gpuStatsReset = false;
        g_GpuTimer.ResetSamples();
      }
      const render::GpuTimer::Snapshot gs = g_GpuTimer.Average();
      g_App.gpuStats.supported = gs.supported;
      g_App.gpuStats.samples = gs.samples;
      g_App.gpuStats.shadowMs = gs.shadowMs;
      g_App.gpuStats.ssaoMs = gs.ssaoMs;
      g_App.gpuStats.mainMs = gs.mainMs;
      g_App.gpuStats.totalMs = gs.totalMs;
      g_App.gpuStats.shadowP50Ms = gs.shadowP50Ms;
      g_App.gpuStats.ssaoP50Ms = gs.ssaoP50Ms;
      g_App.gpuStats.mainP50Ms = gs.mainP50Ms;
      g_App.gpuStats.totalP50Ms = gs.totalP50Ms;
    }

    // Everything above is per-frame CPU work on the document and renderers;
    // the ImGui build is measured on its own
    // (docs/rendering/frame-pacing.md#the-frame-time-instrument).
    g_App.frameTimer.Mark(render::FramePhase::Update, glfwGetTime());

    // Start ImGui frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ui::DrawUI(g_App);

    // Update camera matrices for 3D rendering
    if (g_Camera)
      for (SceneRenderer* r : g_Scene)
        r->UpdateCamera(g_Camera->GetViewMatrix(),
                        g_Camera->GetProjectionMatrix());

    // Rendering
    ImGui::Render();
    g_App.frameTimer.Mark(render::FramePhase::BuildUI, glfwGetTime());
    g_App.frameTimer.SetWorkCpuMs(ThreadCpuMs() - frameCpuStartMs);
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
        const double minFrame =
            std::max(1.0 / 60.0, args.guiTestMinFrameMs / 1000.0);
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
        if (g_OutlineRenderer)
          g_OutlineRenderer->SettleSilhouettes();
        const bool ok = CaptureScreenshot(
            g_Device, g_PhysicalDevice, g_Queue, g_CommandPool, wd->RenderPass,
            wd->SurfaceFormat.format, static_cast<uint32_t>(wd->Width),
            static_cast<uint32_t>(wd->Height), wd->ClearValue,
            [&](VkCommandBuffer cb) {
              for (SceneRenderer* r : g_Scene)
                r->Render(cb, wd->FrameIndex);
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

    // Idle-throttle verdict for the NEXT iteration, from THIS one's state
    // (docs/rendering/frame-pacing.md#idle-throttle). Input covers every drag
    // (a button is down); the rest are the ways the picture changes without
    // input.
    {
      const ImGuiIO& io = ImGui::GetIO();
      bool keyHeld = false;
      for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(k))) {
          keyHeld = true;
          break;
        }
      }
      app::PacerInputs pin;
      pin.enabled = g_App.settings.idleThrottle;
      pin.testMode = args.runGuiTests;
      pin.forceEnable = g_App.pacerForceEnable;
      pin.inputActivity = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
                          io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
                          ImGui::IsAnyMouseDown() || keyHeld;
      pin.wantTextInput = io.WantTextInput;
      pin.cameraAnimating = g_Camera && g_Camera->IsAnimating();
      pin.simPlaying = g_App.camPlaying || g_App.motionPlaying;
      pin.jobRunning = !g_App.backgroundJobs.empty() || g_App.renderJob.active;
      pin.remoteRequests = static_cast<int>(remoteActivity);
      pin.documentVersion = g_App.assembly   ? g_App.assembly->Version()
                            : g_App.drawing  ? g_App.drawing->Version()
                            : g_App.document ? g_App.document->Version()
                                             : 0;
      const double nowPace = glfwGetTime();
      paceIdle = s_pacer.IdleNext(pin, nowPace);
      g_App.pacerIdleFrames = s_pacer.IdleFrames();
      g_App.pacerWake = app::FramePacer::WakeName(s_pacer.LastWake());
      g_App.pacerWakeAgeS = s_pacer.LastWakeAge(nowPace);
    }

#ifdef HAVE_TEST_ENGINE
    if (args.runGuiTests && ImGuiTestEngine_IsTestQueueEmpty(testEngine)) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
#endif
  }

  // Never orphan a child process: the panel's session dies with the app.
  ui::ShutdownTerminal(g_App);
  remoteServer.Stop();

  // Clean exit with nothing unsaved: the sidecar has no recovery value.
  // A dirty exit deliberately KEEPS it (safety over tidiness).
  if (g_App.document && !ui::ProjectDirty(g_App)) {
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
  g_Camera.reset();
  g_GpuTimer.Shutdown(g_Device);
  // Must reset here (not at static-destruction time): ~UnderlayRenderer ->
  // Shutdown() calls vkDeviceWaitIdle, so it has to run while g_Device is still
  // alive — before CleanupVulkan below. Omitting it left the global to destruct
  // at exit on an already-freed device -> SIGSEGV at shutdown.
  // The per-part render cache holds RETAINED asset ids in the mesh and
  // outline renderers; releasing them after the renderers are gone would free
  // nothing and leave the buffers to a dead device. Before, not after.
  ui::ReleasePartRenderCache(g_App);
  ShutdownSceneRenderers();
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
