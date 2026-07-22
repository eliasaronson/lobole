#include "UnderlayRenderer.h"

#include "Logger.h"

#include "thirdparty/stb_image.h" // stbi_load_from_memory (impl in IBL.cpp)

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <unistd.h>

namespace {
std::string ExecutableDir() {
  char result[PATH_MAX];
  ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
  std::string path(result, (count > 0) ? count : 0);
  return std::filesystem::path(path).parent_path().string();
}
} // namespace

bool UnderlayRenderer::Initialize(VkDevice device,
                                  VkPhysicalDevice physicalDevice,
                                  VkRenderPass renderPass,
                                  VkQueue graphicsQueue, uint32_t queueFamily,
                                  VkCommandPool commandPool,
                                  VkSampleCountFlagBits samples) {
  m_device = device;
  m_physicalDevice = physicalDevice;
  m_renderPass = renderPass;
  m_msaaSamples = samples;
  m_graphicsQueue = graphicsQueue;
  m_queueFamily = queueFamily;
  m_commandPool = commandPool;

  if (!CreateDescriptors() || !CreatePipeline()) {
    LOG_ERROR("UnderlayRenderer: initialization failed");
    return false;
  }
  m_initialized = true;
  return true;
}

uint32_t UnderlayRenderer::FindMemoryType(uint32_t typeFilter,
                                          VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
  for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
    if ((typeFilter & (1 << i)) &&
        (memProps.memoryTypes[i].propertyFlags & props) == props)
      return i;
  }
  throw std::runtime_error("UnderlayRenderer: no suitable memory type");
}

bool UnderlayRenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                    VkMemoryPropertyFlags props,
                                    VkBuffer& buffer, VkDeviceMemory& memory) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(m_device, &bi, nullptr, &buffer) != VK_SUCCESS)
    return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(m_device, buffer, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
  if (vkAllocateMemory(m_device, &ai, nullptr, &memory) != VK_SUCCESS)
    return false;
  vkBindBufferMemory(m_device, buffer, memory, 0);
  return true;
}

