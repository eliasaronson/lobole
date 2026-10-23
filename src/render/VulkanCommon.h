// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include <string>
#include <vector>

// Vulkan boilerplate shared by every renderer.
//
// These existed as private members of each renderer class — FindMemoryType 11
// times, CreateBuffer 7, shader loading 11 — and the copies had drifted into
// three different answers for "what does FindMemoryType failure mean":
//
// - `throw std::runtime_error` in Mesh, Outline, RayTracing, Sketch, Field
//   and Underlay;
// - `return 0` in SSAO, Shadow and main.cpp;
// - `return UINT32_MAX` in IBLTextures and Screenshot.
//
// `return 0` is the dangerous one: memory type 0 is a VALID index, so those
// copies allocated from whatever heap happens to be first instead of failing —
// a HOST_VISIBLE request silently satisfied by DEVICE_LOCAL memory, and the
// vkMapMemory that follows fails (or the upload never lands) far from the
// cause. Only UINT32_MAX is a value vkAllocateMemory actually rejects.
//
// So: one contract, stated once. Nothing here throws. Every function reports
// failure in its return value, and every out-parameter is VK_NULL_HANDLE on
// every failure path — a caller that ignores the result still gets a null
// handle rather than an uninitialized one.
//
// The shape is deliberately free functions over explicit handles rather than a
// base class: the renderers already each store the device/queue/pool they need
// and share no other behavior, so inheritance would buy nothing and force
// every one of them to change shape at once. femgpu/ComputeContext.h is the
// same idea one level up (it owns its device); this is the borrowed-handle
// version for renderers that are handed one.
namespace vkutil {

// No memory type satisfies the request. Passing this to vkAllocateMemory is a
// clean failure; passing 0 is a silent wrong answer, which is why this exists.
inline constexpr uint32_t kNoMemoryType = UINT32_MAX;

// Index of a memory type matching `typeFilter` with ALL of `props`, or
// kNoMemoryType. Logs the failure — it means the device cannot serve the
// request at all, which is worth seeing even when the caller handles it.
uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter,
                        VkMemoryPropertyFlags props);

// Create a buffer and bind fresh memory to it. `buffer` and `memory` are set
// to VK_NULL_HANDLE on every failure path, including the one where the buffer
// was created but its memory could not be allocated (the buffer is destroyed
// rather than leaked).
//
// A usage containing VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT automatically
// gets VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT on the allocation — required for
// the ray-tracing SBT and acceleration structures, and harmless otherwise.
bool CreateBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags props, VkBuffer& buffer,
                  VkDeviceMemory& memory);

// A buffer for the GPU to copy INTO and the CPU to read: host-visible and,
// where the device has it, host-CACHED. Plain HOST_COHERENT memory is
// uncached on a discrete GPU, and reading it a byte at a time is two orders
// of magnitude slower than the copy that filled it. Read it through
// MapReadback, which invalidates the mapping for a non-coherent type.
bool CreateReadbackBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                          VkDeviceSize size, VkBuffer& buffer,
                          VkDeviceMemory& memory);
// Map a readback buffer's memory (whole) and make the GPU's writes visible.
// nullptr on failure. Unmap with vkUnmapMemory.
const void* MapReadback(VkDevice device, VkDeviceMemory memory);

// Destroy a buffer and free its memory, tolerating nulls, and null both
// handles so a double-destroy is a no-op rather than a use-after-free.
// `mapped`, when given, is unmapped first and cleared.
void DestroyBuffer(VkDevice device, VkBuffer& buffer, VkDeviceMemory& memory,
                   void** mapped = nullptr);

// Allocate and bind memory for an already-created image. `memory` is
// VK_NULL_HANDLE on failure.
bool AllocateImageMemory(VkDevice device, VkPhysicalDevice physicalDevice,
                         VkImage image, VkMemoryPropertyFlags props,
                         VkDeviceMemory& memory);

// How many device allocations this process has made, ever. Every buffer and
// every image here takes its OWN `vkAllocateMemory` — there is no
// sub-allocator — and a 299-part assembly builds 1196 of them for 51 MB.
//
// This exists because that sounded like a ceiling and turned out not to be
// one, which is the reason to measure rather than reason. `maxMemoryAllocation
// Count` has a Vulkan FLOOR of 4096, and a review inferred from that a limit
// of "around 1000 parts". Measured 2026-09-01 on every device this project
// targets — NVIDIA RTX 4060, Intel Arc (MTL), and llvmpipe, which is what CI
// runs — all three report **4294967295**. There is no handle ceiling on any of
// them, and the sub-allocator that finding implied is not needed.
//
// The counter stays because it is what settled that, and because a driver
// reporting the spec floor is exactly what a port would meet: app/GpuOwners.cpp
// logs the limit beside the device name, so a machine that DOES impose one says
// so on the first line of its log rather than at the allocation that fails.
//
// Monotonic on purpose. A live count would need every `vkFreeMemory` site to
// route through one place, and a counter that drifts is worse than none. The
// question it answers is "how many did that operation make" — a delta across
// the operation, which is also a work-volume signal independent of any limit.
[[nodiscard]] uint64_t TotalDeviceAllocations();

