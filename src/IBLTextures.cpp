#include "IBLTextures.h"

#include "IBL.h"
#include "Logger.h"

#include <glm/gtc/packing.hpp>

#include <cstring>
#include <vector>

namespace {

uint32_t FindMemoryType(VkPhysicalDevice phys, uint32_t typeFilter,
                        VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((typeFilter & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags & props) == props)
      return i;
  return UINT32_MAX;
}

uint16_t ToHalf(float x) {
  // Public glm API: packHalf2x16 puts half(x) in the low 16 bits.
  return static_cast<uint16_t>(glm::packHalf2x16(glm::vec2(x, 0.0f)) & 0xFFFFu);
}

// Packs float RGBA pixels to `channels` half-floats per pixel (R, RG, RGBA).
std::vector<uint16_t> PackHalf(const std::vector<glm::vec4>& px, int channels) {
  std::vector<uint16_t> out(px.size() * channels);
  for (size_t i = 0; i < px.size(); i++)
    for (int c = 0; c < channels; c++)
      out[i * channels + c] = ToHalf(px[i][c]);
  return out;
}

} // namespace

bool IBLTextures::CreateSamplers() {
  auto make = [&](VkSamplerAddressMode addrU, VkSamplerAddressMode addrV,
                  VkSampler& out) {
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = VK_FILTER_LINEAR;
    s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = addrU;
    s.addressModeV = addrV;
    s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.minLod = 0.0f;
    s.maxLod = VK_LOD_CLAMP_NONE;
    s.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    return vkCreateSampler(m_device, &s, nullptr, &out) == VK_SUCCESS;
  };
  // Equirect env maps: wrap longitude (u), clamp latitude (v) — no back seam.
  if (!make(VK_SAMPLER_ADDRESS_MODE_REPEAT,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, m_envSampler))
    return false;
  // BRDF LUT is indexed data: clamp both.
  return make(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, m_lutSampler);
}

namespace {
// One mip's source pixels + dimensions.
struct MipSrc {
  int width;
  int height;
  const std::vector<glm::vec4>* pixels;
};
} // namespace

// Creates a sampled image and uploads all mips through a staging buffer, ending
// in SHADER_READ_ONLY_OPTIMAL. Returns false on any Vulkan failure.
static bool UploadImage(VkDevice device, VkPhysicalDevice phys, VkQueue queue,
                        VkCommandPool pool, VkFormat format, int channels,
                        const std::vector<MipSrc>& mips, VkImage& outImage,
                        VkDeviceMemory& outMemory, VkImageView& outView) {
  const uint32_t mipCount = static_cast<uint32_t>(mips.size());

  VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  img.imageType = VK_IMAGE_TYPE_2D;
  img.format = format;
  img.extent = {static_cast<uint32_t>(mips[0].width),
                static_cast<uint32_t>(mips[0].height), 1};
  img.mipLevels = mipCount;
  img.arrayLayers = 1;
  img.samples = VK_SAMPLE_COUNT_1_BIT;
  img.tiling = VK_IMAGE_TILING_OPTIMAL;
  img.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device, &img, nullptr, &outImage) != VK_SUCCESS)
    return false;

  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, outImage, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex =
      FindMemoryType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device, &alloc, nullptr, &outMemory) != VK_SUCCESS)
    return false;
  vkBindImageMemory(device, outImage, outMemory, 0);

  // Stage every mip in one host buffer.
  std::vector<std::vector<uint16_t>> halfMips(mipCount);
  VkDeviceSize total = 0;
  std::vector<VkDeviceSize> offsets(mipCount);
  for (uint32_t m = 0; m < mipCount; m++) {
    halfMips[m] = PackHalf(*mips[m].pixels, channels);
    offsets[m] = total;
    total += halfMips[m].size() * sizeof(uint16_t);
  }

  VkBufferCreateInfo buf{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buf.size = total;
  buf.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  vkCreateBuffer(device, &buf, nullptr, &staging);
  VkMemoryRequirements sreq;
  vkGetBufferMemoryRequirements(device, staging, &sreq);
  VkMemoryAllocateInfo salloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  salloc.allocationSize = sreq.size;
  salloc.memoryTypeIndex =
      FindMemoryType(phys, sreq.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory stagingMem = VK_NULL_HANDLE;
  vkAllocateMemory(device, &salloc, nullptr, &stagingMem);
  vkBindBufferMemory(device, staging, stagingMem, 0);

  void* mapped = nullptr;
  vkMapMemory(device, stagingMem, 0, total, 0, &mapped);
  for (uint32_t m = 0; m < mipCount; m++)
    std::memcpy(static_cast<uint8_t*>(mapped) + offsets[m], halfMips[m].data(),
                halfMips[m].size() * sizeof(uint16_t));
  vkUnmapMemory(device, stagingMem);

  // One-shot command buffer: transition all mips, copy each, transition to read.
  VkCommandBufferAllocateInfo cbi{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbi.commandPool = pool;
  cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device, &cbi, &cb);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &begin);

  VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcAccessMask = 0;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.image = outImage;
  toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 1};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                       1, &toDst);

  for (uint32_t m = 0; m < mipCount; m++) {
    VkBufferImageCopy region{};
    region.bufferOffset = offsets[m];
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(mips[m].width),
                          static_cast<uint32_t>(mips[m].height), 1};
    vkCmdCopyBufferToImage(cb, staging, outImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }

  VkImageMemoryBarrier toRead = toDst;
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toRead);

  vkEndCommandBuffer(cb);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb;
  vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue);

  vkFreeCommandBuffers(device, pool, 1, &cb);
  vkDestroyBuffer(device, staging, nullptr);
  vkFreeMemory(device, stagingMem, nullptr);

  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = outImage;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 1};
  return vkCreateImageView(device, &view, nullptr, &outView) == VK_SUCCESS;
}

