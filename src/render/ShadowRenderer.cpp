#include "render/ShadowRenderer.h"
#include "common/Logger.h"
#include "common/RuntimePaths.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

ShadowRenderer::ShadowRenderer() {}

ShadowRenderer::~ShadowRenderer() { Shutdown(); }

bool ShadowRenderer::Initialize(VkDevice device,
                                VkPhysicalDevice physicalDevice,
                                VkQueue graphicsQueue, uint32_t queueFamily,
                                VkCommandPool commandPool) {
  m_device = device;
  m_physicalDevice = physicalDevice;
  m_graphicsQueue = graphicsQueue;
  m_queueFamily = queueFamily;
  m_commandPool = commandPool;

  if (!CreateShadowTarget())
    return false;
  if (!CreateRenderPass())
    return false;
  if (!CreatePipeline())
    return false;
  if (!CreateFramebuffer())
    return false;

  return true;
}

bool ShadowRenderer::CreateFramebuffer() {
  // One framebuffer, window-resolution-independent (only SetResolution
  // recreates it).
  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = m_renderPass;
  fb.attachmentCount = 1;
  fb.pAttachments = &m_shadowView;
  fb.width = m_resolution;
  fb.height = m_resolution;
  fb.layers = 1;
  if (vkCreateFramebuffer(m_device, &fb, nullptr, &m_framebuffer) !=
      VK_SUCCESS) {
    LOG_ERROR("ShadowRenderer: failed to create framebuffer");
    return false;
  }
  return true;
}

bool ShadowRenderer::SetResolution(uint32_t resolution) {
  if (resolution == m_resolution)
    return false;
  m_resolution = resolution;
  // The old map may be sampled by an in-flight frame: recreate behind
  // wait-idle; the caller rebinds the new view (RebindShadowMap).
  vkDeviceWaitIdle(m_device);
  if (m_framebuffer)
    vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
  if (m_shadowView)
    vkDestroyImageView(m_device, m_shadowView, nullptr);
  if (m_shadowImage)
    vkDestroyImage(m_device, m_shadowImage, nullptr);
  if (m_shadowMemory)
    vkFreeMemory(m_device, m_shadowMemory, nullptr);
  m_framebuffer = VK_NULL_HANDLE;
  m_shadowView = VK_NULL_HANDLE;
  m_shadowImage = VK_NULL_HANDLE;
  m_shadowMemory = VK_NULL_HANDLE;
  if (!CreateShadowTarget() || !CreateFramebuffer()) {
    LOG_ERROR("ShadowRenderer: SetResolution({}) failed to recreate the shadow "
              "target",
              resolution);
  }
  return true;
}

void ShadowRenderer::Shutdown() {
  if (m_device == VK_NULL_HANDLE)
    return;
  // The shadow map may be referenced by an in-flight frame (sampled by the
  // mesh pass) — the OutlineRenderer in-use-buffer device-lost (1422695) is
  // the cautionary tale.
  vkDeviceWaitIdle(m_device);

  if (m_pipeline)
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
  if (m_pipelineLayout)
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  if (m_framebuffer)
    vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
  if (m_renderPass)
    vkDestroyRenderPass(m_device, m_renderPass, nullptr);
  if (m_sampler)
    vkDestroySampler(m_device, m_sampler, nullptr);
  if (m_shadowView)
    vkDestroyImageView(m_device, m_shadowView, nullptr);
  if (m_shadowImage)
    vkDestroyImage(m_device, m_shadowImage, nullptr);
  if (m_shadowMemory)
    vkFreeMemory(m_device, m_shadowMemory, nullptr);
  m_pipeline = VK_NULL_HANDLE;
  m_pipelineLayout = VK_NULL_HANDLE;
  m_framebuffer = VK_NULL_HANDLE;
  m_renderPass = VK_NULL_HANDLE;
  m_sampler = VK_NULL_HANDLE;
  m_shadowView = VK_NULL_HANDLE;
  m_shadowImage = VK_NULL_HANDLE;
  m_shadowMemory = VK_NULL_HANDLE;
  m_device = VK_NULL_HANDLE;
}

