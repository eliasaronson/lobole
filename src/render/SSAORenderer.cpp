#include "render/SSAORenderer.h"
#include "common/Logger.h"
#include "common/RuntimePaths.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <fstream>
#include <iostream>
#include <random>

namespace {

// std140-clean UBOs (mat4/vec4 only — the trap note in OutlineRenderer.h).
struct PrepassUBO {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 clipPlane;
};

struct SSAOParamsUBO {
    glm::mat4 proj;
    glm::mat4 invProj;
    glm::vec4 params;  // x = radius, y = bias, z = strength, w = unused
    glm::vec4 params2; // xy = noise tiling scale, zw = unused
};

// S3-tuned defaults, all single-site. AO radius scales with the scene bbox
// diagonal but is CAPPED absolutely: uncapped it grew with assemblies until
// samples reached across whole neighboring bodies, ghosting their
// silhouettes onto flat faces (the dogfooding halo bug). 2.5 model units
// keeps the kernel at crevice scale. Strength was raised (0.6 -> 0.9) so
// real creases read now that the tight range window discards the fake
// occlusion that used to contribute. Blur depth-sharpness was raised
// (400 -> 2000) so the bilateral blur rejects cross-silhouette smearing.
constexpr float kRadiusFraction = 0.04f;
constexpr float kRadiusMax = 2.5f;
constexpr float kBiasFraction = 0.02f;
constexpr float kStrength = 0.9f;
constexpr float kBlurDepthSharpness = 2000.0f;

} // namespace

SSAORenderer::SSAORenderer() {}

SSAORenderer::~SSAORenderer() {
    Shutdown();
}

bool SSAORenderer::Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                              VkQueue graphicsQueue, uint32_t queueFamily,
                              VkCommandPool commandPool, uint32_t width,
                              uint32_t height) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_graphicsQueue = graphicsQueue;
    m_queueFamily = queueFamily;
    m_commandPool = commandPool;

    if (!CreateSamplers()) return false;
    if (!CreateRenderPasses()) return false;
    if (!CreateNoiseTexture()) return false;
    if (!CreateUniformBuffers()) return false;
    if (!CreateDescriptors()) return false;
    if (!CreatePipelines()) return false;
    if (!CreateTargets(width, height)) return false;
    WriteTargetDescriptors();
    TransitionTargetsToRead();
    return true;
}

