#include "render/OutlineRenderer.h"
#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "render/MeshRenderer.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

OutlineRenderer::OutlineRenderer() {}

OutlineRenderer::~OutlineRenderer() { Shutdown(); }

bool OutlineRenderer::Initialize(VkDevice device,
                                 VkPhysicalDevice physicalDevice,
                                 VkRenderPass renderPass, VkQueue graphicsQueue,
                                 uint32_t queueFamily,
                                 VkCommandPool commandPool,
                                 VkSampleCountFlagBits samples) {
  m_msaaSamples = samples;
  m_device = device;
  m_physicalDevice = physicalDevice;
  m_renderPass = renderPass;
  m_graphicsQueue = graphicsQueue;
  m_queueFamily = queueFamily;
  m_commandPool = commandPool;

  LOG_INFO("OutlineRenderer: Initializing...");

  // Create descriptor set layout
  if (!CreateDescriptorSetLayout()) {
    LOG_ERROR("OutlineRenderer: Failed to create descriptor set layout");
    return false;
  }

  // Create pipelines
  if (!CreateOutlinePipeline()) {
    LOG_ERROR("OutlineRenderer: Failed to create outline pipeline");
    return false;
  }

  if (!CreateWireframePipeline()) {
    LOG_ERROR("OutlineRenderer: Failed to create wireframe pipeline");
    return false;
  }

  // Create uniform buffers
  if (!CreateUniformBuffers()) {
    LOG_ERROR("OutlineRenderer: Failed to create uniform buffers");
    return false;
  }

  // Create descriptor pool and sets
  if (!CreateDescriptorPool()) {
    LOG_ERROR("OutlineRenderer: Failed to create descriptor pool");
    return false;
  }

  if (!CreateDescriptorSets()) {
    LOG_ERROR("OutlineRenderer: Failed to create descriptor sets");
    return false;
  }

  m_isInitialized = true;
  LOG_INFO("OutlineRenderer: Initialization successful!");

  return true;
}

void OutlineRenderer::Shutdown() {
  if (m_device == VK_NULL_HANDLE)
    return;

  vkDeviceWaitIdle(m_device);

  // Cleanup meshes
  for (auto& mesh : m_meshes) {
    if (mesh) {
      if (mesh->vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, mesh->vertexBuffer, nullptr);
        vkFreeMemory(m_device, mesh->vertexBufferMemory, nullptr);
      }
      if (mesh->indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, mesh->indexBuffer, nullptr);
        vkFreeMemory(m_device, mesh->indexBufferMemory, nullptr);
      }
      for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        if (mesh->edgeIndexBuffers[f] != VK_NULL_HANDLE) {
          vkDestroyBuffer(m_device, mesh->edgeIndexBuffers[f], nullptr);
          vkFreeMemory(m_device, mesh->edgeIndexBufferMemory[f], nullptr);
        }
      }
    }
  }

  // Cleanup uniform buffers
  for (size_t i = 0; i < m_outlineUniformBuffers.size(); i++) {
    vkDestroyBuffer(m_device, m_outlineUniformBuffers[i], nullptr);
    vkFreeMemory(m_device, m_outlineUniformBuffersMemory[i], nullptr);
    vkDestroyBuffer(m_device, m_wireframeUniformBuffers[i], nullptr);
    vkFreeMemory(m_device, m_wireframeUniformBuffersMemory[i], nullptr);
  }

  // Cleanup Vulkan objects
  if (m_descriptorPool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
  }
  if (m_outlinePipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(m_device, m_outlinePipeline, nullptr);
  }
  if (m_wireframePipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(m_device, m_wireframePipeline, nullptr);
  }
  if (m_outlinePipelineLayout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(m_device, m_outlinePipelineLayout, nullptr);
  }
  if (m_wireframePipelineLayout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(m_device, m_wireframePipelineLayout, nullptr);
  }
  if (m_outlineDescriptorSetLayout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(m_device, m_outlineDescriptorSetLayout,
                                 nullptr);
  }
  if (m_wireframeDescriptorSetLayout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(m_device, m_wireframeDescriptorSetLayout,
                                 nullptr);
  }

  m_isInitialized = false;
}

