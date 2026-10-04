#include "render/MeshRenderer.h"

#include "render/VulkanCommon.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "core/Conventions.h"
#include "render/FramesInFlight.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

// Vertex3D implementation
VkVertexInputBindingDescription Vertex3D::getBindingDescription() {
  VkVertexInputBindingDescription bindingDescription{};
  bindingDescription.binding = 0;
  bindingDescription.stride = sizeof(Vertex3D);
  bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  return bindingDescription;
}

std::vector<VkVertexInputAttributeDescription>
Vertex3D::getAttributeDescriptions() {
  std::vector<VkVertexInputAttributeDescription> attributeDescriptions(2);

  // Position
  attributeDescriptions[0].binding = 0;
  attributeDescriptions[0].location = 0;
  attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  attributeDescriptions[0].offset = offsetof(Vertex3D, position);

  // Normal
  attributeDescriptions[1].binding = 0;
  attributeDescriptions[1].location = 1;
  attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
  attributeDescriptions[1].offset = offsetof(Vertex3D, normal);

  return attributeDescriptions;
}

// MeshRenderer implementation
MeshRenderer::MeshRenderer() {}

MeshRenderer::~MeshRenderer() { Shutdown(); }

bool MeshRenderer::Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                              VkRenderPass renderPass, VkQueue graphicsQueue,
                              uint32_t queueFamily, VkCommandPool commandPool,
                              VkSampleCountFlagBits samples) {
  m_msaaSamples = samples;
  m_device = device;
  m_physicalDevice = physicalDevice;
  m_renderPass = renderPass;
  m_graphicsQueue = graphicsQueue;
  m_queueFamily = queueFamily;
  m_commandPool = commandPool;

  if (!CreateDescriptorSetLayout())
    return false;
  if (!CreateGraphicsPipeline())
    return false;
  if (!CreateUniformBuffers())
    return false;
  if (!CreateDescriptorPool())
    return false;
  if (!CreateDescriptorSets())
    return false;

  return true;
}

void MeshRenderer::Shutdown() {
  if (m_device == VK_NULL_HANDLE)
    return;

  // Cleanup assets + instances
  for (auto& asset : m_assets) {
    if (asset) {
      DestroyAssetBuffers(asset.get());
    }
  }
  m_assets.clear();
  m_instances.clear();

  // Cleanup uniform buffers
  for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    vkutil::DestroyBuffer(m_device, m_uniformBuffers[i],
                          m_uniformBuffersMemory[i]);
  }

  if (m_descriptorPool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
  }

  if (m_graphicsPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
  }

  if (m_ghostPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(m_device, m_ghostPipeline, nullptr);
  }

  if (m_pipelineLayout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  }

  if (m_descriptorSetLayout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
  }
}

MeshAsset* MeshRenderer::GetAsset(uint32_t assetId) {
  if (assetId == 0 || assetId > m_assets.size())
    return nullptr;
  return m_assets[assetId - 1].get();
}

MeshInstance* MeshRenderer::GetInstance(uint32_t instanceId) {
  if (instanceId == 0 || instanceId > m_instances.size())
    return nullptr;
  return m_instances[instanceId - 1].get();
}

uint32_t MeshRenderer::CreateAsset(const std::vector<Vertex3D>& vertices,
                                   const std::vector<uint32_t>& indices) {
  auto asset = std::make_unique<MeshAsset>();
  asset->vertices = vertices;
  asset->indices = indices;

  if (!CreateAssetBuffers(asset.get())) {
    return 0; // Invalid asset id
  }

  m_assets.push_back(std::move(asset));
  return static_cast<uint32_t>(m_assets.size());
}

void MeshRenderer::UpdateAsset(uint32_t assetId,
                               const std::vector<Vertex3D>& vertices,
                               const std::vector<uint32_t>& indices) {
  MeshAsset* asset = GetAsset(assetId);
  if (!asset)
    return;

  asset->vertices = vertices;
  asset->indices = indices;

  // Ensure the old buffers aren't referenced by an in-flight frame before
  // destroying them.
  vkDeviceWaitIdle(m_device);
  DestroyAssetBuffers(asset);
  CreateAssetBuffers(asset);
}

uint32_t MeshRenderer::AddInstance(uint32_t assetId,
                                   const glm::mat4& transform) {
  MeshAsset* asset = GetAsset(assetId);
  if (!asset)
    return 0;

  auto instance = std::make_unique<MeshInstance>();
  instance->assetId = assetId;
  instance->transform = transform;
  asset->refCount++;

  m_instances.push_back(std::move(instance));
  return static_cast<uint32_t>(m_instances.size());
}

