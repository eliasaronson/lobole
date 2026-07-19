#pragma once

// Fatal-signal backtrace (SIGSEGV/SIGABRT/SIGBUS/SIGFPE): prints the stack to
// stderr so dev-watch logs show WHERE a crash happened, not just "core
// dumped". Uses glibc backtrace_symbols_fd (async-signal-safe, C++20-clean);
// when the project moves to C++23, std::stacktrace can replace it for better
// symbolization. Frames resolve to offsets — feed them to addr2line/eu-addr2line
// against build/designer for file:line.
void InstallCrashHandler();