bool IBLTextures::Upload(VkDevice device, VkPhysicalDevice physicalDevice,
                         VkQueue queue, VkCommandPool commandPool,
                         const IBLPrecompute& ibl) {
  m_device = device;
  m_physicalDevice = physicalDevice;
  m_queue = queue;
  m_commandPool = commandPool;
  if (!CreateSamplers()) return false;

  std::vector<MipSrc> one = {
      {ibl.Irradiance().width, ibl.Irradiance().height, &ibl.Irradiance().pixels}};
  if (!UploadImage(device, physicalDevice, queue, commandPool,
                   VK_FORMAT_R16G16B16A16_SFLOAT, 4, one, m_irradiance.image,
                   m_irradiance.memory, m_irradiance.view))
    return false;

  std::vector<MipSrc> pref;
  for (const auto& mip : ibl.Prefilter())
    pref.push_back({mip.width, mip.height, &mip.pixels});
  m_prefilterMips = static_cast<uint32_t>(pref.size());
  if (!UploadImage(device, physicalDevice, queue, commandPool,
                   VK_FORMAT_R16G16B16A16_SFLOAT, 4, pref, m_prefilter.image,
                   m_prefilter.memory, m_prefilter.view))
    return false;

  std::vector<MipSrc> brdf = {
      {ibl.BrdfLut().width, ibl.BrdfLut().height, &ibl.BrdfLut().pixels}};
  if (!UploadImage(device, physicalDevice, queue, commandPool,
                   VK_FORMAT_R16G16_SFLOAT, 2, brdf, m_brdf.image,
                   m_brdf.memory, m_brdf.view))
    return false;

  m_ready = true;
  LOG_INFO("IBL: textures uploaded ({} prefilter mips)", m_prefilterMips);
  return true;
}

bool IBLTextures::MakeFallback(VkDevice device, VkPhysicalDevice physicalDevice,
                               VkQueue queue, VkCommandPool commandPool) {
  m_device = device;
  m_physicalDevice = physicalDevice;
  m_queue = queue;
  m_commandPool = commandPool;
  if (!CreateSamplers()) return false;

  // Neutral 1x1 stand-ins: dim gray ambient, identity BRDF (scale 1, bias 0).
  std::vector<glm::vec4> gray = {{0.15f, 0.15f, 0.15f, 1.0f}};
  std::vector<glm::vec4> lut = {{1.0f, 0.0f, 0.0f, 1.0f}};
  std::vector<MipSrc> g = {{1, 1, &gray}};
  std::vector<MipSrc> l = {{1, 1, &lut}};
  m_prefilterMips = 1;
  bool ok =
      UploadImage(device, physicalDevice, queue, commandPool,
                  VK_FORMAT_R16G16B16A16_SFLOAT, 4, g, m_irradiance.image,
                  m_irradiance.memory, m_irradiance.view) &&
      UploadImage(device, physicalDevice, queue, commandPool,
                  VK_FORMAT_R16G16B16A16_SFLOAT, 4, g, m_prefilter.image,
                  m_prefilter.memory, m_prefilter.view) &&
      UploadImage(device, physicalDevice, queue, commandPool,
                  VK_FORMAT_R16G16_SFLOAT, 2, l, m_brdf.image, m_brdf.memory,
                  m_brdf.view);
  m_ready = ok;
  if (ok) LOG_WARN("IBL: using neutral fallback textures (no environment)");
  return ok;
}

void IBLTextures::Shutdown(VkDevice device) {
  auto drop = [&](Tex& t) {
    if (t.view) vkDestroyImageView(device, t.view, nullptr);
    if (t.image) vkDestroyImage(device, t.image, nullptr);
    if (t.memory) vkFreeMemory(device, t.memory, nullptr);
    t = {};
  };
  drop(m_irradiance);
  drop(m_prefilter);
  drop(m_brdf);
  if (m_envSampler) vkDestroySampler(device, m_envSampler, nullptr);
  if (m_lutSampler) vkDestroySampler(device, m_lutSampler, nullptr);
  m_envSampler = m_lutSampler = VK_NULL_HANDLE;
  m_ready = false;
}