void MeshRenderer::BeginAssetBatch() {
  if (m_assetBatch || m_device == VK_NULL_HANDLE)
    return; // already batching, or not initialized
  m_assetBatch = std::make_unique<vkutil::StagingBatch>(
      m_device, m_physicalDevice, m_commandPool, m_graphicsQueue);
}

void MeshRenderer::EndAssetBatch() {
  if (!m_assetBatch)
    return;
  m_assetBatch->Flush();
  m_assetBatch.reset();
}

void MeshRenderer::RetainAsset(uint32_t assetId) {
  if (MeshAsset* asset = GetAsset(assetId))
    asset->refCount++;
}

void MeshRenderer::ReleaseAsset(uint32_t assetId) {
  MeshAsset* asset = GetAsset(assetId);
  if (!asset || asset->refCount == 0)
    return;
  if (--asset->refCount == 0) {
    WaitDeviceIdleOnce();
    DestroyAssetBuffers(asset);
    m_assets[assetId - 1].reset();
  }
}

void MeshRenderer::RemoveInstance(uint32_t instanceId) {
  MeshInstance* instance = GetInstance(instanceId);
  if (!instance)
    return;

  MeshAsset* asset = GetAsset(instance->assetId);
  if (asset && --asset->refCount == 0) {
    // Last occurrence gone: drop the tessellation too. Ensure the buffers
    // aren't referenced by an in-flight frame before destroying them
    // (rapid resync, e.g. accepting a feature preview, otherwise causes a
    // GPU device-lost) — once per batch, not once per asset.
    WaitDeviceIdleOnce();
    DestroyAssetBuffers(asset);
    m_assets[instance->assetId - 1].reset();
  }
  m_instances[instanceId - 1].reset();
}

void MeshRenderer::SetInstanceTransform(uint32_t instanceId,
                                        const glm::mat4& transform) {
  if (MeshInstance* instance = GetInstance(instanceId)) {
    instance->transform = transform;
  }
}

std::optional<glm::mat4>
MeshRenderer::GetInstanceTransform(uint32_t instanceId) const {
  // Same indexing as GetInstance: ids are 1-based slots, and a removed
  // instance leaves a null.
  if (instanceId == 0 || instanceId > m_instances.size())
    return std::nullopt;
  const MeshInstance* inst = m_instances[instanceId - 1].get();
  return inst ? std::optional<glm::mat4>(inst->transform) : std::nullopt;
}

bool MeshRenderer::IsInstanceGhost(uint32_t instanceId) const {
  if (instanceId == 0 || instanceId > m_instances.size())
    return false;
  const auto& instance = m_instances[instanceId - 1];
  return instance && instance->ghost;
}

void MeshRenderer::SetInstanceColor(uint32_t instanceId,
                                    const glm::vec3& albedo) {
  if (MeshInstance* instance = GetInstance(instanceId)) {
    instance->baseColor = glm::vec4(albedo, 1.0f);
    instance->ghost = false; // see SetInstanceMaterial
  }
}

void MeshRenderer::SetInstanceMaterial(uint32_t instanceId,
                                       const glm::vec3& albedo,
                                       const glm::vec4& params) {
  if (MeshInstance* instance = GetInstance(instanceId)) {
    instance->baseColor = glm::vec4(albedo, 1.0f);
    instance->materialParams = params;
    // Opaque again, in the pass as well as in the alpha. `ghost` used to be
    // sticky: an instance ever ghosted stayed in the alpha-blended,
    // depth-write-OFF pipeline for the rest of its life, at full opacity, so
    // it never occluded anything and you could see its own far side through
    // its near wall. `SetInstanceGhost`'s contract already says to call it
    // AFTER this ("those reset baseColor.a to 1"); this finishes the job.
    instance->ghost = false;
  }
}

void MeshRenderer::SetInstanceProcedural(uint32_t instanceId,
                                         CADMaterials::SurfacePattern pattern,
                                         float patternScale,
                                         float patternContrast) {
  if (MeshInstance* instance = GetInstance(instanceId)) {
    // .w (the pattern axis) is filled per-draw from the asset bbox.
    instance->procParams =
        glm::vec4(static_cast<float>(static_cast<int>(pattern)), patternScale,
                  patternContrast, 0.0f);
  }
}

void MeshRenderer::SetInstanceVisible(uint32_t instanceId, bool visible) {
  if (MeshInstance* instance = GetInstance(instanceId)) {
    instance->visible = visible;
  }
}