bool UnderlayRenderer::CreateDescriptors() {
  // Set 0: UBO (view/proj), vertex stage, per-frame.
  VkDescriptorSetLayoutBinding uboBinding{};
  uboBinding.binding = 0;
  uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboBinding.descriptorCount = 1;
  uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  VkDescriptorSetLayoutCreateInfo uboLayout{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  uboLayout.bindingCount = 1;
  uboLayout.pBindings = &uboBinding;
  if (vkCreateDescriptorSetLayout(m_device, &uboLayout, nullptr,
                                  &m_uboSetLayout) != VK_SUCCESS)
    return false;

  // Set 1: combined image sampler, fragment stage, one per texture.
  VkDescriptorSetLayoutBinding texBinding{};
  texBinding.binding = 0;
  texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  texBinding.descriptorCount = 1;
  texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo texLayout{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  texLayout.bindingCount = 1;
  texLayout.pBindings = &texBinding;
  if (vkCreateDescriptorSetLayout(m_device, &texLayout, nullptr,
                                  &m_texSetLayout) != VK_SUCCESS)
    return false;

  // Per-frame UBO buffers (host visible, persistently mapped).
  for (int i = 0; i < kFramesInFlight; i++) {
    DynamicBuffer& u = m_uniformBuffers[i];
    if (!CreateBuffer(sizeof(Ubo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      u.buffer, u.memory))
      return false;
    vkMapMemory(m_device, u.memory, 0, sizeof(Ubo), 0, &u.mapped);
    u.capacity = sizeof(Ubo);
  }

  // UBO descriptor pool + sets (one per frame).
  VkDescriptorPoolSize uboSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                               kFramesInFlight};
  VkDescriptorPoolCreateInfo uboPool{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  uboPool.poolSizeCount = 1;
  uboPool.pPoolSizes = &uboSize;
  uboPool.maxSets = kFramesInFlight;
  if (vkCreateDescriptorPool(m_device, &uboPool, nullptr, &m_uboPool) !=
      VK_SUCCESS)
    return false;
  std::array<VkDescriptorSetLayout, kFramesInFlight> uboLayouts{m_uboSetLayout,
                                                                m_uboSetLayout};
  VkDescriptorSetAllocateInfo uboAlloc{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  uboAlloc.descriptorPool = m_uboPool;
  uboAlloc.descriptorSetCount = kFramesInFlight;
  uboAlloc.pSetLayouts = uboLayouts.data();
  if (vkAllocateDescriptorSets(m_device, &uboAlloc, m_uboSets) != VK_SUCCESS)
    return false;
  for (int i = 0; i < kFramesInFlight; i++) {
    VkDescriptorBufferInfo info{m_uniformBuffers[i].buffer, 0, sizeof(Ubo)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_uboSets[i];
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
  }

  // Texture descriptor pool (grows to kMaxTextures; reset in ReleaseTextures).
  VkDescriptorPoolSize texSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               kMaxTextures};
  VkDescriptorPoolCreateInfo texPool{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  texPool.poolSizeCount = 1;
  texPool.pPoolSizes = &texSize;
  texPool.maxSets = kMaxTextures;
  if (vkCreateDescriptorPool(m_device, &texPool, nullptr, &m_texPool) !=
      VK_SUCCESS)
    return false;

  // One shared sampler (linear, clamp to edge).
  VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samp.magFilter = VK_FILTER_LINEAR;
  samp.minFilter = VK_FILTER_LINEAR;
  samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samp.maxLod = 0.0f;
  if (vkCreateSampler(m_device, &samp, nullptr, &m_sampler) != VK_SUCCESS)
    return false;
  return true;
}

VkShaderModule UnderlayRenderer::LoadShader(const std::string& spvName) {
  std::string path = ExecutableDir() + "/libraries/shaders/" + spvName;
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open())
    throw std::runtime_error("UnderlayRenderer: cannot open " + path);
  size_t size = file.tellg();
  std::vector<char> code(size);
  file.seekg(0);
  file.read(code.data(), size);
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule module;
  if (vkCreateShaderModule(m_device, &ci, nullptr, &module) != VK_SUCCESS)
    throw std::runtime_error("UnderlayRenderer: shader module creation failed");
  return module;
}

bool UnderlayRenderer::CreatePipeline() {
  VkShaderModule vert = LoadShader("underlay_vert.spv");
  VkShaderModule frag = LoadShader("underlay_frag.spv");
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{};
  binding.binding = 0;
  binding.stride = sizeof(Vertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 2> attrs{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)};
  VkPipelineVertexInputStateCreateInfo vin{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vin.vertexBindingDescriptionCount = 1;
  vin.pVertexBindingDescriptions = &binding;
  vin.vertexAttributeDescriptionCount = attrs.size();
  vin.pVertexAttributeDescriptions = attrs.data();

  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.lineWidth = 1.0f;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo msaa{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  msaa.rasterizationSamples = m_msaaSamples;

  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  VkPipelineColorBlendStateCreateInfo colorBlend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blend;

  // Drawn first in the overlay pass with depth test OFF: the underlay always
  // shows, and the sketch grid + lines (drawn after) paint on top of it. This
  // also sidesteps any co-planar depth fight with the sketch plane fill.
  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_FALSE;
  depth.depthWriteEnable = VK_FALSE;
  depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dyn;

  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  push.offset = 0;
  push.size = sizeof(float);
  std::array<VkDescriptorSetLayout, 2> setLayouts{m_uboSetLayout,
                                                  m_texSetLayout};
  VkPipelineLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount = setLayouts.size();
  layoutInfo.pSetLayouts = setLayouts.data();
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr,
                             &m_pipelineLayout) != VK_SUCCESS)
    return false;

  VkGraphicsPipelineCreateInfo info{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vin;
  info.pInputAssemblyState = &ia;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &msaa;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &colorBlend;
  info.pDynamicState = &dynamic;
  info.layout = m_pipelineLayout;
  info.renderPass = m_renderPass;
  info.subpass = 0;
  bool ok = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info,
                                      nullptr, &m_pipeline) == VK_SUCCESS;
  vkDestroyShaderModule(m_device, vert, nullptr);
  vkDestroyShaderModule(m_device, frag, nullptr);
  return ok;
}

bool UnderlayRenderer::UploadTexture(const unsigned char* rgba, int w, int h,
                                     Texture& out) {
  const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;

  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory stagingMem = VK_NULL_HANDLE;
  if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    staging, stagingMem))
    return false;
  void* mapped = nullptr;
  vkMapMemory(m_device, stagingMem, 0, bytes, 0, &mapped);
  std::memcpy(mapped, rgba, bytes);
  vkUnmapMemory(m_device, stagingMem);

  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {uint32_t(w), uint32_t(h), 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM; // raw pixels, no gamma decode
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateImage(m_device, &imageInfo, nullptr, &out.image) != VK_SUCCESS) {
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
    return false;
  }
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(m_device, out.image, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex =
      FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(m_device, &ai, nullptr, &out.memory);
  vkBindImageMemory(m_device, out.image, out.memory, 0);

  // One-time command buffer: layout -> copy -> layout, submit, wait.
  VkCommandBufferAllocateInfo cbAlloc{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbAlloc.commandPool = m_commandPool;
  cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbAlloc.commandBufferCount = 1;
  VkCommandBuffer cb;
  vkAllocateCommandBuffers(m_device, &cbAlloc, &cb);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &begin);

  VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.image = out.image;
  toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  toDst.srcAccessMask = 0;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                       1, &toDst);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {uint32_t(w), uint32_t(h), 1};
  vkCmdCopyBufferToImage(cb, staging, out.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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
  vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(m_graphicsQueue);
  vkFreeCommandBuffers(m_device, m_commandPool, 1, &cb);
  vkDestroyBuffer(m_device, staging, nullptr);
  vkFreeMemory(m_device, stagingMem, nullptr);

  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewInfo.image = out.image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(m_device, &viewInfo, nullptr, &out.view) != VK_SUCCESS)
    return false;
  out.w = w;
  out.h = h;
  return true;
}

UnderlayRenderer::Texture* UnderlayRenderer::GetOrCreateTexture(
    const std::string& asset, const std::vector<unsigned char>* bytes) {
  auto it = m_textures.find(asset);
  if (it != m_textures.end())
    return &it->second;
  if (!bytes || bytes->empty())
    return nullptr;

  int w = 0, h = 0, comp = 0;
  stbi_uc* pixels = stbi_load_from_memory(bytes->data(),
                                          static_cast<int>(bytes->size()), &w,
                                          &h, &comp, STBI_rgb_alpha);
  if (!pixels) {
    LOG_WARN("UnderlayRenderer: failed to decode image '{}'", asset);
    return nullptr;
  }
  Texture tex;
  bool ok = UploadTexture(pixels, w, h, tex);
  stbi_image_free(pixels);
  if (!ok) {
    DestroyTexture(tex);
    return nullptr;
  }

  // Allocate this texture's descriptor set (set 1) and point it at the image.
  VkDescriptorSetAllocateInfo alloc{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = m_texPool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &m_texSetLayout;
  if (vkAllocateDescriptorSets(m_device, &alloc, &tex.set) != VK_SUCCESS) {
    LOG_WARN("UnderlayRenderer: texture descriptor pool exhausted");
    DestroyTexture(tex);
    return nullptr;
  }
  VkDescriptorImageInfo imgInfo{m_sampler, tex.view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = tex.set;
  write.dstBinding = 0;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.descriptorCount = 1;
  write.pImageInfo = &imgInfo;
  vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

  auto [ins, _] = m_textures.emplace(asset, tex);
  return &ins->second;
}

void UnderlayRenderer::SetImages(const std::vector<UnderlayDraw>& images) {
  m_verts.clear();
  m_draws.clear();
  for (const UnderlayDraw& d : images) {
    Texture* tex = GetOrCreateTexture(d.asset, d.bytes);
    if (!tex)
      continue;
    // Two triangles over the pixel corners (0,0),(W,0),(W,H),(0,H). The image's
    // +Y (pixel row) runs DOWN, but the plane's +V runs UP, so the texture V is
    // flipped (1-v) to keep the underlay upright: the image's top-left pixel
    // lands at the plane-top-left corner.
    const glm::vec2 uv0(0, 1), uv1(1, 1), uv2(1, 0), uv3(0, 0);
    const uint32_t first = static_cast<uint32_t>(m_verts.size());
    m_verts.push_back({d.corners[0], uv0});
    m_verts.push_back({d.corners[1], uv1});
    m_verts.push_back({d.corners[2], uv2});
    m_verts.push_back({d.corners[0], uv0});
    m_verts.push_back({d.corners[2], uv2});
    m_verts.push_back({d.corners[3], uv3});
    m_draws.push_back({first, tex->set, d.opacity});
  }
}

uint32_t UnderlayRenderer::UploadVerts(DynamicBuffer& buf,
                                       const std::vector<Vertex>& verts) {
  if (verts.empty())
    return 0;
  const VkDeviceSize bytes = verts.size() * sizeof(Vertex);
  if (bytes > buf.capacity) {
    DestroyDynamic(buf);
    VkDeviceSize cap = bytes * 2;
    if (!CreateBuffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      buf.buffer, buf.memory))
      return 0;
    vkMapMemory(m_device, buf.memory, 0, cap, 0, &buf.mapped);
    buf.capacity = cap;
  }
  std::memcpy(buf.mapped, verts.data(), bytes);
  return static_cast<uint32_t>(verts.size());
}

void UnderlayRenderer::Render(VkCommandBuffer commandBuffer,
                              uint32_t currentFrame) {
  if (!m_initialized || m_draws.empty())
    return;
  const uint32_t frame = currentFrame % kFramesInFlight;

  Ubo ubo{m_view, m_proj};
  std::memcpy(m_uniformBuffers[frame].mapped, &ubo, sizeof(ubo));
  const uint32_t vertCount = UploadVerts(m_vertexBuffers[frame], m_verts);
  if (vertCount == 0)
    return;

  VkViewport vp{};
  vp.x = static_cast<float>(m_viewportX);
  vp.y = static_cast<float>(m_viewportY);
  vp.width = static_cast<float>(m_viewportWidth);
  vp.height = static_cast<float>(m_viewportHeight);
  vp.maxDepth = 1.0f;
  vkCmdSetViewport(commandBuffer, 0, 1, &vp);
  VkRect2D scissor{{m_viewportX, m_viewportY},
                   {m_viewportWidth, m_viewportHeight}};
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipelineLayout, 0, 1, &m_uboSets[frame], 0, nullptr);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffers[frame].buffer,
                         &offset);
  for (const DrawRec& d : m_draws) {
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 1, 1, &d.texSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float),
                       &d.opacity);
    vkCmdDraw(commandBuffer, 6, 1, d.firstVertex, 0);
  }
}

