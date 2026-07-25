#include "app/CrashHandler.h"

#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

// std::stacktrace (C++23) needs libstdc++'s experimental archive (-lstdc++exp),
// which some toolchains ship without (e.g. the Freedesktop Flatpak SDK's gcc).
// CMake probes for it and defines HAVE_STDCXX_STACKTRACE when it links; without
// it we fall back to glibc's backtrace(), which is always available.
#if defined(HAVE_STDCXX_STACKTRACE)
#include <stacktrace>
#else
#include <execinfo.h>
#endif

namespace {

// Async-signal-safe write of a buffer to stderr.
void WriteStderr(const char* s, size_t n) {
  const ssize_t r = write(STDERR_FILENO, s, n);
  (void)r;
}
void WriteStderr(const char* s) { WriteStderr(s, strlen(s)); }

void OnFatalSignal(int sig) {
  WriteStderr("\n=== fatal signal ");
  const char* name = sig == SIGSEGV   ? "SIGSEGV"
                     : sig == SIGABRT ? "SIGABRT"
                     : sig == SIGBUS  ? "SIGBUS"
                     : sig == SIGFPE  ? "SIGFPE"
                                      : "?";
  WriteStderr(name);
  WriteStderr(" — backtrace:\n");
  // Symbolizing allocates (not strictly async-signal-safe) — accepted: the
  // process is already dying, and a symbolized stack in the dev-watch log is
  // worth the small chance of a hang inside a corrupted allocator. The default
  // handler is restored FIRST, so even that failure mode still core-dumps.
  signal(sig, SIG_DFL);
#if defined(HAVE_STDCXX_STACKTRACE)
  const std::string trace = std::to_string(std::stacktrace::current(
      /*skip=*/1)); // skip this handler frame
  WriteStderr(trace.c_str(), trace.size());
#else
  // Fallback: glibc backtrace(). backtrace_symbols_fd writes straight to the fd
  // without heap allocation, and frame 0 (this handler) is skipped.
  void* frames[64];
  const int n = backtrace(frames, 64);
  if (n > 1)
    backtrace_symbols_fd(frames + 1, n - 1, STDERR_FILENO);
#endif
  WriteStderr("\n=== end backtrace\n");
  raise(sig); // default disposition: core dump / exit code as before
}

} // namespace

void InstallCrashHandler() {
  signal(SIGSEGV, OnFatalSignal);
  signal(SIGABRT, OnFatalSignal);
  signal(SIGBUS, OnFatalSignal);
  signal(SIGFPE, OnFatalSignal);
}

void CrashHandlerSelfTest() {
  // --crash-test: deliberately die so the backtrace path can be inspected in
  // a real log. Never reached in normal operation.
  volatile int* p = nullptr;
  *p = 42;  // NOLINT: intentional SIGSEGV
  abort();  // unreachable; satisfies [[noreturn]]
}
