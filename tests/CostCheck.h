// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

// A cost check (docs/ui/testing.md#tests-that-time-things) holds a CPU time
// to a budget measured on the build that ships. Under ASan, UBSan or TSan
// only our own code is instrumented: it costs 2 to 170 times as much, and
// OpenCASCADE, Netgen and the driver, which are not, cost the same, so
// neither a budget nor a ratio against the kernel means anything there (a
// best fit read 97 CPU s against its 1.3, and ten times the budget did not
// hold it). A sanitized build runs the work, and every other check in the
// case, for what it is there to find; only the budget is not judged.
//
// CHECK_COST(expr) is CHECK(expr) in a normal build and says, in a
// sanitized one, that it was not made. A test case that reads CPU time
// states its budgets this way, or says with NOT_A_COST_CHECK("why") that
// what it reads is not one (a hang guard's deadline, a figure it prints):
// tests/test_conventions.cpp holds every case to one or the other.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LOBOLE_COST_CHECKED 0
#else
#define LOBOLE_COST_CHECKED 1
#endif

#if LOBOLE_COST_CHECKED
#define CHECK_COST(...) CHECK(__VA_ARGS__)
#else
#define CHECK_COST(...)                                                        \
  do {                                                                         \
    (void)(__VA_ARGS__);                                                       \
    std::printf("cost check not made under the sanitizers: %s\n",              \
                #__VA_ARGS__);                                                 \
  } while (0)
#endif

#define NOT_A_COST_CHECK(why) static_cast<void>(sizeof(why))
