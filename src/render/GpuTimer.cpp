#include "render/GpuTimer.h"

#include "common/Logger.h"
#include "render/FramesInFlight.h"

#include <vector>

namespace render {

bool GpuTimer::Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                          uint32_t queueFamily) {
  m_device = device;

  uint32_t familyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
                                           nullptr);
  std::vector<VkQueueFamilyProperties> families(familyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
                                           families.data());
  const uint32_t validBits =
      queueFamily < familyCount ? families[queueFamily].timestampValidBits : 0;
  if (validBits == 0) {
    // A queue with no timestamp support (permitted by the spec; seen on some
    // software rasterizers). Degrade to "no GPU numbers", never to a crash.
    LOG_INFO("GpuTimer: queue family {} has no timestamp support", queueFamily);
    m_supported = false;
    return false;
  }
  m_validMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physicalDevice, &props);
  m_periodNs = static_cast<double>(props.limits.timestampPeriod);

  VkQueryPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = kMaxFramesInFlight * kPoints;
  if (vkCreateQueryPool(m_device, &info, nullptr, &m_pool) != VK_SUCCESS) {
    LOG_WARN("GpuTimer: query pool creation failed; GPU timing disabled");
    m_supported = false;
    return false;
  }
  m_supported = true;
  return true;
}

void GpuTimer::Shutdown(VkDevice device) {
  if (m_pool != VK_NULL_HANDLE)
    vkDestroyQueryPool(device, m_pool, nullptr);
  m_pool = VK_NULL_HANDLE;
  m_supported = false;
}

void GpuTimer::Harvest(uint32_t imageIndex) {
  if (!m_supported)
    return;
  const uint32_t slot = FrameSlot(imageIndex, "GpuTimer");
  if (!m_slotUsed[slot])
    return; // never recorded: the queries are uninitialized, not just unready
  // [value, availability] per query. Availability instead of WAIT is the
  // whole design: the caller has already fence-waited this slot's previous
  // submission, so a written slot is available; an unwritten one (first use,
  // a swapchain-rebuild skip) reads unavailable and contributes nothing.
  // Waiting here would be the classic way GPU timing costs 10%.
  uint64_t data[kPoints][2] = {};
  const VkResult res = vkGetQueryPoolResults(
      m_device, m_pool, slot * kPoints, kPoints, sizeof(data), data,
      2 * sizeof(uint64_t),
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if (res != VK_SUCCESS && res != VK_NOT_READY)
    return;
  for (const auto& q : data)
    if (q[1] == 0)
      return; // not a full frame's worth: no sample
  const auto ms = [&](uint64_t from, uint64_t to) {
    // Masked subtraction handles sub-64-bit counters wrapping mid-frame.
    return static_cast<float>(((to - from) & m_validMask) * m_periodNs * 1e-6);
  };
  Sample s;
  s.shadowMs = ms(data[0][0], data[1][0]);
  s.ssaoMs = ms(data[1][0], data[2][0]);
  s.mainMs = ms(data[2][0], data[3][0]);
  m_ring[m_next] = s;
  m_next = (m_next + 1) % kRing;
  if (m_count < kRing)
    m_count++;
}

void GpuTimer::BeginFrame(VkCommandBuffer cb, uint32_t imageIndex) {
  if (!m_supported)
    return;
  const uint32_t slot = FrameSlot(imageIndex, "GpuTimer");
  m_slotUsed[slot] = true;
  vkCmdResetQueryPool(cb, m_pool, slot * kPoints, kPoints);
  // TOP for the start (when the GPU begins this buffer), BOTTOM for every
  // later point (when the pass's work has drained) — the usual pairing.
  vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_pool,
                      slot * kPoints + static_cast<uint32_t>(Point::Begin));
}

void GpuTimer::Write(VkCommandBuffer cb, uint32_t imageIndex, Point point) {
  if (!m_supported)
    return;
  const uint32_t slot = FrameSlot(imageIndex, "GpuTimer");
  vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool,
                      slot * kPoints + static_cast<uint32_t>(point));
}

GpuTimer::Snapshot GpuTimer::Average() const {
  Snapshot s;
  s.supported = m_supported;
  s.samples = m_count;
  if (m_count == 0)
    return s;
  for (size_t i = 0; i < m_count; i++) {
    const Sample& f = m_ring[i];
    s.shadowMs += f.shadowMs;
    s.ssaoMs += f.ssaoMs;
    s.mainMs += f.mainMs;
  }
  const double n = static_cast<double>(m_count);
  s.shadowMs /= n;
  s.ssaoMs /= n;
  s.mainMs /= n;
  s.totalMs = s.shadowMs + s.ssaoMs + s.mainMs;
  return s;
}

} // namespace render