bool OutlineRenderer::CreateDescriptorSetLayout() {
  // Same layout for both outline and wireframe
  VkDescriptorSetLayoutBinding uboLayoutBinding{};
  uboLayoutBinding.binding = 0;
  uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboLayoutBinding.descriptorCount = 1;
  uboLayoutBinding.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 1;
  layoutInfo.pBindings = &uboLayoutBinding;

  if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                  &m_outlineDescriptorSetLayout) !=
          VK_SUCCESS ||
      vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                  &m_wireframeDescriptorSetLayout) !=
          VK_SUCCESS) {
    return false;
  }

  return true;
}

bool OutlineRenderer::CreateOutlinePipeline() {
  // Load shaders
  const std::string shaderDir = RuntimePaths::ShaderDir().string() + "/";
  auto vertShaderCode = ReadShaderFile(shaderDir + "outline_vert.spv");
  auto fragShaderCode = ReadShaderFile(shaderDir + "outline_frag.spv");

  VkShaderModule vertShaderModule = CreateShaderModule(vertShaderCode);
  VkShaderModule fragShaderModule = CreateShaderModule(fragShaderCode);

  VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
  vertShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertShaderStageInfo.module = vertShaderModule;
  vertShaderStageInfo.pName = "main";

  VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
  fragShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fragShaderStageInfo.module = fragShaderModule;
  fragShaderStageInfo.pName = "main";

  VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo,
                                                    fragShaderStageInfo};

  // Vertex input
  VkVertexInputBindingDescription bindingDescription{};
  bindingDescription.binding = 0;
  bindingDescription.stride = sizeof(Vertex3D);
  bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};
  attributeDescriptions[0].binding = 0;
  attributeDescriptions[0].location = 0;
  attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  attributeDescriptions[0].offset = offsetof(Vertex3D, position);

  attributeDescriptions[1].binding = 0;
  attributeDescriptions[1].location = 1;
  attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
  attributeDescriptions[1].offset = offsetof(Vertex3D, normal);

  VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
  vertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = 1;
  vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
  vertexInputInfo.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(attributeDescriptions.size());
  vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  // Viewport and scissor (dynamic)
  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_NONE; // No culling for outlines
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_FALSE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = m_msaaSamples;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_FALSE; // Disable depth testing for outlines
  depthStencil.depthWriteEnable = VK_FALSE; // Don't write depth for outlines
  depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.stencilTestEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState colorBlendAttachment{};
  colorBlendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment.blendEnable = VK_TRUE;
  colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  colorBlendAttachment.dstColorBlendFactor =
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;

  std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                               VK_DYNAMIC_STATE_SCISSOR};

  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
  dynamicState.pDynamicStates = dynamicStates.data();

  // Per-mesh model matrix (A2.5): a vertex-stage push constant so each mesh
  // draws at its own transform instead of the shared UBO identity.
  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pushRange.offset = 0;
  pushRange.size = sizeof(glm::mat4);

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &m_outlineDescriptorSetLayout;
  pipelineLayoutInfo.pushConstantRangeCount = 1;
  pipelineLayoutInfo.pPushConstantRanges = &pushRange;

  if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr,
                             &m_outlinePipelineLayout) != VK_SUCCESS) {
    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    return false;
  }

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = shaderStages;
  pipelineInfo.pVertexInputState = &vertexInputInfo;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = m_outlinePipelineLayout;
  pipelineInfo.renderPass = m_renderPass;
  pipelineInfo.subpass = 0;

  if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                nullptr, &m_outlinePipeline) != VK_SUCCESS) {
    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    return false;
  }

  vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
  vkDestroyShaderModule(m_device, vertShaderModule, nullptr);

  return true;
}