void MeshRenderer::SetInstanceLookRuns(uint32_t instanceId,
                                       std::vector<MeshLookRun> runs) {
  if (MeshInstance* instance = GetInstance(instanceId))
    instance->runs = std::move(runs);
}

std::vector<MeshLookRun>
MeshRenderer::InstanceLookRuns(uint32_t instanceId) const {
  if (instanceId == 0 || instanceId > m_instances.size() ||
      !m_instances[instanceId - 1])
    return {};
  return m_instances[instanceId - 1]->runs;
}

void MeshRenderer::SetInstanceGlass(uint32_t instanceId, float alpha) {
  if (MeshInstance* instance = GetInstance(instanceId)) {
    instance->ghost = true;
    instance->baseColor.a = -std::max(alpha, 0.01f); // the sign says glass
  }
}

void MeshRenderer::SetInstanceGhost(uint32_t instanceId, float alpha) {
  // Call after SetInstanceColor/Material — those reset baseColor.a to 1.
  if (MeshInstance* instance = GetInstance(instanceId)) {
    instance->ghost = true;
    instance->baseColor.a = alpha;
  }
}

uint32_t MeshRenderer::AddMesh(const std::vector<Vertex3D>& vertices,
                               const std::vector<uint32_t>& indices) {
  uint32_t assetId = CreateAsset(vertices, indices);
  if (assetId == 0)
    return 0;
  return AddInstance(assetId);
}

void MeshRenderer::UpdateMesh(uint32_t instanceId,
                              const std::vector<Vertex3D>& vertices,
                              const std::vector<uint32_t>& indices) {
  MeshInstance* instance = GetInstance(instanceId);
  if (!instance)
    return;
  UpdateAsset(instance->assetId, vertices, indices);
}

void MeshRenderer::UpdateCamera(const glm::mat4& view,
                                const glm::mat4& projection) {
  m_viewMatrix = view;
  m_projectionMatrix = projection;
}

void MeshRenderer::WaitDeviceIdleOnce() {
  if (m_deviceIdle || m_device == VK_NULL_HANDLE)
    return;
  vkDeviceWaitIdle(m_device);
  m_deviceIdle = true;
}

