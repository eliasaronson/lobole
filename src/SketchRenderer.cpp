#include "SketchRenderer.h"

#include "Logger.h"
#include "MeshRenderer.h" // Vertex3D

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

bool SketchRenderer::Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                VkRenderPass renderPass, VkQueue graphicsQueue,
                                uint32_t queueFamily,
                                VkCommandPool commandPool) {
  m_device = device;
  m_physicalDevice = physicalDevice;
  m_renderPass = renderPass;
  m_graphicsQueue = graphicsQueue;
  m_queueFamily = queueFamily;
  m_commandPool = commandPool;

  if (!CreateDescriptors() || !CreatePipelines()) {
    LOG_ERROR("SketchRenderer: initialization failed");
    return false;
  }
  m_initialized = true;
  return true;
}

uint32_t SketchRenderer::FindMemoryType(uint32_t typeFilter,
                                        VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
  for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
    if ((typeFilter & (1 << i)) &&
        (memProps.memoryTypes[i].propertyFlags & props) == props) {
      return i;
    }
  }
  throw std::runtime_error("SketchRenderer: no suitable memory type");
}

bool SketchRenderer::CreateDescriptors() {
  VkDescriptorSetLayoutBinding uboBinding{};
  uboBinding.binding = 0;
  uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboBinding.descriptorCount = 1;
  uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 1;
  layoutInfo.pBindings = &uboBinding;
  if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                  &m_descriptorSetLayout) != VK_SUCCESS)
    return false;

  // Per-frame uniform buffers (host visible, persistently mapped)
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

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSize.descriptorCount = kFramesInFlight;
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  poolInfo.maxSets = kFramesInFlight;
  if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) !=
      VK_SUCCESS)
    return false;

  std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{
      m_descriptorSetLayout, m_descriptorSetLayout};
  VkDescriptorSetAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = m_descriptorPool;
  alloc.descriptorSetCount = kFramesInFlight;
  alloc.pSetLayouts = layouts.data();
  if (vkAllocateDescriptorSets(m_device, &alloc, m_descriptorSets) != VK_SUCCESS)
    return false;

  for (int i = 0; i < kFramesInFlight; i++) {
    VkDescriptorBufferInfo info{};
    info.buffer = m_uniformBuffers[i].buffer;
    info.range = sizeof(Ubo);
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

VkShaderModule SketchRenderer::LoadShader(const std::string& spvName) {
  std::string path = ExecutableDir() + "/libraries/shaders/" + spvName;
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open())
    throw std::runtime_error("SketchRenderer: cannot open " + path);
  size_t size = file.tellg();
  std::vector<char> code(size);
  file.seekg(0);
  file.read(code.data(), size);

  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule module;
  if (vkCreateShaderModule(m_device, &ci, nullptr, &module) != VK_SUCCESS)
    throw std::runtime_error("SketchRenderer: shader module creation failed");
  return module;
}

bool SketchRenderer::CreatePipelines() {
  VkShaderModule vert = LoadShader("sketch_vert.spv");
  VkShaderModule frag = LoadShader("sketch_frag.spv");

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
  binding.stride = sizeof(Vertex3D);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 3> attrs{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, position)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, normal)};
  attrs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex3D, color)};
  VkPipelineVertexInputStateCreateInfo vertexInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = attrs.size();
  vertexInput.pVertexAttributeDescriptions = attrs.data();

  VkPipelineViewportStateCreateInfo viewport{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineMultisampleStateCreateInfo msaa{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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

  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_FALSE; // overlay geometry, don't occlude
  depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dyn;

  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  push.offset = 0;
  push.size = sizeof(glm::vec4);
  VkPipelineLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &m_descriptorSetLayout;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr,
                             &m_pipelineLayout) != VK_SUCCESS)
    return false;

  auto makePipeline = [&](VkPrimitiveTopology topology, bool depthBias,
                          VkPipeline& out) -> bool {
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topology;

    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (depthBias) {
      raster.depthBiasEnable = VK_TRUE;
      raster.depthBiasConstantFactor = -1.0f; // pull lines toward the camera
      raster.depthBiasSlopeFactor = -1.0f;
    }

    VkGraphicsPipelineCreateInfo info{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
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
    return vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr,
                                     &out) == VK_SUCCESS;
  };

  bool ok = makePipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, true, m_linePipeline) &&
            makePipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false,
                         m_quadPipeline);

  vkDestroyShaderModule(m_device, vert, nullptr);
  vkDestroyShaderModule(m_device, frag, nullptr);
  return ok;
}