bool OutlineRenderer::CreateWireframePipeline() {
  // Load shaders
  const std::string shaderDir = RuntimePaths::ShaderDir().string() + "/";
  auto vertShaderCode = ReadShaderFile(shaderDir + "wireframe_vert.spv");
  auto fragShaderCode = ReadShaderFile(shaderDir + "wireframe_frag.spv");

  VkShaderModule vertShaderModule = CreateShaderModule(vertShaderCode);
  VkShaderModule fragShaderModule = CreateShaderModule(fragShaderCode);

  VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
  vertShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertShaderStageInfo.module = vertShaderModule;
  vertShaderStageInfo.pName = "main";

  VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
  fragShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fragShaderStageInfo.module = fragShaderModule;
  fragShaderStageInfo.pName = "main";

  VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo,
                                                    fragShaderStageInfo};

  // Vertex input (same as outline)
  VkVertexInputBindingDescription bindingDescription{};
  bindingDescription.binding = 0;
  bindingDescription.stride = sizeof(Vertex3D);
  bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};
  attributeDescriptions[0].binding = 0;
  attributeDescriptions[0].location = 0;
  attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  attributeDescriptions[0].offset = offsetof(Vertex3D, position);

  attributeDescriptions[1].binding = 0;
  attributeDescriptions[1].location = 1;
  attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
  attributeDescriptions[1].offset = offsetof(Vertex3D, normal);

  VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
  vertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = 1;
  vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
  vertexInputInfo.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(attributeDescriptions.size());
  vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology =
      VK_PRIMITIVE_TOPOLOGY_LINE_LIST; // Lines for wireframe
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  // Viewport and scissor (dynamic)
  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_LINE;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_TRUE;
  rasterizer.depthBiasConstantFactor = -1.0f; // Pull wireframe toward camera
  rasterizer.depthBiasClamp = 0.0f;
  rasterizer.depthBiasSlopeFactor = -1.0f;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = m_msaaSamples;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp =
      VK_COMPARE_OP_LESS; // Stricter depth test to hide back edges
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.stencilTestEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState colorBlendAttachment{};
  colorBlendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment.blendEnable = VK_TRUE;
  colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  colorBlendAttachment.dstColorBlendFactor =
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;

  std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                               VK_DYNAMIC_STATE_SCISSOR};

  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
  dynamicState.pDynamicStates = dynamicStates.data();

  VkPushConstantRange pushRange{}; // per-mesh model matrix (A2.5)
  pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pushRange.offset = 0;
  pushRange.size = sizeof(glm::mat4);

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &m_wireframeDescriptorSetLayout;
  pipelineLayoutInfo.pushConstantRangeCount = 1;
  pipelineLayoutInfo.pPushConstantRanges = &pushRange;

  if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr,
                             &m_wireframePipelineLayout) != VK_SUCCESS) {
    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    return false;
  }

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = shaderStages;
  pipelineInfo.pVertexInputState = &vertexInputInfo;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = m_wireframePipelineLayout;
  pipelineInfo.renderPass = m_renderPass;
  pipelineInfo.subpass = 0;

  if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                nullptr, &m_wireframePipeline) != VK_SUCCESS) {
    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    return false;
  }

  vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
  vkDestroyShaderModule(m_device, vertShaderModule, nullptr);

  return true;
}

// Placeholder implementations for the remaining methods
bool OutlineRenderer::CreateUniformBuffers() {
  VkDeviceSize outlineBufferSize = sizeof(OutlineUniformBufferObject);
  VkDeviceSize wireframeBufferSize = sizeof(WireframeUniformBufferObject);

  m_outlineUniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
  m_outlineUniformBuffersMemory.resize(MAX_FRAMES_IN_FLIGHT);
  m_wireframeUniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
  m_wireframeUniformBuffersMemory.resize(MAX_FRAMES_IN_FLIGHT);

  for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    m_outlineUniformBuffers[i] =
        CreateBuffer(outlineBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_outlineUniformBuffersMemory[i]);

    m_wireframeUniformBuffers[i] =
        CreateBuffer(wireframeBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_wireframeUniformBuffersMemory[i]);
  }

  return true;
}

bool OutlineRenderer::CreateDescriptorPool() {
  std::array<VkDescriptorPoolSize, 1> poolSizes{};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[0].descriptorCount =
      static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT * 2); // outline + wireframe

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT * 2);

  return vkCreateDescriptorPool(m_device, &poolInfo, nullptr,
                                &m_descriptorPool) == VK_SUCCESS;
}