void MeshRenderer::Render(VkCommandBuffer commandBuffer,
                          uint32_t currentFrame) {
  // A batch left open would mean drawing from buffers whose staging copy has
  // not executed — garbage geometry. Never happens on the paired path; this
  // makes "assets are uploaded before anything draws them" an invariant of the
  // renderer rather than of every caller.
  EndAssetBatch();
  // Recording a draw ends the "device is idle" guarantee for the batch.
  m_deviceIdle = false;
  // Update uniform buffer
  UniformBufferObject ubo{};
  ubo.view = m_viewMatrix;
  ubo.proj = m_projectionMatrix;

  // Extract camera position from inverse view matrix
  glm::mat4 invView = glm::inverse(m_viewMatrix);
  ubo.viewPos = glm::vec4(invView[3][0], invView[3][1], invView[3][2], 1.0f);

  // The rig (core/LightRig.h): placed from the camera, or frozen at the
  // home view when locked to the model; the colours are core/Conventions.h,
  // shared with the stock block and the ray tracer.
  const conv::LightRig rig = Rig();
  ubo.primaryLight.direction = glm::vec4(rig.key, conv::kKeyLight.a);
  ubo.primaryLight.color = glm::vec4(glm::vec3(conv::kKeyLight), 1.0f);
  ubo.fillLight.direction = glm::vec4(rig.fill, conv::kFillLight.a);
  ubo.fillLight.color = glm::vec4(glm::vec3(conv::kFillLight), 1.0f);
  ubo.rimLight.direction = glm::vec4(rig.rim, conv::kRimLight.a);
  ubo.rimLight.color = glm::vec4(glm::vec3(conv::kRimLight), 1.0f);
  ubo.envRotation = glm::mat4(rig.envRotation);
  ubo.ambientColor = conv::kAmbientLight;

  // Section clip plane is global (same for every instance), so it rides in
  // the shared UBO rather than per-draw push constants.
  for (int i = 0; i < kMaxClipPlanes; i++)
    ubo.clipPlanes[i] = m_clipPlanes[static_cast<size_t>(i)];

  // Key-light shadow map (S1): per-frame light fit from the ShadowRenderer
  // (via SetShadowState). Strength 0.75 and a ~1.5-texel normal-offset bias
  // are the screenshot-tuned defaults; only the primary light is attenuated
  // in the shader — fill/rim/IBL stay unshadowed (they are the soft fill).
  ubo.lightViewProj = m_lightViewProj;
  ubo.shadowParams = glm::vec4(0.75f, 1.5f * m_shadowTexelWorld,
                               m_shadowTexelUV, m_shadowsEnabled ? 1.0f : 0.0f);

  // SSAO (S2): the blurred AO map multiplies only the ambient/IBL term in
  // the shader — direct light is handled by the shadow map, not AO.
  // Both images are the same projection stretched to fill a rectangle — the
  // scene fills the viewport sub-rect, the AO target fills itself — so the
  // affine remap between those rectangles aligns them exactly, whatever the
  // aspect or AO resolution. xy = scale, zw = offset; the enable flag rides
  // in aoParams2.x.
  ubo.aoParams = ViewportToTargetUV(m_viewportX, m_viewportY, m_viewportWidth,
                                    m_viewportHeight);
  ubo.aoParams2 = glm::vec4(m_aoEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

  // Set viewport to the central region (excludes the side panels)
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

  // One slot per swapchain image, indexed directly — see
  // render/FramesInFlight.h for why the old "% 2" made the viewport
  // shake on a 3-image swapchain.
  const uint32_t frame = render::FrameSlot(currentFrame, "MeshRenderer");

  // Shared UBO (camera + lights + clip plane); one write serves every draw
  // this frame — everything per-instance rides in push constants
  memcpy(m_uniformBuffersMapped[frame], &ubo, sizeof(ubo));

  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipelineLayout, 0, 1, &m_descriptorSets[frame], 0,
                          nullptr);

  // One draw per instance; the asset's buffers are bound once per run of
  // consecutive instances sharing it. Push constants live in the command
  // stream, so per-instance data costs no descriptor or sync machinery.
  // Two passes (A4): all opaque instances through the blend-disabled
  // pipeline first, then ghosts through the alpha-blended, depth-write-off
  // ghost pipeline (translucent context geometry never occludes anything).
  struct PushConstants {
    glm::mat4 model;
    glm::uvec4 highlight;
    glm::vec4 material;  // metallic, roughness, specular, reflectance
    glm::vec4 baseColor; // per-instance albedo (a = ghost opacity)
    // Procedural surface pattern: x = CADMaterials::SurfacePattern,
    // y = patternScale, z = patternContrast, w = pattern-axis index (0/1/2).
    glm::vec4 procMaterial;
  } push;

  for (const bool ghostPass : {false, true}) {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      ghostPass ? m_ghostPipeline : m_graphicsPipeline);
    uint32_t boundAssetId = 0;
    for (const auto& instance : m_instances) {
      if (!instance || !instance->visible)
        continue;
      if (instance->ghost != ghostPass)
        continue;

      MeshAsset* asset = GetAsset(instance->assetId);
      if (!asset || asset->vertices.empty())
        continue;

      push.model = instance->transform;
      const glm::uvec2 sel0 = instance->selRanges.empty()
                                  ? glm::uvec2(0)
                                  : instance->selRanges.front();
      push.highlight = glm::uvec4(sel0.x, sel0.y, instance->hoverRange.x,
                                  instance->hoverRange.y);
      // The pattern axis belongs to the asset, whichever look draws it.
      const auto procFor = [&](const glm::vec4& proc) {
        glm::vec4 out = proc;
        if (proc.x > 0.5f) {
          // The asset's bbox is genuinely object-local (the model matrix
          // rides in the push constant), so this is the frame PatternAxis
          // wants. Cheap enough per-draw.
          out.w = static_cast<float>(CADMaterials::PatternAxis(
              static_cast<CADMaterials::SurfacePattern>(
                  static_cast<int>(proc.x + 0.5f)),
              asset->bboxMax - asset->bboxMin));
        }
        return out;
      };

      if (instance->assetId != boundAssetId) {
        VkBuffer vertexBuffers[] = {asset->vertexBuffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, asset->indexBuffer, 0,
                             VK_INDEX_TYPE_UINT32);
        boundAssetId = instance->assetId;
      }

      // One look over [firstIndex, firstIndex + count): the main draw, then
      // the additional selected ranges (multi-select on one instance) —
      // re-drawn with only that range highlighted: depth LESS_OR_EQUAL lets
      // the identical geometry pass, and the 0xFFFFFFFF hover-slot sentinel
      // makes the fragment stage DISCARD everything outside the range (a
      // plain redraw would overwrite the other ranges' tint). Selection
      // counts are small, so the extra draws are cheap. The highlight ranges
      // are VERTEX ranges, which a sub-draw of the index buffer keeps (its
      // vertexOffset is 0), so a body drawn in looks highlights the same.
      const auto drawLook = [&](uint32_t firstIndex, uint32_t count,
                                const glm::vec4& baseColor,
                                const glm::vec4& material,
                                const glm::vec4& proc) {
        push.highlight = glm::uvec4(sel0.x, sel0.y, instance->hoverRange.x,
                                    instance->hoverRange.y);
        push.material = material;
        push.baseColor = baseColor;
        push.procMaterial = procFor(proc);
        vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDrawIndexed(commandBuffer, count, 1, firstIndex, 0, 0);
        for (size_t r = 1; r < instance->selRanges.size(); r++) {
          push.highlight =
              glm::uvec4(instance->selRanges[r].x, instance->selRanges[r].y,
                         0xFFFFFFFFu, 0xFFFFFFFFu);
          vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                             VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(push), &push);
          vkCmdDrawIndexed(commandBuffer, count, 1, firstIndex, 0, 0);
        }
      };

      const uint32_t indexCount = static_cast<uint32_t>(asset->indices.size());
      if (instance->runs.empty()) {
        drawLook(0, indexCount, instance->baseColor, instance->materialParams,
                 instance->procParams);
        continue;
      }
      // Faces with looks of their own (v164): one draw per run. A run past
      // the end of the asset (a stale run on a re-uploaded asset) is clipped
      // rather than read out of bounds.
      for (const MeshLookRun& run : instance->runs) {
        if (run.firstIndex >= indexCount)
          continue;
        const uint32_t count =
            std::min(run.indexCount, indexCount - run.firstIndex);
        if (!run.own) {
          drawLook(run.firstIndex, count, instance->baseColor,
                   instance->materialParams, instance->procParams);
          continue;
        }
        // The instance's alpha (ghost opacity, the glass sign) stays its own.
        drawLook(run.firstIndex, count,
                 glm::vec4(run.baseColor, instance->baseColor.a),
                 run.materialParams, run.procParams);
      }
    }
  }
}