uint32_t SketchRenderer::Upload(DynamicBuffer& buf,
                                const std::vector<Vertex3D>& verts) {
  if (verts.empty())
    return 0;
  const VkDeviceSize needed = verts.size() * sizeof(Vertex3D);
  if (needed > buf.capacity) {
    DestroyDynamic(buf);
    VkDeviceSize cap = needed * 2; // grow with headroom
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = cap;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
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
  std::memcpy(buf.mapped, verts.data(), needed);
  return static_cast<uint32_t>(verts.size());
}

void SketchRenderer::DrawBuffer(VkCommandBuffer cb, VkPipeline pipeline,
                                DynamicBuffer& buf, uint32_t count,
                                float alpha) {
  if (count == 0)
    return;
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  glm::vec4 tint(1.0f, 1.0f, 1.0f, alpha);
  vkCmdPushConstants(cb, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(glm::vec4), &tint);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cb, 0, 1, &buf.buffer, &offset);
  vkCmdDraw(cb, count, 1, 0, 0);
}

void SketchRenderer::Render(VkCommandBuffer commandBuffer,
                            uint32_t currentFrame) {
  if (!m_initialized)
    return;
  const uint32_t frame = currentFrame % kFramesInFlight;

  Ubo ubo{m_view, m_proj};
  std::memcpy(m_uniformBuffers[frame].mapped, &ubo, sizeof(ubo));

  const uint32_t quadCount = Upload(m_quadBuffers[frame], m_quads);
  const uint32_t lineCount = Upload(m_lineBuffers[frame], m_staticLines);
  const uint32_t overlayCount = Upload(m_overlayBuffers[frame], m_overlayLines);
  if (quadCount == 0 && lineCount == 0 && overlayCount == 0)
    return;

  VkViewport vp{};
  vp.width = static_cast<float>(m_viewportWidth);
  vp.height = static_cast<float>(m_viewportHeight);
  vp.maxDepth = 1.0f;
  vkCmdSetViewport(commandBuffer, 0, 1, &vp);
  VkRect2D scissor{{0, 0}, {m_viewportWidth, m_viewportHeight}};
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipelineLayout, 0, 1, &m_descriptorSets[frame], 0,
                          nullptr);

  DrawBuffer(commandBuffer, m_quadPipeline, m_quadBuffers[frame], quadCount,
             0.14f);
  DrawBuffer(commandBuffer, m_linePipeline, m_lineBuffers[frame], lineCount,
             1.0f);
  DrawBuffer(commandBuffer, m_linePipeline, m_overlayBuffers[frame],
             overlayCount, 1.0f);
}

void SketchRenderer::DestroyDynamic(DynamicBuffer& buf) {
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

void SketchRenderer::Shutdown() {
  if (m_device == VK_NULL_HANDLE)
    return;
  vkDeviceWaitIdle(m_device);
  for (int i = 0; i < kFramesInFlight; i++) {
    DestroyDynamic(m_uniformBuffers[i]);
    DestroyDynamic(m_lineBuffers[i]);
    DestroyDynamic(m_overlayBuffers[i]);
    DestroyDynamic(m_quadBuffers[i]);
  }
  if (m_linePipeline)
    vkDestroyPipeline(m_device, m_linePipeline, nullptr);
  if (m_quadPipeline)
    vkDestroyPipeline(m_device, m_quadPipeline, nullptr);
  if (m_pipelineLayout)
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  if (m_descriptorPool)
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
  if (m_descriptorSetLayout)
    vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
  m_device = VK_NULL_HANDLE;
  m_initialized = false;
}
