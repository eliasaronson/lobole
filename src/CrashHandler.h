#pragma once

// Fatal-signal backtrace (SIGSEGV/SIGABRT/SIGBUS/SIGFPE): prints a
// SYMBOLIZED std::stacktrace (C++23) to stderr so dev-watch logs show where
// a crash happened, not just "core dumped". The default handler is restored
// before symbolizing, so a failure inside the (not strictly signal-safe)
// stacktrace machinery still core-dumps normally.
void InstallCrashHandler();

// The --crash-test flag's payload: deliberately SIGSEGVs to exercise the
// backtrace path end-to-end in a real log.
[[noreturn]] void CrashHandlerSelfTest();
