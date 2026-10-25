// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// CAD Application with working Vulkan + ImGui setup
// Based on Dear ImGui GLFW+Vulkan example

#include "app/CrashHandler.h"
#include "app/FramePacer.h"
#include "app/GpuOwners.h"
#include "app/GuiTests.h"
#include "app/HeadlessMain.h"
#include "app/ParentWatch.h"
#include "app/StartupReport.h"
#include "cad/FaceSelector.h"
#include "common/DesignerVersion.h"
#include "common/DevicePreference.h"
#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "common/Watchdog.h"
#include "core/io/Autosaver.h"
#include "core/io/CrashReports.h"
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
#include "ui/PrintProfileActions.h"
#include "ui/ProjectActions.h"
#include "ui/RendererSync.h"
#include "ui/SelectionPipeline.h"
#include "ui/SettingsWindow.h"
#include "ui/SimulationActions.h"
#include "ui/SketchOverlay.h" // SyncUnderlays
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

// The device, its swapchain and every renderer are rows of the owner table
// (app/GpuOwners.h; docs/rendering/README.md#rebuilding-the-device), built by
// the start, torn down by the exit, and walked back and forth by a rebuild in
// place. What is left here is the frame: the loop, its captures, its
// swapchain recreate.
static bool g_SwapChainRebuild = false;

// Application objects
static std::unique_ptr<Camera> g_Camera;

// All state the UI layer works with (owns the document + undo stack; holds
// non-owning pointers to the objects above once they exist)
static ui::AppState g_App;

// GLFW's last complaint, kept so a startup failure can quote it: glfwInit
// and glfwCreateWindow say why only through this callback.
static std::string g_GlfwLastError;
static void glfw_error_callback(int error, const char* description) {
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
  g_GlfwLastError = "GLFW said: " + std::string(description) + " (error " +
                    std::to_string(error) + ")";
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

// The model's renderers only: the sketch and CAM stock renderers carry the
// overlays (sketches, datums, gizmos, handles). What a quick render and a
// motion video draw. The underlay renderer is here for the show-always
// reference images (v166) — AimRenderersAt hands it only those — drawn after
// the model so the depth test hides what is behind it, and under the edges.
static void RecordModel(VkCommandBuffer cb, uint32_t frame) {
  g_MeshRenderer->Render(cb, frame);
  if (g_FieldRenderer)
    g_FieldRenderer->Render(cb, frame);
  if (g_UnderlayRenderer)
    g_UnderlayRenderer->Render(cb, frame);
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
  // The underlays a capture shows: the show-always ones, and not the edited
  // sketch's (a capture draws no sketch). The frame loop's list is the only
  // other thing that fills the renderer, and offscreen there is no frame
  // loop, so this is where a quick still gets them at all.
  ui::SyncUnderlays(g_App, /*withEditedSketch=*/false);
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

// The frame loop's Vulkan results (docs/rendering/README.md#a-lost-device):
// a lost device is a state, a lost surface is rebuilt, and anything else
// still aborts through check_vk_result — a crash report, with the result's
// name. False when the frame cannot go on.
static bool g_SurfaceLost = false;
static bool FrameOk(VkResult err, const char* where) {
  if (err >= 0)
    return true;
  if (err == VK_ERROR_DEVICE_LOST) {
    ui::MarkDeviceLost(g_App, where);
    return false;
  }
  if (err == VK_ERROR_SURFACE_LOST_KHR) {
    LOG_WARN("Rendering: the window surface was lost ({}); rebuilding it",
             where);
    g_SurfaceLost = true;
    g_SwapChainRebuild = true;
    return false;
  }
  check_vk_result(err);
  return false;
}

// The renderers sized to a framebuffer of w x h: after a swapchain recreate,
// and after every build of the owner table (the start's and each rebuild's).
// SetViewportSize is a DIFFERENT question from the scene viewport rect (which
// UI.cpp sets from the sub-region between the panels every frame) and only
// some renderers have it.
static void ApplyFramebufferSize(int fb_width, int fb_height) {
  if (g_MeshRenderer)
    g_MeshRenderer->SetViewportSize(fb_width, fb_height);
  // SSAO targets are window-sized: recreate (waits for idle) and rebind the
  // new AO view into the mesh renderer's descriptor sets.
  if (g_SSAORenderer) {
    g_SSAORenderer->Resize((uint32_t)fb_width, (uint32_t)fb_height);
    if (g_MeshRenderer)
      g_MeshRenderer->RebindAOMap(g_SSAORenderer->AOView());
  }
  if (g_OutlineRenderer)
    g_OutlineRenderer->SetViewportSize(fb_width, fb_height);
  if (g_SketchRenderer)
    g_SketchRenderer->SetViewportSize(fb_width, fb_height);
  if (g_UnderlayRenderer)
    g_UnderlayRenderer->SetViewportSize(fb_width, fb_height);
  for (SceneRenderer* r : g_Scene)
    r->SetViewport(0, 0, fb_width, fb_height);
  if (g_Camera)
    g_Camera->SetAspectRatio(static_cast<float>(fb_width) /
                             static_cast<float>(fb_height));
}

// The GPU waits take a finite timeout, in a loop under a watchdog label, so
// a GPU that hangs without resetting is reported as "main thread stalled
// (in waiting on the GPU: frame fence)" instead of a stack deep in a driver
// (docs/architecture.md#crashes-and-stalls). A loss ends the wait.
static constexpr uint64_t kGpuWaitSliceNs = 100'000'000; // 0.1 s

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
  VkSemaphore image_acquired_semaphore =
      wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore render_complete_semaphore =
      wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
  VkResult err;
  {
    watchdog::Label label("waiting on the GPU: swapchain image");
    do {
      err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, kGpuWaitSliceNs,
                                  image_acquired_semaphore, VK_NULL_HANDLE,
                                  &wd->FrameIndex);
    } while (err == VK_TIMEOUT || err == VK_NOT_READY);
  }
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    g_SwapChainRebuild = true;
    return;
  }
  if (!FrameOk(err, "acquiring a swapchain image"))
    return;

  ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
  {
    {
      watchdog::Label label("waiting on the GPU: frame fence");
      do {
        err =
            vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, kGpuWaitSliceNs);
      } while (err == VK_TIMEOUT);
    }
    if (!FrameOk(err, "at the frame fence"))
      return;

    err = vkResetFences(g_Device, 1, &fd->Fence);
    if (!FrameOk(err, "resetting the frame fence"))
      return;
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
    if (!FrameOk(err, "resetting the frame's command pool"))
      return;
    VkCommandBufferBeginInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
    if (!FrameOk(err, "beginning the frame's commands"))
      return;
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
    if (!FrameOk(err, "ending the frame's commands"))
      return;
    err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
    if (!FrameOk(err, "submitting the frame"))
      return;
  }
  g_App.frameTimer.Mark(render::FramePhase::Record, glfwGetTime());
}

