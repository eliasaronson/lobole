// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/CrashHandler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <csignal>
#include <cxxabi.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>
// std::stacktrace (C++23) needs libstdc++'s experimental archive (-lstdc++exp),
// which some toolchains ship without (e.g. the Freedesktop Flatpak SDK's gcc).
// CMake probes for it and defines HAVE_STDCXX_STACKTRACE when it links; without
// it we fall back to glibc's backtrace(), which is always available.
#if defined(HAVE_STDCXX_STACKTRACE)
#include <stacktrace>
#endif
#include <execinfo.h>
#endif

namespace {

// Raw write to stderr (fd 2) — no stdio buffering, safe from a dying process.
void WriteStderr(const char* s, size_t n) {
#if defined(_WIN32)
  _write(2, s, static_cast<unsigned>(n));
#else
  const ssize_t r = write(STDERR_FILENO, s, n);
  (void)r;
#endif
}
void WriteStderr(const char* s) { WriteStderr(s, strlen(s)); }

#if !defined(_WIN32)
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
#else // _WIN32
// Windows has no POSIX signals/execinfo. Catch unhandled SEH exceptions, print
// the code, and continue the search so the process still crashes/dumps as
// before. (A symbolized DbgHelp stack walk can be added later.)
LONG WINAPI OnWindowsException(EXCEPTION_POINTERS* info) {
  WriteStderr("\n=== fatal Windows exception (code ");
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08lx",
                info->ExceptionRecord->ExceptionCode);
  WriteStderr(buf);
  WriteStderr(") ===\n");
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

} // namespace

void InstallCrashHandler() {
#if defined(_WIN32)
  SetUnhandledExceptionFilter(OnWindowsException);
#else
  signal(SIGSEGV, OnFatalSignal);
  signal(SIGABRT, OnFatalSignal);
  signal(SIGBUS, OnFatalSignal);
  signal(SIGFPE, OnFatalSignal);
#endif
}

void CrashHandlerSelfTest() {
  // --crash-test: deliberately die so the backtrace path can be inspected in
  // a real log. Never reached in normal operation.
  volatile int* p = nullptr;
  *p = 42; // NOLINT: intentional SIGSEGV
  abort(); // unreachable; satisfies [[noreturn]]
}

// --- Stall stacks (the frame watchdog) ---------------------------------------

