#pragma once

// Headless Vulkan compute for the topology solver
// (docs/topopt/README.md#the-gpu-solver). Lives in fem_gpu, a library separate
// from designer_core, which must stay GPU-free so it can be unit-tested and
// scripted headlessly. No window, no swapchain: instance -> best
// compute-capable device -> queue, plus the small set of primitives the solver
// kernels need (host-visible storage buffers, SPIR-V compute pipelines,
// dispatch + wait). Everything is synchronous and host-visible in GPU0 —
// correctness first; the on-device CG loop (GPU1) restructures for persistence
// and device-local memory.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gpu {

// A host-visible, host-coherent storage buffer with a persistent mapping.
// Allocation prefers DEVICE_LOCAL memory that is also host-visible (resizable
// BAR / iGPU / software devices) so the SpMV streams from VRAM bandwidth, and
// falls back to plain host-visible when the device has no such type.
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

// A pipeline's descriptor set bound to a fixed list of buffers, created once
// and reused across dispatches — the CG loop records hundreds of dispatches
// per submit and its buffer bindings never change.
struct BoundSet {
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
};

class ComputeContext {
public:
  // Picks the best device with a compute queue AND float32 buffer atomicAdd
  // (VK_EXT_shader_atomic_float — the SpMV scatter needs it): discrete GPU
  // first, then integrated, then anything else (llvmpipe software fallback).
  // Returns null with *error set when no usable device exists — the tests'
  // skip signal, and the app's stay-on-CPU signal.
  // `preferredDevice` is the user's GPU preference (settings.gpu_device),
  // matched by the shared rule in common/DevicePreference.h — the same one
  // the renderer uses. Empty means "best available". A named device that
  // cannot do float32 atomics is ignored rather than obeyed.
  static std::unique_ptr<ComputeContext>
  Create(std::string* error, const std::string& preferredDevice = "");
  ~ComputeContext();

  ComputeContext(const ComputeContext&) = delete;
  ComputeContext& operator=(const ComputeContext&) = delete;

  const std::string& DeviceName() const { return m_deviceName; }
  // True when the picked device is a software rasterizer (llvmpipe). Correct
  // but slower than the CPU Eigen path — the app should not auto-select it.
  bool IsSoftwareDevice() const { return m_software; }

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

  // ---- Persistent descriptor sets + recorded batches (the CG loop) --------
  // One CG iteration is ~8 dependent dispatches and a solve is hundreds of
  // iterations; submitting each dispatch alone would drown in round trips.
  // Instead: pre-bind each kernel's buffers once (CreateBoundSet), then record
  // many dispatches into one command buffer and submit the lot.

  bool CreateBoundSet(const Pipeline& p,
                      const std::vector<const Buffer*>& buffers, BoundSet& out,
                      std::string* error);
  void Destroy(BoundSet& s);

  // Begin recording. Only one batch may be open at a time.
  bool BeginBatch(std::string* error);
  // vkCmdFillBuffer (size must be a multiple of 4; VK_WHOLE_SIZE allowed).
  void BatchFill(const Buffer& b, VkDeviceSize offset, VkDeviceSize size,
                 uint32_t value);
  void BatchDispatch(const Pipeline& p, const BoundSet& set, const void* push,
                     uint32_t pushBytes, uint32_t gx, uint32_t gy);
  // Submits the recorded commands and waits for completion.
  bool SubmitBatch(std::string* error);

private:
  // Every batched command is preceded by a full compute/transfer memory
  // barrier — each CG kernel depends on its predecessor, so precision-scoped
  // barriers would buy nothing.
  void BatchBarrier();
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
  bool m_software = false;
  bool m_batchOpen = false;
  bool m_batchFirst = false; // no barrier before the batch's first command
};

} // namespace gpu
