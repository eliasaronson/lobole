#include "render/FieldRenderer.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

bool FieldRenderer::Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                               VkRenderPass renderPass, VkQueue graphicsQueue,
                               uint32_t queueFamily, VkCommandPool commandPool,
                               VkSampleCountFlagBits samples) {
  m_device = device;
  m_physicalDevice = physicalDevice;
  m_renderPass = renderPass;
  m_msaaSamples = samples;
  m_graphicsQueue = graphicsQueue;
  m_queueFamily = queueFamily;
  m_commandPool = commandPool;
  if (!CreateDescriptors() || !CreatePipeline()) {
    LOG_ERROR("FieldRenderer: initialization failed");
    return false;
  }
  m_initialized = true;
  return true;
}

uint32_t FieldRenderer::FindMemoryType(uint32_t typeFilter,
                                       VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
  for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    if ((typeFilter & (1 << i)) &&
        (memProps.memoryTypes[i].propertyFlags & props) == props)
      return i;
  throw std::runtime_error("FieldRenderer: no suitable memory type");
}

bool FieldRenderer::CreateDescriptors() {
  VkDescriptorSetLayoutBinding uboBinding{};
  uboBinding.binding = 0;
  uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboBinding.descriptorCount = 1;
  uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  VkDescriptorSetLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layoutInfo.bindingCount = 1;
  layoutInfo.pBindings = &uboBinding;
  if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                  &m_descriptorSetLayout) != VK_SUCCESS)
    return false;

  for (int i = 0; i < kFramesInFlight; i++) {
    DynamicBuffer& u = m_uniformBuffers[i];
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = sizeof(Ubo);
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bi, nullptr, &u.buffer) != VK_SUCCESS)
      return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, u.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &u.memory) != VK_SUCCESS)
      return false;
    vkBindBufferMemory(m_device, u.buffer, u.memory, 0);
    vkMapMemory(m_device, u.memory, 0, sizeof(Ubo), 0, &u.mapped);
    u.capacity = sizeof(Ubo);
  }

  VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight};
  VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  poolInfo.maxSets = kFramesInFlight;
  if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) !=
      VK_SUCCESS)
    return false;

  std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{m_descriptorSetLayout,
                                                             m_descriptorSetLayout};
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = m_descriptorPool;
  alloc.descriptorSetCount = kFramesInFlight;
  alloc.pSetLayouts = layouts.data();
  if (vkAllocateDescriptorSets(m_device, &alloc, m_descriptorSets) != VK_SUCCESS)
    return false;
  for (int i = 0; i < kFramesInFlight; i++) {
    VkDescriptorBufferInfo info{m_uniformBuffers[i].buffer, 0, sizeof(Ubo)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_descriptorSets[i];
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
  }
  return true;
}

VkShaderModule FieldRenderer::LoadShader(const std::string& spvName) {
  const std::string path = (RuntimePaths::ShaderDir() / spvName).string();
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open())
    throw std::runtime_error("FieldRenderer: cannot open " + path);
  size_t size = file.tellg();
  std::vector<char> code(size);
  file.seekg(0);
  file.read(code.data(), size);
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule module;
  if (vkCreateShaderModule(m_device, &ci, nullptr, &module) != VK_SUCCESS)
    throw std::runtime_error("FieldRenderer: shader module creation failed");
  return module;
}

bool FieldRenderer::CreatePipeline() {
  VkShaderModule vert = LoadShader("field_vert.spv");
  VkShaderModule frag = LoadShader("field_frag.spv");
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
  binding.stride = sizeof(FieldVertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 3> attrs{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FieldVertex, position)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FieldVertex, displacement)};
  attrs[2] = {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(FieldVertex, scalar)};
  VkPipelineVertexInputStateCreateInfo vin{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vin.vertexBindingDescriptionCount = 1;
  vin.pVertexBindingDescriptions = &binding;
  vin.vertexAttributeDescriptionCount = attrs.size();
  vin.pVertexAttributeDescriptions = attrs.data();

  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vp{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.lineWidth = 1.0f;
  raster.cullMode = VK_CULL_MODE_NONE; // show both sides (deformed/ clipped)
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo msaa{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  msaa.rasterizationSamples = m_msaaSamples;

  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blend.blendEnable = VK_FALSE;
  VkPipelineColorBlendStateCreateInfo colorBlend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blend;

  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE; // stands in for the body: occlude normally
  depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dyn;

  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float)};
  VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &m_descriptorSetLayout;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) !=
      VK_SUCCESS)
    return false;

  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vin;
  info.pInputAssemblyState = &ia;
  info.pViewportState = &vp;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &msaa;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &colorBlend;
  info.pDynamicState = &dynamic;
  info.layout = m_pipelineLayout;
  info.renderPass = m_renderPass;
  info.subpass = 0;
  const bool ok = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info,
                                            nullptr, &m_pipeline) == VK_SUCCESS;
  vkDestroyShaderModule(m_device, vert, nullptr);
  vkDestroyShaderModule(m_device, frag, nullptr);
  return ok;
}

