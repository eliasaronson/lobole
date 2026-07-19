#include "CrashHandler.h"

#include <csignal>
#include <cstring>
#include <execinfo.h>
#include <unistd.h>

namespace {

// Async-signal-safe write of a literal/buffer to stderr.
void WriteStderr(const char* s) {
  const ssize_t n = write(STDERR_FILENO, s, strlen(s));
  (void)n;
}

void OnFatalSignal(int sig) {
  // Everything here must be async-signal-safe: write() and
  // backtrace_symbols_fd() only — no malloc, no stdio, no Logger.
  WriteStderr("\n=== fatal signal ");
  const char* name = sig == SIGSEGV   ? "SIGSEGV"
                     : sig == SIGABRT ? "SIGABRT"
                     : sig == SIGBUS  ? "SIGBUS"
                     : sig == SIGFPE  ? "SIGFPE"
                                      : "?";
  WriteStderr(name);
  WriteStderr(" — backtrace (addr2line -e build/designer <addr> for lines):\n");
  void* frames[64];
  const int count = backtrace(frames, 64);
  backtrace_symbols_fd(frames, count, STDERR_FILENO);
  WriteStderr("=== end backtrace\n");
  // Restore the default handler and re-raise so the core dump / exit code
  // behave exactly as before this handler existed.
  signal(sig, SIG_DFL);
  raise(sig);
}

} // namespace

void InstallCrashHandler() {
  // sigaction with SA_RESETHAND would also work; plain signal() keeps it
  // minimal and portable across the glibc versions we build on.
  signal(SIGSEGV, OnFatalSignal);
  signal(SIGABRT, OnFatalSignal);
  signal(SIGBUS, OnFatalSignal);
  signal(SIGFPE, OnFatalSignal);
}