void SSAORenderer::Shutdown() {
    if (m_device == VK_NULL_HANDLE) return;
    // Targets may be referenced by an in-flight frame (sampled by the mesh
    // pass) — destruction goes behind wait-idle (the 1422695 discipline).
    vkDeviceWaitIdle(m_device);

    DestroyTargets();

    if (m_prepassPipeline) vkDestroyPipeline(m_device, m_prepassPipeline, nullptr);
    if (m_aoPipeline) vkDestroyPipeline(m_device, m_aoPipeline, nullptr);
    if (m_blurPipeline) vkDestroyPipeline(m_device, m_blurPipeline, nullptr);
    if (m_prepassPipelineLayout)
        vkDestroyPipelineLayout(m_device, m_prepassPipelineLayout, nullptr);
    if (m_aoPipelineLayout)
        vkDestroyPipelineLayout(m_device, m_aoPipelineLayout, nullptr);
    if (m_blurPipelineLayout)
        vkDestroyPipelineLayout(m_device, m_blurPipelineLayout, nullptr);
    if (m_descriptorPool) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_prepassSetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_prepassSetLayout, nullptr);
    if (m_aoSetLayout) vkDestroyDescriptorSetLayout(m_device, m_aoSetLayout, nullptr);
    if (m_blurSetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_blurSetLayout, nullptr);
    for (size_t i = 0; i < m_prepassUBOs.size(); i++) {
        if (m_prepassUBOs[i]) vkDestroyBuffer(m_device, m_prepassUBOs[i], nullptr);
        if (m_prepassUBOMemory[i]) vkFreeMemory(m_device, m_prepassUBOMemory[i], nullptr);
    }
    for (size_t i = 0; i < m_aoUBOs.size(); i++) {
        if (m_aoUBOs[i]) vkDestroyBuffer(m_device, m_aoUBOs[i], nullptr);
        if (m_aoUBOMemory[i]) vkFreeMemory(m_device, m_aoUBOMemory[i], nullptr);
    }
    m_prepassUBOs.clear();
    m_prepassUBOMemory.clear();
    m_prepassUBOMapped.clear();
    m_aoUBOs.clear();
    m_aoUBOMemory.clear();
    m_aoUBOMapped.clear();
    if (m_noise.view) vkDestroyImageView(m_device, m_noise.view, nullptr);
    if (m_noise.image) vkDestroyImage(m_device, m_noise.image, nullptr);
    if (m_noise.memory) vkFreeMemory(m_device, m_noise.memory, nullptr);
    if (m_nearestSampler) vkDestroySampler(m_device, m_nearestSampler, nullptr);
    if (m_linearSampler) vkDestroySampler(m_device, m_linearSampler, nullptr);
    if (m_noiseSampler) vkDestroySampler(m_device, m_noiseSampler, nullptr);
    if (m_prepassRenderPass) vkDestroyRenderPass(m_device, m_prepassRenderPass, nullptr);
    if (m_r8RenderPass) vkDestroyRenderPass(m_device, m_r8RenderPass, nullptr);

    m_prepassPipeline = m_aoPipeline = m_blurPipeline = VK_NULL_HANDLE;
    m_prepassPipelineLayout = m_aoPipelineLayout = m_blurPipelineLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_prepassSetLayout = m_aoSetLayout = m_blurSetLayout = VK_NULL_HANDLE;
    m_noise = {};
    m_nearestSampler = m_linearSampler = m_noiseSampler = VK_NULL_HANDLE;
    m_prepassRenderPass = m_r8RenderPass = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
}

bool SSAORenderer::Resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return true;
    // The old targets may be in flight; recreation goes behind wait-idle.
    vkDeviceWaitIdle(m_device);
    DestroyTargets();
    if (!CreateTargets(width, height)) return false;
    WriteTargetDescriptors();
    TransitionTargetsToRead();
    return true;
}

bool SSAORenderer::SetFullResolution(bool fullRes) {
    if (fullRes == m_fullRes) return false;
    m_fullRes = fullRes;
    // Same recreation dance as Resize, at the current window size.
    vkDeviceWaitIdle(m_device);
    DestroyTargets();
    if (!CreateTargets(m_width, m_height)) {
        LOG_ERROR("SSAORenderer: SetFullResolution failed to recreate targets");
    }
    WriteTargetDescriptors();
    TransitionTargetsToRead();
    return true;
}