void MeshRenderer::SetFaceHighlight(uint32_t instanceId, uint32_t firstVertex,
                                    uint32_t count, uint32_t tint) {
  MeshInstance* instance = GetInstance(instanceId);
  if (!instance)
    return;
  // Additive: multi-select shows every face (the projection clears per
  // frame). The old single-slot semantics kept only the LAST synced face.
  // The tint rides in the end's top bits (see the header): the shader
  // unpacks it, and so does InstanceHighlightRanges' reader.
  const uint32_t end = (firstVertex + count) & kRangeMask;
  instance->selRanges.push_back(
      glm::uvec2(firstVertex, end | ((tint & 0xFu) << kTintShift)));
}

std::vector<glm::uvec2>
MeshRenderer::InstanceHighlightRanges(uint32_t instanceId) const {
  if (instanceId == 0 || instanceId > m_instances.size() ||
      !m_instances[instanceId - 1])
    return {};
  return m_instances[instanceId - 1]->selRanges;
}

void MeshRenderer::SetFaceHover(uint32_t instanceId, uint32_t firstVertex,
                                uint32_t count) {
  MeshInstance* instance = GetInstance(instanceId);
  if (!instance)
    return;
  ClearFaceHover();
  instance->hoverRange = glm::uvec2(firstVertex, firstVertex + count);
}

void MeshRenderer::HighlightInstance(uint32_t instanceId) {
  MeshInstance* instance = GetInstance(instanceId);
  if (!instance)
    return;
  MeshAsset* asset = GetAsset(instance->assetId);
  if (!asset)
    return;
  // Full vertex range = the whole instance; additive like face highlights.
  instance->selRanges.push_back(
      glm::uvec2(0, static_cast<uint32_t>(asset->vertices.size())));
}

void MeshRenderer::HoverInstance(uint32_t instanceId) {
  MeshInstance* instance = GetInstance(instanceId);
  if (!instance)
    return;
  MeshAsset* asset = GetAsset(instance->assetId);
  if (!asset)
    return;
  // Full vertex range = the whole instance, which the vertex stage's range
  // test already understands. Additive, so every mesh of one occurrence
  // lights together.
  instance->hoverRange =
      glm::uvec2(0, static_cast<uint32_t>(asset->vertices.size()));
}