#if !defined(_WIN32)
namespace {

pthread_t g_mainThread{};
bool g_stallCaptureInstalled = false;
std::atomic<bool> g_stackReady{false};
constexpr size_t kMaxFrames = 64;

#if defined(HAVE_STDCXX_STACKTRACE)
// The capture's only allocation comes from here: a bump arena reset at each
// capture, so the handler never reaches the heap. Large enough for the frame
// vector at kMaxFrames several times over (it may grow as it fills).
alignas(std::max_align_t) unsigned char g_arena[32 * 1024];
size_t g_arenaUsed = 0;

template <class T> struct ArenaAllocator {
  using value_type = T;
  ArenaAllocator() noexcept = default;
  template <class U> ArenaAllocator(const ArenaAllocator<U>&) noexcept {}
  T* allocate(size_t n) {
    const size_t align = alignof(std::max_align_t);
    const size_t start = (g_arenaUsed + align - 1) / align * align;
    const size_t bytes = n * sizeof(T);
    if (start + bytes > sizeof(g_arena))
      throw std::bad_alloc(); // current() is noexcept: yields an empty trace
    g_arenaUsed = start + bytes;
    return reinterpret_cast<T*>(g_arena + start);
  }
  void deallocate(T*, size_t) noexcept {}
  template <class U> bool operator==(const ArenaAllocator<U>&) const noexcept {
    return true;
  }
};

std::array<std::stacktrace_entry, kMaxFrames> g_frames;
size_t g_frameCount = 0;

void OnStackRequest(int) {
  const int savedErrno = errno;
  g_arenaUsed = 0;
  const auto trace =
      std::basic_stacktrace<ArenaAllocator<std::stacktrace_entry>>::current(
          /*skip=*/1, kMaxFrames);
  g_frameCount = std::min<size_t>(trace.size(), kMaxFrames);
  for (size_t i = 0; i < g_frameCount; ++i)
    g_frames[i] = trace[i];
  g_stackReady.store(true, std::memory_order_release);
  errno = savedErrno;
}
#else
void* g_raw[kMaxFrames];
int g_rawCount = 0;

void OnStackRequest(int) {
  const int savedErrno = errno;
  g_rawCount = backtrace(g_raw, static_cast<int>(kMaxFrames));
  g_stackReady.store(true, std::memory_order_release);
  errno = savedErrno;
}
#endif

#if defined(HAVE_STDCXX_STACKTRACE)
// The function a return address lies in, by the executable's own symbol
// table. std::stacktrace names a frame from the debug info, and there a frame
// inside an inlined call is named by the INNERMOST function: an -O2 -g build
// stalled in StallSelfTest's inlined sleep_for reports `sleep_for at
// this_thread_sleep.h:82`, and the function that stalled appears nowhere. The
// symbol table has one entry per real function, so it names the frame's owner.
// Main executable only (the libraries are not built with our debug info);
// read once, on the watchdog's thread, never in the signal handler.
class ExeSymbols {
public:
  // Empty when the address is not in the executable or not in a function.
  std::string Owner(uintptr_t address) {
    std::call_once(m_once, [this] { Load(); });
    if (address < m_bias)
      return {};
    const uintptr_t at = address - m_bias;
    auto it = std::upper_bound(
        m_functions.begin(), m_functions.end(), at,
        [](uintptr_t a, const Function& f) { return a < f.start; });
    if (it == m_functions.begin())
      return {};
    --it;
    if (at >= it->start + it->size)
      return {};
    const char* mangled = m_names.c_str() + it->name;
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    std::string out = status == 0 && demangled ? demangled : mangled;
    std::free(demangled);
    return out;
  }

private:
  struct Function {
    uintptr_t start, size;
    size_t name; // offset into m_names
  };

  void Load() {
    // The executable is the first object dl_iterate_phdr reports; its
    // dlpi_addr is the load bias a PIE's symbol values are relative to.
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* self) {
          static_cast<ExeSymbols*>(self)->m_bias = info->dlpi_addr;
          return 1;
        },
        this);
    const int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      return;
    auto readAt = [fd](void* out, size_t n, off_t at) {
      return pread(fd, out, n, at) == static_cast<ssize_t>(n);
    };
    Elf64_Ehdr eh{};
    std::vector<Elf64_Shdr> sections;
    if (readAt(&eh, sizeof eh, 0) &&
        std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 &&
        eh.e_ident[EI_CLASS] == ELFCLASS64 &&
        eh.e_shentsize == sizeof(Elf64_Shdr)) {
      sections.resize(eh.e_shnum);
      if (!readAt(sections.data(), sections.size() * sizeof(Elf64_Shdr),
                  static_cast<off_t>(eh.e_shoff)))
        sections.clear();
    }
    for (const Elf64_Shdr& s : sections) {
      if (s.sh_type != SHT_SYMTAB || s.sh_link >= sections.size())
        continue;
      const Elf64_Shdr& str = sections[s.sh_link];
      std::vector<Elf64_Sym> symbols(s.sh_size / sizeof(Elf64_Sym));
      m_names.resize(str.sh_size);
      if (!readAt(symbols.data(), symbols.size() * sizeof(Elf64_Sym),
                  static_cast<off_t>(s.sh_offset)) ||
          !readAt(m_names.data(), m_names.size(),
                  static_cast<off_t>(str.sh_offset))) {
        m_names.clear();
        break;
      }
      for (const Elf64_Sym& sym : symbols)
        if (ELF64_ST_TYPE(sym.st_info) == STT_FUNC && sym.st_size > 0 &&
            sym.st_name < m_names.size())
          m_functions.push_back({sym.st_value, sym.st_size, sym.st_name});
      break;
    }
    close(fd);
    std::sort(
        m_functions.begin(), m_functions.end(),
        [](const Function& a, const Function& b) { return a.start < b.start; });
  }

  std::once_flag m_once;
  uintptr_t m_bias = 0;
  std::vector<Function> m_functions;
  std::string m_names;
};

