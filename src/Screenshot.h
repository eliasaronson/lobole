#pragma once

#include <vulkan/vulkan.h>

#include <functional>
#include <string>

// Captures the current frame to a PNG by re-rendering it into an offscreen
// image (the swapchain lacks TRANSFER_SRC usage, so it can't be read back
// directly). recordScene records the same render-pass content as the
// on-screen frame. Synchronous (waits for the GPU); intended for remote
// control / debugging, not per-frame use.
bool CaptureScreenshot(
    VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue,
    VkCommandPool commandPool, VkRenderPass renderPass, VkFormat format,
    uint32_t width, uint32_t height, const VkClearValue& clearValue,
    const std::function<void(VkCommandBuffer)>& recordScene,
    const std::string& outputPath, std::string* error = nullptr);