glm::uvec2 MeshRenderer::InstanceHoverRange(uint32_t instanceId) {
  const MeshInstance* instance = GetInstance(instanceId);
  return instance ? instance->hoverRange : glm::uvec2(0);
}

void MeshRenderer::ClearFaceHighlight() {
  for (auto& instance : m_instances) {
    if (instance) {
      instance->selRanges.clear();
    }
  }
}

void MeshRenderer::ClearFaceHover() {
  for (auto& instance : m_instances) {
    if (instance) {
      instance->hoverRange = glm::uvec2(0);
    }
  }
}

void MeshRenderer::SetViewport(int32_t x, int32_t y, uint32_t width,
                               uint32_t height) {
  m_viewportX = x;
  m_viewportY = y;
  m_viewportWidth = width;
  m_viewportHeight = height;
}

bool MeshRenderer::GetShadowCasters(std::vector<ShadowCaster>& out,
                                    glm::vec3& bbMin, glm::vec3& bbMax) {
  out.clear();
  bbMin = glm::vec3(std::numeric_limits<float>::max());
  bbMax = -bbMin;
  for (const auto& instance : m_instances) {
    // Exactly the main pass's opaque set: visible, non-ghost.
    if (!instance || !instance->visible || instance->ghost)
      continue;
    MeshAsset* asset = GetAsset(instance->assetId);
    if (!asset || asset->vertices.empty())
      continue;

    out.push_back({asset->vertexBuffer, asset->indexBuffer,
                   static_cast<uint32_t>(asset->indices.size()),
                   instance->transform});
    for (int c = 0; c < 8; c++) {
      const glm::vec3 corner((c & 1) ? asset->bboxMax.x : asset->bboxMin.x,
                             (c & 2) ? asset->bboxMax.y : asset->bboxMin.y,
                             (c & 4) ? asset->bboxMax.z : asset->bboxMin.z);
      const glm::vec3 wp =
          glm::vec3(instance->transform * glm::vec4(corner, 1.0f));
      bbMin = glm::min(bbMin, wp);
      bbMax = glm::max(bbMax, wp);
    }
  }
  return !out.empty();
}

