// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "render/SketchRenderer.h"

#include "render/VulkanCommon.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "render/MeshRenderer.h" // LineVertex3D

#include <array>
#include <cstring>
#include <fstream>

bool SketchRenderer::Initialize(VkDevice device,
                                VkPhysicalDevice physicalDevice,
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

  if (!CreateDescriptors() || !CreatePipelines()) {
    LOG_ERROR("SketchRenderer: initialization failed");
    return false;
  }
  m_initialized = true;
  return true;
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
    if (!vkutil::CreateBuffer(m_device, m_physicalDevice, sizeof(Ubo),
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              u.buffer, u.memory)) {
      return false;
    }
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

  std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
  layouts.fill(m_descriptorSetLayout);
  VkDescriptorSetAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = m_descriptorPool;
  alloc.descriptorSetCount = kFramesInFlight;
  alloc.pSetLayouts = layouts.data();
  if (vkAllocateDescriptorSets(m_device, &alloc, m_descriptorSets) !=
      VK_SUCCESS)
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

bool SketchRenderer::CreatePipelines() {
  VkShaderModule vert = vkutil::LoadShaderModule(m_device, "sketch_vert.spv");
  VkShaderModule frag = vkutil::LoadShaderModule(m_device, "sketch_frag.spv");
  if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
    vkDestroyShaderModule(m_device, frag, nullptr);
    vkDestroyShaderModule(m_device, vert, nullptr);
    return false;
  }

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
  binding.stride = sizeof(LineVertex3D);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 2> attrs{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
              offsetof(LineVertex3D, position)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex3D, color)};
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

  auto makePipeline = [&](VkPipelineShaderStageCreateInfo* st,
                          const VkPipelineVertexInputStateCreateInfo* vin,
                          VkPrimitiveTopology topology, bool depthBias,
                          VkPipeline& out, bool depthTest = true) -> bool {
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topology;

    // A no-depth-test variant (the gizmo) draws over geometry so its handles
    // stay visible/grabbable; everything else keeps the shared depth state.
    VkPipelineDepthStencilStateCreateInfo depthLocal = depth;
    if (!depthTest)
      depthLocal.depthTestEnable = VK_FALSE;

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
    info.pStages = st;
    info.pVertexInputState = vin;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &msaa;
    info.pDepthStencilState = &depthLocal;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = m_pipelineLayout;
    info.renderPass = m_renderPass;
    info.subpass = 0;
    return vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info,
                                     nullptr, &out) == VK_SUCCESS;
  };

  bool ok =
      makePipeline(stages, &vertexInput, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, true,
                   m_linePipeline) &&
      makePipeline(stages, &vertexInput, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                   false, m_quadPipeline) &&
      makePipeline(stages, &vertexInput, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false,
                   m_gizmoPipeline, /*depthTest=*/false); // A2.5 on-top gizmo

  // Screen-space-expanded primitive pipeline (thick AA lines + point dots).
  VkShaderModule primVert =
      vkutil::LoadShaderModule(m_device, "sketch_prim_vert.spv");
  VkShaderModule primFrag =
      vkutil::LoadShaderModule(m_device, "sketch_prim_frag.spv");
  if (primVert == VK_NULL_HANDLE || primFrag == VK_NULL_HANDLE) {
    vkDestroyShaderModule(m_device, primFrag, nullptr);
    vkDestroyShaderModule(m_device, primVert, nullptr);
    return false;
  }
  VkPipelineShaderStageCreateInfo primStages[2] = {stages[0], stages[1]};
  primStages[0].module = primVert;
  primStages[1].module = primFrag;

  VkVertexInputBindingDescription primBinding{};
  primBinding.binding = 0;
  primBinding.stride = sizeof(PrimVertex);
  primBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 5> primAttrs{};
  primAttrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                  offsetof(PrimVertex, posThis)};
  primAttrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                  offsetof(PrimVertex, posOther)};
  primAttrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PrimVertex, corner)};
  primAttrs[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PrimVertex, params)};
  primAttrs[4] = {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                  offsetof(PrimVertex, color)};
  VkPipelineVertexInputStateCreateInfo primInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  primInput.vertexBindingDescriptionCount = 1;
  primInput.pVertexBindingDescriptions = &primBinding;
  primInput.vertexAttributeDescriptionCount = primAttrs.size();
  primInput.pVertexAttributeDescriptions = primAttrs.data();

  ok = ok &&
       makePipeline(primStages, &primInput, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                    true, m_primPipeline);
  // Same pipeline, depth test off — measurement annotations that must stay
  // visible where they pass through material.
  ok = ok &&
       makePipeline(primStages, &primInput, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                    true, m_primOnTopPipeline, /*depthTest=*/false);

  vkDestroyShaderModule(m_device, vert, nullptr);
  vkDestroyShaderModule(m_device, frag, nullptr);
  vkDestroyShaderModule(m_device, primVert, nullptr);
  vkDestroyShaderModule(m_device, primFrag, nullptr);
  return ok;
}

uint32_t SketchRenderer::UploadRaw(DynamicBuffer& buf, const void* data,
                                   VkDeviceSize bytes, uint32_t count) {
  if (count == 0)
    return 0;
  if (bytes > buf.capacity) {
    DestroyDynamic(buf);
    const VkDeviceSize cap = bytes * 2; // grow with headroom
    // Nothing here used to be checked, and the memcpy below writes through
    // buf.mapped — a failed grow was a null write, not a dropped frame.
    if (!vkutil::CreateBuffer(m_device, m_physicalDevice, cap,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              buf.buffer, buf.memory)) {
      return 0; // nothing drawn this frame rather than a null write
    }
    vkMapMemory(m_device, buf.memory, 0, cap, 0, &buf.mapped);
    buf.capacity = cap;
  }
  if (buf.mapped == nullptr)
    return 0;
  std::memcpy(buf.mapped, data, bytes);
  return count;
}

