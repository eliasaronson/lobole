#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Wall-clock accounting for the main loop (docs/rendering/frame-timing.md).
//
// Header-only and clock-free — every entry point takes the timestamp — so the
// statistics can be unit-tested against a synthetic frame sequence instead of
// by watching a HUD. Costs five `glfwGetTime()` calls a frame, so it always
// runs; only the DISPLAY is opt-in.
namespace render {

// Where a frame's time went. The split answers the one question an fps
// readout cannot: is a slow frame our CPU work, or waiting on the GPU?
enum class FramePhase : uint8_t {
  Update = 0, // events, remote requests, document -> renderer sync
  BuildUI,    // ImGui NewFrame..Render: panels, picking, overlays
  Wait,       // vkAcquireNextImage + the in-flight fence (GPU, or vsync)
  Record,     // command buffer recording and submit
  Present,    // vkQueuePresentKHR
  Count
};

inline constexpr size_t kPhaseCount = static_cast<size_t>(FramePhase::Count);

inline const char* PhaseName(FramePhase p) {
  switch (p) {
  case FramePhase::Update:
    return "update";
  case FramePhase::BuildUI:
    return "ui";
  case FramePhase::Wait:
    return "wait";
  case FramePhase::Record:
    return "record";
  case FramePhase::Present:
    return "present";
  default:
    return "?";
  }
}

class FrameTimer {
public:
  // ~4 s at 60 fps: long enough that a drag's tail shows up, short enough
  // that the numbers still track what you are doing right now.
  static constexpr size_t kHistory = 240;

  struct Stats {
    size_t frames = 0;     // frames the window actually holds
    size_t idleFrames = 0; // of those, iterations the idle pacer slowed
    double fps = 0.0;
    double avgMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double maxMs = 0.0;
    std::array<double, kPhaseCount> phaseMs{}; // averages over the window
    // The WORST frame's own phase split — spike attribution. The averages
    // above cannot say which phase held a 50 ms hitch that happened once in
    // the window; this is that frame's breakdown (its total is maxMs).
    // Caveat carried from Mark(): a frame that early-returned (swapchain
    // rebuild) lands its unmarked phases' time in whichever phase closed
    // next, so a resize hitch can read oddly here.
    std::array<double, kPhaseCount> worstPhaseMs{};
    bool worstIdle = false; // the worst frame ran idle-paced (rare; ~10 Hz
                            // iterations do the same work, just later)
  };

  void BeginFrame(double now) {
    m_frameStart = now;
    m_phaseStart = now;
    m_pending = Frame{};
    m_inFrame = true;
  }

  // This iteration ran idle-paced (the loop waited for events before it —
  // docs/rendering/idle-throttle.md). The wait itself is NOT in the frame's
  // time (it happens before BeginFrame); the flag is what lets frame_stats
  // and the HUD say "10 fps because idle", not "10 fps because slow".
  void SetIdlePaced(bool idle) {
    if (m_inFrame)
      m_pending.idle = idle;
  }

  // Closes `phase` at `now`. Phases are closed in order; a phase never marked
  // (an early return on swapchain rebuild) stays zero and its time lands in
  // whatever phase closes next, which is why a rebuild frame reads oddly.
  void Mark(FramePhase phase, double now) {
    if (!m_inFrame)
      return;
    const size_t i = static_cast<size_t>(phase);
    if (i < kPhaseCount)
      m_pending.phase[i] += static_cast<float>((now - m_phaseStart) * 1000.0);
    m_phaseStart = now;
  }

  // The remainder goes to Present, so the phases always sum to the total —
  // an unaccounted sliver reported as nothing at all is how "the frame is
  // slow but every phase is fast" happens.
  void EndFrame(double now) {
    if (!m_inFrame)
      return;
    m_inFrame = false;
    m_pending.phase[static_cast<size_t>(FramePhase::Present)] +=
        static_cast<float>((now - m_phaseStart) * 1000.0);
    m_pending.total = static_cast<float>((now - m_frameStart) * 1000.0);
    m_ring[m_next] = m_pending;
    m_next = (m_next + 1) % kHistory;
    if (m_count < kHistory)
      m_count++;
    m_total++;
  }

  [[nodiscard]] size_t TotalFrames() const { return m_total; }

  // Newest `window` frames, oldest first — the sparkline's input.
  [[nodiscard]] std::vector<float> Recent(size_t window) const {
    const size_t n = std::min(window, m_count);
    std::vector<float> out;
    out.reserve(n);
    for (size_t k = n; k > 0; k--)
      out.push_back(m_ring[(m_next + kHistory - k) % kHistory].total);
    return out;
  }

  [[nodiscard]] Stats Snapshot(size_t window = kHistory) const {
    Stats s;
    const size_t n = std::min(window, m_count);
    s.frames = n;
    if (n == 0)
      return s;

    std::vector<float> totals;
    totals.reserve(n);
    double sum = 0.0;
    const Frame* worst = nullptr;
    for (size_t k = n; k > 0; k--) {
      const Frame& f = m_ring[(m_next + kHistory - k) % kHistory];
      totals.push_back(f.total);
      sum += f.total;
      if (f.idle)
        s.idleFrames++;
      if (worst == nullptr || f.total > worst->total)
        worst = &f;
      for (size_t p = 0; p < kPhaseCount; p++)
        s.phaseMs[p] += f.phase[p];
    }
    for (size_t p = 0; p < kPhaseCount; p++)
      s.worstPhaseMs[p] = worst->phase[p];
    s.worstIdle = worst->idle;
    for (size_t p = 0; p < kPhaseCount; p++)
      s.phaseMs[p] /= static_cast<double>(n);

    s.avgMs = sum / static_cast<double>(n);
    s.fps = s.avgMs > 0.0 ? 1000.0 / s.avgMs : 0.0;
    std::sort(totals.begin(), totals.end());
    s.p50Ms = totals[Rank(n, 0.50)];
    s.p95Ms = totals[Rank(n, 0.95)];
    s.maxMs = totals.back();
    return s;
  }

private:
  struct Frame {
    float total = 0.0f;
    bool idle = false; // iteration was idle-paced (SetIdlePaced)
    std::array<float, kPhaseCount> phase{};
  };

  // Nearest-rank on a sorted array: p95 of 100 frames is the 95th-slowest,
  // and of a single frame is that frame (not an out-of-range index).
  static size_t Rank(size_t n, double q) {
    const double r = std::ceil(q * static_cast<double>(n));
    const size_t idx = r < 1.0 ? 0 : static_cast<size_t>(r) - 1;
    return std::min(idx, n - 1);
  }

  std::array<Frame, kHistory> m_ring{};
  Frame m_pending;
  size_t m_next = 0;  // where the next frame lands
  size_t m_count = 0; // frames held, <= kHistory
  size_t m_total = 0; // frames ever
  double m_frameStart = 0.0;
  double m_phaseStart = 0.0;
  bool m_inFrame = false;
};

} // namespace render