// True when the frame reached the screen.
static bool FramePresent(ImGui_ImplVulkanH_Window* wd) {
  if (g_SwapChainRebuild || g_App.renderer.lost)
    return false;
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
    return false;
  }
  if (!FrameOk(err, "presenting the frame"))
    return false;
  wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
  return true;
}

#ifdef HAVE_TEST_ENGINE
// The test engine's screen capture: its screenshots and the videos a demo
// scene records (docs/ui/testing.md#recording-a-scene). The engine asks for
// a rect of the frame just presented, from inside ImGuiTestEngine_PostSwap;
// this re-renders that frame -- the scene AND the interface, from the same
// draw data -- into a readable image through CaptureScreenshot, the remote
// screenshot's path. The swapchain itself is never read: its images are not
// created for transfer, and under Xvfb the driver presents them without
// passing through the X framebuffer, so nothing outside the app could read
// them either. The engine calls this only while it captures, so an ordinary
// frame pays nothing.
static ImDrawData* s_CaptureDrawData = nullptr; // set only across PostSwap
static int s_CaptureFrame = -1;
static std::vector<uint8_t> s_CaptureRgb;
static uint32_t s_CaptureW = 0;
static uint32_t s_CaptureH = 0;

static bool CaptureForTestEngine(ImGuiID /*viewport*/, int x, int y, int w,
                                 int h, unsigned int* pixels, void* /*user*/) {
  if (s_CaptureDrawData == nullptr || w <= 0 || h <= 0)
    return false;
  ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
  // One render per frame, however many rects the engine asks for in it.
  if (ImGui::GetFrameCount() != s_CaptureFrame) {
    vkQueueWaitIdle(g_Queue); // the frame's resources must be reusable
    if (g_OutlineRenderer)
      g_OutlineRenderer->SettleSilhouettes();
    s_CaptureW = static_cast<uint32_t>(wd->Width);
    s_CaptureH = static_cast<uint32_t>(wd->Height);
    s_CaptureRgb.clear();
    std::string error;
    ImDrawData* drawData = s_CaptureDrawData;
    if (!CaptureScreenshot(
            g_Device, g_PhysicalDevice, g_Queue, g_CommandPool, wd->RenderPass,
            wd->SurfaceFormat.format, s_CaptureW, s_CaptureH, wd->ClearValue,
            [&](VkCommandBuffer cb) {
              for (SceneRenderer* r : g_Scene)
                r->Render(cb, wd->FrameIndex);
              ImGui_ImplVulkan_RenderDrawData(drawData, cb);
            },
            /*outputPath=*/"", &error, g_MSAASamples,
            [&](VkCommandBuffer cb) {
              RecordScenePrepasses(cb, wd->FrameIndex);
            },
            nullptr, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1, {}, &s_CaptureRgb)) {
      LOG_ERROR("test engine capture: {}", error);
      return false;
    }
    s_CaptureFrame = ImGui::GetFrameCount();
  }
  // The engine's rect is in ImGui units; the frame is in framebuffer pixels.
  const ImVec2 k = ImGui::GetIO().DisplayFramebufferScale;
  for (int row = 0; row < h; row++) {
    const int fy = static_cast<int>((y + row) * k.y);
    for (int col = 0; col < w; col++) {
      const int fx = static_cast<int>((x + col) * k.x);
      unsigned char* out =
          reinterpret_cast<unsigned char*>(&pixels[size_t(row) * w + col]);
      if (fx < 0 || fy < 0 || fx >= int(s_CaptureW) || fy >= int(s_CaptureH)) {
        out[0] = out[1] = out[2] = 0;
      } else {
        const uint8_t* in = &s_CaptureRgb[(size_t(fy) * s_CaptureW + fx) * 3];
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
      }
      out[3] = 255; // RGBA in memory: what the engine pipes to ffmpeg
    }
  }
  return true;
}
#endif

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
  std::cout << "  --version            Show the Lobole version" << std::endl;
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
         "than this after it started (starved workers; GUI tests and "
         "--headless)"
      << std::endl;
  std::cout << "  --gui-test-speed <fast|normal|cinematic>  How the GUI tests "
               "move the mouse and pause (cinematic: for recording)"
            << std::endl;
  std::cout << "  --window-size <WxH>  Open the window at this size (e.g. "
               "1920x1080 for a recording)"
            << std::endl;
  std::cout << "  --ui-scale <s>       Scale the interface by s, over the "
               "display's (e.g. 2 at 3840x2160)"
            << std::endl;
  std::cout << "  --capture-dir <dir>  Demo scenes record their clips here "
               "(needs ffmpeg on PATH)"
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
               "LOBOLE_EXIT_WITH_PARENT=<launcher pid> does the same"
            << std::endl;
  std::cout << "  --perf-hud           Show the frame-time overlay from "
               "launch (also Ctrl+Shift+F, or the\n                       "
               "frame_stats RPC; off by default)"
            << std::endl;
  std::cout << "  --safe-mode          Start the window on default settings "
               "and the default layout,\n                       without ray "
               "tracing or opening a file; writes neither\n                  "
               "     settings.json nor imgui.ini (LOBOLE_SAFE_MODE=1 too)"
            << std::endl;
  std::cout << "  --after-device-loss  (Given by the restart after a lost "
               "graphics device: offers\n                       that "
               "session's unsaved work)"
            << std::endl;
  std::cout << "Environment:\n"
               "  LOBOLE_RENDERER=software|hardware  Which kind of graphics "
               "device to use\n"
               "  LOBOLE_NO_DIALOG=1   Never show a native message box (tests, "
               "CI)\n";
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
#ifdef HAVE_TEST_ENGINE
  // --gui-test-speed: the engine's own speeds. Cinematic moves the mouse
  // visibly and pauses between actions, for a scene that is being recorded.
  ImGuiTestRunSpeed guiTestSpeed = ImGuiTestRunSpeed_Fast;
