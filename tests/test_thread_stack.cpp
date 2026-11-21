// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#if defined(__APPLE__)

#include <cstddef>
#include <future>
#include <pthread.h>
#include <thread>

namespace {

size_t StackOfThisThread() { return pthread_get_stacksize_np(pthread_self()); }

constexpr size_t kLinuxStackBytes = 8u * 1024u * 1024u;

} // namespace

TEST_CASE("threads get Linux's stack size on macOS", "[platform]") {
  size_t fromThread = 0;
  std::thread([&] { fromThread = StackOfThisThread(); }).join();
  CHECK(fromThread >= kLinuxStackBytes);

  size_t fromJthread = 0;
  std::jthread([&] { fromJthread = StackOfThisThread(); }).join();
  CHECK(fromJthread >= kLinuxStackBytes);

  const size_t fromAsync =
      std::async(std::launch::async, StackOfThisThread).get();
  CHECK(fromAsync >= kLinuxStackBytes);
}

#endif
