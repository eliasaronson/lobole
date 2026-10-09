// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "render/VulkanCommon.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"

#include <atomic>
#include <fstream>

namespace vkutil {

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter,
                        VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
  for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
    if ((typeFilter & (1u << i)) &&
        (memProps.memoryTypes[i].propertyFlags & props) == props) {
      return i;
    }
  }
  LOG_ERROR("Vulkan: no memory type matches filter {:#x} with properties {:#x}",
            typeFilter, static_cast<uint32_t>(props));
  return kNoMemoryType;
}

// Every successful vkAllocateMemory below bumps this. See the header for why
// it is monotonic rather than a live count.
std::atomic<uint64_t> g_deviceAllocations{0};

bool CreateBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags props, VkBuffer& buffer,
                  VkDeviceMemory& memory) {
  // Cleared up front so a caller that ignores the return value still sees
  // null handles rather than whatever was on the stack.
  buffer = VK_NULL_HANDLE;
  memory = VK_NULL_HANDLE;

  VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
    LOG_ERROR("Vulkan: vkCreateBuffer failed for {} bytes", size);
    buffer = VK_NULL_HANDLE;
    return false;
  }

  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device, buffer, &req);
  const uint32_t typeIndex =
      FindMemoryType(physicalDevice, req.memoryTypeBits, props);
  if (typeIndex == kNoMemoryType) {
    vkDestroyBuffer(device, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    return false;
  }

  VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocInfo.allocationSize = req.size;
  allocInfo.memoryTypeIndex = typeIndex;

  // A buffer whose device address is taken (the ray-tracing SBT, acceleration
  // structure storage) must have the matching allocate flag or
  // vkGetBufferDeviceAddress is invalid use.
  VkMemoryAllocateFlagsInfo flagsInfo{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
    allocInfo.pNext = &flagsInfo;

  if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
    LOG_ERROR("Vulkan: vkAllocateMemory failed for {} bytes (this process has "
              "made {} device allocations; the limit is a device property)",
              req.size, g_deviceAllocations.load());
    vkDestroyBuffer(device, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    return false;
  }
  g_deviceAllocations.fetch_add(1, std::memory_order_relaxed);

  vkBindBufferMemory(device, buffer, memory, 0);
  return true;
}

bool CreateReadbackBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                          VkDeviceSize size, VkBuffer& buffer,
                          VkDeviceMemory& memory) {
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
  const auto has = [&](VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
      if ((memProps.memoryTypes[i].propertyFlags & want) == want)
        return true;
    return false;
  };
  constexpr VkMemoryPropertyFlags kVisible =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  constexpr VkMemoryPropertyFlags kCached =
      kVisible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  constexpr VkMemoryPropertyFlags kCoherent =
      kVisible | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  // Cached first: uncached memory is write-combined, and reading it back a
  // pixel at a time cost 27 s for a 10240 x 6400 frame the GPU drew in 0.3.
  const VkMemoryPropertyFlags props = has(kCached | kCoherent)
                                          ? (kCached | kCoherent)
                                      : has(kCached) ? kCached
                                                     : kCoherent;
  return CreateBuffer(device, physicalDevice, size,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, props, buffer, memory);
}

const void* MapReadback(VkDevice device, VkDeviceMemory memory) {
  void* mapped = nullptr;
  if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
    return nullptr;
  // Needed when the memory is cached but not coherent; allowed (and free)
  // when it is coherent.
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = memory;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  vkInvalidateMappedMemoryRanges(device, 1, &range);
  return mapped;
}

void DestroyBuffer(VkDevice device, VkBuffer& buffer, VkDeviceMemory& memory,
                   void** mapped) {
  if (mapped != nullptr && *mapped != nullptr) {
    vkUnmapMemory(device, memory);
    *mapped = nullptr;
  }
  if (buffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(device, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
  }
  if (memory != VK_NULL_HANDLE) {
    vkFreeMemory(device, memory, nullptr);
    memory = VK_NULL_HANDLE;
  }
}

bool AllocateImageMemory(VkDevice device, VkPhysicalDevice physicalDevice,
                         VkImage image, VkMemoryPropertyFlags props,
                         VkDeviceMemory& memory) {
  memory = VK_NULL_HANDLE;

  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, image, &req);
  const uint32_t typeIndex =
      FindMemoryType(physicalDevice, req.memoryTypeBits, props);
  if (typeIndex == kNoMemoryType)
    return false;

  VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocInfo.allocationSize = req.size;
  allocInfo.memoryTypeIndex = typeIndex;
  if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
    LOG_ERROR("Vulkan: image vkAllocateMemory failed for {} bytes (this "
              "process has made {} device allocations; the limit is a device "
              "property)",
              req.size, g_deviceAllocations.load());
    memory = VK_NULL_HANDLE;
    return false;
  }

  g_deviceAllocations.fetch_add(1, std::memory_order_relaxed);

  vkBindImageMemory(device, image, memory, 0);
  return true;
}