void MeshRenderer::RebindShadowMap(VkImageView shadowMap) {
  m_shadowMapView = shadowMap;
  if (m_descriptorSets.empty())
    return;
  for (size_t i = 0; i < m_descriptorSets.size(); i++) {
    VkDescriptorImageInfo info{m_shadowSampler, m_shadowMapView,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSets[i];
    write.dstBinding = 4;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
  }
}

void MeshRenderer::RebindAOMap(VkImageView aoMap) {
  m_aoMapView = aoMap;
  if (m_descriptorSets.empty())
    return;
  for (size_t i = 0; i < m_descriptorSets.size(); i++) {
    VkDescriptorImageInfo info{m_aoSampler, m_aoMapView,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSets[i];
    write.dstBinding = 5;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
  }
}

// Private helper functions
bool MeshRenderer::CreateDescriptorSetLayout() {
  // 0: camera+lights UBO (vertex; the fragment stage reads its envRotation).
  // 1-3: IBL maps (fragment) — irradiance, prefiltered specular, BRDF LUT.
  // 4: key-light shadow map (fragment, compare sampler). 5: blurred SSAO map
  // (fragment).
  VkDescriptorSetLayoutBinding bindings[6]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  for (int i = 1; i <= 5; i++) {
    bindings[i].binding = static_cast<uint32_t>(i);
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 6;
  layoutInfo.pBindings = bindings;

  if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                  &m_descriptorSetLayout) != VK_SUCCESS) {
    LOG_ERROR("Failed to create descriptor set layout!");
    return false;
  }

  return true;
}

bool MeshRenderer::CreateGraphicsPipeline() {
  // Get executable directory and construct shader paths
  const std::string shaderDir = RuntimePaths::ShaderDir().string();
  std::string vertShaderPath = shaderDir + "/mesh_vert.spv";
  std::string fragShaderPath = shaderDir + "/mesh_frag.spv";

  VkShaderModule vertShaderModule =
      vkutil::LoadShaderModule(m_device, "mesh_vert.spv");
  VkShaderModule fragShaderModule =
      vkutil::LoadShaderModule(m_device, "mesh_frag.spv");
  if (vertShaderModule == VK_NULL_HANDLE ||
      fragShaderModule == VK_NULL_HANDLE) {
    LOG_ERROR("Failed to load mesh shaders from: {}", shaderDir);
    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    return false;
  }

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
  auto bindingDescription = Vertex3D::getBindingDescription();
  auto attributeDescriptions = Vertex3D::getAttributeDescriptions();

  VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
  vertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = 1;
  vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
  vertexInputInfo.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(attributeDescriptions.size());
  vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

  // Input assembly
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

  // Rasterizer
  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  // With the Y-flipped projection, OCCT's outward-oriented triangles are
  // counter-clockwise in framebuffer space (verified empirically; pinned
  // by the "camera-facing triangles are CCW" unit test)
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_FALSE;

  // Multisampling
  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = m_msaaSamples;

  // Depth testing
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  // LESS_OR_EQUAL, not LESS: multi-select highlight re-draws the same
  // geometry at identical depth (see Render's extra selRanges sub-draws).
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.stencilTestEnable = VK_FALSE;

  // Color blending
  VkPipelineColorBlendAttachmentState colorBlendAttachment{};
  colorBlendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment.blendEnable = VK_FALSE;

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;

  // Dynamic state
  std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                               VK_DYNAMIC_STATE_SCISSOR};

  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
  dynamicState.pDynamicStates = dynamicStates.data();

  // Pipeline layout
  // Per-instance model matrix + highlight ranges (vertex) + PBR material +
  // albedo + procedural-pattern params (fragment) via push constants. The
  // total is 128 bytes, exactly the Vulkan-guaranteed maxPushConstantsSize
  // floor — the proc vec4 leaves no further headroom here, which is why a new
  // SurfacePattern packs into it rather than adding a float.
  VkPushConstantRange pushConstantRange{};
  pushConstantRange.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pushConstantRange.offset = 0;
  pushConstantRange.size = sizeof(glm::mat4) + sizeof(glm::uvec4) +
                           sizeof(glm::vec4) + sizeof(glm::vec4) +
                           sizeof(glm::vec4);

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
  pipelineLayoutInfo.pushConstantRangeCount = 1;
  pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

  if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr,
                             &m_pipelineLayout) != VK_SUCCESS) {
    LOG_ERROR("Failed to create pipeline layout!");
    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    return false;
  }

  // Create pipeline
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
  pipelineInfo.layout = m_pipelineLayout;
  pipelineInfo.renderPass = m_renderPass;
  pipelineInfo.subpass = 0;

  if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                nullptr, &m_graphicsPipeline) != VK_SUCCESS) {
    LOG_ERROR("Failed to create graphics pipeline!");
    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    return false;
  }

  // Ghost pipeline (A4 in-context editing): identical except alpha blending
  // on and depth writes off — translucent context geometry drawn in a
  // second pass, depth-tested against (but never occluding) the opaque
  // scene. Unsorted; fine at reference-visual alphas.
  depthStencil.depthWriteEnable = VK_FALSE;
  colorBlendAttachment.blendEnable = VK_TRUE;
  colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  colorBlendAttachment.dstColorBlendFactor =
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
  if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                nullptr, &m_ghostPipeline) != VK_SUCCESS) {
    LOG_ERROR("Failed to create ghost pipeline!");
    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    return false;
  }

  vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
  vkDestroyShaderModule(m_device, vertShaderModule, nullptr);

  return true;
}

bool MeshRenderer::CreateUniformBuffers() {
  VkDeviceSize bufferSize = sizeof(UniformBufferObject);

  m_uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
  m_uniformBuffersMemory.resize(MAX_FRAMES_IN_FLIGHT);
  m_uniformBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);

  for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (!vkutil::CreateBuffer(m_device, m_physicalDevice, bufferSize,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              m_uniformBuffers[i], m_uniformBuffersMemory[i])) {
      return false;
    }

    vkMapMemory(m_device, m_uniformBuffersMemory[i], 0, bufferSize, 0,
                &m_uniformBuffersMapped[i]);
  }

  return true;
}

bool MeshRenderer::CreateDescriptorPool() {
  VkDescriptorPoolSize poolSizes[2]{};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[0].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
  // 3 IBL samplers + shadow map + AO map per frame.
  poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSizes[1].descriptorCount =
      static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) * 5;

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
  poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

  if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) !=
      VK_SUCCESS) {
    LOG_ERROR("Failed to create descriptor pool!");
    return false;
  }

  return true;
}

