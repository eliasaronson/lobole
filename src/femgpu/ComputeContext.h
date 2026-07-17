#pragma once

// Headless Vulkan compute for the topology solver (docs/topopt/gpu-plan.md).
// Lives in fem_gpu, a library separate from designer_core, which must stay
// GPU-free so it can be unit-tested and scripted headlessly. No window, no
// swapchain: instance -> best compute-capable device -> queue, plus the small
// set of primitives the solver kernels need (host-visible storage buffers,
// SPIR-V compute pipelines, dispatch + wait). Everything is synchronous and
// host-visible in GPU0 — correctness first; the on-device CG loop (GPU1)
// restructures for persistence and device-local memory.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gpu {

// A host-visible, host-coherent storage buffer with a persistent mapping.
struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  void* mapped = nullptr;
};

// A compute pipeline built from embedded SPIR-V: `bufferCount` storage-buffer
// bindings 0..n-1 plus an optional push-constant block.
struct Pipeline {
  VkShaderModule module = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  uint32_t bufferCount = 0;
  uint32_t pushBytes = 0;
};

class ComputeContext {
 public:
  // Picks the best device with a compute queue AND float32 buffer atomicAdd
  // (VK_EXT_shader_atomic_float — the SpMV scatter needs it): discrete GPU
  // first, then integrated, then anything else (llvmpipe software fallback).
  // Returns null with *error set when no usable device exists — the tests'
  // skip signal, and the app's stay-on-CPU signal.
  static std::unique_ptr<ComputeContext> Create(std::string* error);
  ~ComputeContext();

  ComputeContext(const ComputeContext&) = delete;
  ComputeContext& operator=(const ComputeContext&) = delete;

  const std::string& DeviceName() const { return m_deviceName; }

  // Buffers are zero-initialized. False + *error on failure.
  bool CreateBuffer(VkDeviceSize size, Buffer& out, std::string* error);
  void Destroy(Buffer& b);

  bool CreatePipeline(const unsigned char* spirv, size_t spirvBytes,
                      uint32_t bufferCount, uint32_t pushBytes, Pipeline& out,
                      std::string* error);
  void Destroy(Pipeline& p);

  // Records bind + push + dispatch(gx, gy, 1), submits and waits for
  // completion. `buffers` bind to 0..n-1 in order and must match the
  // pipeline's bufferCount. Host-coherent memory means results are visible in
  // the mappings on return.
  bool Dispatch(const Pipeline& p, const std::vector<const Buffer*>& buffers,
                const void* push, uint32_t pushBytes, uint32_t gx, uint32_t gy,
                std::string* error);

 private:
  ComputeContext() = default;

  VkInstance m_instance = VK_NULL_HANDLE;
  VkPhysicalDevice m_phys = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;
  VkQueue m_queue = VK_NULL_HANDLE;
  uint32_t m_queueFamily = 0;
  VkCommandPool m_cmdPool = VK_NULL_HANDLE;
  VkCommandBuffer m_cmd = VK_NULL_HANDLE;
  VkFence m_fence = VK_NULL_HANDLE;
  std::string m_deviceName;
};

}  // namespace gpu
