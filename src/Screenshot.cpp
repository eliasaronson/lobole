#include "Screenshot.h"

#include "Logger.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "thirdparty/stb_image_write.h"

#include <cstring>
#include <vector>

namespace {

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter,
                        VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memProperties;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
  for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
    if ((typeFilter & (1 << i)) &&
        (memProperties.memoryTypes[i].propertyFlags & properties) ==
            properties) {
      return i;
    }
  }
  return UINT32_MAX;
}

} // namespace

bool CaptureScreenshot(
    VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue,
    VkCommandPool commandPool, VkRenderPass renderPass, VkFormat format,
    uint32_t width, uint32_t height, const VkClearValue& clearValue,
    const std::function<void(VkCommandBuffer)>& recordScene,
    const std::string& outputPath, std::string* error) {
  auto fail = [&](const char* what) {
    if (error) *error = what;
    LOG_ERROR("Screenshot failed: {}", what);
    return false;
  };

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

  VkMemoryRequirements memReq;
  vkGetImageMemoryRequirements(device, image, &memReq);
  VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = FindMemoryType(
      physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory imageMemory = VK_NULL_HANDLE;
  if (allocInfo.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) !=
          VK_SUCCESS) {
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

  VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbInfo.renderPass = renderPass;
  fbInfo.attachmentCount = 1;
  fbInfo.pAttachments = &view;
  fbInfo.width = width;
  fbInfo.height = height;
  fbInfo.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer);

  // Host-visible readback buffer
  const VkDeviceSize bufferSize = VkDeviceSize(width) * height * 4;
  VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufInfo.size = bufferSize;
  bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer = VK_NULL_HANDLE;
  vkCreateBuffer(device, &bufInfo, nullptr, &buffer);
  vkGetBufferMemoryRequirements(device, buffer, &memReq);
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex =
      FindMemoryType(physicalDevice, memReq.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
  vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory);
  vkBindBufferMemory(device, buffer, bufferMemory, 0);

  // One-shot command buffer: render the scene again, then copy out
  VkCommandBufferAllocateInfo cbInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbInfo.commandPool = commandPool;
  cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbInfo.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device, &cbInfo, &cb);

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &begin);

  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = renderPass;
  rp.framebuffer = framebuffer;
  rp.renderArea.extent = {width, height};
  rp.clearValueCount = 1;
  rp.pClearValues = &clearValue;
  vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
  recordScene(cb);
  vkCmdEndRenderPass(cb);

  // Render pass final layout is PRESENT_SRC; transition for the copy
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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

  vkEndCommandBuffer(cb);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb;
  vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue);

  // BGRA (typical swapchain format) -> RGB PNG
  void* mapped = nullptr;
  vkMapMemory(device, bufferMemory, 0, bufferSize, 0, &mapped);
  const uint8_t* src = static_cast<const uint8_t*>(mapped);
  std::vector<uint8_t> rgb(size_t(width) * height * 3);
  const bool bgra = (format == VK_FORMAT_B8G8R8A8_UNORM ||
                     format == VK_FORMAT_B8G8R8A8_SRGB);
  for (size_t i = 0; i < size_t(width) * height; i++) {
    rgb[i * 3 + 0] = src[i * 4 + (bgra ? 2 : 0)];
    rgb[i * 3 + 1] = src[i * 4 + 1];
    rgb[i * 3 + 2] = src[i * 4 + (bgra ? 0 : 2)];
  }
  vkUnmapMemory(device, bufferMemory);

  const bool ok = stbi_write_png(outputPath.c_str(), (int)width, (int)height,
                                 3, rgb.data(), (int)width * 3) != 0;

  vkFreeCommandBuffers(device, commandPool, 1, &cb);
  vkDestroyFramebuffer(device, framebuffer, nullptr);
  vkDestroyImageView(device, view, nullptr);
  vkDestroyImage(device, image, nullptr);
  vkFreeMemory(device, imageMemory, nullptr);
  vkDestroyBuffer(device, buffer, nullptr);
  vkFreeMemory(device, bufferMemory, nullptr);

  if (!ok) return fail("PNG encode/write");
  LOG_INFO("Screenshot written to {}", outputPath);
  return true;
}
