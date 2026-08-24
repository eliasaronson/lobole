#include "render/VulkanCommon.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"

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
    LOG_ERROR("Vulkan: vkAllocateMemory failed for {} bytes", req.size);
    vkDestroyBuffer(device, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    return false;
  }

  vkBindBufferMemory(device, buffer, memory, 0);
  return true;
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
    LOG_ERROR("Vulkan: image vkAllocateMemory failed for {} bytes", req.size);
    memory = VK_NULL_HANDLE;
    return false;
  }

  vkBindImageMemory(device, image, memory, 0);
  return true;
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