// A primary command buffer already in the recording state, for a one-shot
// upload or layout transition. VK_NULL_HANDLE if allocation failed.
VkCommandBuffer BeginSingleTimeCommands(VkDevice device, VkCommandPool pool);

// End, submit, WAIT FOR IDLE, and free. Pairs with BeginSingleTimeCommands;
// tolerates VK_NULL_HANDLE so the caller need not branch. Returns the
// submit's result, or the wait's when the submit succeeded (NoteResult has
// seen both).
VkResult EndSingleTimeCommands(VkDevice device, VkCommandPool pool,
                               VkQueue queue, VkCommandBuffer cb);

// Every one-shot submit and wait in the renderers passes its result through
// here, and VK_ERROR_DEVICE_LOST is remembered: the frame loop reads it
// (DeviceLostSeen), marks the device lost, and the next frame boundary
// rebuilds it in place (docs/rendering/README.md#rebuilding-the-device). They
// used to ignore the result, so a loss there passed in silence and the frame
// loop aborted later. Thread-safe. Returns `result`.
VkResult NoteResult(VkResult result);
[[nodiscard]] bool DeviceLostSeen();
// The loss is behind us: the device was rebuilt in place
// (docs/rendering/README.md#rebuilding-the-device), and a loss seen from now on
// is the new device's.
void ClearDeviceLost();

// Staging copy: begin, vkCmdCopyBuffer, end + wait. False if the command
// buffer could not be allocated.
bool CopyBuffer(VkDevice device, VkCommandPool pool, VkQueue queue,
                VkBuffer src, VkBuffer dst, VkDeviceSize size);

// Many device-local uploads behind ONE submit and ONE wait.
//
// The per-buffer form above (CopyBuffer -> EndSingleTimeCommands) drains the
// queue with vkQueueWaitIdle every time. That is fine for one asset and ruinous
// in bulk: a 299-part assembly builds 1196 buffers / 51 MB, so it paid 1196
// full queue drains — around 4 s of a 5.9 s resync. Batched, it is one.
//
// Usage: construct, Upload() each buffer, then Flush() (or let the destructor
// do it). Every Upload records into one command buffer and keeps its staging
// buffer alive until the flush, so the SOURCE data must stay valid only for
// the Upload call itself — it is memcpy'd into staging immediately.
//
// The destination handles are usable only after a successful Flush.
class StagingBatch {
public:
  StagingBatch(VkDevice device, VkPhysicalDevice physicalDevice,
               VkCommandPool pool, VkQueue queue);
  ~StagingBatch(); // flushes if the caller did not

  StagingBatch(const StagingBatch&) = delete;
  StagingBatch& operator=(const StagingBatch&) = delete;

  // Creates the device-local buffer, copies `src` into a staging buffer, and
  // records the transfer. False (with both out-handles null) if any allocation
  // failed; the batch is then poisoned and Flush() reports false too, so a
  // caller may check only the final Flush if it prefers.
  bool Upload(VkDeviceSize size, const void* src, VkBufferUsageFlags usage,
              VkBuffer& outBuffer, VkDeviceMemory& outMemory);

  // Submits everything recorded, waits, and frees the staging buffers. Safe to
  // call twice (the second is a no-op) and safe with nothing recorded.
  bool Flush();

  // How many uploads are pending — the number a naive path would have turned
  // into separate submits. For logging the win, not for control flow.
  [[nodiscard]] size_t Pending() const { return m_staging.size(); }

private:
  VkDevice m_device = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkCommandPool m_pool = VK_NULL_HANDLE;
  VkQueue m_queue = VK_NULL_HANDLE;
  VkCommandBuffer m_cb = VK_NULL_HANDLE;
  struct Staged {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
  };
  std::vector<Staged> m_staging;
  bool m_failed = false;
};

// Whole-file read for SPIR-V. Empty on failure (logged) — callers check
// `.empty()` rather than catching.
std::vector<char> ReadFile(const std::string& path);

// VK_NULL_HANDLE on failure (logged). Empty `code` is a failure, not a
// zero-size module.
VkShaderModule CreateShaderModule(VkDevice device,
                                  const std::vector<char>& code);

// ReadFile + CreateShaderModule against RuntimePaths::ShaderDir(), which is
// how every renderer resolves its own shaders. `spvName` is a bare filename,
// e.g. "sketch_vert.spv". VK_NULL_HANDLE on failure (logged).
VkShaderModule LoadShaderModule(VkDevice device, const std::string& spvName);

} // namespace vkutil
