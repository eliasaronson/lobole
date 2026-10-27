// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>

// Fatal-signal handling (SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL; unhandled SEH
// exceptions and abort() on Windows). In order:
//
//   1. this session's crash file (SetCrashReportFiles) is opened with
//      open()/CreateFileW, and the signal or exception code is written;
//   2. then the RAW frames, each as module + offset, from a module table
//      recorded at start-up — nothing up to here allocates, so a corrupt heap
//      cannot stop it;
//   3. then, best effort, the SYMBOLIZED std::stacktrace (C++23), to the file
//      and to stderr as it always went, so dev-watch logs show where a crash
//      happened. On Windows, a minidump beside the crash file instead.
//
// The default handler is restored first, and the signal re-raised at the
// end, so the process still core-dumps as before. The next start turns the
// files into a report (core/io/CrashReports.h,
// docs/architecture.md#crashes-and-stalls).
//
// SIGTERM, SIGINT and SIGHUP delete the session marker and die as before: a
// process asked to stop has not crashed. (Headless and offscreen replace the
// first two with their own clean shutdown, bounded: app/StopSignal.h.) On
// Windows the console's events (Ctrl+C, Ctrl+Break, close, logoff, shutdown) do
// the same, through SetConsoleCtrlHandler; a mode with a SIGINT handler is
// asked to stop.
void InstallCrashHandler();

// Where this session's crash file and Windows minidump go, and the marker the
// termination signals delete. Copied into fixed buffers here, while
// allocating is safe; also records the loaded modules.
void SetCrashReportFiles(const std::filesystem::path& crashFile,
                         const std::filesystem::path& dumpFile,
                         const std::filesystem::path& markerFile);

// Records the loaded modules again — after start-up has loaded the libraries
// it loads late (the Vulkan driver), so their frames get a module too.
void RefreshCrashModules();

// The --crash-test flag's payload: deliberately SIGSEGVs to exercise the
// backtrace path end-to-end in a real log.
[[noreturn]] void CrashHandlerSelfTest();

// Stall stacks for the frame watchdog (common/Watchdog.h). Call from the MAIN
// thread: it records which thread to ask, and installs a SIGUSR2 handler that
// only CAPTURES return addresses into preallocated memory — no allocation, no
// symbolizing, no locks — because the thread it interrupts may be slow rather
// than dead, and a handler that allocated while that thread held the heap lock
// would turn a slow frame into a permanent hang. On Windows it keeps a handle
// to the thread instead, which the capture suspends.
void InstallStallCapture();

// From the watchdog's thread: interrupt the main thread, wait up to
// `timeoutSeconds` for its capture, and symbolize it here, where allocating is
// safe. Empty when the capture did not arrive. On Windows: the suspended
// thread's stack walked from its registers, as module + RVA frames
// (tools/symbolize-crash.py names them).
std::string CaptureMainThreadStack(double timeoutSeconds);