void ShadowRenderer::RecordShadowPass(VkCommandBuffer cb, MeshRenderer& scene) {
  glm::vec3 bbMin, bbMax;
  const bool haveCasters = scene.GetShadowCasters(m_casters, bbMin, bbMax);

  if (haveCasters) {
    // Fit the ortho frustum to the scene bbox from the primary light's
    // direction. Pinned to MeshRenderer::UpdateCamera's primary light —
    // shadowing any other light would need its own map.
    const glm::vec3 lightDir = glm::normalize(glm::vec3(1.0f, 1.5f, 1.0f));
    const glm::vec3 center = 0.5f * (bbMin + bbMax);
    const float diag = std::max(glm::length(bbMax - bbMin), 1e-3f);
    const glm::mat4 view = glm::lookAt(center + lightDir * diag, center,
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx = -mn;
    for (int c = 0; c < 8; c++) {
      const glm::vec3 corner((c & 1) ? bbMax.x : bbMin.x,
                             (c & 2) ? bbMax.y : bbMin.y,
                             (c & 4) ? bbMax.z : bbMin.z);
      const glm::vec3 lp = glm::vec3(view * glm::vec4(corner, 1.0f));
      mn = glm::min(mn, lp);
      mx = glm::max(mx, lp);
    }
    // Small margin so edge texels and the depth range have slack without
    // wasting resolution (a slack far plane invites peter-panning).
    const glm::vec3 pad = glm::vec3(0.01f * diag + 1e-4f);
    mn -= pad;
    mx += pad;
    // Light view looks down -Z, so near = -mx.z, far = -mn.z. Depth range
    // is [0,1] (GLM_FORCE_DEPTH_ZERO_TO_ONE, public on designer_core).
    m_lightViewProj = glm::ortho(mn.x, mx.x, mn.y, mx.y, -mx.z, -mn.z) * view;
    m_texelWorld = std::max(mx.x - mn.x, mx.y - mn.y) / m_resolution;
  }

  // Always run the pass: a cleared map means "nothing casts" and keeps the
  // image layout deterministic (final layout = SHADER_READ_ONLY_OPTIMAL).
  VkClearValue clear;
  clear.depthStencil = {1.0f, 0};
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = m_renderPass;
  rp.framebuffer = m_framebuffer;
  rp.renderArea.extent = {m_resolution, m_resolution};
  rp.clearValueCount = 1;
  rp.pClearValues = &clear;
  vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

  if (haveCasters) {
    VkViewport vp{0.0f, 0.0f, (float)m_resolution, (float)m_resolution,
                  0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {m_resolution, m_resolution}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // World section planes as vec4 P with signed dist = dot(P, (wp, 1)):
    // mesh.vert's dot(n, wp) - w convention gives P = (n, -w). The
    // object-space plane per instance is then transpose(model) * P. Disabled
    // slots stay all-zero and the shader skips them.
    ClipPlaneArray worldPlanes = NoClipPlanes();
    for (int i = 0; i < kMaxClipPlanes; i++) {
      const glm::vec4& p = m_clipPlanes[static_cast<size_t>(i)];
      if (glm::dot(glm::vec3(p), glm::vec3(p)) > 0.0f)
        worldPlanes[static_cast<size_t>(i)] = glm::vec4(glm::vec3(p), -p.w);
    }

    // 64 + 64 = 128 bytes, the guaranteed push-constant minimum; this is why
    // kMaxClipPlanes is 4 (see render/ClipPlanes.h).
    struct Push {
      glm::mat4 lightMVP;
      glm::vec4 objClipPlanes[kMaxClipPlanes];
    } push;
    static_assert(sizeof(Push) == 128, "shadow push constants must fit 128 B");

    VkBuffer boundVertexBuffer = VK_NULL_HANDLE;
    for (const ShadowCaster& caster : m_casters) {
      push.lightMVP = m_lightViewProj * caster.transform;
      for (int i = 0; i < kMaxClipPlanes; i++) {
        const glm::vec4& wp = worldPlanes[static_cast<size_t>(i)];
        push.objClipPlanes[i] = glm::dot(glm::vec3(wp), glm::vec3(wp)) > 0.0f
                                    ? glm::transpose(caster.transform) * wp
                                    : glm::vec4(0.0f);
      }
      vkCmdPushConstants(cb, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                         sizeof(push), &push);

      if (caster.vertexBuffer != boundVertexBuffer) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &caster.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cb, caster.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        boundVertexBuffer = caster.vertexBuffer;
      }
      vkCmdDrawIndexed(cb, caster.indexCount, 1, 0, 0, 0);
    }
  }

  vkCmdEndRenderPass(cb);
}

bool ShadowRenderer::CreateShadowTarget() {
  VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  img.imageType = VK_IMAGE_TYPE_2D;
  img.format = VK_FORMAT_D32_SFLOAT;
  img.extent = {m_resolution, m_resolution, 1};
  img.mipLevels = 1;
  img.arrayLayers = 1;
  img.samples = VK_SAMPLE_COUNT_1_BIT;
  img.tiling = VK_IMAGE_TILING_OPTIMAL;
  img.usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (vkCreateImage(m_device, &img, nullptr, &m_shadowImage) != VK_SUCCESS) {
    LOG_ERROR("ShadowRenderer: failed to create shadow image");
    return false;
  }

  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(m_device, m_shadowImage, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex =
      FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(m_device, &alloc, nullptr, &m_shadowMemory) !=
      VK_SUCCESS) {
    LOG_ERROR("ShadowRenderer: failed to allocate shadow memory");
    return false;
  }
  vkBindImageMemory(m_device, m_shadowImage, m_shadowMemory, 0);

  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = m_shadowImage;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = VK_FORMAT_D32_SFLOAT;
  view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(m_device, &view, nullptr, &m_shadowView) !=
      VK_SUCCESS) {
    LOG_ERROR("ShadowRenderer: failed to create shadow view");
    return false;
  }

  // Compare sampler (sampler2DShadow): LINEAR gives 2x2 hardware PCF per
  // tap; the 3x3 loop in mesh.frag widens the penumbra. White border =
  // unshadowed outside the frustum. Resolution-independent — created once
  // and kept across SetResolution recreations.
  if (!m_sampler) {
    VkSamplerCreateInfo smp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smp.magFilter = VK_FILTER_LINEAR;
    smp.minFilter = VK_FILTER_LINEAR;
    smp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    smp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    smp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    smp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    smp.compareEnable = VK_TRUE;
    smp.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(m_device, &smp, nullptr, &m_sampler) != VK_SUCCESS) {
      LOG_ERROR("ShadowRenderer: failed to create sampler");
      return false;
    }
  }

  // One-time UNDEFINED -> SHADER_READ_ONLY transition so the mesh pass can
  // legally bind the map even before the first shadow pass runs (or while
  // shadows are toggled off). The render pass re-enters from UNDEFINED
  // (contents cleared), so this layout also holds between disabled frames.
  VkCommandBufferAllocateInfo cbInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbInfo.commandPool = m_commandPool;
  cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbInfo.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(m_device, &cbInfo, &cb);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &begin);
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.image = m_shadowImage;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);
  vkEndCommandBuffer(cb);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb;
  vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(m_graphicsQueue);
  vkFreeCommandBuffers(m_device, m_commandPool, 1, &cb);

  return true;
}

