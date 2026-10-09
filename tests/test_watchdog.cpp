// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/Watchdog.h"

#include <cstdlib>
#include <string>

// The frame watchdog's stall decision (common/Watchdog.h), with made-up
// times. The thread and the stack capture around it are exercised end to end
// by tests/watchdog_smoke.py.

using watchdog::StallDetector;
using Event = StallDetector::Event;

TEST_CASE("Watchdog: nothing is watched before the first beat", "[watchdog]") {
  // Start-up (a project opened from the command line) is not a stall.
  StallDetector d(3.0);
  CHECK(d.Observe(0, 0.0) == Event::None);
  CHECK(d.Observe(0, 100.0) == Event::None);
}

TEST_CASE("Watchdog: a stall past the threshold is reported once, and its end",
          "[watchdog]") {
  StallDetector d(3.0);
  CHECK(d.Observe(1, 10.0) == Event::None); // armed at t=10
  CHECK(d.Observe(1, 12.9) == Event::None);
  CHECK(d.Observe(1, 13.0) == Event::Stalled);
  CHECK(d.Duration() == 3.0);
  CHECK(d.Observe(1, 20.0) == Event::None); // once per stall
  CHECK(d.Observe(2, 21.5) == Event::Resumed);
  CHECK(d.Duration() == 11.5);
  CHECK(d.Observe(3, 21.75) == Event::None);
}

TEST_CASE("Watchdog: a loop that keeps beating is never reported",
          "[watchdog]") {
  StallDetector d(3.0);
  uint64_t beat = 1;
  for (double t = 0.0; t < 60.0; t += 0.25)
    CHECK(d.Observe(beat++, t) == Event::None);
  // A gap just under the threshold is not a stall either.
  CHECK(d.Observe(beat, 62.0) == Event::None);
  CHECK(d.Observe(beat + 1, 62.5) == Event::None);
}

TEST_CASE("Watchdog: a zero threshold reports nothing", "[watchdog]") {
  StallDetector d(0.0);
  CHECK(d.Observe(1, 0.0) == Event::None);
  CHECK(d.Observe(1, 1e6) == Event::None);
}

#if !defined(_WIN32) // setenv/unsetenv are POSIX
TEST_CASE("Watchdog: the threshold is read from the environment or refused",
          "[watchdog]") {
  double s = -1.0;
  std::string error;
  unsetenv("DESIGNER_WATCHDOG_S");
  CHECK(watchdog::ThresholdFromEnvironment(&s, &error));
  CHECK(s == 3.0);
  setenv("DESIGNER_WATCHDOG_S", "0", 1);
  CHECK(watchdog::ThresholdFromEnvironment(&s, &error));
  CHECK(s == 0.0);
  setenv("DESIGNER_WATCHDOG_S", "1.5", 1);
  CHECK(watchdog::ThresholdFromEnvironment(&s, &error));
  CHECK(s == 1.5);
  // Not a number of seconds: refused, naming what is accepted — never
  // quietly replaced by the default.
  for (const char* bad : {"3s", "fast", "-1"}) {
    setenv("DESIGNER_WATCHDOG_S", bad, 1);
    error.clear();
    CHECK_FALSE(watchdog::ThresholdFromEnvironment(&s, &error));
    CHECK(error.find("seconds") != std::string::npos);
  }
  unsetenv("DESIGNER_WATCHDOG_S");
}
#endif
