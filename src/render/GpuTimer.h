#pragma once

#include "render/FramesInFlight.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace render {

// Per-pass GPU timing via timestamp queries (docs/rendering/frame-timing.md,
// "GPU timestamps"). The CPU frame timer splits OUR work by phase but reports
// the GPU as one opaque display number; this says which PASS the GPU spends
// it on — the question every "slow on my GPU" report needs answered, and the
// one the Intel assessment could not (an offscreen harness cannot even run on
// that GPU; the numbers have to come from the real session).
//
// Four write points per frame: command-buffer start, after the shadow pass,
// after the SSAO chain, after the main pass. The deltas are contiguous, so
// shadow+ssao+main == total by construction.
//
// The lifecycle leans on the frame loop's own synchronization instead of
// adding any: queries live in one slot per swapchain image (the
// FramesInFlight convention), and a slot is harvested right after the frame
// loop has waited that image's fence — at that point the slot's previous
// submission has COMPLETED, so results are read with the availability flag
// and never a wait. A slot that never ran (first use, swapchain rebuild)
// simply reads unavailable and contributes no sample. That is the whole
// reason this costs nothing: no stalls, ~4 counter writes on the GPU, one
// non-blocking readback on the CPU.
class GpuTimer {
public:
  enum class Point : uint32_t {
    Begin = 0,
    AfterShadow = 1,
    AfterSSAO = 2,
    AfterMain = 3,
  };
  static constexpr uint32_t kPoints = 4;

  // Averages over the sample ring, in milliseconds. Pure data — this is what
  // AppState mirrors for frame_stats and the perf HUD.
  struct Snapshot {
    bool supported = false;
    size_t samples = 0; // frames the averages cover (<= ring size)
    double shadowMs = 0.0;
    double ssaoMs = 0.0;
    double mainMs = 0.0;
    double totalMs = 0.0;
  };

  // False (and inert thereafter) when the queue family reports
  // timestampValidBits == 0 — every call below is then a cheap no-op, so no
  // caller needs its own guard.
  bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                  uint32_t queueFamily);
  void Shutdown(VkDevice device);

  // AFTER the slot's fence wait, BEFORE re-recording it: read the slot's
  // previous results into the ring (availability-checked, never waiting).
  void Harvest(uint32_t imageIndex);

  // At command-buffer start, OUTSIDE any render pass: reset the slot's
  // queries and write Point::Begin.
  void BeginFrame(VkCommandBuffer cb, uint32_t imageIndex);
  void Write(VkCommandBuffer cb, uint32_t imageIndex, Point point);

  Snapshot Average() const;

private:
  static constexpr size_t kRing = 120; // ~2 s at 60 fps, ~12 s idle-paced

  bool m_supported = false;
  VkDevice m_device = VK_NULL_HANDLE;
  VkQueryPool m_pool = VK_NULL_HANDLE;
  double m_periodNs = 0.0; // nanoseconds per timestamp tick
  uint64_t m_validMask = ~0ull;

  struct Sample {
    float shadowMs = 0.0f, ssaoMs = 0.0f, mainMs = 0.0f;
  };
  std::array<Sample, kRing> m_ring{};
  size_t m_next = 0;
  size_t m_count = 0;
  // Slots that have had BeginFrame recorded at least once. Reading a slot
  // before its first reset+write is invalid (VUID-vkGetQueryPoolResults-
  // None-09401, caught by the validation layer on this exact code) — the
  // availability flag covers "reset but not finished", not "uninitialized".
  std::array<bool, kMaxFramesInFlight> m_slotUsed{};
};

} // namespace render