void FieldRenderer::EnsureCapacity(DynamicBuffer& buf, VkDeviceSize bytes,
                                   VkBufferUsageFlags usage) {
  if (bytes <= buf.capacity) return;
  DestroyDynamic(buf);
  VkDeviceSize cap = bytes * 2;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = cap;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  vkCreateBuffer(m_device, &bi, nullptr, &buf.buffer);
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(m_device, buf.buffer, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex =
      FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vkAllocateMemory(m_device, &ai, nullptr, &buf.memory);
  vkBindBufferMemory(m_device, buf.buffer, buf.memory, 0);
  vkMapMemory(m_device, buf.memory, 0, cap, 0, &buf.mapped);
  buf.capacity = cap;
}

void FieldRenderer::Render(VkCommandBuffer commandBuffer, uint32_t currentFrame) {
  if (!m_initialized || m_indices.empty()) return;
  const uint32_t frame = currentFrame % kFramesInFlight;

  Ubo ubo{m_view, m_proj, m_clipPlane};
  std::memcpy(m_uniformBuffers[frame].mapped, &ubo, sizeof(ubo));

  const VkDeviceSize vbytes = m_vertices.size() * sizeof(FieldVertex);
  const VkDeviceSize ibytes = m_indices.size() * sizeof(uint32_t);
  EnsureCapacity(m_vertexBuffers[frame], vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  EnsureCapacity(m_indexBuffers[frame], ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  std::memcpy(m_vertexBuffers[frame].mapped, m_vertices.data(), vbytes);
  std::memcpy(m_indexBuffers[frame].mapped, m_indices.data(), ibytes);

  VkViewport vp{};
  vp.x = static_cast<float>(m_vx);
  vp.y = static_cast<float>(m_vy);
  vp.width = static_cast<float>(m_vw);
  vp.height = static_cast<float>(m_vh);
  vp.maxDepth = 1.0f;
  vkCmdSetViewport(commandBuffer, 0, 1, &vp);
  VkRect2D scissor{{m_vx, m_vy}, {m_vw, m_vh}};
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipelineLayout, 0, 1, &m_descriptorSets[frame], 0,
                          nullptr);
  vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                     sizeof(float), &m_deformScale);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffers[frame].buffer,
                         &offset);
  vkCmdBindIndexBuffer(commandBuffer, m_indexBuffers[frame].buffer, 0,
                       VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(m_indices.size()), 1, 0, 0,
                   0);
}

void FieldRenderer::DestroyDynamic(DynamicBuffer& buf) {
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

void FieldRenderer::Shutdown() {
  if (!m_device) return;
  vkDeviceWaitIdle(m_device);
  for (int i = 0; i < kFramesInFlight; i++) {
    DestroyDynamic(m_uniformBuffers[i]);
    DestroyDynamic(m_vertexBuffers[i]);
    DestroyDynamic(m_indexBuffers[i]);
  }
  if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
  if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  if (m_descriptorPool) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
  if (m_descriptorSetLayout)
    vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
  m_pipeline = VK_NULL_HANDLE;
  m_pipelineLayout = VK_NULL_HANDLE;
  m_descriptorPool = VK_NULL_HANDLE;
  m_descriptorSetLayout = VK_NULL_HANDLE;
  m_device = VK_NULL_HANDLE;
  m_initialized = false;
}
