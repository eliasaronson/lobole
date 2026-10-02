#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

// Captures the current frame to a PNG by re-rendering it into an offscreen
// image (the swapchain lacks TRANSFER_SRC usage, so it can't be read back
// directly). recordScene records the same render-pass content as the
// on-screen frame. Synchronous (waits for the GPU); intended for remote
// control / debugging, not per-frame use.
// `samples` must match the render pass; when >1 the offscreen target is
// multisampled and resolved into the readback image, matching the on-screen
// MSAA pass so screenshots look like the viewport.
// `recordPrepass` (optional) records scene prepasses — e.g. the shadow map —
// into the same command buffer BEFORE the render pass begins, mirroring
// FrameRender's pass order so screenshots show identical shadows.
// Screenshot postconditions (error-audit follow-up): cheap statistics from
// the readback, so callers can WARN when a capture is blank or unchanged
// instead of returning a confident picture of nothing.
struct FrameStats {
  float dominantFraction = 0.0f; // share of sampled pixels in the modal
                                 // 5-bit/channel color bucket (1.0 = flat)
  uint64_t hash = 0;             // FNV-1a over the sampled pixels
  // Input: restrict sampling to this rect (the 3D viewport — panel chrome
  // and text would otherwise dilute a flat frame below any threshold).
  // w/h 0 = the whole frame.
  int x = 0, y = 0, w = 0, h = 0;
};

// `colorFinalLayout` is what the render pass leaves the readback attachment
// in — the barrier before the copy names it as oldLayout. The windowed pass
// ends at PRESENT_SRC_KHR (the default); an offscreen pass has no swapchain
// (PRESENT_SRC is a VK_KHR_swapchain layout) and ends at
// COLOR_ATTACHMENT_OPTIMAL instead.
// An EMPTY `outputPath` renders and reads nothing back — no copy, no PNG,
// no FrameStats. That is the measuring shape: the encode between renders is
// CPU work that leaves the GPU idle, and an idle GPU on a laptop drops to its
// lowest clock, so a burst that saves nothing measures the card as it
// actually runs (docs/rendering/frame-pacing.md#measuring-a-pass-offscreen).
//
// `repeat` renders the same scene that many times into the same target — one
// submit each, each waited on, only the last reading back. `afterEachSubmit`
// runs on the CPU between them, which is where a timestamp harvest goes.
bool CaptureScreenshot(
    VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue,
    VkCommandPool commandPool, VkRenderPass renderPass, VkFormat format,
    uint32_t width, uint32_t height, const VkClearValue& clearValue,
    const std::function<void(VkCommandBuffer)>& recordScene,
    const std::string& outputPath, std::string* error = nullptr,
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
    const std::function<void(VkCommandBuffer)>& recordPrepass = {},
    FrameStats* stats = nullptr,
    VkImageLayout colorFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    uint32_t repeat = 1, const std::function<void()>& afterEachSubmit = {},
    std::vector<uint8_t>* pixelsOut = nullptr);

// `pixelsOut` (above) receives the frame as tightly packed RGB8 when given;
// with an empty `outputPath` nothing is written to disk.

// The averaging alone, into `out` ((w / k) x (h / k) RGB8), for a caller
// that encodes the frame itself. The same average as below; `progress` runs
// 0..`progressShare`.
bool DownsampleRgb(const std::vector<uint8_t>& rgb, uint32_t width,
                   uint32_t height, uint32_t factor, std::vector<uint8_t>* out,
                   std::string* error = nullptr,
                   std::atomic<float>* progress = nullptr,
                   const std::atomic<bool>* cancel = nullptr,
                   float progressShare = 1.0f);

// Average a k x k supersampled RGB8 frame down to (w / k) x (h / k) and write
// it as a PNG. The average is of the ENCODED values -- the space the
// viewport's own MSAA resolve averages in -- so a supersampled still's edges
// and lines come out as dark as the viewport's. (A linear-light average
// thinned a 1 px dark line on a light face by a third.)
// Safe on a worker thread. `progress` (0..1) advances as it goes; a set
// `cancel` stops it before the file is written, returning false.
bool DownsampleToPng(const std::vector<uint8_t>& rgb, uint32_t width,
                     uint32_t height, uint32_t factor, const std::string& path,
                     std::string* error = nullptr,
                     std::atomic<float>* progress = nullptr,
                     const std::atomic<bool>* cancel = nullptr);