void UnderlayRenderer::DestroyTexture(Texture& tex) {
  if (tex.view) vkDestroyImageView(m_device, tex.view, nullptr);
  if (tex.image) vkDestroyImage(m_device, tex.image, nullptr);
  if (tex.memory) vkFreeMemory(m_device, tex.memory, nullptr);
  tex = Texture{};
}

void UnderlayRenderer::ReleaseTextures() {
  if (m_device == VK_NULL_HANDLE || m_textures.empty())
    return;
  vkDeviceWaitIdle(m_device); // never free a texture a frame may still sample
  for (auto& [name, tex] : m_textures)
    DestroyTexture(tex);
  m_textures.clear();
  if (m_texPool)
    vkResetDescriptorPool(m_device, m_texPool, 0); // frees all texture sets
  m_verts.clear();
  m_draws.clear();
}

void UnderlayRenderer::DestroyDynamic(DynamicBuffer& buf) {
  if (buf.mapped) {
    vkUnmapMemory(m_device, buf.memory);
    buf.mapped = nullptr;
  }
  if (buf.buffer) {
    vkDestroyBuffer(m_device, buf.buffer, nullptr);
    buf.buffer = VK_NULL_HANDLE;
  }
  if (buf.memory) {
    vkFreeMemory(m_device, buf.memory, nullptr);
    buf.memory = VK_NULL_HANDLE;
  }
  buf.capacity = 0;
}

void UnderlayRenderer::Shutdown() {
  if (m_device == VK_NULL_HANDLE)
    return;
  vkDeviceWaitIdle(m_device);
  for (auto& [name, tex] : m_textures)
    DestroyTexture(tex);
  m_textures.clear();
  for (int i = 0; i < kFramesInFlight; i++) {
    DestroyDynamic(m_uniformBuffers[i]);
    DestroyDynamic(m_vertexBuffers[i]);
  }
  if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
  if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
  if (m_pipelineLayout)
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  if (m_texPool) vkDestroyDescriptorPool(m_device, m_texPool, nullptr);
  if (m_uboPool) vkDestroyDescriptorPool(m_device, m_uboPool, nullptr);
  if (m_texSetLayout)
    vkDestroyDescriptorSetLayout(m_device, m_texSetLayout, nullptr);
  if (m_uboSetLayout)
    vkDestroyDescriptorSetLayout(m_device, m_uboSetLayout, nullptr);
  m_device = VK_NULL_HANDLE;
  m_initialized = false;
}
