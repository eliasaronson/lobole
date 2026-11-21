// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#ifdef __linux__
#include <sched.h>
#endif

// Holds the calling thread, and the threads it starts, on the core it is on
// for as long as this lives. For a test that compares two CPU times taken
// one after the other: on a hybrid CPU the same work costs 1.8 times the
// CPU seconds on an efficiency core and 5 to 6 times on a low-power one, so
// a move between the two measurements changes their ratio by that much
// while the code is unchanged (docs/ui/testing.md#tests-that-time-things).
// Does nothing where the affinity cannot be read or set.
class SameCore {
public:
  SameCore() {
#ifdef __linux__
    const int cpu = sched_getcpu();
    if (cpu >= 0 && sched_getaffinity(0, sizeof(was_), &was_) == 0) {
      cpu_set_t one;
      CPU_ZERO(&one);
      CPU_SET(cpu, &one);
      held_ = sched_setaffinity(0, sizeof(one), &one) == 0;
    }
#endif
  }
  ~SameCore() {
#ifdef __linux__
    if (held_)
      sched_setaffinity(0, sizeof(was_), &was_);
#endif
  }
  SameCore(const SameCore&) = delete;
  SameCore& operator=(const SameCore&) = delete;

private:
#ifdef __linux__
  cpu_set_t was_{};
#endif
  bool held_ = false;
};
