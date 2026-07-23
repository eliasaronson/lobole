#pragma once

#include "render/MeshRenderer.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>

// SSAO (S2, docs/rendering/ssao-soft-shadows.md): three passes recorded
// before the main render pass each frame —
//   1. depth+normal prepass (full-res, non-MSAA): the opaque scene into a
//      D32 depth + RGBA8 view-space-normal target (same instance set and
//      section-clip behavior as the main pass's opaque loop);
//   2. AO (half-res R8): 16-sample hemisphere kernel + 4x4 tiled rotation
//      noise, fullscreen-triangle fragment pass;
//   3. depth-aware 4x4 bilateral blur (half-res R8) — its output is what
//      mesh.frag multiplies into the ambient/IBL term (AOView()).
// All targets are window-resolution-dependent: call Resize on swapchain
// rebuild (waits for idle internally) and rebind AOView() into the mesh
// renderer. Each pass's subpass dependencies order write vs. fragment-sample
// (the S1 ShadowRenderer pattern), so the call site needs no barriers.
class SSAORenderer {
public:
    SSAORenderer();
    ~SSAORenderer();

    bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    VkQueue graphicsQueue, uint32_t queueFamily,
                    VkCommandPool commandPool, uint32_t width, uint32_t height);
    void Shutdown();

    // Recreates the resolution-dependent targets (swapchain resize). Waits
    // for idle; rebind AOView() into MeshRenderer afterwards.
    bool Resize(uint32_t width, uint32_t height);

    // AO target resolution (View > Render Quality / set_render_options RPC):
    // full-res skips the half-res downscale. Recreates the targets behind
    // wait-idle when the mode changes; returns true when recreated — the
    // caller must rebind AOView() like after Resize.
    bool SetFullResolution(bool fullRes);
    bool FullResolution() const { return m_fullRes; }

    // Section clip plane (world): matches MeshRenderer::SetClipPlane —
    // sectioned-away material must not occlude.
    void SetClipPlane(const glm::vec4& plane) { m_clipPlane = plane; }

    // Records prepass + AO + blur. Camera matrices are read from `scene`
    // (guaranteed identical to what the main pass will use); the AO radius is
    // fit from the scene bbox. Call before the main render pass begins.
    void RecordSSAOPasses(VkCommandBuffer cb, uint32_t currentFrame,
                          MeshRenderer& scene);

    // Blurred AO for MeshRenderer::SetAOMap / RebindAOMap.
    VkImageView AOView() const { return m_aoBlur.view; }
    VkSampler AOSampler() const { return m_linearSampler; }
    // For mesh.frag's gl_FragCoord -> AO uv mapping.
    glm::vec2 InvFullResolution() const {
        return glm::vec2(1.0f / m_width, 1.0f / m_height);
    }

    static constexpr uint32_t kNoiseSize = 4;

private:
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool CreateTargets(uint32_t width, uint32_t height);
    void DestroyTargets();
    bool CreateRenderPasses();
    bool CreateNoiseTexture();
    bool CreateSamplers();
    bool CreateUniformBuffers();
    bool CreateDescriptors();
    void WriteTargetDescriptors(); // (re)binds target views after (re)creation
    bool CreatePipelines();
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties, VkBuffer& buffer,
                      VkDeviceMemory& memory);
    bool CreateTargetImage(uint32_t width, uint32_t height, VkFormat format,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect,
                           Target& target);
    void TransitionTargetsToRead(); // one-shot: all targets -> SHADER_READ_ONLY
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    std::vector<char> ReadFile(const std::string& filename);
    VkShaderModule CreateShaderModule(const std::vector<char>& code);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;

    // Resolution-dependent targets (Resize recreates).
    uint32_t m_width = 0, m_height = 0;   // full res (prepass)
    uint32_t m_aoWidth = 0, m_aoHeight = 0; // AO + blur (half res by default)
    bool m_fullRes = false; // full-res AO targets (quality setting)
    Target m_prepassDepth;  // D32
    Target m_prepassNormal; // RGBA8, view-space normal
    Target m_aoRaw;         // R8
    Target m_aoBlur;        // R8 — what mesh.frag samples
    VkFramebuffer m_prepassFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer m_aoFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer m_blurFramebuffer = VK_NULL_HANDLE;

    // Resolution-independent objects (created once).
    VkRenderPass m_prepassRenderPass = VK_NULL_HANDLE; // normal + depth
    VkRenderPass m_r8RenderPass = VK_NULL_HANDLE;      // AO and blur passes
    Target m_noise; // 4x4 RGBA32F rotation vectors
    VkSampler m_nearestSampler = VK_NULL_HANDLE; // depth (no linear-filter guarantee on D32)
    VkSampler m_linearSampler = VK_NULL_HANDLE;  // normals + AO
    VkSampler m_noiseSampler = VK_NULL_HANDLE;   // nearest + repeat

    VkDescriptorSetLayout m_prepassSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_aoSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_blurSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_prepassPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_aoPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_blurPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_prepassPipeline = VK_NULL_HANDLE;
    VkPipeline m_aoPipeline = VK_NULL_HANDLE;
    VkPipeline m_blurPipeline = VK_NULL_HANDLE;

    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_prepassSets; // per frame (UBO)
    std::vector<VkDescriptorSet> m_aoSets;      // per frame (UBO + images)
    VkDescriptorSet m_blurSet = VK_NULL_HANDLE; // images only

    std::vector<VkBuffer> m_prepassUBOs;
    std::vector<VkDeviceMemory> m_prepassUBOMemory;
    std::vector<void*> m_prepassUBOMapped;
    std::vector<VkBuffer> m_aoUBOs;
    std::vector<VkDeviceMemory> m_aoUBOMemory;
    std::vector<void*> m_aoUBOMapped;

    glm::vec4 m_clipPlane = glm::vec4(0.0f); // section view; 0 = disabled

    // Scratch reused across frames (avoids per-frame allocations).
    std::vector<ShadowCaster> m_casters;

    static const int MAX_FRAMES_IN_FLIGHT = 2;
};
