#pragma once

#include <vulkan/vulkan.h>

class IBLPrecompute;

// Uploads the CPU-precomputed IBL maps (see IBLPrecompute) as sampled Vulkan
// textures for the mesh shader: irradiance (2D), prefiltered specular (2D with
// roughness mips), and the BRDF LUT (2D). Stored as R16G16B16A16_SFLOAT /
// R16G16_SFLOAT so linear filtering is universally supported.
//
// Two samplers: env maps wrap in u / clamp in v (equirectangular, no back
// seam); the LUT clamps both. If precompute is unavailable, MakeFallback()
// creates 1x1 neutral textures so the descriptors are always valid.
class IBLTextures {
public:
  bool Upload(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue,
              VkCommandPool commandPool, const IBLPrecompute& ibl);
  bool MakeFallback(VkDevice device, VkPhysicalDevice physicalDevice,
                    VkQueue queue, VkCommandPool commandPool);
  void Shutdown(VkDevice device);

  bool IsReady() const { return m_ready; }
  VkImageView EnvironmentView() const { return m_environment.view; }
  VkImageView IrradianceView() const { return m_irradiance.view; }
  VkImageView PrefilterView() const { return m_prefilter.view; }
  VkImageView BrdfLutView() const { return m_brdf.view; }
  VkSampler EnvSampler() const { return m_envSampler; }
  VkSampler LutSampler() const { return m_lutSampler; }
  uint32_t PrefilterMips() const { return m_prefilterMips; }

private:
  struct Tex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  bool m_ready = false;
  Tex m_environment; // full-res source HDR (background / sharp reflections)
  Tex m_irradiance;
  Tex m_prefilter;
  Tex m_brdf;
  uint32_t m_prefilterMips = 1;
  VkSampler m_envSampler = VK_NULL_HANDLE;
  VkSampler m_lutSampler = VK_NULL_HANDLE;

  VkDevice m_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkQueue m_queue = VK_NULL_HANDLE;
  VkCommandPool m_commandPool = VK_NULL_HANDLE;

  bool CreateSamplers();
};