bool OutlineRenderer::CreateDescriptorSets() {
  // Create outline descriptor sets
  std::vector<VkDescriptorSetLayout> outlineLayouts(
      MAX_FRAMES_IN_FLIGHT, m_outlineDescriptorSetLayout);
  VkDescriptorSetAllocateInfo outlineAllocInfo{};
  outlineAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  outlineAllocInfo.descriptorPool = m_descriptorPool;
  outlineAllocInfo.descriptorSetCount =
      static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
  outlineAllocInfo.pSetLayouts = outlineLayouts.data();

  m_outlineDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
  if (vkAllocateDescriptorSets(m_device, &outlineAllocInfo,
                               m_outlineDescriptorSets.data()) != VK_SUCCESS) {
    return false;
  }

  // Create wireframe descriptor sets
  std::vector<VkDescriptorSetLayout> wireframeLayouts(
      MAX_FRAMES_IN_FLIGHT, m_wireframeDescriptorSetLayout);
  VkDescriptorSetAllocateInfo wireframeAllocInfo{};
  wireframeAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  wireframeAllocInfo.descriptorPool = m_descriptorPool;
  wireframeAllocInfo.descriptorSetCount =
      static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
  wireframeAllocInfo.pSetLayouts = wireframeLayouts.data();

  m_wireframeDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
  if (vkAllocateDescriptorSets(m_device, &wireframeAllocInfo,
                               m_wireframeDescriptorSets.data()) !=
      VK_SUCCESS) {
    return false;
  }

  // Update descriptor sets
  for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    // Outline
    VkDescriptorBufferInfo outlineBufferInfo{};
    outlineBufferInfo.buffer = m_outlineUniformBuffers[i];
    outlineBufferInfo.offset = 0;
    outlineBufferInfo.range = sizeof(OutlineUniformBufferObject);

    VkWriteDescriptorSet outlineDescriptorWrite{};
    outlineDescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    outlineDescriptorWrite.dstSet = m_outlineDescriptorSets[i];
    outlineDescriptorWrite.dstBinding = 0;
    outlineDescriptorWrite.dstArrayElement = 0;
    outlineDescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    outlineDescriptorWrite.descriptorCount = 1;
    outlineDescriptorWrite.pBufferInfo = &outlineBufferInfo;

    // Wireframe
    VkDescriptorBufferInfo wireframeBufferInfo{};
    wireframeBufferInfo.buffer = m_wireframeUniformBuffers[i];
    wireframeBufferInfo.offset = 0;
    wireframeBufferInfo.range = sizeof(WireframeUniformBufferObject);

    VkWriteDescriptorSet wireframeDescriptorWrite{};
    wireframeDescriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wireframeDescriptorWrite.dstSet = m_wireframeDescriptorSets[i];
    wireframeDescriptorWrite.dstBinding = 0;
    wireframeDescriptorWrite.dstArrayElement = 0;
    wireframeDescriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wireframeDescriptorWrite.descriptorCount = 1;
    wireframeDescriptorWrite.pBufferInfo = &wireframeBufferInfo;

    std::array<VkWriteDescriptorSet, 2> descriptorWrites = {
        outlineDescriptorWrite, wireframeDescriptorWrite};
    vkUpdateDescriptorSets(m_device,
                           static_cast<uint32_t>(descriptorWrites.size()),
                           descriptorWrites.data(), 0, nullptr);
  }

  return true;
}

