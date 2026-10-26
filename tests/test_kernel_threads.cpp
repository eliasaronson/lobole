// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// cad::BooleanWorkers: how many threads a loop of booleans runs on, half
// the machine where the kernel keeps one error-handler stack for every
// thread (cad/KernelThreads.h).

#include <catch2/catch_test_macros.hpp>

#include "cad/KernelThreads.h"
#include "cad/OccPatches.h"
#include "core/Parallel.h"

#include <Standard_Version.hxx>

#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("A loop of booleans runs on half the machine where the kernel "
          "shares its error-handler stack",
          "[parallel][occ]") {
  // The leg-pair study's machine: 22 hardware threads, 11 of them.
  CHECK(cad::BooleanWorkers(22, 22, false) == 11);
  CHECK(cad::BooleanWorkers(22, 22, true) == 22);
  // A worker's share below the cap is the share.
  CHECK(cad::BooleanWorkers(5, 22, false) == 5);
  // Odd machines round up, and one thread is one thread.
  CHECK(cad::BooleanWorkers(3, 3, false) == 2);
  CHECK(cad::BooleanWorkers(1, 1, false) == 1);
  CHECK(cad::BooleanWorkers(0, 8, false) == 1);
}

// The library this runs is asked, not the list the build was configured
// with; on the kernels we build the two agree, and on stock OCCT 7.x both
// say shared.
TEST_CASE("The running kernel says whether its handler stack is per thread",
          "[parallel][occ]") {
#if OCC_VERSION_HEX >= 0x080000
  CHECK(cad::PerThreadErrorHandlers());
#else
  CHECK(cad::PerThreadErrorHandlers() ==
        occpatch::Carries("occt-error-handler-thread-local"));
#endif
  // Asking twice asks once, and changes nothing about this thread.
  CHECK(cad::PerThreadErrorHandlers() == cad::PerThreadErrorHandlers());
}

TEST_CASE("A capped loop does every item once", "[parallel]") {
  constexpr size_t kItems = 1000;
  std::vector<std::atomic<int>> seen(kItems);
  std::atomic<size_t> most{0}, running{0};
  parallel::For(
      kItems,
      [&](size_t k) {
        const size_t now = running.fetch_add(1) + 1;
        size_t was = most.load();
        while (now > was && !most.compare_exchange_weak(was, now))
          ;
        seen[k].fetch_add(1);
        std::this_thread::yield();
        running.fetch_sub(1);
      },
      2);
  for (size_t k = 0; k < kItems; ++k) {
    INFO("item " << k);
    REQUIRE(seen[k].load() == 1);
  }
  CHECK(most.load() <= 2);
}