void SSAORenderer::RecordSSAOPasses(VkCommandBuffer cb, uint32_t currentFrame,
                                    MeshRenderer& scene) {
    glm::vec3 bbMin, bbMax;
    const bool haveGeometry = scene.GetShadowCasters(m_casters, bbMin, bbMax);
    const float diag =
        haveGeometry ? std::max(glm::length(bbMax - bbMin), 1e-3f) : 1.0f;
    const float radius = std::min(kRadiusFraction * diag, kRadiusMax);

    const uint32_t frame = currentFrame % MAX_FRAMES_IN_FLIGHT;
    const glm::mat4& view = scene.ViewMatrix();
    const glm::mat4& proj = scene.ProjectionMatrix();

    PrepassUBO pre{};
    pre.view = view;
    pre.proj = proj;
    pre.clipPlane = m_clipPlane;
    memcpy(m_prepassUBOMapped[frame], &pre, sizeof(pre));

    SSAOParamsUBO ao{};
    ao.proj = proj;
    ao.invProj = glm::inverse(proj);
    ao.params = glm::vec4(radius, kBiasFraction * radius, kStrength, 0.0f);
    ao.params2 = glm::vec4(m_aoWidth / float(kNoiseSize),
                           m_aoHeight / float(kNoiseSize), 0.0f, 0.0f);
    memcpy(m_aoUBOMapped[frame], &ao, sizeof(ao));

    // 1. Depth+normal prepass (full res). Always recorded — with no geometry
    // the cleared depth (1.0) makes the AO pass output 1 everywhere and the
    // target layouts stay deterministic.
    {
        VkClearValue clears[2];
        clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = m_prepassRenderPass;
        rp.framebuffer = m_prepassFramebuffer;
        rp.renderArea.extent = {m_width, m_height};
        rp.clearValueCount = 2;
        rp.pClearValues = clears;
        vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {m_width, m_height}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);

        if (haveGeometry) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_prepassPipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_prepassPipelineLayout, 0, 1,
                                    &m_prepassSets[frame], 0, nullptr);
            VkBuffer boundVertexBuffer = VK_NULL_HANDLE;
            for (const ShadowCaster& caster : m_casters) {
                vkCmdPushConstants(cb, m_prepassPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(glm::mat4), &caster.transform);
                if (caster.vertexBuffer != boundVertexBuffer) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cb, 0, 1, &caster.vertexBuffer, &offset);
                    vkCmdBindIndexBuffer(cb, caster.indexBuffer, 0,
                                         VK_INDEX_TYPE_UINT32);
                    boundVertexBuffer = caster.vertexBuffer;
                }
                vkCmdDrawIndexed(cb, caster.indexCount, 1, 0, 0, 0);
            }
        }
        vkCmdEndRenderPass(cb);
    }

    // 2. AO pass (half res, fullscreen triangle).
    {
        VkClearValue clear;
        clear.color = {{1.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = m_r8RenderPass;
        rp.framebuffer = m_aoFramebuffer;
        rp.renderArea.extent = {m_aoWidth, m_aoHeight};
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp{0.0f, 0.0f, (float)m_aoWidth, (float)m_aoHeight, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {m_aoWidth, m_aoHeight}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_aoPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_aoPipelineLayout, 0, 1, &m_aoSets[frame], 0,
                                nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    }

    // 3. Depth-aware blur (half res, fullscreen triangle) into the target
    // that mesh.frag samples.
    {
        VkClearValue clear;
        clear.color = {{1.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = m_r8RenderPass;
        rp.framebuffer = m_blurFramebuffer;
        rp.renderArea.extent = {m_aoWidth, m_aoHeight};
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp{0.0f, 0.0f, (float)m_aoWidth, (float)m_aoHeight, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {m_aoWidth, m_aoHeight}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_blurPipelineLayout, 0, 1, &m_blurSet, 0,
                                nullptr);
        const glm::vec4 blurParams(1.0f / m_aoWidth, 1.0f / m_aoHeight,
                                   kBlurDepthSharpness, 0.0f);
        vkCmdPushConstants(cb, m_blurPipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(blurParams),
                           &blurParams);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    }
}

bool SSAORenderer::CreateTargetImage(uint32_t width, uint32_t height,
                                     VkFormat format, VkImageUsageFlags usage,
                                     VkImageAspectFlags aspect, Target& target) {
    VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img.imageType = VK_IMAGE_TYPE_2D;
    img.format = format;
    img.extent = {width, height, 1};
    img.mipLevels = 1;
    img.arrayLayers = 1;
    img.samples = VK_SAMPLE_COUNT_1_BIT;
    img.tiling = VK_IMAGE_TILING_OPTIMAL;
    img.usage = usage;
    if (vkCreateImage(m_device, &img, nullptr, &target.image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_device, target.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &target.memory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(m_device, target.image, target.memory, 0);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = target.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    return vkCreateImageView(m_device, &view, nullptr, &target.view) == VK_SUCCESS;
}

bool SSAORenderer::CreateTargets(uint32_t width, uint32_t height) {
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    const uint32_t divisor = m_fullRes ? 1 : 2;
    m_aoWidth = std::max(m_width / divisor, 1u);
    m_aoHeight = std::max(m_height / divisor, 1u);

    if (!CreateTargetImage(m_width, m_height, VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT, m_prepassDepth) ||
        !CreateTargetImage(m_width, m_height, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, m_prepassNormal) ||
        !CreateTargetImage(m_aoWidth, m_aoHeight, VK_FORMAT_R8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, m_aoRaw) ||
        !CreateTargetImage(m_aoWidth, m_aoHeight, VK_FORMAT_R8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, m_aoBlur)) {
        LOG_ERROR("SSAORenderer: failed to create targets");
        return false;
    }

    VkImageView prepassAttachments[2] = {m_prepassNormal.view, m_prepassDepth.view};
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = m_prepassRenderPass;
    fb.attachmentCount = 2;
    fb.pAttachments = prepassAttachments;
    fb.width = m_width;
    fb.height = m_height;
    fb.layers = 1;
    if (vkCreateFramebuffer(m_device, &fb, nullptr, &m_prepassFramebuffer) != VK_SUCCESS)
        return false;

    fb.renderPass = m_r8RenderPass;
    fb.attachmentCount = 1;
    fb.width = m_aoWidth;
    fb.height = m_aoHeight;
    fb.pAttachments = &m_aoRaw.view;
    if (vkCreateFramebuffer(m_device, &fb, nullptr, &m_aoFramebuffer) != VK_SUCCESS)
        return false;
    fb.pAttachments = &m_aoBlur.view;
    if (vkCreateFramebuffer(m_device, &fb, nullptr, &m_blurFramebuffer) != VK_SUCCESS)
        return false;

    return true;
}

void SSAORenderer::DestroyTargets() {
    auto destroy = [&](Target& t) {
        if (t.view) vkDestroyImageView(m_device, t.view, nullptr);
        if (t.image) vkDestroyImage(m_device, t.image, nullptr);
        if (t.memory) vkFreeMemory(m_device, t.memory, nullptr);
        t = {};
    };
    if (m_prepassFramebuffer)
        vkDestroyFramebuffer(m_device, m_prepassFramebuffer, nullptr);
    if (m_aoFramebuffer) vkDestroyFramebuffer(m_device, m_aoFramebuffer, nullptr);
    if (m_blurFramebuffer) vkDestroyFramebuffer(m_device, m_blurFramebuffer, nullptr);
    m_prepassFramebuffer = m_aoFramebuffer = m_blurFramebuffer = VK_NULL_HANDLE;
    destroy(m_prepassDepth);
    destroy(m_prepassNormal);
    destroy(m_aoRaw);
    destroy(m_aoBlur);
}

bool SSAORenderer::CreateRenderPasses() {
    // Both passes end in SHADER_READ_ONLY and carry their hazards as subpass
    // dependencies (the S1 pattern): previous-frame fragment reads must
    // finish before this frame's writes (WAR), and the writes must finish
    // before this frame's fragment reads.
    {
        VkAttachmentDescription atts[2]{};
        atts[0].format = VK_FORMAT_R8G8B8A8_UNORM; // view-space normal
        atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        atts[1] = atts[0];
        atts[1].format = VK_FORMAT_D32_SFLOAT;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 2;
        rp.pAttachments = atts;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 2;
        rp.pDependencies = deps;
        if (vkCreateRenderPass(m_device, &rp, nullptr, &m_prepassRenderPass) != VK_SUCCESS) {
            LOG_ERROR("SSAORenderer: failed to create prepass render pass");
            return false;
        }
    }
    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &colorRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 2;
        rp.pDependencies = deps;
        if (vkCreateRenderPass(m_device, &rp, nullptr, &m_r8RenderPass) != VK_SUCCESS) {
            LOG_ERROR("SSAORenderer: failed to create R8 render pass");
            return false;
        }
    }
    return true;
}

bool SSAORenderer::CreateNoiseTexture() {
    // 4x4 rotation vectors in the tangent xy-plane, tiled across the AO
    // target (nearest + repeat sampling). Fixed seed: deterministic frames.
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    glm::vec4 noise[kNoiseSize * kNoiseSize];
    for (auto& n : noise) n = glm::vec4(dist(rng), dist(rng), 0.0f, 0.0f);

    if (!CreateTargetImage(kNoiseSize, kNoiseSize, VK_FORMAT_R32G32B32A32_SFLOAT,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, m_noise)) {
        LOG_ERROR("SSAORenderer: failed to create noise texture");
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!CreateBuffer(sizeof(noise), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, stagingMemory))
        return false;
    void* data = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, sizeof(noise), 0, &data);
    memcpy(data, noise, sizeof(noise));
    vkUnmapMemory(m_device, stagingMemory);

    VkCommandBufferAllocateInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
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
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = m_noise.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kNoiseSize, kNoiseSize, 1};
    vkCmdCopyBufferToImage(cb, staging, m_noise.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    vkEndCommandBuffer(cb);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cb);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    return true;
}

bool SSAORenderer::CreateSamplers() {
    VkSamplerCreateInfo smp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    smp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    // Depth: NEAREST — linear filtering of D32_SFLOAT is not a guaranteed
    // format feature.
    smp.magFilter = VK_FILTER_NEAREST;
    smp.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(m_device, &smp, nullptr, &m_nearestSampler) != VK_SUCCESS)
        return false;

    smp.magFilter = VK_FILTER_LINEAR;
    smp.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(m_device, &smp, nullptr, &m_linearSampler) != VK_SUCCESS)
        return false;

    smp.magFilter = VK_FILTER_NEAREST;
    smp.minFilter = VK_FILTER_NEAREST;
    smp.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    return vkCreateSampler(m_device, &smp, nullptr, &m_noiseSampler) == VK_SUCCESS;
}

bool SSAORenderer::CreateUniformBuffers() {
    m_prepassUBOs.resize(MAX_FRAMES_IN_FLIGHT);
    m_prepassUBOMemory.resize(MAX_FRAMES_IN_FLIGHT);
    m_prepassUBOMapped.resize(MAX_FRAMES_IN_FLIGHT);
    m_aoUBOs.resize(MAX_FRAMES_IN_FLIGHT);
    m_aoUBOMemory.resize(MAX_FRAMES_IN_FLIGHT);
    m_aoUBOMapped.resize(MAX_FRAMES_IN_FLIGHT);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!CreateBuffer(sizeof(PrepassUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          m_prepassUBOs[i], m_prepassUBOMemory[i]))
            return false;
        vkMapMemory(m_device, m_prepassUBOMemory[i], 0, sizeof(PrepassUBO), 0,
                    &m_prepassUBOMapped[i]);
        if (!CreateBuffer(sizeof(SSAOParamsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          m_aoUBOs[i], m_aoUBOMemory[i]))
            return false;
        vkMapMemory(m_device, m_aoUBOMemory[i], 0, sizeof(SSAOParamsUBO), 0,
                    &m_aoUBOMapped[i]);
    }
    return true;
}

bool SSAORenderer::CreateDescriptors() {
    // Layouts: prepass {0: UBO, vertex}; AO {0: UBO + 1: depth + 2: normal +
    // 3: noise, fragment}; blur {0: raw AO + 1: depth, fragment}.
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_prepassSetLayout) != VK_SUCCESS)
            return false;
    }
    {
        VkDescriptorSetLayoutBinding bindings[4]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        for (int i = 1; i <= 3; i++) {
            bindings[i].binding = static_cast<uint32_t>(i);
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 4;
        li.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_aoSetLayout) != VK_SUCCESS)
            return false;
    }
    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        for (int i = 0; i < 2; i++) {
            bindings[i].binding = static_cast<uint32_t>(i);
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 2;
        li.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_blurSetLayout) != VK_SUCCESS)
            return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT * 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT * 3 + 2;
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 2;
    pi.pPoolSizes = poolSizes;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT * 2 + 1;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    std::vector<VkDescriptorSetLayout> prepassLayouts(MAX_FRAMES_IN_FLIGHT, m_prepassSetLayout);
    std::vector<VkDescriptorSetLayout> aoLayouts(MAX_FRAMES_IN_FLIGHT, m_aoSetLayout);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_descriptorPool;
    ai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    ai.pSetLayouts = prepassLayouts.data();
    m_prepassSets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(m_device, &ai, m_prepassSets.data()) != VK_SUCCESS)
        return false;
    ai.pSetLayouts = aoLayouts.data();
    m_aoSets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(m_device, &ai, m_aoSets.data()) != VK_SUCCESS)
        return false;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_blurSetLayout;
    if (vkAllocateDescriptorSets(m_device, &ai, &m_blurSet) != VK_SUCCESS)
        return false;

    // UBO bindings are stable; image bindings go via WriteTargetDescriptors
    // (they change on resize).
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo pre{m_prepassUBOs[i], 0, sizeof(PrepassUBO)};
        VkDescriptorBufferInfo ao{m_aoUBOs[i], 0, sizeof(SSAOParamsUBO)};
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_prepassSets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &pre;
        writes[1] = writes[0];
        writes[1].dstSet = m_aoSets[i];
        writes[1].pBufferInfo = &ao;
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    }
    return true;
}