// Utility function implementations will be added in the next part...
uint32_t OutlineRenderer::AddMesh(const std::vector<Vertex3D>& vertices,
                                  const std::vector<uint32_t>& indices) {
  LOG_INFO("OutlineRenderer: Adding mesh with {} vertices", vertices.size());

  auto mesh = std::make_unique<OutlineMesh>();
  mesh->vertexCount = static_cast<uint32_t>(vertices.size());
  mesh->indexCount = static_cast<uint32_t>(indices.size());

  // Create vertex buffer
  VkDeviceSize vertexBufferSize = sizeof(Vertex3D) * vertices.size();
  VkBuffer stagingBuffer;
  VkDeviceMemory stagingBufferMemory;

  stagingBuffer =
      CreateBuffer(vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   stagingBufferMemory);

  void* data;
  vkMapMemory(m_device, stagingBufferMemory, 0, vertexBufferSize, 0, &data);
  memcpy(data, vertices.data(), (size_t)vertexBufferSize);
  vkUnmapMemory(m_device, stagingBufferMemory);

  mesh->vertexBuffer = CreateBuffer(
      vertexBufferSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mesh->vertexBufferMemory);

  CopyBuffer(stagingBuffer, mesh->vertexBuffer, vertexBufferSize);

  vkDestroyBuffer(m_device, stagingBuffer, nullptr);
  vkFreeMemory(m_device, stagingBufferMemory, nullptr);

  // Create index buffer
  VkDeviceSize indexBufferSize = sizeof(uint32_t) * indices.size();

  stagingBuffer =
      CreateBuffer(indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   stagingBufferMemory);

  vkMapMemory(m_device, stagingBufferMemory, 0, indexBufferSize, 0, &data);
  memcpy(data, indices.data(), (size_t)indexBufferSize);
  vkUnmapMemory(m_device, stagingBufferMemory);

  mesh->indexBuffer = CreateBuffer(
      indexBufferSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mesh->indexBufferMemory);

  CopyBuffer(stagingBuffer, mesh->indexBuffer, indexBufferSize);

  vkDestroyBuffer(m_device, stagingBuffer, nullptr);
  vkFreeMemory(m_device, stagingBufferMemory, nullptr);

  // Precompute edge adjacency once; classification per view is then a flat
  // pass (see OutlineEdges.h). The per-frame edge index buffers are sized to
  // the worst case any view can produce, so they are never reallocated.
  mesh->adjacency = outline::BuildEdgeAdjacency(
      vertices.empty() ? nullptr : &vertices[0].position, vertices.size(),
      sizeof(Vertex3D), indices.data(), indices.size());
  mesh->edgeIndices = mesh->adjacency.staticIndices;
  mesh->staticIndexCount = static_cast<uint32_t>(mesh->edgeIndices.size());

  const VkDeviceSize edgeCapacityBytes =
      sizeof(uint32_t) * mesh->adjacency.MaxIndexCount();
  if (edgeCapacityBytes > 0) {
    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
      mesh->edgeIndexBuffers[f] =
          CreateBuffer(edgeCapacityBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       mesh->edgeIndexBufferMemory[f]);
      vkMapMemory(m_device, mesh->edgeIndexBufferMemory[f], 0,
                  edgeCapacityBytes, 0, &mesh->edgeIndexMapped[f]);
    }
  }
  m_silhouettesDirty = true;

  uint32_t meshId = m_nextMeshId++;
  m_meshes.push_back(std::move(mesh));

  LOG_INFO("OutlineRenderer: Created mesh {} with {} vertices, {} static edge "
           "indices, {} silhouette candidates",
           meshId, vertices.size(), m_meshes.back()->staticIndexCount,
           m_meshes.back()->adjacency.smoothEdges.size());

  return meshId;
}

void OutlineRenderer::UpdateCamera(const glm::mat4& view,
                                   const glm::mat4& projection) {
  glm::mat4 prevView = m_viewMatrix;
  m_viewMatrix = view;
  m_projectionMatrix = projection;

  // Extract camera position from view matrix
  glm::mat4 invView = glm::inverse(view);
  glm::vec3 newCameraPosition = glm::vec3(invView[3]);

  // A significant move restarts the settle clock; silhouettes reclassify
  // once the camera has been still for silhouetteSettleMs (RefreshSilhouettes
  // via UpdateUniformBuffers). Static feature/boundary edges draw throughout.
  float cameraMoveDistance = glm::length(newCameraPosition - m_cameraPosition);
  float viewChangeAmount = glm::length(glm::vec3(view[0] - prevView[0]));

  if (cameraMoveDistance > 0.05f || viewChangeAmount > 0.05f) {
    m_lastCameraMove = std::chrono::steady_clock::now();
    m_silhouettesDirty = true;
  }

  m_cameraPosition = newCameraPosition;
}

void OutlineRenderer::Render(VkCommandBuffer commandBuffer,
                             uint32_t currentFrame) {
  if (!m_isInitialized || m_settings.mode == OutlineMode::NONE ||
      m_meshes.empty()) {
    return;
  }

  // Per-frame resources are sized to MAX_FRAMES_IN_FLIGHT, but the caller
  // passes the swapchain image index (0..ImageCount-1), which exceeds that on
  // a >2-image swapchain. Wrap to stay in bounds (matches SSAORenderer); the
  // raw index overruns the uniform-buffer/descriptor-set arrays otherwise.
  const uint32_t frame = currentFrame % MAX_FRAMES_IN_FLIGHT;

  // Update uniform buffers for current frame
  UpdateUniformBuffers(frame);

  // Render based on mode
  switch (m_settings.mode) {
  case OutlineMode::EDGES:
  case OutlineMode::SILHOUETTE:
    RenderOutlines(commandBuffer, frame);
    break;

  case OutlineMode::WIREFRAME:
    RenderWireframes(commandBuffer, frame);
    break;

  default:
    break;
  }
}

