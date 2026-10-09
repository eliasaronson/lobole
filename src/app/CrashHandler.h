// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>

// Fatal-signal backtrace (SIGSEGV/SIGABRT/SIGBUS/SIGFPE): prints a
// SYMBOLIZED std::stacktrace (C++23) to stderr so dev-watch logs show where
// a crash happened, not just "core dumped". The default handler is restored
// before symbolizing, so a failure inside the (not strictly signal-safe)
// stacktrace machinery still core-dumps normally.
void InstallCrashHandler();

// The --crash-test flag's payload: deliberately SIGSEGVs to exercise the
// backtrace path end-to-end in a real log.
[[noreturn]] void CrashHandlerSelfTest();

// Stall stacks for the frame watchdog (common/Watchdog.h). Call from the MAIN
// thread: it records which thread to ask, and installs a SIGUSR2 handler that
// only CAPTURES return addresses into preallocated memory — no allocation, no
// symbolizing, no locks — because the thread it interrupts may be slow rather
// than dead, and a handler that allocated while that thread held the heap lock
// would turn a slow frame into a permanent hang.
void InstallStallCapture();

// From the watchdog's thread: interrupt the main thread, wait up to
// `timeoutSeconds` for its capture, and symbolize it here, where allocating is
// safe. Empty when the capture did not arrive, or on Windows.
std::string CaptureMainThreadStack(double timeoutSeconds);