uint32_t SketchRenderer::Upload(DynamicBuffer& buf,
                                const std::vector<LineVertex3D>& verts) {
  return UploadRaw(buf, verts.data(), verts.size() * sizeof(LineVertex3D),
                   static_cast<uint32_t>(verts.size()));
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
  const uint32_t frame = render::FrameSlot(currentFrame, "SketchRenderer");

  Ubo ubo{m_view, m_proj,
          glm::vec4(static_cast<float>(m_viewportWidth),
                    static_cast<float>(m_viewportHeight), 0.0f, 0.0f)};
  std::memcpy(m_uniformBuffers[frame].mapped, &ubo, sizeof(ubo));

  const uint32_t quadCount = Upload(m_quadBuffers[frame], m_quads);
  const uint32_t lineCount = Upload(m_lineBuffers[frame], m_staticLines);
  const uint32_t overlayCount = Upload(m_overlayBuffers[frame], m_overlayLines);
  const uint32_t gizmoCount = Upload(m_gizmoBuffers[frame], m_gizmoLines);
  const uint32_t primCount =
      UploadRaw(m_primBuffers[frame], m_primitives.data(),
                m_primitives.size() * sizeof(PrimVertex),
                static_cast<uint32_t>(m_primitives.size()));
  const uint32_t onTopCount =
      UploadRaw(m_primOnTopBuffers[frame], m_overlayPrims.data(),
                m_overlayPrims.size() * sizeof(PrimVertex),
                static_cast<uint32_t>(m_overlayPrims.size()));
  // The cached list: copied into this frame's buffer only when the buffer
  // holds an older version. A frame buffer is reused only once the GPU is
  // done with it -- the same guarantee every per-frame upload above relies on.
  uint32_t cachedCount = 0;
  if (m_cachedPrims && !m_cachedPrims->empty()) {
    const auto n = static_cast<uint32_t>(m_cachedPrims->size());
    if (m_cachedUploaded[frame] != m_cachedVersion) {
      cachedCount = UploadRaw(m_cachedBuffers[frame], m_cachedPrims->data(),
                              m_cachedPrims->size() * sizeof(PrimVertex), n);
      m_cachedUploaded[frame] = cachedCount == n ? m_cachedVersion : 0;
      m_cachedUploads++;
    } else {
      cachedCount = n;
    }
  }
  if (quadCount == 0 && lineCount == 0 && overlayCount == 0 && primCount == 0 &&
      gizmoCount == 0 && onTopCount == 0 && cachedCount == 0)
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

  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipelineLayout, 0, 1, &m_descriptorSets[frame], 0,
                          nullptr);

  DrawBuffer(commandBuffer, m_quadPipeline, m_quadBuffers[frame], quadCount,
             0.14f);
  DrawBuffer(commandBuffer, m_linePipeline, m_lineBuffers[frame], lineCount,
             1.0f);
  DrawBuffer(commandBuffer, m_linePipeline, m_overlayBuffers[frame],
             overlayCount, 1.0f);
  // Expanded primitives (thick entity lines + dots) draw last, on top. The
  // cached list first: a backplot is the ground the rest is drawn over.
  DrawBuffer(commandBuffer, m_primPipeline, m_cachedBuffers[frame], cachedCount,
             1.0f);
  DrawBuffer(commandBuffer, m_primPipeline, m_primBuffers[frame], primCount,
             1.0f);
  // Gizmo handles draw absolutely last with depth test off (A2.5).
  DrawBuffer(commandBuffer, m_gizmoPipeline, m_gizmoBuffers[frame], gizmoCount,
             1.0f);
  // ...and beside them the on-top thick primitives (measurement dimensions).
  DrawBuffer(commandBuffer, m_primOnTopPipeline, m_primOnTopBuffers[frame],
             onTopCount, 1.0f);
}

void SketchRenderer::DestroyDynamic(DynamicBuffer& buf) {
  vkutil::DestroyBuffer(m_device, buf.buffer, buf.memory, &buf.mapped);
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
    DestroyDynamic(m_gizmoBuffers[i]);
    DestroyDynamic(m_quadBuffers[i]);
    DestroyDynamic(m_primBuffers[i]);
    DestroyDynamic(m_primOnTopBuffers[i]);
    DestroyDynamic(m_cachedBuffers[i]);
    m_cachedUploaded[i] = 0;
  }
  if (m_linePipeline)
    vkDestroyPipeline(m_device, m_linePipeline, nullptr);
  if (m_gizmoPipeline)
    vkDestroyPipeline(m_device, m_gizmoPipeline, nullptr);
  if (m_quadPipeline)
    vkDestroyPipeline(m_device, m_quadPipeline, nullptr);
  if (m_primPipeline)
    vkDestroyPipeline(m_device, m_primPipeline, nullptr);
  if (m_primOnTopPipeline)
    vkDestroyPipeline(m_device, m_primOnTopPipeline, nullptr);
  if (m_pipelineLayout)
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  if (m_descriptorPool)
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
  if (m_descriptorSetLayout)
    vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
  m_device = VK_NULL_HANDLE;
  m_initialized = false;
}