// Rest of utility functions (same as in MeshRenderer/RayTracingRenderer)
VkBuffer OutlineRenderer::CreateBuffer(VkDeviceSize size,
                                       VkBufferUsageFlags usage,
                                       VkMemoryPropertyFlags properties,
                                       VkDeviceMemory& bufferMemory) {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkBuffer buffer;
  if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to create buffer!");
  }

  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(m_device, buffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex =
      FindMemoryType(memRequirements.memoryTypeBits, properties);

  if (vkAllocateMemory(m_device, &allocInfo, nullptr, &bufferMemory) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to allocate buffer memory!");
  }

  vkBindBufferMemory(m_device, buffer, bufferMemory, 0);
  return buffer;
}

void OutlineRenderer::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer,
                                 VkDeviceSize size) {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandPool = m_commandPool;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer;
  vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  vkBeginCommandBuffer(commandBuffer, &beginInfo);

  VkBufferCopy copyRegion{};
  copyRegion.size = size;
  vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

  vkEndCommandBuffer(commandBuffer);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;

  vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(m_graphicsQueue);

  vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
}

uint32_t OutlineRenderer::FindMemoryType(uint32_t typeFilter,
                                         VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memProperties;
  vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

  for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
    if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags &
                                    properties) == properties) {
      return i;
    }
  }

  throw std::runtime_error("failed to find suitable memory type!");
}

std::vector<char> OutlineRenderer::ReadShaderFile(const std::string& filename) {
  std::ifstream file(filename, std::ios::ate | std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("failed to open file: " + filename);
  }

  size_t fileSize = (size_t)file.tellg();
  std::vector<char> buffer(fileSize);

  file.seekg(0);
  file.read(buffer.data(), fileSize);
  file.close();

  return buffer;
}

VkShaderModule
OutlineRenderer::CreateShaderModule(const std::vector<char>& code) {
  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = code.size();
  createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

  VkShaderModule shaderModule;
  if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create shader module!");
  }

  return shaderModule;
}

// Placeholder implementations for interface methods
void OutlineRenderer::UpdateMesh(uint32_t meshId,
                                 const std::vector<Vertex3D>& vertices,
                                 const std::vector<uint32_t>& indices) {
  // Placeholder
}

void OutlineRenderer::RemoveMesh(uint32_t meshId) {
  if (meshId == 0 || meshId > m_meshes.size())
    return;
  OutlineMesh* mesh = m_meshes[meshId - 1].get();
  if (!mesh)
    return;

  // Removal is rare (body delete/regen), so a full wait before freeing
  // buffers an in-flight frame may still reference is acceptable here —
  // unlike the old per-frame silhouette path, this never runs mid-orbit.
  vkDeviceWaitIdle(m_device);

  if (mesh->vertexBuffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(m_device, mesh->vertexBuffer, nullptr);
    vkFreeMemory(m_device, mesh->vertexBufferMemory, nullptr);
  }
  if (mesh->indexBuffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(m_device, mesh->indexBuffer, nullptr);
    vkFreeMemory(m_device, mesh->indexBufferMemory, nullptr);
  }
  for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
    if (mesh->edgeIndexBuffers[f] != VK_NULL_HANDLE) {
      vkDestroyBuffer(m_device, mesh->edgeIndexBuffers[f], nullptr);
      vkFreeMemory(m_device, mesh->edgeIndexBufferMemory[f], nullptr);
    }
  }
  m_meshes[meshId - 1].reset();
}

void OutlineRenderer::SetMeshTransform(uint32_t meshId,
                                       const glm::mat4& transform) {
  if (meshId > 0 && meshId <= m_meshes.size() && m_meshes[meshId - 1]) {
    if (m_meshes[meshId - 1]->transform != transform) {
      m_meshes[meshId - 1]->transform = transform;
      // Silhouettes are classified in mesh-local space against a transformed
      // camera, so a moved occurrence needs a fresh pass.
      m_silhouettesDirty = true;
    }
  }
}