uint64_t TotalDeviceAllocations() {
  return g_deviceAllocations.load(std::memory_order_relaxed);
}

VkCommandBuffer BeginSingleTimeCommands(VkDevice device, VkCommandPool pool) {
  VkCommandBufferAllocateInfo alloc{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;

  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device, &alloc, &cb) != VK_SUCCESS) {
    LOG_ERROR("Vulkan: vkAllocateCommandBuffers failed");
    return VK_NULL_HANDLE;
  }

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &begin);
  return cb;
}

void EndSingleTimeCommands(VkDevice device, VkCommandPool pool, VkQueue queue,
                           VkCommandBuffer cb) {
  if (cb == VK_NULL_HANDLE)
    return;
  vkEndCommandBuffer(cb);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb;
  vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue);

  vkFreeCommandBuffers(device, pool, 1, &cb);
}

bool CopyBuffer(VkDevice device, VkCommandPool pool, VkQueue queue,
                VkBuffer src, VkBuffer dst, VkDeviceSize size) {
  VkCommandBuffer cb = BeginSingleTimeCommands(device, pool);
  if (cb == VK_NULL_HANDLE)
    return false;
  VkBufferCopy region{};
  region.size = size;
  vkCmdCopyBuffer(cb, src, dst, 1, &region);
  EndSingleTimeCommands(device, pool, queue, cb);
  return true;
}

StagingBatch::StagingBatch(VkDevice device, VkPhysicalDevice physicalDevice,
                           VkCommandPool pool, VkQueue queue)
    : m_device(device), m_physicalDevice(physicalDevice), m_pool(pool),
      m_queue(queue) {}

StagingBatch::~StagingBatch() { Flush(); }

bool StagingBatch::Upload(VkDeviceSize size, const void* src,
                          VkBufferUsageFlags usage, VkBuffer& outBuffer,
                          VkDeviceMemory& outMemory) {
  outBuffer = VK_NULL_HANDLE;
  outMemory = VK_NULL_HANDLE;
  if (size == 0 || src == nullptr)
    return false;

  // One command buffer for the whole batch, opened lazily so a batch nothing
  // uploads into costs nothing.
  if (m_cb == VK_NULL_HANDLE) {
    m_cb = BeginSingleTimeCommands(m_device, m_pool);
    if (m_cb == VK_NULL_HANDLE) {
      m_failed = true;
      return false;
    }
  }

  Staged staging;
  if (!CreateBuffer(m_device, m_physicalDevice, size,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    staging.buffer, staging.memory)) {
    m_failed = true;
    return false;
  }

  void* data = nullptr;
  vkMapMemory(m_device, staging.memory, 0, size, 0, &data);
  memcpy(data, src, static_cast<size_t>(size));
  vkUnmapMemory(m_device, staging.memory);

  if (!CreateBuffer(m_device, m_physicalDevice, size,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outBuffer,
                    outMemory)) {
    DestroyBuffer(m_device, staging.buffer, staging.memory);
    m_failed = true;
    return false;
  }

  VkBufferCopy region{};
  region.size = size;
  vkCmdCopyBuffer(m_cb, staging.buffer, outBuffer, 1, &region);
  // Held until Flush: the copy has not executed yet, so freeing the source now
  // would be a use-after-free on the GPU.
  m_staging.push_back(staging);
  return true;
}

bool StagingBatch::Flush() {
  if (m_cb == VK_NULL_HANDLE) {
    const bool ok = !m_failed;
    m_failed = false;
    return ok;
  }
  // ONE submit + ONE wait for everything recorded since construction.
  EndSingleTimeCommands(m_device, m_pool, m_queue, m_cb);
  m_cb = VK_NULL_HANDLE;
  for (Staged& s : m_staging)
    DestroyBuffer(m_device, s.buffer, s.memory);
  m_staging.clear();
  const bool ok = !m_failed;
  m_failed = false;
  return ok;
}

std::vector<char> ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    LOG_ERROR("Vulkan: cannot open {}", path);
    return {};
  }
  const size_t size = static_cast<size_t>(file.tellg());
  std::vector<char> buffer(size);
  file.seekg(0);
  file.read(buffer.data(), static_cast<std::streamsize>(size));
  return buffer;
}

VkShaderModule CreateShaderModule(VkDevice device,
                                  const std::vector<char>& code) {
  if (code.empty()) {
    LOG_ERROR("Vulkan: refusing to create a shader module from empty SPIR-V");
    return VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());

  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS) {
    LOG_ERROR("Vulkan: vkCreateShaderModule failed ({} bytes)", code.size());
    return VK_NULL_HANDLE;
  }
  return module;
}

VkShaderModule LoadShaderModule(VkDevice device, const std::string& spvName) {
  const std::string path = (RuntimePaths::ShaderDir() / spvName).string();
  const std::vector<char> code = ReadFile(path);
  if (code.empty())
    return VK_NULL_HANDLE; // ReadFile already logged the path
  return CreateShaderModule(device, code);
}

} // namespace vkutil