// "f() [clone .cold]" is still f.
std::string WithoutClone(std::string name) {
  const size_t clone = name.find(" [clone ");
  if (clone != std::string::npos)
    name.resize(clone);
  return name;
}

// The bare name of a function, for telling whether two spellings are the same
// one: debug info may give `StallSelfTest` where the symbol table demangles to
// `watchdog::(anonymous namespace)::StallSelfTest()`, and a template carries
// its return type and arguments.
std::string BareName(const std::string& full) {
  std::string name = WithoutClone(full);
  const std::string anon = "(anonymous namespace)::";
  for (size_t at; (at = name.find(anon)) != std::string::npos;)
    name.erase(at, anon.size());
  name = name.substr(0, name.find('('));
  std::string flat; // template arguments dropped
  int depth = 0;
  for (const char c : name) {
    if (c == '<')
      ++depth;
    else if (c == '>')
      depth = std::max(0, depth - 1);
    else if (depth == 0)
      flat += c;
  }
  const size_t scope = flat.rfind("::");
  if (scope != std::string::npos)
    flat.erase(0, scope + 2);
  const size_t space = flat.rfind(' ');
  if (space != std::string::npos)
    flat.erase(0, space + 1);
  return flat;
}
#endif

} // namespace

void InstallStallCapture() {
  g_mainThread = pthread_self();
  // Warm the unwinder here, on a normal call: its first use can load code
  // and allocate, which must not happen inside the handler.
#if defined(HAVE_STDCXX_STACKTRACE)
  (void)std::stacktrace::current().size();
#endif
  void* warm[4];
  (void)backtrace(warm, 4);

  struct sigaction sa{};
  sa.sa_handler = OnStackRequest;
  sigemptyset(&sa.sa_mask);
  // SA_RESTART: the interrupted call (a poll, a read) carries on as if the
  // capture had not happened.
  sa.sa_flags = SA_RESTART;
  g_stallCaptureInstalled = sigaction(SIGUSR2, &sa, nullptr) == 0;
}

std::string CaptureMainThreadStack(double timeoutSeconds) {
  if (!g_stallCaptureInstalled)
    return {};
  g_stackReady.store(false, std::memory_order_release);
  if (pthread_kill(g_mainThread, SIGUSR2) != 0)
    return {};
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration<double>(timeoutSeconds);
  while (!g_stackReady.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() > deadline)
      return {};
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::string out;
#if defined(HAVE_STDCXX_STACKTRACE)
  // Name and, where debug info has it, file:line — a release build has
  // names only, so "at :0" is left off rather than printed on every frame.
  // A frame whose name is an inlined callee also names the function it was
  // inlined into (ExeSymbols says why).
  static ExeSymbols symbols;
  size_t shown = 0;
  for (size_t i = 0; i < g_frameCount; ++i) {
    const std::stacktrace_entry& e = g_frames[i];
    std::string name = e.description();
    if (name.empty())
      continue; // the signal trampoline, the end of the stack
    if (!e.source_file().empty())
      name += " at " + e.source_file() + ":" + std::to_string(e.source_line());
    // A return address, so one back to stay inside the call instruction's
    // function when the call was its last.
    const std::string owner = WithoutClone(
        symbols.Owner(static_cast<uintptr_t>(e.native_handle()) - 1));
    if (!owner.empty() && BareName(e.description()) != BareName(owner))
      name += ", inlined in " + owner;
    out += "  #" + std::to_string(shown++) + " " + name + "\n";
  }
#else
  char** names = backtrace_symbols(g_raw, g_rawCount);
  for (int i = 0; i < g_rawCount; ++i)
    out += "  #" + std::to_string(i) + " " +
           (names ? names[i] : std::string("?")) + "\n";
  std::free(names);
#endif
  return out;
}
#else // _WIN32
void InstallStallCapture() {}
std::string CaptureMainThreadStack(double) { return {}; }
#endif