bool ShadowRenderer::CreateRenderPass() {
  VkAttachmentDescription depth{};
  depth.format = VK_FORMAT_D32_SFLOAT;
  depth.samples = VK_SAMPLE_COUNT_1_BIT;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentReference depthRef{
      0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.pDepthStencilAttachment = &depthRef;

  // The two hazards around the pass, expressed as subpass dependencies so
  // no explicit barriers are needed at the record site: the previous
  // frame's fragment sampling must finish before this frame's depth write
  // (WAR), and the depth write must finish before this frame samples it.
  VkSubpassDependency deps[2]{};
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 1;
  rp.pAttachments = &depth;
  rp.subpassCount = 1;
  rp.pSubpasses = &sub;
  rp.dependencyCount = 2;
  rp.pDependencies = deps;
  if (vkCreateRenderPass(m_device, &rp, nullptr, &m_renderPass) != VK_SUCCESS) {
    LOG_ERROR("ShadowRenderer: failed to create render pass");
    return false;
  }
  return true;
}

bool ShadowRenderer::CreatePipeline() {
  const std::string shaderDir = RuntimePaths::ShaderDir().string();
  auto vertCode = ReadFile(shaderDir + "/shadow_vert.spv");
  auto fragCode = ReadFile(shaderDir + "/shadow_frag.spv");
  if (vertCode.empty() || fragCode.empty()) {
    LOG_ERROR("ShadowRenderer: failed to load shaders from {}", shaderDir);
    return false;
  }
  VkShaderModule vertModule = CreateShaderModule(vertCode);
  VkShaderModule fragModule = CreateShaderModule(fragCode);
  if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
    if (vertModule)
      vkDestroyShaderModule(m_device, vertModule, nullptr);
    if (fragModule)
      vkDestroyShaderModule(m_device, fragModule, nullptr);
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertModule;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragModule;
  stages[1].pName = "main";

  // Same Vertex3D buffer as the main pass; only position is consumed.
  auto binding = Vertex3D::getBindingDescription();
  VkVertexInputAttributeDescription attr{};
  attr.binding = 0;
  attr.location = 0;
  attr.format = VK_FORMAT_R32G32B32_SFLOAT;
  attr.offset = 0;
  VkPipelineVertexInputStateCreateInfo vertexInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  // Dynamic viewport/scissor: the map size is switchable (SetResolution)
  // without pipeline recreation.
  VkPipelineViewportStateCreateInfo viewportState{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;
  VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                     VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamicStates;

  // No culling: robust for meshes left open by the section clip, and it
  // avoids depending on winding under the light's (un-flipped) projection.
  // Static depth bias fights acne; normal-offset in mesh.vert is the other
  // half of the tuning knob.
  VkPipelineRasterizationStateCreateInfo rasterizer{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_TRUE;
  rasterizer.depthBiasConstantFactor = 1.25f;
  rasterizer.depthBiasSlopeFactor = 1.75f;

  VkPipelineMultisampleStateCreateInfo multisampling{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

  // Depth-only: no color attachments.
  VkPipelineColorBlendStateCreateInfo colorBlending{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlending.attachmentCount = 0;

  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pushRange.offset = 0;
  pushRange.size = sizeof(glm::mat4) + sizeof(glm::vec4) * kMaxClipPlanes;

  VkPipelineLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &pushRange;
  if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr,
                             &m_pipelineLayout) != VK_SUCCESS) {
    LOG_ERROR("ShadowRenderer: failed to create pipeline layout");
    vkDestroyShaderModule(m_device, fragModule, nullptr);
    vkDestroyShaderModule(m_device, vertModule, nullptr);
    return false;
  }

  VkGraphicsPipelineCreateInfo pipelineInfo{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = m_pipelineLayout;
  pipelineInfo.renderPass = m_renderPass;
  pipelineInfo.subpass = 0;

  const VkResult res = vkCreateGraphicsPipelines(
      m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
  vkDestroyShaderModule(m_device, fragModule, nullptr);
  vkDestroyShaderModule(m_device, vertModule, nullptr);
  if (res != VK_SUCCESS) {
    LOG_ERROR("ShadowRenderer: failed to create pipeline");
    return false;
  }
  return true;
}

uint32_t ShadowRenderer::FindMemoryType(uint32_t typeFilter,
                                        VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memProperties;
  vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
  for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
    if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags &
                                    properties) == properties) {
      return i;
    }
  }
  return 0;
}

std::vector<char> ShadowRenderer::ReadFile(const std::string& filename) {
  std::ifstream file(filename, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    LOG_ERROR("ShadowRenderer: failed to open {}", filename);
    return {};
  }
  const size_t fileSize = static_cast<size_t>(file.tellg());
  std::vector<char> buffer(fileSize);
  file.seekg(0);
  file.read(buffer.data(), fileSize);
  return buffer;
}

VkShaderModule
ShadowRenderer::CreateShaderModule(const std::vector<char>& code) {
  VkShaderModuleCreateInfo createInfo{
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  createInfo.codeSize = code.size();
  createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule shaderModule = VK_NULL_HANDLE;
  if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) !=
      VK_SUCCESS) {
    LOG_ERROR("ShadowRenderer: failed to create shader module");
    return VK_NULL_HANDLE;
  }
  return shaderModule;
}