void SSAORenderer::WriteTargetDescriptors() {
    VkDescriptorImageInfo depth{m_nearestSampler, m_prepassDepth.view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo normal{m_linearSampler, m_prepassNormal.view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo noise{m_noiseSampler, m_noise.view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo aoRaw{m_linearSampler, m_aoRaw.view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    std::vector<VkWriteDescriptorSet> writes;
    auto push = [&](VkDescriptorSet set, uint32_t binding,
                    const VkDescriptorImageInfo* info) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo = info;
        writes.push_back(w);
    };
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        push(m_aoSets[i], 1, &depth);
        push(m_aoSets[i], 2, &normal);
        push(m_aoSets[i], 3, &noise);
    }
    push(m_blurSet, 0, &aoRaw);
    push(m_blurSet, 1, &depth);
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void SSAORenderer::TransitionTargetsToRead() {
    // One-shot UNDEFINED -> SHADER_READ_ONLY for the freshly (re)created
    // targets, so every bound descriptor is legal even before the passes
    // first run (or while AO is toggled off). The render passes re-enter
    // from UNDEFINED, so this holds between disabled frames too.
    VkCommandBufferAllocateInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbInfo.commandPool = m_commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &cbInfo, &cb);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    const struct {
        VkImage image;
        VkImageAspectFlags aspect;
    } targets[4] = {
        {m_prepassDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT},
        {m_prepassNormal.image, VK_IMAGE_ASPECT_COLOR_BIT},
        {m_aoRaw.image, VK_IMAGE_ASPECT_COLOR_BIT},
        {m_aoBlur.image, VK_IMAGE_ASPECT_COLOR_BIT},
    };
    VkImageMemoryBarrier barriers[4]{};
    for (int i = 0; i < 4; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].image = targets[i].image;
        barriers[i].subresourceRange = {targets[i].aspect, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 4, barriers);
    vkEndCommandBuffer(cb);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cb);
}

bool SSAORenderer::CreatePipelines() {
    const std::string dir = RuntimePaths::ShaderDir().string() + "/";
    auto load = [&](const char* name) { return ReadFile(dir + name); };
    auto prepassVert = load("ssao_prepass_vert.spv");
    auto prepassFrag = load("ssao_prepass_frag.spv");
    auto fullscreenVert = load("fullscreen_vert.spv");
    auto ssaoFrag = load("ssao_frag.spv");
    auto blurFrag = load("ssao_blur_frag.spv");
    if (prepassVert.empty() || prepassFrag.empty() || fullscreenVert.empty() ||
        ssaoFrag.empty() || blurFrag.empty()) {
        LOG_ERROR("SSAORenderer: failed to load shaders from {}", dir);
        return false;
    }

    VkShaderModule modules[5] = {
        CreateShaderModule(prepassVert), CreateShaderModule(prepassFrag),
        CreateShaderModule(fullscreenVert), CreateShaderModule(ssaoFrag),
        CreateShaderModule(blurFrag)};
    auto destroyModules = [&] {
        for (VkShaderModule m : modules)
            if (m) vkDestroyShaderModule(m_device, m, nullptr);
    };
    for (VkShaderModule m : modules) {
        if (m == VK_NULL_HANDLE) {
            destroyModules();
            return false;
        }
    }

    // Shared fixed-function state (dynamic viewport/scissor so resize never
    // rebuilds pipelines).
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo multisampling{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlending{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;
    VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                       VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.subpass = 0;

    // 1. Prepass: Vertex3D input (position + normal), back-face culling and
    // depth state matching the main mesh pipeline, into the prepass RP.
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = modules[0];
        stages[0].pName = "main";
        stages[1] = stages[0];
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = modules[1];

        auto binding = Vertex3D::getBindingDescription();
        auto attrs = Vertex3D::getAttributeDescriptions();
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<uint32_t>(attrs.size());
        vertexInput.pVertexAttributeDescriptions = attrs.data();

        VkPipelineRasterizationStateCreateInfo prepassRaster = rasterizer;
        prepassRaster.cullMode = VK_CULL_MODE_BACK_BIT; // as the main pass

        VkPipelineDepthStencilStateCreateInfo depthStencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT, 0,
                                      sizeof(glm::mat4)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &m_prepassSetLayout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(m_device, &li, nullptr, &m_prepassPipelineLayout) != VK_SUCCESS) {
            destroyModules();
            return false;
        }

        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pRasterizationState = &prepassRaster;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.layout = m_prepassPipelineLayout;
        pipelineInfo.renderPass = m_prepassRenderPass;
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                      nullptr, &m_prepassPipeline) != VK_SUCCESS) {
            LOG_ERROR("SSAORenderer: failed to create prepass pipeline");
            destroyModules();
            return false;
        }
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pDepthStencilState = nullptr;
    }

    // 2 + 3. Fullscreen passes: no vertex input, no depth.
    VkPipelineVertexInputStateCreateInfo emptyInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    pipelineInfo.pVertexInputState = &emptyInput;
    pipelineInfo.renderPass = m_r8RenderPass;
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = modules[2];
        stages[0].pName = "main";
        stages[1] = stages[0];
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = modules[3];

        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &m_aoSetLayout;
        if (vkCreatePipelineLayout(m_device, &li, nullptr, &m_aoPipelineLayout) != VK_SUCCESS) {
            destroyModules();
            return false;
        }
        pipelineInfo.pStages = stages;
        pipelineInfo.layout = m_aoPipelineLayout;
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                      nullptr, &m_aoPipeline) != VK_SUCCESS) {
            LOG_ERROR("SSAORenderer: failed to create AO pipeline");
            destroyModules();
            return false;
        }
    }
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = modules[2];
        stages[0].pName = "main";
        stages[1] = stages[0];
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = modules[4];

        VkPushConstantRange pushRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(glm::vec4)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &m_blurSetLayout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(m_device, &li, nullptr, &m_blurPipelineLayout) != VK_SUCCESS) {
            destroyModules();
            return false;
        }
        pipelineInfo.pStages = stages;
        pipelineInfo.layout = m_blurPipelineLayout;
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                      nullptr, &m_blurPipeline) != VK_SUCCESS) {
            LOG_ERROR("SSAORenderer: failed to create blur pipeline");
            destroyModules();
            return false;
        }
    }

    destroyModules();
    return true;
}

bool SSAORenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags properties,
                                VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bi, nullptr, &buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, properties);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &memory) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(m_device, buffer, memory, 0);
    return true;
}

uint32_t SSAORenderer::FindMemoryType(uint32_t typeFilter,
                                      VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

std::vector<char> SSAORenderer::ReadFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("SSAORenderer: failed to open {}", filename);
        return {};
    }
    const size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    return buffer;
}

VkShaderModule SSAORenderer::CreateShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &ci, nullptr, &shaderModule) != VK_SUCCESS) {
        LOG_ERROR("SSAORenderer: failed to create shader module");
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}
