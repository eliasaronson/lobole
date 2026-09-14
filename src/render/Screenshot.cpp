#include "render/Screenshot.h"

#include "render/VulkanCommon.h"

#include "common/Logger.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "thirdparty/stb_image_write.h"

#include <cstring>
#include <vector>

bool CaptureScreenshot(
    VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue,
    VkCommandPool commandPool, VkRenderPass renderPass, VkFormat format,
    uint32_t width, uint32_t height, const VkClearValue& clearValue,
    const std::function<void(VkCommandBuffer)>& recordScene,
    const std::string& outputPath, std::string* error,
    VkSampleCountFlagBits samples,
    const std::function<void(VkCommandBuffer)>& recordPrepass,
    FrameStats* stats, VkImageLayout colorFinalLayout, uint32_t repeat,
    const std::function<void()>& afterEachSubmit) {
  auto fail = [&](const char* what) {
    if (error)
      *error = what;
    LOG_ERROR("Screenshot failed: {}", what);
    return false;
  };
  const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

  // Offscreen color target we are allowed to copy from
  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage image = VK_NULL_HANDLE;
  if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS)
    return fail("vkCreateImage");

  VkDeviceMemory imageMemory = VK_NULL_HANDLE;
  if (!vkutil::AllocateImageMemory(device, physicalDevice, image,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   imageMemory)) {
    vkDestroyImage(device, image, nullptr);
    return fail("image memory allocation");
  }
  vkBindImageMemory(device, image, imageMemory, 0);

  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewInfo.image = image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = format;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(device, &viewInfo, nullptr, &view);

  // Transient depth attachment to match the color+depth render pass.
  VkImageCreateInfo depthInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  depthInfo.imageType = VK_IMAGE_TYPE_2D;
  depthInfo.format = VK_FORMAT_D32_SFLOAT;
  depthInfo.extent = {width, height, 1};
  depthInfo.mipLevels = 1;
  depthInfo.arrayLayers = 1;
  depthInfo.samples = samples;
  depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  VkImage depthImage = VK_NULL_HANDLE;
  vkCreateImage(device, &depthInfo, nullptr, &depthImage);
  VkDeviceMemory depthMemory = VK_NULL_HANDLE;
  vkutil::AllocateImageMemory(device, physicalDevice, depthImage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthMemory);
  VkImageViewCreateInfo depthView{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  depthView.image = depthImage;
  depthView.viewType = VK_IMAGE_VIEW_TYPE_2D;
  depthView.format = VK_FORMAT_D32_SFLOAT;
  depthView.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  VkImageView depthImageView = VK_NULL_HANDLE;
  vkCreateImageView(device, &depthView, nullptr, &depthImageView);

  // Transient multisampled color target that resolves into `image` (MSAA only),
  // mirroring the on-screen render pass's attachment order [msaaColor, depth,
  // resolve].
  VkImage msaaColorImage = VK_NULL_HANDLE;
  VkDeviceMemory msaaColorMemory = VK_NULL_HANDLE;
  VkImageView msaaColorView = VK_NULL_HANDLE;
  if (msaa) {
    VkImageCreateInfo cinfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    cinfo.imageType = VK_IMAGE_TYPE_2D;
    cinfo.format = format;
    cinfo.extent = {width, height, 1};
    cinfo.mipLevels = 1;
    cinfo.arrayLayers = 1;
    cinfo.samples = samples;
    cinfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    cinfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    vkCreateImage(device, &cinfo, nullptr, &msaaColorImage);
    vkutil::AllocateImageMemory(device, physicalDevice, msaaColorImage,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                msaaColorMemory);
    VkImageViewCreateInfo cview{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    cview.image = msaaColorImage;
    cview.viewType = VK_IMAGE_VIEW_TYPE_2D;
    cview.format = format;
    cview.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &cview, nullptr, &msaaColorView);
  }

  // Framebuffer attachments match the render pass: MSAA → [msaaColor, depth,
  // resolve=image]; single-sample → [image, depth].
  VkImageView attachments[3];
  uint32_t attachmentCount;
  if (msaa) {
    attachments[0] = msaaColorView;
    attachments[1] = depthImageView;
    attachments[2] = view;
    attachmentCount = 3;
  } else {
    attachments[0] = view;
    attachments[1] = depthImageView;
    attachmentCount = 2;
  }
  VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbInfo.renderPass = renderPass;
  fbInfo.attachmentCount = attachmentCount;
  fbInfo.pAttachments = attachments;
  fbInfo.width = width;
  fbInfo.height = height;
  fbInfo.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer);

  // An empty path means MEASURE, not save: nothing is copied out and nothing
  // is encoded, so the loop below is render-submit-render and the GPU keeps
  // its clocks up.
  const bool write = !outputPath.empty();

  // Host-visible readback buffer
  const VkDeviceSize bufferSize = VkDeviceSize(width) * height * 4;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
  if (write)
    vkutil::CreateBuffer(device, physicalDevice, bufferSize,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         buffer, bufferMemory);

  // One-shot command buffer: render the scene again, then copy out
  VkCommandBufferAllocateInfo cbInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbInfo.commandPool = commandPool;
  cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbInfo.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device, &cbInfo, &cb);

  // The same scene, `repeat` times into the same target. The render pass
  // clears and declares an UNDEFINED initial layout, so a re-run discards
  // what the last one left and needs no transition of its own. The command
  // buffer is re-recorded each pass (the pool carries
  // RESET_COMMAND_BUFFER_BIT, so beginning it again resets it).
  const uint32_t passes = std::max(1u, repeat);
  for (uint32_t pass = 0; pass < passes; pass++) {
    const bool last = pass + 1 == passes;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    // Scene prepasses (shadow map) go before the render pass, as on-screen.
    if (recordPrepass)
      recordPrepass(cb);

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = renderPass;
    rp.framebuffer = framebuffer;
    rp.renderArea.extent = {width, height};
    VkClearValue clears[2];
    clears[0] = clearValue;
    clears[1].depthStencil = {1.0f, 0};
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    recordScene(cb);
    vkCmdEndRenderPass(cb);

    if (write && last) {
      // Transition from wherever the render pass left the image for the copy.
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout = colorFinalLayout;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.image = image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);

      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {width, height, 1};
      vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             buffer, 1, &region);
    }

    vkEndCommandBuffer(cb);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    if (afterEachSubmit)
      afterEachSubmit();
  }

  // Measuring only: nothing to read back, nothing to encode.
  if (!write) {
    vkFreeCommandBuffers(device, commandPool, 1, &cb);
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    vkDestroyImageView(device, view, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkFreeMemory(device, imageMemory, nullptr);
    vkDestroyImageView(device, depthImageView, nullptr);
    vkDestroyImage(device, depthImage, nullptr);
    vkFreeMemory(device, depthMemory, nullptr);
    if (msaaColorView)
      vkDestroyImageView(device, msaaColorView, nullptr);
    if (msaaColorImage)
      vkDestroyImage(device, msaaColorImage, nullptr);
    if (msaaColorMemory)
      vkFreeMemory(device, msaaColorMemory, nullptr);
    return true;
  }

  // BGRA (typical swapchain format) -> RGB PNG
  void* mapped = nullptr;
  vkMapMemory(device, bufferMemory, 0, bufferSize, 0, &mapped);
  const uint8_t* src = static_cast<const uint8_t*>(mapped);
  std::vector<uint8_t> rgb(size_t(width) * height * 3);
  const bool bgra =
      (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB);
  for (size_t i = 0; i < size_t(width) * height; i++) {
    rgb[i * 3 + 0] = src[i * 4 + (bgra ? 2 : 0)];
    rgb[i * 3 + 1] = src[i * 4 + 1];
    rgb[i * 3 + 2] = src[i * 4 + (bgra ? 0 : 2)];
  }
  vkUnmapMemory(device, bufferMemory);

  if (stats != nullptr) {
    // ~10k samples over the requested rect (the viewport): enough for a
    // stable modal-color fraction and a hash that changes when anything
    // visible changes.
    int rx = stats->x, ry = stats->y, rw = stats->w, rh = stats->h;
    if (rw <= 0 || rh <= 0) {
      rx = ry = 0;
      rw = static_cast<int>(width);
      rh = static_cast<int>(height);
    }
    rx = std::clamp(rx, 0, static_cast<int>(width) - 1);
    ry = std::clamp(ry, 0, static_cast<int>(height) - 1);
    rw = std::clamp(rw, 1, static_cast<int>(width) - rx);
    rh = std::clamp(rh, 1, static_cast<int>(height) - ry);
    const size_t total = size_t(rw) * rh;
    const size_t stride = std::max<size_t>(1, total / 10000);
    std::vector<uint32_t> counts(1u << 15, 0);
    uint32_t modal = 0;
    uint64_t h = 1469598103934665603ull;
    size_t sampled = 0;
    for (size_t s = 0; s < total; s += stride, sampled++) {
      const size_t i = (size_t(ry) + s / rw) * width + rx + s % rw;
      const uint8_t r = rgb[i * 3 + 0], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
      const uint32_t bucket =
          (uint32_t(r >> 3) << 10) | (uint32_t(g >> 3) << 5) | uint32_t(b >> 3);
      modal = std::max(modal, ++counts[bucket]);
      for (const uint8_t c : {r, g, b}) {
        h ^= c;
        h *= 1099511628211ull;
      }
    }
    stats->dominantFraction = sampled ? float(modal) / sampled : 0.0f;
    stats->hash = h;
  }

  const bool ok = stbi_write_png(outputPath.c_str(), (int)width, (int)height, 3,
                                 rgb.data(), (int)width * 3) != 0;

  vkFreeCommandBuffers(device, commandPool, 1, &cb);
  vkDestroyFramebuffer(device, framebuffer, nullptr);
  vkDestroyImageView(device, view, nullptr);
  vkDestroyImage(device, image, nullptr);
  vkFreeMemory(device, imageMemory, nullptr);
  vkDestroyImageView(device, depthImageView, nullptr);
  vkDestroyImage(device, depthImage, nullptr);
  vkFreeMemory(device, depthMemory, nullptr);
  if (msaaColorView)
    vkDestroyImageView(device, msaaColorView, nullptr);
  if (msaaColorImage)
    vkDestroyImage(device, msaaColorImage, nullptr);
  if (msaaColorMemory)
    vkFreeMemory(device, msaaColorMemory, nullptr);
  vkDestroyBuffer(device, buffer, nullptr);
  vkFreeMemory(device, bufferMemory, nullptr);

  if (!ok)
    return fail("PNG encode/write");
  LOG_INFO("Screenshot written to {}", outputPath);
  return true;
}