#endif
  std::string captureDir; // --capture-dir (GuiTestCaptureDir)
  // --window-size WxH: the window's size, over the remembered one and the
  // GUI suite's fixed 1280x720. Only a run that asks gets it -- a demo
  // recording at 1920x1080 -- never the suite, whose layout tests assert
  // what fits at the default.
  int windowW = 0;
  int windowH = 0;
  // --ui-scale S: the interface's scale, over what the display reports. A
  // recording at 3840x2160 with 2 is a 1920x1080 layout drawn at HiDPI
  // density; Xvfb reports 1.0, so without it a recording looked coarser than
  // the app on the developer's own scaled display. 0 = the display's.
  float uiScale = 0.0f;
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
  bool safeMode = false;        // --safe-mode
  bool afterDeviceLoss = false; // --after-device-loss
  // --crash-test: crashes once the session marker exists, so the next start
  // collects it like any other crash (and a windowed one counts towards
  // safe mode: it died before its first frame).
  bool crashTest = false;
};

CommandLineArgs parseCommandLineArgs(int argc, char** argv) {
  CommandLineArgs args;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      exit(0);
    } else if (arg == "--version") {
      std::cout << "Lobole " << LOBOLE_VERSION << std::endl;
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
    } else if (arg == "--gui-test-speed") {
      const std::string speed = i + 1 < argc ? argv[++i] : "";
#ifdef HAVE_TEST_ENGINE
      if (speed == "fast")
        args.guiTestSpeed = ImGuiTestRunSpeed_Fast;
      else if (speed == "normal")
        args.guiTestSpeed = ImGuiTestRunSpeed_Normal;
      else if (speed == "cinematic")
        args.guiTestSpeed = ImGuiTestRunSpeed_Cinematic;
      else {
        std::cerr << "--gui-test-speed takes fast, normal or cinematic, not '"
                  << speed << "'" << std::endl;
        exit(1);
      }
#else
      (void)speed;
#endif
    } else if (arg == "--window-size") {
      const std::string size = i + 1 < argc ? argv[++i] : "";
      int w = 0, h = 0;
      char tail = 0;
      if (std::sscanf(size.c_str(), "%dx%d%c", &w, &h, &tail) != 2 || w < 320 ||
          h < 240 || w > 7680 || h > 4320) {
        std::cerr << "--window-size takes WIDTHxHEIGHT in pixels, 320x240 "
                     "to 7680x4320, not '"
                  << size << "'" << std::endl;
        exit(1);
      }
      args.windowW = w;
      args.windowH = h;
    } else if (arg == "--ui-scale") {
      char* end = nullptr;
      const double s = i + 1 < argc ? std::strtod(argv[i + 1], &end) : 0.0;
      if (end == nullptr || *end != '\0' || !(s >= 0.5 && s <= 4.0)) {
        std::cerr << "--ui-scale takes a scale from 0.5 to 4" << std::endl;
        exit(1);
      }
      args.uiScale = static_cast<float>(s);
      ++i;
    } else if (arg == "--capture-dir") {
      if (i + 1 >= argc) {
        std::cerr << "--capture-dir requires a directory" << std::endl;
        exit(1);
      }
      args.captureDir = argv[++i];
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
      // sanity). Installed handler prints the stack, then core-dumps. Not
      // here but once the session has begun, so it leaves a report.
      args.crashTest = true;
    } else if (arg == "--safe-mode") {
      args.safeMode = true;
    } else if (arg == "--after-device-loss") {
      args.afterDeviceLoss = true;
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
// Lives here rather than beside RunHeadless because the frame it fulfils
// captures in -- the capture paths, RecordScenePrepasses -- is this
// translation unit's; the device and renderers are the owner table's
// (app/GpuOwners.h), as the window's are.

// What the backstop does at once, in any mode: the work saved for recovery, and
// a report folder. Returns the report's folder, or empty.
static std::filesystem::path SaveWorkAfterDeviceLoss(int* autosaved) {
  *autosaved = gpuowners::FlushAutosaveNow(g_App);
  LOG_WARN("Rendering: device lost; {} document(s) autosaved for recovery",
           *autosaved);
  const std::optional<crash::Report> report = crash::ReportThisSession(
      std::string(crash::kDeviceLostReason) + ": " + g_App.renderer.lostReason);
  return report ? report->dir : std::filesystem::path();
}

static std::string DeviceLostRefusal() {
  return "device_lost: the graphics device was lost (" +
         g_App.renderer.lostReason + "); nothing renders until Loböle restarts";
}

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
  if (g_FieldRenderer)
    g_FieldRenderer->SetDeformScale(g_App.simulation.deformScale);
}

static std::atomic<bool> g_OffscreenStopRequested{false};
static void HandleOffscreenStop(int) { g_OffscreenStopRequested = true; }