void OutlineRenderer::SetViewport(int32_t x, int32_t y, uint32_t width,
                                  uint32_t height) {
  m_viewportX = x;
  m_viewportY = y;
  m_viewportWidth = width;
  m_viewportHeight = height;
}

void OutlineRenderer::DropToStaticEdges() {
  for (auto& mesh : m_meshes) {
    if (mesh && mesh->edgeIndices.size() != mesh->staticIndexCount) {
      mesh->edgeIndices.resize(mesh->staticIndexCount);
      mesh->edgeVersion++;
    }
  }
  m_silhouettesShown = false;
}

void OutlineRenderer::RefreshSilhouettes() {
  const auto t0 = std::chrono::steady_clock::now();
  size_t meshCount = 0;
  size_t silhouetteIndices = 0;
  for (auto& mesh : m_meshes) {
    if (!mesh || mesh->adjacency.smoothEdges.empty()) {
      // Nothing view-dependent; the static prefix (already in edgeIndices)
      // is the whole wireframe.
      continue;
    }
    mesh->edgeIndices.resize(mesh->staticIndexCount);
    const glm::vec3 cameraLocal = glm::vec3(glm::inverse(mesh->transform) *
                                            glm::vec4(m_cameraPosition, 1.0f));
    outline::AppendSilhouetteIndices(mesh->adjacency, cameraLocal,
                                     m_silhouetteScratch, mesh->edgeIndices);
    mesh->edgeVersion++;
    meshCount++;
    silhouetteIndices += mesh->edgeIndices.size() - mesh->staticIndexCount;
  }
  m_silhouettesShown = true;
  m_silhouettesDirty = false;

  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - t0);
  LOG_DEBUG("OutlineRenderer: silhouette pass: {} meshes, {} silhouette "
            "indices, {:.2f} ms",
            meshCount, silhouetteIndices, elapsed.count());
}

void OutlineRenderer::UpdateUniformBuffers(uint32_t currentFrame) {
  // Wireframe edge policy: while the camera moves, only the static
  // feature/boundary prefix draws; silhouettes are reclassified in one flat
  // pass once it has settled (or every change with liveSilhouettes).
  using clock = std::chrono::steady_clock;
  const bool settled = m_settings.liveSilhouettes ||
                       clock::now() - m_lastCameraMove >=
                           std::chrono::duration_cast<clock::duration>(
                               std::chrono::duration<float, std::milli>(
                                   m_settings.silhouetteSettleMs));

  if (!settled && m_silhouettesShown)
    DropToStaticEdges();
  if (settled && m_silhouettesDirty)
    RefreshSilhouettes();

  // Upload this frame slot's edge buffer where its content is stale. The
  // frame loop has already fence-waited this slot, so writing the mapped
  // buffer here cannot race an in-flight read.
  for (auto& mesh : m_meshes) {
    if (mesh && mesh->edgeIndexBuffers[currentFrame] != VK_NULL_HANDLE &&
        mesh->uploadedVersion[currentFrame] != mesh->edgeVersion) {
      memcpy(mesh->edgeIndexMapped[currentFrame], mesh->edgeIndices.data(),
             mesh->edgeIndices.size() * sizeof(uint32_t));
      mesh->uploadedIndexCount[currentFrame] =
          static_cast<uint32_t>(mesh->edgeIndices.size());
      mesh->uploadedVersion[currentFrame] = mesh->edgeVersion;
    }
  }

  // We need to update uniform buffer for each mesh individually, but for now
  // use identity
  // TODO: This should be updated per-mesh in the render loop
  glm::mat4 model = glm::mat4(1.0f);

  // Update outline uniform buffer
  OutlineUniformBufferObject outlineUBO{};
  outlineUBO.model = model;
  outlineUBO.view = m_viewMatrix;
  outlineUBO.proj = m_projectionMatrix;
  outlineUBO.viewPos = glm::vec4(m_cameraPosition, 1.0f);
  outlineUBO.outlineColor = m_settings.outlineColor;
  outlineUBO.outlineWidth = m_settings.outlineWidth;

  void* data;
  vkMapMemory(m_device, m_outlineUniformBuffersMemory[currentFrame], 0,
              sizeof(outlineUBO), 0, &data);
  memcpy(data, &outlineUBO, sizeof(outlineUBO));
  vkUnmapMemory(m_device, m_outlineUniformBuffersMemory[currentFrame]);

  // Update wireframe uniform buffer
  WireframeUniformBufferObject wireframeUBO{};
  wireframeUBO.model = model;
  for (int i = 0; i < kMaxClipPlanes; i++)
    wireframeUBO.clipPlanes[i] = m_clipPlanes[static_cast<size_t>(i)];
  wireframeUBO.view = m_viewMatrix;
  wireframeUBO.proj = m_projectionMatrix;
  wireframeUBO.viewPos = glm::vec4(m_cameraPosition, 1.0f);
  wireframeUBO.wireframeColor = m_settings.wireframeColor;
  wireframeUBO.lineWidth = m_settings.lineWidth;

  vkMapMemory(m_device, m_wireframeUniformBuffersMemory[currentFrame], 0,
              sizeof(wireframeUBO), 0, &data);
  memcpy(data, &wireframeUBO, sizeof(wireframeUBO));
  vkUnmapMemory(m_device, m_wireframeUniformBuffersMemory[currentFrame]);
}

