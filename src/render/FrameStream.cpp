// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "render/FrameStream.h"

#include "render/VulkanCommon.h"

#include <cstring>

namespace render {

namespace {

bool MakeImage(const FrameStream::Config& c, VkFormat format,
               VkSampleCountFlagBits samples, VkImageUsageFlags usage,
               VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& mem,
               VkImageView& view) {
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {c.width, c.height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(c.device, &info, nullptr, &image) != VK_SUCCESS)
    return false;
  if (!vkutil::AllocateImageMemory(c.device, c.physicalDevice, image,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem))
    return false;
  vkBindImageMemory(c.device, image, mem, 0);
  VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  v.image = image;
  v.viewType = VK_IMAGE_VIEW_TYPE_2D;
  v.format = format;
  v.subresourceRange = {aspect, 0, 1, 0, 1};
  return vkCreateImageView(c.device, &v, nullptr, &view) == VK_SUCCESS;
}

} // namespace

FrameStream::~FrameStream() { Destroy(); }

bool FrameStream::Bgra() const {
  return m_cfg.format == VK_FORMAT_B8G8R8A8_UNORM ||
         m_cfg.format == VK_FORMAT_B8G8R8A8_SRGB;
}

bool FrameStream::Init(const Config& config, std::string* error) {
  const auto fail = [&](const char* what) {
    if (error)
      *error = std::string("frame capture: ") + what;
    Destroy();
    return false;
  };
  m_cfg = config;
  const bool msaa = m_cfg.samples != VK_SAMPLE_COUNT_1_BIT;
  if (!MakeImage(m_cfg, m_cfg.format, VK_SAMPLE_COUNT_1_BIT,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT, m_image, m_imageMem, m_imageView))
    return fail("the colour target");
  if (!MakeImage(m_cfg, VK_FORMAT_D32_SFLOAT, m_cfg.samples,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT, m_depth, m_depthMem, m_depthView))
    return fail("the depth target");
  if (msaa &&
      !MakeImage(m_cfg, m_cfg.format, m_cfg.samples,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT, m_msaa, m_msaaMem, m_msaaView))
    return fail("the multisampled target");
  // Attachment order as the render pass has it: MSAA [colour, depth,
  // resolve], single-sample [colour, depth] (CaptureScreenshot's too).
  const VkImageView msaaViews[] = {m_msaaView, m_depthView, m_imageView};
  const VkImageView plainViews[] = {m_imageView, m_depthView};
  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = m_cfg.renderPass;
  fb.attachmentCount = msaa ? 3 : 2;
  fb.pAttachments = msaa ? msaaViews : plainViews;
  fb.width = m_cfg.width;
  fb.height = m_cfg.height;
  fb.layers = 1;
  if (vkCreateFramebuffer(m_cfg.device, &fb, nullptr, &m_framebuffer) !=
      VK_SUCCESS)
    return fail("the framebuffer");

  const VkDeviceSize bytes = VkDeviceSize(m_cfg.width) * m_cfg.height * 4;
  for (Slot& s : m_slots) {
    VkCommandBufferAllocateInfo cb{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = m_cfg.commandPool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_cfg.device, &cb, &s.cb) != VK_SUCCESS)
      return fail("a command buffer");
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(m_cfg.device, &f, nullptr, &s.fence) != VK_SUCCESS)
      return fail("a fence");
    if (!vkutil::CreateReadbackBuffer(m_cfg.device, m_cfg.physicalDevice, bytes,
                                      s.buffer, s.memory))
      return fail("a readback buffer");
    // Mapped once, for the life of the stream (MapReadback also invalidates,
    // which Collect repeats per frame).
    s.mapped = vkutil::MapReadback(m_cfg.device, s.memory);
    if (s.mapped == nullptr)
      return fail("mapping a readback buffer");
  }
  m_next = 0;
  m_inFlight = 0;
  return true;
}

