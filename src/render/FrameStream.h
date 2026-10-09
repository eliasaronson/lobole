// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace render {

// A stream of frames read back from the GPU, PIPELINED: frame N+1 is recorded
// and submitted while frame N is still rendering, and N's pixels are read
// back only when the caller wants them. CaptureScreenshot makes its target,
// renders, waits for the queue to drain and frees everything, once per
// picture — right for one picture, and most of a frame's time when a motion
// video takes a hundred (docs/assemblies/README.md#motion-video).
//
// The target (colour, depth, the MSAA image when the pass has one, and the
// framebuffer) is made ONCE; each of the `kSlots` slots has its own command
// buffer, fence and host-cached readback buffer. A slot is also the frame
// index the renderers are recorded with, so the per-frame buffers they write
// while recording (uniforms, the outline's edge indices) are never ones an
// in-flight frame is reading. Every command buffer opens with a full barrier:
// consecutive frames share the target, the shadow map and the SSAO images,
// so the GPU runs them one after another. What overlaps is the CPU — posing,
// recording and the readback of the frame before — with the GPU's rendering.
class FrameStream {
public:
  static constexpr uint32_t kSlots = 3;

  struct Config {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    // What the render pass leaves the colour attachment in (CaptureScreenshot
    // says why it differs between the windowed and offscreen passes).
    VkImageLayout colorFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    uint32_t width = 0, height = 0;
    VkClearValue clear{};
  };
  // Records one frame: `prepass` before the render pass begins (the shadow
  // map, SSAO), `scene` inside it. Both get the slot's frame index.
  using Record = std::function<void(VkCommandBuffer, uint32_t frame)>;

  FrameStream() = default;
  FrameStream(const FrameStream&) = delete;
  FrameStream& operator=(const FrameStream&) = delete;
  ~FrameStream(); // waits for every frame in flight, then frees

  bool Init(const Config& config, std::string* error);
  // True when a Submit would have to wait for the oldest frame first:
  // Collect it before submitting another.
  bool Full() const { return m_inFlight == kSlots; }
  size_t InFlight() const { return m_inFlight; }
  // Record and submit one frame into the next free slot. Does not wait.
  bool Submit(const Record& prepass, const Record& scene, std::string* error);
  // The OLDEST frame in flight, waited for and copied out as 4 bytes a pixel
  // in the target's format order (`Bgra()`), rows top to bottom.
  bool Collect(std::vector<uint8_t>* pixels, std::string* error);
  bool Bgra() const;

private:
  struct Slot {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const void* mapped = nullptr;
  };
  void Destroy();

  Config m_cfg;
  VkImage m_image = VK_NULL_HANDLE, m_depth = VK_NULL_HANDLE,
          m_msaa = VK_NULL_HANDLE;
  VkDeviceMemory m_imageMem = VK_NULL_HANDLE, m_depthMem = VK_NULL_HANDLE,
                 m_msaaMem = VK_NULL_HANDLE;
  VkImageView m_imageView = VK_NULL_HANDLE, m_depthView = VK_NULL_HANDLE,
              m_msaaView = VK_NULL_HANDLE;
  VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
  std::array<Slot, kSlots> m_slots{};
  uint32_t m_next = 0;     // the slot the next Submit uses
  uint32_t m_inFlight = 0; // submitted and not yet collected
};

} // namespace render