void OutlineRenderer::RenderOutlines(VkCommandBuffer commandBuffer,
                                     uint32_t currentFrame) {
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_outlinePipeline);

  // Set dynamic viewport and scissor
  VkViewport viewport{};
  viewport.x = static_cast<float>(m_viewportX);
  viewport.y = static_cast<float>(m_viewportY);
  viewport.width = static_cast<float>(m_viewportWidth);
  viewport.height = static_cast<float>(m_viewportHeight);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {m_viewportX, m_viewportY};
  scissor.extent = {m_viewportWidth, m_viewportHeight};
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_outlinePipelineLayout, 0, 1,
                          &m_outlineDescriptorSets[currentFrame], 0, nullptr);

  for (const auto& mesh : m_meshes) {
    if (mesh && mesh->vertexBuffer != VK_NULL_HANDLE &&
        mesh->indexBuffer != VK_NULL_HANDLE) {
      vkCmdPushConstants(commandBuffer, m_outlinePipelineLayout,
                         VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4),
                         &mesh->transform); // per-mesh model (A2.5)
      VkBuffer vertexBuffers[] = {mesh->vertexBuffer};
      VkDeviceSize offsets[] = {0};
      vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
      vkCmdBindIndexBuffer(commandBuffer, mesh->indexBuffer, 0,
                           VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(commandBuffer, mesh->indexCount, 1, 0, 0, 0);
    }
  }
}

void OutlineRenderer::RenderWireframes(VkCommandBuffer commandBuffer,
                                       uint32_t currentFrame) {
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_wireframePipeline);

  // Set dynamic viewport and scissor
  VkViewport viewport{};
  viewport.x = static_cast<float>(m_viewportX);
  viewport.y = static_cast<float>(m_viewportY);
  viewport.width = static_cast<float>(m_viewportWidth);
  viewport.height = static_cast<float>(m_viewportHeight);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {m_viewportX, m_viewportY};
  scissor.extent = {m_viewportWidth, m_viewportHeight};
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_wireframePipelineLayout, 0, 1,
                          &m_wireframeDescriptorSets[currentFrame], 0, nullptr);

  for (const auto& mesh : m_meshes) {
    if (mesh && mesh->vertexBuffer != VK_NULL_HANDLE &&
        mesh->edgeIndexBuffers[currentFrame] != VK_NULL_HANDLE &&
        mesh->uploadedIndexCount[currentFrame] > 0) {
      vkCmdPushConstants(commandBuffer, m_wireframePipelineLayout,
                         VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4),
                         &mesh->transform); // per-mesh model (A2.5)
      VkBuffer vertexBuffers[] = {mesh->vertexBuffer};
      VkDeviceSize offsets[] = {0};
      vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
      // Bind this frame slot's buffer and draw exactly what was uploaded to
      // it — the CPU-side list may already be newer.
      vkCmdBindIndexBuffer(commandBuffer, mesh->edgeIndexBuffers[currentFrame],
                           0, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(commandBuffer, mesh->uploadedIndexCount[currentFrame], 1,
                       0, 0, 0);
    }
  }
}