static int RunOffscreen(const CommandLineArgs& args) {
  LOG_INFO("Offscreen mode: real renderer, no window or display server");

  // The owner table (app/GpuOwners.h): the device and the raster stack at
  // the fixed 1280x720 the hidden/gui-test carve-out also uses (a
  // host-derived size would make captures depend on whose machine rendered
  // them), against --offscreen's own render pass. The GPU timer is why this
  // mode is worth measuring in: with no swapchain there is no image to wait
  // for, so the main pass reports shading and nothing else
  // (docs/rendering/frame-pacing.md#measuring-a-pass-offscreen).
  {
    gpuowners::Start start;
    start.app = &g_App;
    gpuowners::Build(start);
  }
  RefreshCrashModules(); // the Vulkan driver is loaded now

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
  constexpr uint32_t kOffW = kOffscreenWidth, kOffH = kOffscreenHeight;
  constexpr VkFormat kOffFormat = kOffscreenFormat;
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
          g_Device, g_PhysicalDevice, g_Queue, g_CommandPool, g_OffscreenPass,
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
    t.pass = g_OffscreenPass;
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
  bool lossHandled = false;
  while (!g_OffscreenStopRequested) {
    watchdog::Beat();
    crash::NoteProject(g_App.projectPath);
    // The frame boundary, before the tick's requests
    // (docs/rendering/README.md#rebuilding-the-device): a lost device is
    // rebuilt in place, and a gpu_device change that moves the first rung
    // rebuilds onto it. The renderers are then projected again lazily, as
    // after any edit: RenderMeshesCurrent answers false.
    if (vkutil::DeviceLostSeen())
      ui::MarkDeviceLost(g_App, "in a one-shot submit");
    gpuowners::AtFrameBoundary(g_App, {}, seconds());
    server.ProcessPending(g_App);
    if (vkutil::DeviceLostSeen())
      ui::MarkDeviceLost(g_App, "in a one-shot submit");
    // The backstop (docs/rendering/README.md#a-lost-device): there is no
    // window to restart, so this mode saves the work, writes the report, and
    // serves on with every capture refused; ping says the renderer is lost.
    if (g_App.renderer.backstop && !lossHandled) {
      lossHandled = true;
      int autosaved = 0;
      SaveWorkAfterDeviceLoss(&autosaved);
      LOG_ERROR("Offscreen: the graphics device was lost; serving on without "
                "renders (every capture is refused as device_lost)");
    }
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
    if (g_App.renderer.lost) {
      // Lost and about to be rebuilt: the capture waits for the new
      // renderers at the next boundary. Lost for good: refused.
      if (g_App.renderer.backstop && server.WantsScreenshot(shotPath))
        server.ScreenshotDone(false, DeviceLostRefusal());
    } else if ((animatingSince < 0 || now - animatingSince > 5.0) &&
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
          g_Device, g_PhysicalDevice, g_Queue, g_CommandPool, g_OffscreenPass,
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
    // Up to 2 ms for jobs and autosave; a request ends it at once.
    server.WaitForRequest(std::chrono::milliseconds(2));
  }

  LOG_INFO("Offscreen: stop requested, shutting down");
  server.Stop();
  // Clean exit with nothing unsaved: drop the sidecar. Dirty exits keep it.
  if (g_App.document && !ui::ProjectDirty(g_App)) {
    Autosaver::Clear(g_App.projectPath);
  }

  g_App.document.reset();
  g_App.commands.reset();
  g_Camera.reset();
  // The per-part render cache holds RETAINED asset ids in the mesh and
  // outline renderers; releasing them after the renderers are gone would free
  // nothing and leave the buffers to a dead device. Before, not after. Then
  // the owner table, backward: the same walk a rebuild starts with.
  ui::ReleasePartRenderCache(g_App);
  gpuowners::Teardown();
  return 0;
}

static int RunWindowed(CommandLineArgs& args, int argc, char** argv);

// A start that found no renderer: said everywhere it can be read
// (startup::ReportStartupFailure), the session ended as a clean exit ends it
// — a start that explained itself has not crashed — and exit code 3. _Exit,
// not a return: what was half built (a window, a device, renderers) is not
// torn down by destructors that expect it whole.
[[noreturn]] static void EndStartup(const startup::StartupError& e,
                                    bool windowed) {
  const crash::Session* session = crash::CurrentSession();
  const int code = startup::ReportStartupFailure(
      e.failure(), RuntimePaths::UserStateDir(),
      session ? session->logFile.string() : std::string(), windowed);
  crash::EndSession();
  watchdog::Stop();
  std::fflush(nullptr);
  std::_Exit(code);
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
  // The rest of the environment this start reads, refused the same way: a
  // value that is not one of the accepted ones is said, never read as the
  // nearest (app/StartupReport.h).
  bool safeModeFromEnv = false;
  {
    const auto env = [](const char* name) {
      const char* v = std::getenv(name);
      return std::string_view(v ? v : "");
    };
    std::string error;
    bool noDialog = false;
    const char* allow = std::getenv("LOBOLE_ALLOW_DEBUG_CRASH");
    if (!startup::RendererChoiceFromString(env("LOBOLE_RENDERER"),
                                           &g_RendererChoice, &error) ||
        !startup::ZeroOneFromString("LOBOLE_SAFE_MODE", env("LOBOLE_SAFE_MODE"),
                                    &safeModeFromEnv, &error) ||
        !startup::ZeroOneFromString("LOBOLE_NO_DIALOG", env("LOBOLE_NO_DIALOG"),
                                    &noDialog, &error) ||
        !startup::FaultFromString(env("LOBOLE_FAULT"),
                                  allow != nullptr &&
                                      std::string_view(allow) == "1",
                                  &g_Fault, &error)) {
      std::cerr << "Error: " << error << std::endl;
      return 2;
    }
  }
  // The GUI suite is never watched by anyone who could click a box.
  if (args.runGuiTests) {
#if defined(_WIN32)
    _putenv_s("LOBOLE_NO_DIALOG", "1");
#else
    setenv("LOBOLE_NO_DIALOG", "1", 1);
#endif
  }
  // Safe mode is the WINDOW on defaults; the other modes have no layout, and
  // the GUI tests already run on defaults. Refused rather than ignored.
  if (args.safeMode && (args.headless || args.offscreen || args.runGuiTests)) {
    std::cerr << "Error: --safe-mode starts the window on defaults; it does "
                 "not run with --headless, --offscreen or --gui-tests"
              << std::endl;
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

  // This run's session (core/io/CrashReports.h): the log file, and a locked
  // marker that only a run which ended badly leaves behind. Then every such
  // marker an earlier run left becomes a report folder, before the first
  // frame and in every mode, so a headless crash is collected by the next
  // headless start too. Not under the GUI tests, which run on defaults: what
  // an earlier run left must not decide what a test sees (their marker is
  // still made, so a crashing suite leaves its report).
  const char* sessionMode = args.runGuiTests ? "gui-tests"
                            : args.offscreen ? "offscreen"
                            : args.headless  ? "headless"
                                             : "windowed";
  struct SessionEnd {
    ~SessionEnd() { crash::EndSession(); } // a clean exit leaves no marker
  } sessionEnd;
  const bool windowed = !args.runGuiTests && !args.headless && !args.offscreen;
  if (auto session =
          crash::BeginSession(RuntimePaths::UserStateDir(), sessionMode)) {
    SetCrashReportFiles(session->crashFile, session->dumpFile, session->marker);
    // --crash-test: once there is a marker to leave behind, and before the
    // first frame, so a windowed one counts towards safe mode.
    if (args.crashTest)
      CrashHandlerSelfTest();
    if (!args.runGuiTests) {
      std::vector<crash::Report> reports =
          crash::CollectDeadSessions(session->stateDir);
      // The windowed app says so; the others have the log line and the
      // crash_reports RPC.
      if (!reports.empty() && windowed)
        crash::SetNotice(std::move(reports.front()));
    }
    if (windowed) {
      // A restart after a lost device: the report that session wrote is the
      // notice, with its autosaves to recover
      // (docs/rendering/README.md#a-lost-device).
      if (args.afterDeviceLoss) {
        for (crash::Report& r : crash::ListReports(session->stateDir))
          if (r.reason.rfind(crash::kDeviceLostReason, 0) == 0) {
            LOG_INFO("Restarted after a lost graphics device ({})", r.reason);
            crash::SetNotice(std::move(r));
            break;
          }
      }
      // The last start found no renderer: said once by the next window, as
      // long as no window has drawn since (the stamp NoteFirstFrame writes).
      namespace fs = std::filesystem;
      std::error_code ec;
      const fs::path failure = startup::FailureFile(session->stateDir);
      const fs::path good = crash::LastGoodStartFile(session->stateDir);
      if (fs::exists(failure, ec) &&
          (!fs::exists(good, ec) ||
           fs::last_write_time(failure, ec) > fs::last_write_time(good, ec))) {
        LOG_WARN("The last start found no renderer ({}); this window says so "
                 "once",
                 failure.string());
        std::ifstream in(failure, std::ios::binary);
        crash::SetStartupFailureNotice(
            std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>()));
      }
      // Safe mode (docs/ui/settings.md#safe-mode): asked for, or because the
      // last two windowed starts died before they drew.
      const size_t early = crash::EarlyDeathsInARow(session->stateDir);
      if (args.safeMode || safeModeFromEnv || early >= 2) {
        g_App.safeMode.active = true;
        g_App.safeMode.why = args.safeMode ? "it was started with --safe-mode"
                             : safeModeFromEnv
                                 ? "LOBOLE_SAFE_MODE=1 is set"
                                 : "the last " + std::to_string(early) +
                                       " starts crashed before the window drew";
        LOG_WARN("Safe mode: {}", g_App.safeMode.why);
      }
    }
  } else if (windowed && (args.safeMode || safeModeFromEnv)) {
    g_App.safeMode.active = true;
    g_App.safeMode.why = args.safeMode ? "it was started with --safe-mode"
                                       : "LOBOLE_SAFE_MODE=1 is set";
  }

  // User preferences, before anything reads them: the GPU choice is consumed
  // by device selection below, and the font scale by the first theme apply.
  // GUI tests deliberately run on DEFAULTS — otherwise the developer's own
  // config decides whether a test passes, and CI and the desk disagree.
  if (g_App.safeMode.active) {
    // Defaults, and nothing written: the user's own file is what safe mode
    // keeps out of the way, not what it rewrites.
    settings::SetSessionOnly(true);
    g_App.safeMode.settingsPath = settings::ConfigPath();
  } else if (!args.runGuiTests) {
    g_App.settings = settings::Load(&g_App.settingsNote);
    g_App.settingsSaved = g_App.settings;
  }
  // The machine store lives beside the settings and loads with them. Under
  // the GUI tests too: the runner points XDG_CONFIG_HOME at a scratch
  // directory, so the tests see an empty store rather than the desk's.
  ui::ReloadMachines(g_App);
  ui::ReloadPrintProfiles(g_App); // the print profile store, the same way
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
    try {
      return RunOffscreen(args);
    } catch (const startup::StartupError& e) {
      EndStartup(e, /*windowed=*/false);
    }
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
    // The smokes' slow-job worst case: a job they read while it runs is
    // still there a moment later (hostile_params_smoke --self-test).
    ui::SetTestJobLatency(args.guiTestJobLatencyMs / 1000.0);
    return RunHeadless(args.remotePort, args.fileToLoad, args.autosaveSeconds,
                       args.recordPath, args.remoteBindAll);
  }

  try {
    return RunWindowed(args, argc, argv);
  } catch (const startup::StartupError& e) {
    EndStartup(e, /*windowed=*/true);
  }
}