bool FrameStream::Submit(const Record& prepass, const Record& scene,
                         std::string* error) {
  if (m_framebuffer == VK_NULL_HANDLE || Full()) {
    if (error)
      *error = Full() ? "frame capture: every slot is in flight; collect one "
                        "first"
                      : "frame capture: not initialised";
    return false;
  }
  Slot& s = m_slots[m_next];
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(s.cb, &begin);

  // The previous frame is finished with everything this one writes — the
  // shared target, the shadow map, the SSAO images, before its copy-out has
  // read the target.
  VkMemoryBarrier all{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  all.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
  all.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
  vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &all, 0,
                       nullptr, 0, nullptr);

  if (prepass)
    prepass(s.cb, m_next);
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = m_cfg.renderPass;
  rp.framebuffer = m_framebuffer;
  rp.renderArea.extent = {m_cfg.width, m_cfg.height};
  VkClearValue clears[2];
  clears[0] = m_cfg.clear;
  clears[1].depthStencil = {1.0f, 0};
  rp.clearValueCount = 2;
  rp.pClearValues = clears;
  vkCmdBeginRenderPass(s.cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
  scene(s.cb, m_next);
  vkCmdEndRenderPass(s.cb);

  VkImageMemoryBarrier toCopy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toCopy.oldLayout = m_cfg.colorFinalLayout;
  toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toCopy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toCopy.image = m_image;
  toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toCopy);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {m_cfg.width, m_cfg.height, 1};
  vkCmdCopyImageToBuffer(s.cb, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         s.buffer, 1, &region);
  // The host reads the buffer once the fence says so.
  VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toHost.buffer = s.buffer;
  toHost.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                       nullptr);
  vkEndCommandBuffer(s.cb);

  vkResetFences(m_cfg.device, 1, &s.fence);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &s.cb;
  if (vkutil::NoteResult(vkQueueSubmit(m_cfg.queue, 1, &submit, s.fence)) !=
      VK_SUCCESS) {
    if (error)
      *error = "frame capture: vkQueueSubmit failed";
    return false;
  }
  m_next = (m_next + 1) % kSlots;
  m_inFlight++;
  return true;
}

bool FrameStream::Collect(std::vector<uint8_t>* pixels, std::string* error) {
  if (m_inFlight == 0) {
    if (error)
      *error = "frame capture: nothing in flight to collect";
    return false;
  }
  const uint32_t oldest = (m_next + kSlots - m_inFlight) % kSlots;
  Slot& s = m_slots[oldest];
  if (vkutil::NoteResult(vkWaitForFences(m_cfg.device, 1, &s.fence, VK_TRUE,
                                         UINT64_MAX)) != VK_SUCCESS) {
    if (error)
      *error = "frame capture: waiting for a frame failed (device lost?)";
    return false;
  }
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = s.memory;
  range.size = VK_WHOLE_SIZE;
  vkInvalidateMappedMemoryRanges(m_cfg.device, 1, &range);
  const size_t bytes = size_t(m_cfg.width) * m_cfg.height * 4;
  pixels->resize(bytes);
  std::memcpy(pixels->data(), s.mapped, bytes);
  m_inFlight--;
  return true;
}

void FrameStream::Destroy() {
  VkDevice d = m_cfg.device;
  if (d == VK_NULL_HANDLE)
    return;
  // Only the slots in flight: a fence never submitted would never signal.
  for (uint32_t k = 0; k < m_inFlight; k++) {
    Slot& s = m_slots[(m_next + kSlots - m_inFlight + k) % kSlots];
    vkWaitForFences(d, 1, &s.fence, VK_TRUE, UINT64_MAX);
  }
  m_inFlight = 0;
  for (Slot& s : m_slots) {
    if (s.mapped != nullptr)
      vkUnmapMemory(d, s.memory);
    if (s.buffer != VK_NULL_HANDLE)
      vkDestroyBuffer(d, s.buffer, nullptr);
    if (s.memory != VK_NULL_HANDLE)
      vkFreeMemory(d, s.memory, nullptr);
    if (s.fence != VK_NULL_HANDLE)
      vkDestroyFence(d, s.fence, nullptr);
    if (s.cb != VK_NULL_HANDLE)
      vkFreeCommandBuffers(d, m_cfg.commandPool, 1, &s.cb);
    s = Slot{};
  }
  if (m_framebuffer)
    vkDestroyFramebuffer(d, m_framebuffer, nullptr);
  for (VkImageView v : {m_imageView, m_depthView, m_msaaView})
    if (v)
      vkDestroyImageView(d, v, nullptr);
  for (VkImage i : {m_image, m_depth, m_msaa})
    if (i)
      vkDestroyImage(d, i, nullptr);
  for (VkDeviceMemory m : {m_imageMem, m_depthMem, m_msaaMem})
    if (m)
      vkFreeMemory(d, m, nullptr);
  m_framebuffer = VK_NULL_HANDLE;
  m_imageView = m_depthView = m_msaaView = VK_NULL_HANDLE;
  m_image = m_depth = m_msaa = VK_NULL_HANDLE;
  m_imageMem = m_depthMem = m_msaaMem = VK_NULL_HANDLE;
  m_cfg.device = VK_NULL_HANDLE;
}

} // namespace render