bool MeshRenderer::CreateDescriptorSets() {
  std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT,
                                             m_descriptorSetLayout);
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = m_descriptorPool;
  allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
  allocInfo.pSetLayouts = layouts.data();

  m_descriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
  if (vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets.data()) !=
      VK_SUCCESS) {
    LOG_ERROR("Failed to allocate descriptor sets!");
    return false;
  }

  for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_uniformBuffers[i];
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(UniformBufferObject);

    // IBL image infos (bindings 1-3, from SetIBL) + shadow map (binding
    // 4, from SetShadowMap) + blurred AO (binding 5, from SetAOMap);
    // layouts match the prepasses' final SHADER_READ_ONLY layout.
    VkDescriptorImageInfo irr{m_iblEnvSampler, m_iblIrradiance,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo pref{m_iblEnvSampler, m_iblPrefilter,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo brdf{m_iblLutSampler, m_iblBrdf,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo shadow{m_shadowSampler, m_shadowMapView,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo ao{m_aoSampler, m_aoMapView,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet writes[6]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptorSets[i];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufferInfo;

    const VkDescriptorImageInfo* imgs[5] = {&irr, &pref, &brdf, &shadow, &ao};
    for (int b = 1; b <= 5; b++) {
      writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[b].dstSet = m_descriptorSets[i];
      writes[b].dstBinding = static_cast<uint32_t>(b);
      writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[b].descriptorCount = 1;
      writes[b].pImageInfo = imgs[b - 1];
    }

    vkUpdateDescriptorSets(m_device, 6, writes, 0, nullptr);
  }

  return true;
}

bool MeshRenderer::CreateAssetBuffers(MeshAsset* asset) {
  if (!asset || asset->vertices.empty())
    return false;

  // Local-space bounds for the shadow frustum fit (see GetShadowCasters).
  asset->bboxMin = asset->bboxMax = asset->vertices[0].position;
  for (const Vertex3D& v : asset->vertices) {
    asset->bboxMin = glm::min(asset->bboxMin, v.position);
    asset->bboxMax = glm::max(asset->bboxMax, v.position);
  }

  const VkDeviceSize vertexBufferSize =
      sizeof(asset->vertices[0]) * asset->vertices.size();
  const VkDeviceSize indexBufferSize =
      sizeof(asset->indices[0]) * asset->indices.size();

  // Every CreateBuffer here used to drop its bool. On an allocation failure
  // the out-params were left UNINITIALIZED and handed straight to
  // vkMapMemory — so an OOM on a large assembly read as a crash in the
  // upload rather than as a failed upload. StagingBatch::Upload checks and
  // nulls both out-handles on every failure path, so that stays true.
  //
  // Batched when a bulk build is in progress (BeginAssetBatch), one-shot
  // otherwise — the one-shot batch flushes in its destructor, which is the
  // submit+wait this used to do inline.
  vkutil::StagingBatch oneShot(m_device, m_physicalDevice, m_commandPool,
                               m_graphicsQueue);
  vkutil::StagingBatch& batch = m_assetBatch ? *m_assetBatch : oneShot;
  auto uploadViaStaging = [&](VkDeviceSize size, const void* src,
                              VkBufferUsageFlags usage, VkBuffer& outBuffer,
                              VkDeviceMemory& outMemory) {
    return batch.Upload(size, src, usage, outBuffer, outMemory);
  };

  if (!uploadViaStaging(vertexBufferSize, asset->vertices.data(),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, asset->vertexBuffer,
                        asset->vertexBufferMemory)) {
    LOG_ERROR("MeshRenderer: vertex buffer upload failed ({} bytes)",
              vertexBufferSize);
    // Flush before destroying: a batch may hold a recorded copy TARGETING one
    // of these buffers, and freeing the destination of a pending transfer is a
    // use-after-free the moment it executes. Flush is idempotent and the next
    // Upload opens a fresh command buffer, so an in-progress bulk build simply
    // continues with one extra submit.
    batch.Flush();
    DestroyAssetBuffers(asset);
    return false;
  }

  if (!asset->indices.empty() &&
      !uploadViaStaging(indexBufferSize, asset->indices.data(),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, asset->indexBuffer,
                        asset->indexBufferMemory)) {
    LOG_ERROR("MeshRenderer: index buffer upload failed ({} bytes)",
              indexBufferSize);
    batch.Flush(); // see above: the vertex copy is still pending
    DestroyAssetBuffers(asset);
    return false;
  }

  return true;
}

void MeshRenderer::DestroyAssetBuffers(MeshAsset* asset) {
  if (!asset)
    return;

  vkutil::DestroyBuffer(m_device, asset->vertexBuffer,
                        asset->vertexBufferMemory);
  vkutil::DestroyBuffer(m_device, asset->indexBuffer, asset->indexBufferMemory);
}
