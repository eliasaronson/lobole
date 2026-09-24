#include "common/Watchdog.h"

#include "common/Logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stop_token>
#include <thread>

namespace watchdog {

namespace {

std::atomic<uint64_t> g_beat{0};

// The label, written by the main thread and read by the monitor while the
// main thread may be writing it: a sequence count around the copy (odd while
// writing), and atomic characters so the read is never a data race.
constexpr size_t kLabelSize = 64;
std::array<std::atomic<char>, kLabelSize> g_label{};
std::atomic<uint32_t> g_labelSeq{0};
std::atomic<bool> g_labelWantsStack{true};

void WriteLabel(std::string_view what, bool wantStack) {
  g_labelSeq.fetch_add(1, std::memory_order_acq_rel); // odd: writing
  const size_t n = std::min(what.size(), kLabelSize - 1);
  for (size_t i = 0; i < n; ++i)
    g_label[i].store(what[i], std::memory_order_relaxed);
  g_label[n].store('\0', std::memory_order_relaxed);
  g_labelWantsStack.store(wantStack, std::memory_order_relaxed);
  g_labelSeq.fetch_add(1, std::memory_order_acq_rel); // even: done
}

// A consistent copy, or empty if the main thread kept changing it.
std::string ReadLabel(bool* wantStack) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    const uint32_t before = g_labelSeq.load(std::memory_order_acquire);
    if (before & 1u)
      continue;
    std::string out;
    for (size_t i = 0; i < kLabelSize; ++i) {
      const char c = g_label[i].load(std::memory_order_relaxed);
      if (c == '\0')
        break;
      out += c;
    }
    const bool stack = g_labelWantsStack.load(std::memory_order_relaxed);
    if (g_labelSeq.load(std::memory_order_acquire) == before) {
      *wantStack = stack;
      return out;
    }
  }
  *wantStack = true;
  return {};
}

double Now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::jthread g_thread;
std::function<std::string()> g_stack;

void Monitor(std::stop_token stop, double threshold) {
  StallDetector detector(threshold);
  std::mutex m;
  std::condition_variable_any wake;
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(m);
      wake.wait_for(lock, stop, std::chrono::milliseconds(250),
                    [] { return false; });
    }
    if (stop.stop_requested())
      break;
    switch (detector.Observe(g_beat.load(std::memory_order_relaxed), Now())) {
    case StallDetector::Event::Stalled: {
      bool wantStack = true;
      const std::string label = ReadLabel(&wantStack);
      LOG_WARN("main thread stalled {:.1f} s{}", detector.Duration(),
               label.empty() ? std::string() : " (in " + label + ")");
      if (wantStack && g_stack) {
        const std::string stack = g_stack();
        if (stack.empty())
          LOG_WARN("main thread stack: unavailable");
        else
          LOG_WARN("main thread stack:\n{}", stack);
      }
      break;
    }
    case StallDetector::Event::Resumed:
      LOG_WARN("main thread resumed after {:.1f} s", detector.Duration());
      break;
    case StallDetector::Event::None:
      break;
    }
  }
}

} // namespace

std::atomic<bool> g_selfTest{false};

namespace {
// Not inlined, so the stack names it.
[[gnu::noinline]] void StallSelfTest() {
  LOG_INFO("--stall-test: stalling the main thread for 4 s");
  // Labelled like an RPC is, so the self-test covers the label path too.
  const Label label("stall self-test");
  std::this_thread::sleep_for(std::chrono::seconds(4));
}
} // namespace

void RequestSelfTest() { g_selfTest.store(true, std::memory_order_relaxed); }

void Beat() {
  const uint64_t n = g_beat.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 20 && g_selfTest.load(std::memory_order_relaxed))
    StallSelfTest();
}

Label::Label(std::string_view what, bool wantStack) {
  WriteLabel(what, wantStack);
}
Label::~Label() { WriteLabel({}, true); }

StallDetector::Event StallDetector::Observe(uint64_t beat, double now) {
  // Nothing counts until the loop has beaten once: start-up is not a stall.
  if (!m_armed) {
    if (beat == 0)
      return Event::None;
    m_armed = true;
    m_beat = beat;
    m_since = now;
    return Event::None;
  }
  if (beat != m_beat) {
    const bool wasStalled = m_reported;
    m_duration = now - m_since;
    m_beat = beat;
    m_since = now;
    m_reported = false;
    return wasStalled ? Event::Resumed : Event::None;
  }
  if (!m_reported && m_threshold > 0.0 && now - m_since >= m_threshold) {
    m_reported = true;
    m_duration = now - m_since;
    return Event::Stalled;
  }
  return Event::None;
}

bool ThresholdFromEnvironment(double* seconds, std::string* error) {
  *seconds = 3.0;
  const char* env = std::getenv("DESIGNER_WATCHDOG_S");
  if (env == nullptr || *env == '\0')
    return true;
  char* end = nullptr;
  const double v = std::strtod(env, &end);
  if (end == env || *end != '\0' || v < 0.0) {
    *error = std::string("DESIGNER_WATCHDOG_S=") + env +
             " is not a number of seconds; give the stall threshold in "
             "seconds (default 3), or 0 to turn the watchdog off";
    return false;
  }
  *seconds = v;
  return true;
}

void Start(double thresholdSeconds, std::function<std::string()> stack) {
  if (thresholdSeconds <= 0.0 || g_thread.joinable())
    return;
  g_stack = std::move(stack);
  g_thread = std::jthread(Monitor, thresholdSeconds);
}

void Stop() {
  if (g_thread.joinable()) {
    g_thread.request_stop();
    g_thread.join();
  }
}

} // namespace watchdog
