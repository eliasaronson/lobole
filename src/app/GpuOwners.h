// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The graphics device and everything made on it, as ONE table of owners
// (docs/rendering/README.md#rebuilding-the-device): rows of {name, create,
// destroy} in creation order, walked forward to build and backward to tear
// down. The start walks it, the exit walks it back, and a rebuild in place --
// after VK_ERROR_DEVICE_LOST, or a gpu_device preference that moves the first
// rung of the device ladder -- is the two walks back to back, so the rebuild
// path IS the start path every launch exercises.
//
// tests/test_conventions.cpp holds the table to that: every file that makes a
// Vulkan object is a row's or a named transient user, every scene renderer
// has a row, and nothing else asks for a rebuild.
//
// The handles keep the names main.cpp always gave them; main.cpp's frame
// loop, captures and swapchain recreate read them from here.

#include "app/StartupReport.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "render/GpuTimer.h"

#include <vulkan/vulkan.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct GLFWwindow;
class MeshRenderer;
class OutlineRenderer;
class SketchRenderer;
class UnderlayRenderer;
class FieldRenderer;
class StockRenderer;
class SceneRenderer;
class RayTracingRenderer;
class ShadowRenderer;
class SSAORenderer;
class FaceSelector;
class IBLTextures;

namespace ui {
struct AppState;
}

// --- the handles ------------------------------------------------------------

extern VkAllocationCallbacks* g_Allocator;
extern VkInstance g_Instance;
extern VkPhysicalDevice g_PhysicalDevice;
extern VkDevice g_Device;
extern uint32_t g_QueueFamily;
extern VkQueue g_Queue;
extern VkPipelineCache g_PipelineCache;
extern VkDescriptorPool g_DescriptorPool;
extern VkCommandPool g_CommandPool;

// The window's surface, swapchain, frames and (ours) depth render pass.
extern ImGui_ImplVulkanH_Window g_MainWindowData;
extern uint32_t g_MinImageCount;
extern bool g_DisableVSync; // GUI tests: uncapped present
extern VkSampleCountFlagBits g_MSAASamples;
// --offscreen's render pass, which its renderers are built against.
extern VkRenderPass g_OffscreenPass;
inline constexpr uint32_t kOffscreenWidth = 1280, kOffscreenHeight = 720;
inline constexpr VkFormat kOffscreenFormat = VK_FORMAT_B8G8R8A8_UNORM;

extern std::unique_ptr<MeshRenderer> g_MeshRenderer;
extern std::unique_ptr<OutlineRenderer> g_OutlineRenderer;
extern std::unique_ptr<SketchRenderer> g_SketchRenderer;
extern std::unique_ptr<UnderlayRenderer> g_UnderlayRenderer;
extern std::unique_ptr<FieldRenderer> g_FieldRenderer;
extern std::unique_ptr<StockRenderer> g_StockRenderer;
// The scene renderers in DRAW ORDER (RegisterSceneRenderers).
extern std::vector<SceneRenderer*> g_Scene;
extern std::unique_ptr<RayTracingRenderer> g_RayTracingRenderer;
extern std::unique_ptr<ShadowRenderer> g_ShadowRenderer;
extern std::unique_ptr<SSAORenderer> g_SSAORenderer;
extern render::GpuTimer g_GpuTimer;
extern std::unique_ptr<FaceSelector> g_FaceSelector;
extern std::unique_ptr<IBLTextures> g_IBLTextures;

// LOBOLE_RENDERER and LOBOLE_FAULT, read once at start by main().
extern startup::RendererChoice g_RendererChoice;
extern startup::Fault g_Fault;

// ImGui's Vulkan calls report through check_vk_result, which aborts on an
// error. While a swapchain recreate or a rebuild runs, the first error is
// kept here instead, and the caller decides.
extern bool g_CollectVkErrors;
extern VkResult g_CollectedVkError;
void check_vk_result(VkResult err);

// A start-up failure: a StartupError naming the step (and the result), which
// main() turns into the report and exit code 3, and a rebuild into the
// backstop.
void CheckStartup(const char* step, VkResult r, const std::string& detail = "");
[[noreturn]] void FailStartup(const char* step, const std::string& detail,
                              const std::string& result = "");

// Overrides ImGui's colour-only render pass and framebuffers with ours
// (colour + depth + MSAA resolve). After every swapchain (re)create.
void SetupDepthBuffer(ImGui_ImplVulkanH_Window* wd);
// The window's surface again after VK_ERROR_SURFACE_LOST_KHR.
bool RebuildSurface(GLFWwindow* window);
// The procedural sky's maps for a sun (azimuth, elevation) degrees.
IBLTextures* SkyEnvironment(double azimuthDeg, double elevationDeg);

namespace gpuowners {

// What a build needs that the table does not own: the window (null =
// --offscreen), the instance extensions GLFW asks for, safe mode (no ray
// tracing), and the state the renderers are announced in.
struct Start {
  GLFWwindow* window = nullptr;
  std::vector<const char*> instanceExtensions;
  bool safeMode = false;
  ui::AppState* app = nullptr;
};

// Autosave every dirty document NOW, whatever the interval says (and even
// with autosave off): a rebuild does it before anything is torn down, and the
// lost state does it for the copy a restart recovers from. Returns how many.
int FlushAutosaveNow(ui::AppState& app);

// The start: the instance, then the table forward along the device ladder.
// Throws startup::StartupError. Points `app` at the renderers it made.
void Build(const Start& start);
// Every row made, backward, then the instance: the exit. Safe after a failed
// rebuild, which made nothing.
void Teardown();

// The ImGui backend's row creates nothing outside a frame boundary, and
// main.cpp's frame loop is where that is. These are the hooks the boundary
// takes from the loop that calls it.
struct Boundary {
  // The framebuffer has an extent: a minimised window cannot make a
  // swapchain, so a rebuild waits for it.
  bool canBuild = true;
  // A capture the test engine is taking: a preference switch waits for it.
  bool capturing = false;
  // Applied after every build -- the start's and each rebuild's -- by the
  // code that applies it at start-up: the renderers' viewport sizes from the
  // framebuffer, the loop's own swapchain flags.
  std::function<void()> afterBuild;
};

// The ImGui test engine restores a test's whole ImGuiIO when the test ends
// (ImGuiTestContextUiContextBackup) -- the backends' user-data pointers with
// it. A rebuild during a test (a GUI test of it, a remote ui_action) has
// freed those, and the next ImGui_ImplVulkan_RenderDrawData dereferences the
// old device. Called after ImGui::Render() and before the frame records,
// and before the platform's NewFrame: points them at the backends the last
// build made, and logs when it had to.
void KeepImGuiBackends();

// What the boundary did.
enum class Outcome { Nothing, Rebuilt, Backstop };

// At the top of a frame (windowed: after the previous present, before
// glfwPollEvents and ImGui_ImplVulkan_NewFrame; offscreen: before the tick's
// requests), and nowhere else: a lost device is rebuilt in place or ends at
// the backstop, and a changed gpu_device preference that moves the first
// rung rebuilds onto it (waiting while a render, a motion video or a capture
// holds the device). `now` is the loop's clock in seconds.
Outcome AtFrameBoundary(ui::AppState& app, const Boundary& b, double now);

} // namespace gpuowners
