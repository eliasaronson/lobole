// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// The frame watchdog: when the main thread stops coming round its loop for a
// few seconds — a hang, or work far longer than a frame — the log says so,
// says what the thread was doing if that is known, and prints its stack.
// The crash handler covers the process dying; this covers it stopping. Every
// RPC, `ping` included, runs on the main thread, so a stall also silences
// the remote, and this log line is the only place it is explained
// (docs/architecture.md#crashes-and-stalls).
//
// Pure C++ (atomics, one thread, the logger), so it lives in core: the
// windowed, offscreen and headless loops, the RPC pump and the file dialogs
// can all reach it. Getting the stuck thread's STACK needs a signal handler
// and <stacktrace>, which only the executable has; main() passes that in.
namespace watchdog {

// Once per main-loop iteration — iteration, not frame: a minimized window and
// the offscreen and headless loops finish no frames but still iterate. Nothing
// is monitored before the first beat, so start-up (a large project opened from
// the command line) is not reported as a stall.
void Beat();

// --stall-test: the 20th beat sleeps the main thread 4 s, once, inside a
// function named StallSelfTest, so the stall line, the stack and the resume
// line can be checked end to end (like --crash-test for the crash handler).
void RequestSelfTest();

// What the main thread is doing, while it does something that can take a
// while: "RPC render_still", "file dialog". Shown on the stall line. A label
// with `wantStack` false reports the stall without a stack — a file dialog's
// wait is the user, and the label already says everything. One level: a
// nested label replaces the outer one and clears on exit.
class Label {
public:
  explicit Label(std::string_view what, bool wantStack = true);
  ~Label();
  Label(const Label&) = delete;
  Label& operator=(const Label&) = delete;
};

// The stall decision, separated from the clock and the thread so it can be
// tested with made-up times (like app::FramePacer).
class StallDetector {
public:
  enum class Event { None, Stalled, Resumed };
  explicit StallDetector(double thresholdSeconds)
      : m_threshold(thresholdSeconds) {}
  // `beat` is the heartbeat count, `now` any monotonic time in seconds.
  Event Observe(uint64_t beat, double now);
  // Seconds without a beat, as of the last Stalled or Resumed event.
  double Duration() const { return m_duration; }

private:
  double m_threshold;
  bool m_armed = false;    // a beat has been seen
  uint64_t m_beat = 0;     // the last count seen
  double m_since = 0.0;    // when it last changed
  bool m_reported = false; // this stall was logged
  double m_duration = 0.0;
};

// DESIGNER_WATCHDOG_S, default 3 s; 0 turns the watchdog off (a debugger
// stopping the process is not a hang). A value that is not a number of
// seconds is REFUSED — false, with `error` saying what is accepted — rather
// than replaced by the default.
[[nodiscard]] bool ThresholdFromEnvironment(double* seconds,
                                            std::string* error);

// Starts the monitor thread. `stack` is called from the MONITOR thread when a
// stall is reported and returns the main thread's stack as text (empty when
// it cannot be had). A threshold <= 0 starts nothing.
void Start(double thresholdSeconds, std::function<std::string()> stack);
void Stop();

} // namespace watchdog
