// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/StopSignal.h"

#include "common/Watchdog.h"
#include "core/io/CrashReports.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

namespace stopsignal {

namespace {

// Written by the handler: lock-free atomics only.
std::atomic<int> g_signals{0}; // how many arrived
std::atomic<int> g_signal{0};  // the first one

std::mutex g_stepMutex;
std::function<void()> g_step;

void Handle(int sig) {
  int expected = 0;
  g_signal.compare_exchange_strong(expected, sig);
  g_signals.fetch_add(1);
}

[[noreturn]] void ForceExit(const char* why) {
  const int sig = g_signal.load();
  char label[160] = {};
  watchdog::CopyLabel(label, sizeof label);
  std::fprintf(
      stderr, "[stop] %s; exiting without waiting (the main thread is %s%s)\n",
      why, label[0] ? "in " : "between calls, or joining a job", label);
  std::fflush(stderr);
  {
    std::lock_guard<std::mutex> lock(g_stepMutex);
    if (g_step)
      g_step();
  }
  crash::EndSession(); // asked to stop: not a crash
  // No destructors, no atexit: the main thread may be inside anything.
  std::_Exit(128 + (sig != 0 ? sig : SIGTERM));
}

void Watch() {
  using clock = std::chrono::steady_clock;
  // Polled rather than woken: a handler may only touch atomics, and a 20 ms
  // sleep costs nothing a person or a test could see.
  while (g_signals.load() == 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto deadline =
      clock::now() + std::chrono::duration_cast<clock::duration>(
                         std::chrono::duration<double>(kStopGraceSeconds));
  while (clock::now() < deadline) {
    if (g_signals.load() > 1)
      ForceExit("stop asked twice");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ForceExit("still running after the stop grace");
}

} // namespace

void Install() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::signal(SIGINT, Handle);
    std::signal(SIGTERM, Handle);
    std::thread(Watch).detach(); // dies with the process, whichever way
  });
}

bool Requested() { return g_signals.load() > 0; }

void OnForcedExit(std::function<void()> step) {
  std::lock_guard<std::mutex> lock(g_stepMutex);
  g_step = std::move(step);
}

} // namespace stopsignal