// The windowed app: GLFW, Vulkan with its device ladder, ImGui and the frame
// loop. A step that cannot start throws StartupError, which main() reports.
static int RunWindowed(CommandLineArgs& args, int argc, char** argv) {
#ifdef HAVE_NATIVEFILEDIALOG
  LOG_INFO("nativefiledialog available");
#else
  LOG_INFO("Using ImGui file browser (nativefiledialog not available)");
#endif

  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    FailStartup(startup::step::kGlfwInit, g_GlfwLastError);

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
  // A size given on the command line is the run's, not the user's: it is
  // neither placed by the remembered rect nor saved over it.
  const bool rememberGeometry =
      !args.hidden && !args.runGuiTests && args.windowW == 0;
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
  if (args.windowW > 0) {
    geom.w = args.windowW;
    geom.h = args.windowH;
  }

  GLFWwindow* window = glfwCreateWindow(
      geom.w, geom.h, "Loböle - 3D CAD Software", nullptr, nullptr);
  if (window != nullptr && rememberGeometry) {
    // Position after creation: the hint-based route needs a monitor handle
    // and would make this fullscreen. Wayland refuses positioning outright --
    // the compositor places windows -- so there it is not attempted.
    if (!OnWayland())
      glfwSetWindowPos(window, geom.x, geom.y);
    if (g_App.settings.window.maximized)
      glfwMaximizeWindow(window);
  }
  // A null window used to carry on into Vulkan with nothing to draw in.
  if (window == nullptr)
    FailStartup(startup::step::kWindow, g_GlfwLastError);
  if (!args.hidden)
    ShowWindowNonBlocking(window);

  if (!glfwVulkanSupported())
    FailStartup(
        startup::step::kVulkanLoader,
        "No Vulkan loader could be loaded (libvulkan.so.1, "
        "vulkan-1.dll), or it offers no way to draw in a window." +
            (g_GlfwLastError.empty() ? std::string() : " " + g_GlfwLastError));

  std::vector<const char*> extensions;
  uint32_t extensions_count = 0;
  const char** glfw_extensions =
      glfwGetRequiredInstanceExtensions(&extensions_count);
  for (uint32_t i = 0; i < extensions_count; i++)
    extensions.push_back(glfw_extensions[i]);
  ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;

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
  } else if (g_App.safeMode.active) {
    // Safe mode: the default layout, kept in a scratch file of its own so
    // the user's imgui.ini is neither read (a layout that trips a docking
    // assert is one way a start crashes) nor written.
    const std::filesystem::path scratch =
        RuntimePaths::UserStateDir() / "safe-mode-imgui.ini";
    std::error_code ec;
    std::filesystem::create_directories(scratch.parent_path(), ec);
    std::filesystem::remove(scratch, ec);
    s_iniPath = scratch.string();
    io.IniFilename = s_iniPath.c_str();
    g_App.buildDefaultDockLayout = true;
    g_App.safeMode.layoutPath =
        (std::filesystem::path(::settings::ConfigDir()) / "imgui.ini").string();
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
  static float s_contentScale = args.uiScale > 0.0f
                                    ? args.uiScale
                                    : std::max(1.0f, std::max(scaleX, scaleY));
  static float s_pendingContentScale = 0.0f; // 0 = no change pending
  static bool s_uiScaleFixed = args.uiScale > 0.0f;
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
        if (s_uiScaleFixed)
          return; // --ui-scale: the run's, whatever the display says
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

  // The owner table (app/GpuOwners.h): the instance, the device ladder --
  // each rung makes the window's surface and swapchain on its device, and a
  // rung that fails takes them down again before the next -- then ImGui's
  // GLFW and Vulkan backends and every renderer. After the ImGui context,
  // whose backends are a row; the context itself is never rebuilt.
  {
    gpuowners::Start start;
    start.window = window;
    start.instanceExtensions = extensions;
    start.safeMode = g_App.safeMode.active;
    start.app = &g_App;
    gpuowners::Build(start);
  }
  RefreshCrashModules(); // the Vulkan driver is loaded now

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

  // Load startup file if specified. Not in safe mode, which opens no file:
  // the file may be what crashed the last start.
  if (!args.fileToLoad.empty() && g_App.safeMode.active) {
    LOG_WARN("Safe mode: not opening {}", args.fileToLoad);
  } else if (!args.fileToLoad.empty()) {
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
  testEngineIO.ConfigRunSpeed = args.guiTestSpeed;
  // Capture: screenshots and a demo scene's video. The frames come from
  // CaptureForTestEngine; ffmpeg (from PATH, as export_motion_video finds it)
  // encodes them. Without ffmpeg a scene still runs -- it just cannot record.
  testEngineIO.ScreenCaptureFunc = &CaptureForTestEngine;
  if (const std::string ffmpeg = ui::FfmpegPath(); !ffmpeg.empty())
    std::snprintf(testEngineIO.VideoCaptureEncoderPath,
                  sizeof(testEngineIO.VideoCaptureEncoderPath), "%s",
                  ffmpeg.c_str());
  // The engine's own parameters, at a quality for a video rather than a
  // test log: the default (-preset ultrafast -crf 20) blurred thin interface
  // text. The capture runs on the fixed clock, so a slower encoder only
  // makes the recording take longer.
  std::snprintf(testEngineIO.VideoCaptureEncoderParams,
                sizeof(testEngineIO.VideoCaptureEncoderParams), "%s",
                "-hide_banner -loglevel error -r $FPS -f rawvideo -pix_fmt "
                "rgba -s $WIDTHx$HEIGHT -i - -threads 0 -y -c:v libx264 "
                "-preset medium -crf 14 -pix_fmt yuv420p $OUTPUT");
  SetGuiTestCaptureDir(args.captureDir);
  // The engine moves the mouse and scrolls in pixels a second: at a larger
  // interface scale the same gesture covers proportionally more pixels.
  if (args.uiScale > 0.0f) {
    testEngineIO.MouseSpeed *= args.uiScale;
    testEngineIO.ScrollSpeed *= args.uiScale;
  }
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

  // Starting again: this process's own arguments, less the ones that must
  // not repeat, plus `extra` — after letting go of what the new process
  // will want (the port, the session marker, the terminal's child). The
  // restart after a lost device and safe mode's two buttons both end here.
  const auto relaunch = [&](const std::vector<std::string>& extra) {
    std::vector<std::string> again;
    for (int i = 1; i < argc; i++) {
      const std::string a = argv[i];
      if (a != "--safe-mode" && a != "--after-device-loss" &&
          a != "--crash-test")
        again.push_back(a);
    }
    again.insert(again.end(), extra.begin(), extra.end());
    LOG_INFO("Restarting{}", extra.empty() ? "" : " with " + extra.front());
    ui::ShutdownTerminal(g_App);
    remoteServer.Stop();
    crash::EndSession(); // asked to stop: not a crash
    std::string error;
    if (startup::Relaunch(again, &error)) {
      std::fflush(nullptr);
      std::_Exit(0); // the new process is started (Windows)
    }
    LOG_ERROR("Could not start Loböle again: {}; quitting", error);
    glfwSetWindowShouldClose(window, GLFW_TRUE);
  };
  g_App.restartNormally = [&] {
#if defined(_WIN32)
    _putenv_s("LOBOLE_SAFE_MODE", "");
#else
    unsetenv("LOBOLE_SAFE_MODE");
#endif
    relaunch({});
  };
  g_App.setAsideAndRestart = [&] {
    for (const std::string& file :
         {g_App.safeMode.settingsPath, g_App.safeMode.layoutPath}) {
      std::error_code ec;
      if (file.empty() || !std::filesystem::exists(file, ec))
        continue;
      const std::filesystem::path kept = settings::SetAside(file, ec);
      if (kept.empty())
        LOG_ERROR("Could not set {} aside: {}", file, ec.message());
      else
        LOG_WARN("Set {} aside as {}", file, kept.string());
    }
    g_App.restartNormally();
  };
  // A lost device's question (startup::Question) while it is open.
  std::unique_ptr<startup::Question> lossQuestion;
  bool lossHandled = false;
  bool firstFrameNoted = false;
  bool recreateFailing = false;
  // What follows every build of the owner table, the start's and each
  // rebuild's (gpuowners::Boundary::afterBuild): the renderers at the
  // framebuffer's size, and this loop's own swapchain flags for the new
  // swapchain.
  const std::function<void()> afterBuild = [&] {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    if (w > 0 && h > 0)
      ApplyFramebufferSize(w, h);
    g_SwapChainRebuild = false;
    g_SurfaceLost = false;
    recreateFailing = false;
    g_MainWindowData.FrameIndex = 0;
#ifdef HAVE_TEST_ENGINE
    s_CaptureFrame = -1; // a capture renders again, on the new renderers
#endif
  };
  afterBuild();

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
    crash::NoteProject(g_App.projectPath); // the marker names the project

    // The frame boundary (docs/rendering/README.md#rebuilding-the-device):
    // after the previous frame's present (or its abandoned submit), before
    // glfwPollEvents and ImGui_ImplVulkan_NewFrame. A lost device is rebuilt
    // in place here, and a gpu_device change that moves the first rung
    // rebuilds onto it; nowhere else.
    if (vkutil::DeviceLostSeen())
      ui::MarkDeviceLost(g_App, "in a one-shot submit");
    {
      int bw = 0, bh = 0;
      glfwGetFramebufferSize(window, &bw, &bh);
      gpuowners::Boundary boundary;
      boundary.canBuild = bw > 0 && bh > 0;
#ifdef HAVE_TEST_ENGINE
      boundary.capturing = testEngineIO.IsCapturing;
#endif
      boundary.afterBuild = afterBuild;
      gpuowners::AtFrameBoundary(g_App, boundary, glfwGetTime());
    }
    // Lost, and the rebuild waits for the window to have an extent again:
    // nothing is drawn meanwhile, and requests wait as for any frame.
    if (g_App.renderer.lost && !g_App.renderer.backstop) {
      glfwWaitEventsTimeout(0.05);
      continue;
    }
    // The backstop (docs/rendering/README.md#a-lost-device): the rebuild
    // failed, or the device died again right after one. Nothing more is
    // recorded, submitted or presented, and no renderer is called. The
    // document is CPU state and intact, so the work is saved at once, the
    // report written, and the user asked to restart. Until then the remote
    // keeps serving (a capture is refused as device_lost).
    if (g_App.renderer.backstop) {
      if (!lossHandled) {
        lossHandled = true;
        int autosaved = 0;
        const std::filesystem::path report =
            SaveWorkAfterDeviceLoss(&autosaved);
        const std::string text = startup::DeviceLossText(
            g_App.renderer.lostReason, autosaved, report);
        std::fprintf(stderr, "%s\n", text.c_str());
        if (startup::DialogsWanted())
          lossQuestion = std::make_unique<startup::Question>(
              "Loböle lost its graphics device", text);
        else
          LOG_WARN("Rendering: no dialog (LOBOLE_NO_DIALOG=1, or a "
                   "terminal reads stderr): close the window to quit, or "
                   "start again with --after-device-loss to recover");
      }
      glfwWaitEventsTimeout(0.05);
      remoteServer.ProcessPending(g_App);
      if (std::string shot; remoteServer.WantsScreenshot(shot))
        remoteServer.ScreenshotDone(false, DeviceLostRefusal());
      ui::PollBackgroundJobs(g_App);
      ui::AutosaveProject(g_App, autosaver, glfwGetTime());
      if (lossQuestion) {
        const startup::Question::Answer answer = lossQuestion->Poll();
        if (answer == startup::Question::Answer::Restart) {
          lossQuestion.reset();
          relaunch({"--after-device-loss"});
        } else if (answer == startup::Question::Answer::Quit) {
          lossQuestion.reset();
          glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
      }
      continue;
    }
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
      // A lost surface (the window's native surface went away under the
      // driver) is made again first; the swapchain is rebuilt on it below.
      if (g_SurfaceLost && RebuildSurface(window))
        g_SurfaceLost = false;
      // ImGui's recreate reports through check_vk_result, which aborted on
      // any error: it is caught here instead, and a recreate that failed is
      // logged once and tried again next frame (nothing renders meanwhile).
      VkResult recreate = VK_ERROR_SURFACE_LOST_KHR;
      if (!g_SurfaceLost) {
        g_CollectVkErrors = true;
        g_CollectedVkError = VK_SUCCESS;
        ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
        // ImGui's swapchain recreate destroys wd->RenderPass at its top.
        // Since we point that at our shared g_DepthRenderPass, null it first
        // so ImGui frees only its own (throwaway) pass and ours survives;
        // SetupDepthBuffer reattaches it.
        g_MainWindowData.RenderPass = VK_NULL_HANDLE;
        ImGui_ImplVulkanH_CreateOrResizeWindow(
            g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData,
            g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount);
        g_CollectVkErrors = false;
        recreate = g_CollectedVkError;
        if (recreate == VK_SUCCESS &&
            (g_MainWindowData.Swapchain == VK_NULL_HANDLE ||
             g_MainWindowData.ImageCount == 0))
          recreate = VK_ERROR_INITIALIZATION_FAILED;
      }
      if (recreate != VK_SUCCESS) {
        if (recreate == VK_ERROR_DEVICE_LOST)
          ui::MarkDeviceLost(g_App, "recreating the swapchain");
        else if (recreate == VK_ERROR_SURFACE_LOST_KHR)
          g_SurfaceLost = true;
        if (!recreateFailing)
          LOG_WARN("Rendering: recreating the swapchain failed ({}); trying "
                   "again each frame",
                   startup::VkResultName(recreate));
        recreateFailing = true;
        g_SwapChainRebuild = true; // and nothing renders until it holds
      } else {
        if (recreateFailing)
          LOG_INFO("Rendering: the swapchain is back");
        recreateFailing = false;
        g_App.swapchainImages = g_MainWindowData.ImageCount;
        SetupDepthBuffer(&g_MainWindowData);
        g_MainWindowData.FrameIndex = 0;
        g_SwapChainRebuild = false;

        ApplyFramebufferSize(fb_width, fb_height);
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
    // On the interface's clock, which is the wall clock -- except while a
    // demo scene records, when the test engine fixes it at 1/60 s a frame:
    // a played motion or sequence then runs at its own pace in the clip
    // however long a 4K frame takes to render (tools/demo/README.md).
    ui::TickMotionPlayback(g_App, ImGui::GetTime());
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
    gpuowners::KeepImGuiBackends();
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
    // A test-engine test that ended inside NewFrame may have put back the
    // backends of before a rebuild (gpuowners::KeepImGuiBackends).
    gpuowners::KeepImGuiBackends();
    g_App.frameTimer.Mark(render::FramePhase::BuildUI, glfwGetTime());
    g_App.frameTimer.SetWorkCpuMs(ThreadCpuMs() - frameCpuStartMs);
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool is_minimized =
        (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
    // Not while a swapchain recreate is failing: there is nothing to draw
    // into until it holds.
    // Nor once the device is lost mid-frame: the frame's ImGui CPU side has
    // finished, and nothing is submitted until the boundary has rebuilt it.
    if (!is_minimized && !g_SwapChainRebuild && !g_App.renderer.lost) {
      wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
      wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
      wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
      wd->ClearValue.color.float32[3] = clear_color.w;
      FrameRender(wd, draw_data);
      // The first frame on screen: this start did not die starting up
      // (crash::NoteFirstFrame), which is what safe mode counts.
      if (FramePresent(wd) && !firstFrameNoted && !args.runGuiTests) {
        firstFrameNoted = true;
        crash::NoteFirstFrame();
      }
      // Before the uncapped-mode sleep below: the frame is over when the
      // image is presented, not when we have finished idling.
      g_App.frameTimer.EndFrame(glfwGetTime());
#ifdef HAVE_TEST_ENGINE
      s_CaptureDrawData = draw_data; // what CaptureForTestEngine re-renders
      ImGuiTestEngine_PostSwap(testEngine);
      s_CaptureDrawData = nullptr;
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

  // They hold references into this function's frame.
  g_App.restartNormally = nullptr;
  g_App.setAsideAndRestart = nullptr;
  lossQuestion.reset();

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
  g_App.document.reset();
  g_App.commands.reset();
  g_Camera.reset();
  // The per-part render cache holds RETAINED asset ids in the mesh and
  // outline renderers; releasing them after the renderers are gone would free
  // nothing and leave the buffers to a dead device. Before, not after.
  ui::ReleasePartRenderCache(g_App);
  // The owner table backward, ImGui's backends with it, then the
  // instance: the walk a rebuild starts with. Every renderer is reset while
  // the device is alive -- one left to static destruction once ran its
  // Shutdown's vkDeviceWaitIdle on a freed device, the long-standing
  // shutdown crash.
  gpuowners::Teardown();
  ImGui::DestroyContext();
#ifdef HAVE_TEST_ENGINE
  // Test engine context must outlive the ImGui context it hooks
  ImGuiTestEngine_DestroyContext(testEngine);
#endif

  glfwDestroyWindow(window);
  glfwTerminate();

  return exitCode;
}
