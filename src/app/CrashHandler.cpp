// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/CrashHandler.h"

#include "common/Watchdog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <thread>

#if defined(_WIN32)
#include "cad/OccSignals.h"

#include <io.h>
#include <vector>
#include <windows.h>
// After windows.h, which they need.
#include <dbghelp.h>
// The K32 functions, which are kernel32's: no psapi.dll to link.
#define PSAPI_VERSION 2
#include <psapi.h>
#else
#include <cxxabi.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <elf.h>
#include <link.h>
#endif
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

// --- what the handler may touch ----------------------------------------------
//
// Everything below is filled at start-up, while allocating is safe, into
// fixed buffers: the handler only reads them. Writing the crash file is
// open/write/close (CreateFileW/WriteFile on Windows), which are safe in a
// signal handler; the raw frames go out before anything that allocates.

constexpr size_t kPathMax = 4096;
char g_crashPath[kPathMax] = {};
char g_markerPath[kPathMax] = {};
// What std::terminate was handed, for the SIGABRT that follows it.
char g_exceptionWhat[512] = {};
constexpr int kMaxCrashFrames = 64;

// A crash file being written: the bytes go to the file only, so stderr keeps
// what it always printed. No allocation, no stdio.
struct Out {
#if defined(_WIN32)
  HANDLE file = INVALID_HANDLE_VALUE;
  void Put(const char* s, size_t n) const {
    if (file == INVALID_HANDLE_VALUE)
      return;
    DWORD written = 0;
    WriteFile(file, s, static_cast<DWORD>(n), &written, nullptr);
  }
#else
  int fd = -1;
  void Put(const char* s, size_t n) const {
    while (fd >= 0 && n > 0) {
      const ssize_t w = write(fd, s, n);
      if (w < 0 && errno == EINTR)
        continue;
      if (w <= 0)
        return;
      s += w;
      n -= static_cast<size_t>(w);
    }
  }
#endif
  void Put(const char* s) const { Put(s, strlen(s)); }
  void Hex(uintptr_t v) const {
    char buf[2 + 2 * sizeof(uintptr_t)];
    size_t n = sizeof buf;
    do {
      buf[--n] = "0123456789abcdef"[v & 0xf];
      v >>= 4;
    } while (v != 0);
    buf[--n] = 'x';
    buf[--n] = '0';
    Put(buf + n, sizeof buf - n);
  }
  void Dec(uint64_t v) const {
    char buf[24];
    size_t n = sizeof buf;
    do {
      buf[--n] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v != 0);
    Put(buf + n, sizeof buf - n);
  }
};

// The header every crash file starts with; the collector reads the first
// line (core/io/CrashReports.cpp).
void PutHeader(const Out& out, const char* reason, const char* detail) {
  out.Put("reason: ");
  out.Put(reason);
  if (detail && *detail) {
    out.Put(" (");
    out.Put(detail);
    out.Put(")");
  }
  out.Put("\n");
  char label[96];
  if (watchdog::CopyLabel(label, sizeof label) > 0) {
    out.Put("in: ");
    out.Put(label);
    out.Put("\n");
  }
  if (g_exceptionWhat[0]) {
    out.Put("uncaught exception: ");
    out.Put(g_exceptionWhat);
    out.Put("\n");
  }
}

void CopyInto(char* dst, size_t size, const std::string& src) {
  const size_t n = std::min(src.size(), size - 1);
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

// std::terminate's handler: note what was thrown, then the usual one (which
// prints it and aborts — into the SIGABRT handler below).
std::terminate_handler g_previousTerminate = nullptr;
void OnTerminate() {
  if (std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& ex) {
      CopyInto(g_exceptionWhat, sizeof g_exceptionWhat, ex.what());
    } catch (...) {
      CopyInto(g_exceptionWhat, sizeof g_exceptionWhat,
               "(not a std::exception)");
    }
  }
  if (g_previousTerminate)
    g_previousTerminate();
  std::abort();
}

#if !defined(_WIN32)

// --- the loaded modules, for module+offset -------------------------------
//
// A raw address is useless in a report: every library loads somewhere
// different each run. Module + offset is what addr2line takes. The handler
// cannot ask the loader (dladdr takes its lock), so the table is recorded
// ahead of time, twice-buffered so a refresh never tears what a crash reads.

struct Module {
  uintptr_t lo = 0, hi = 0; // where its segments are mapped
  uintptr_t bias = 0;       // what to subtract for a file-relative address
  char name[256] = {};
};
constexpr size_t kMaxModules = 512;
struct ModuleTable {
  size_t count = 0;
  Module modules[kMaxModules];
};
ModuleTable g_tables[2];
std::atomic<int> g_activeTable{-1};

void RecordModules() {
  const int next = g_activeTable.load(std::memory_order_acquire) == 0 ? 1 : 0;
  ModuleTable& t = g_tables[next];
  t.count = 0;
#if defined(__APPLE__)
  const uint32_t images = _dyld_image_count();
  for (uint32_t i = 0; i < images && t.count < kMaxModules; ++i) {
    Module& m = t.modules[t.count++];
    const auto* header =
        reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
    m.bias = static_cast<uintptr_t>(_dyld_get_image_vmaddr_slide(i));
    m.lo = reinterpret_cast<uintptr_t>(header);
    // The image's extent from its load commands' segments.
    m.hi = m.lo;
    const auto* cmd = reinterpret_cast<const load_command*>(header + 1);
    for (uint32_t c = 0; c < header->ncmds; ++c) {
      if (cmd->cmd == LC_SEGMENT_64) {
        const auto* seg = reinterpret_cast<const segment_command_64*>(cmd);
        m.hi = std::max<uintptr_t>(m.hi, m.bias + seg->vmaddr + seg->vmsize);
      }
      cmd = reinterpret_cast<const load_command*>(
          reinterpret_cast<const char*>(cmd) + cmd->cmdsize);
    }
    CopyInto(m.name, sizeof m.name, _dyld_get_image_name(i));
  }
#else
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        auto& table = *static_cast<ModuleTable*>(data);
        if (table.count >= kMaxModules)
          return 1;
        Module m;
        m.bias = info->dlpi_addr;
        bool any = false;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          const auto& ph = info->dlpi_phdr[i];
          if (ph.p_type != PT_LOAD)
            continue;
          const uintptr_t lo = info->dlpi_addr + ph.p_vaddr;
          const uintptr_t hi = lo + ph.p_memsz;
          m.lo = any ? std::min(m.lo, lo) : lo;
          m.hi = any ? std::max(m.hi, hi) : hi;
          any = true;
        }
        if (!any)
          return 0;
        if (info->dlpi_name && info->dlpi_name[0]) {
          CopyInto(m.name, sizeof m.name, info->dlpi_name);
        } else if (table.count == 0) {
          // The executable reports no name.
          const ssize_t n =
              readlink("/proc/self/exe", m.name, sizeof m.name - 1);
          m.name[n > 0 ? n : 0] = '\0';
        }
        if (!m.name[0])
          return 0; // the vDSO
        table.modules[table.count++] = m;
        return 0;
      },
      &t);
#endif
  g_activeTable.store(next, std::memory_order_release);
}

void PutFrame(const Out& out, int index, uintptr_t address) {
  out.Put("  #");
  out.Dec(static_cast<uint64_t>(index));
  out.Put(" ");
  const int active = g_activeTable.load(std::memory_order_acquire);
  if (active >= 0) {
    const ModuleTable& t = g_tables[active];
    for (size_t i = 0; i < t.count; ++i) {
      const Module& m = t.modules[i];
      if (address >= m.lo && address < m.hi) {
        out.Put(m.name);
        out.Put("+");
        out.Hex(address - m.bias);
        out.Put("\n");
        return;
      }
    }
  }
  out.Hex(address); // a module loaded after the last refresh
  out.Put("\n");
}

const char* SignalName(int sig) {
  return sig == SIGSEGV   ? "SIGSEGV"
         : sig == SIGABRT ? "SIGABRT"
         : sig == SIGBUS  ? "SIGBUS"
         : sig == SIGFPE  ? "SIGFPE"
         : sig == SIGILL  ? "SIGILL"
                          : "?";
}

// An alternate stack, so a stack overflow — the one SIGSEGV with no stack
// left to run a handler on — is reported too. The main thread's only, as
// sigaltstack is per thread; static, so it costs nothing until it is used.
alignas(16) unsigned char g_altStack[1 << 20];

void OnFatalSignal(int sig, siginfo_t* info, void*) {
  // Default disposition first, so a second fault anywhere below dies the
  // way it always did, core dump included.
  signal(sig, SIG_DFL);
  const char* name = SignalName(sig);

  // 1-2: the crash file and its raw frames — nothing here allocates.
  Out file;
  if (g_crashPath[0])
    file.fd = open(g_crashPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  PutHeader(file, name, nullptr);
  if (info && (sig == SIGSEGV || sig == SIGBUS)) {
    file.Put("address: ");
    file.Hex(reinterpret_cast<uintptr_t>(info->si_addr));
    file.Put("\n");
  }
  void* frames[kMaxCrashFrames];
  const int n = backtrace(frames, kMaxCrashFrames);
  file.Put("raw frames (module+offset):\n");
  for (int i = 1; i < n; ++i) // frame 0 is this handler
    PutFrame(file, i - 1, reinterpret_cast<uintptr_t>(frames[i]));

  // 3: best effort from here — symbolizing allocates (not strictly
  // async-signal-safe), and in a corrupt heap it may hang; what is needed is
  // already in the file. The same stack goes to stderr, as it always has.
  WriteStderr("\n=== fatal signal ");
  WriteStderr(name);
  WriteStderr(" — backtrace:\n");
  file.Put("symbolized:\n");
#if defined(HAVE_STDCXX_STACKTRACE)
  const std::string trace = std::to_string(std::stacktrace::current(
      /*skip=*/1)); // skip this handler frame
  WriteStderr(trace.c_str(), trace.size());
  file.Put(trace.c_str(), trace.size());
  file.Put("\n");
#else
  // Fallback: glibc backtrace(). backtrace_symbols_fd writes straight to the fd
  // without heap allocation, and frame 0 (this handler) is skipped.
  if (n > 1) {
    backtrace_symbols_fd(frames + 1, n - 1, STDERR_FILENO);
    if (file.fd >= 0)
      backtrace_symbols_fd(frames + 1, n - 1, file.fd);
  }
#endif
  WriteStderr("\n=== end backtrace\n");
  if (file.fd >= 0) {
    close(file.fd);
    WriteStderr("=== crash report: ");
    WriteStderr(g_crashPath);
    WriteStderr("\n");
  }
  raise(sig); // default disposition: core dump / exit code as before
}

// SIGTERM, SIGINT, SIGHUP where nothing else handles them (the windowed
// app): somebody asked the process to stop, which is not a crash. Delete the
// session marker so the next start does not report one, then die exactly as
// before. The headless and offscreen loops install their own handlers for
// SIGINT/SIGTERM and exit cleanly, which ends the session the ordinary way.
void OnTerminationSignal(int sig) {
  if (g_markerPath[0])
    unlink(g_markerPath);
  signal(sig, SIG_DFL);
  raise(sig);
}

#else // _WIN32

wchar_t g_crashPathW[kPathMax] = {};
wchar_t g_dumpPathW[kPathMax] = {};
wchar_t g_markerPathW[kPathMax] = {};

void PutFrame(const Out& out, const char* tag, uintptr_t address) {
  out.Put(tag);
  out.Put(" ");
  HMODULE module = nullptr;
  wchar_t wide[MAX_PATH];
  char name[MAX_PATH * 3];
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module) &&
      module) {
    const DWORD len = GetModuleFileNameW(module, wide, MAX_PATH);
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len), name,
                            sizeof name - 1, nullptr, nullptr);
    name[bytes > 0 ? bytes : 0] = '\0';
    out.Put(name[0] ? name : "?");
    out.Put("+");
    // The module handle IS its load address, so this is the RVA.
    out.Hex(address - reinterpret_cast<uintptr_t>(module));
  } else {
    out.Hex(address);
  }
  out.Put("\n");
}

void PutIndexedFrame(const Out& out, int index, uintptr_t address) {
  char tag[16] = "  #";
  size_t n = 3;
  char digits[8];
  size_t d = 0;
  do {
    digits[d++] = static_cast<char>('0' + index % 10);
    index /= 10;
  } while (index > 0 && d < sizeof digits);
  while (d > 0)
    tag[n++] = digits[--d];
  tag[n] = '\0';
  PutFrame(out, tag, address);
}

const char* ExceptionName(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
    return "access violation";
  case EXCEPTION_STACK_OVERFLOW:
    return "stack overflow";
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    return "illegal instruction";
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    return "integer divide by zero";
  case EXCEPTION_IN_PAGE_ERROR:
    return "in-page error";
  case 0xE06D7363:
    return "C++ exception";
  default:
    return "";
  }
}

HANDLE OpenForWrite(const wchar_t* path) {
  if (!path[0])
    return INVALID_HANDLE_VALUE;
  return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void WriteMinidump(EXCEPTION_POINTERS* info) {
  HANDLE dump = OpenForWrite(g_dumpPathW);
  if (dump == INVALID_HANDLE_VALUE)
    return;
  MINIDUMP_EXCEPTION_INFORMATION mei{};
  mei.ThreadId = GetCurrentThreadId();
  mei.ExceptionPointers = info;
  mei.ClientPointers = FALSE;
  // MiniDumpNormal: threads, registers and stacks, a few hundred KB — no
  // heap, so it carries no more of the user's design than the stacks do.
  MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                    MiniDumpNormal, info ? &mei : nullptr, nullptr, nullptr);
  CloseHandle(dump);
}

// --- the modules we ship -----------------------------------------------------
//
// Everything in the executable's directory: lobole.exe and the DLLs built
// with it (OpenCASCADE, libstdc++, mcfgthread, ...). Recorded at start-up and
// again once the libraries loaded late are in, into a table the handler only
// reads, twice-buffered like the POSIX module table.

struct CodeRange {
  uintptr_t lo = 0, hi = 0;
};
constexpr size_t kMaxShipped = 512;
struct ShippedTable {
  size_t count = 0;
  CodeRange ranges[kMaxShipped];
};
ShippedTable g_shipped[2];
std::atomic<int> g_activeShipped{-1};

void RecordShippedModules() {
  wchar_t exe[MAX_PATH];
  const DWORD exeLen = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  size_t dirLen = exeLen;
  while (dirLen > 0 && exe[dirLen - 1] != L'\\' && exe[dirLen - 1] != L'/')
    --dirLen;
  if (dirLen == 0)
    return;
  std::vector<HMODULE> modules(1024);
  DWORD needed = 0;
  if (!K32EnumProcessModules(
          GetCurrentProcess(), modules.data(),
          static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed))
    return;
  modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));
  const int next = g_activeShipped.load(std::memory_order_acquire) == 0 ? 1 : 0;
  ShippedTable& t = g_shipped[next];
  t.count = 0;
  for (HMODULE m : modules) {
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(m, path, MAX_PATH);
    MODULEINFO info{};
    if (len <= dirLen || _wcsnicmp(path, exe, dirLen) != 0 ||
        std::wcschr(path + dirLen, L'\\') != nullptr ||
        !K32GetModuleInformation(GetCurrentProcess(), m, &info, sizeof info))
      continue;
    if (t.count == kMaxShipped)
      break;
    const auto lo = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    t.ranges[t.count++] = {lo, lo + info.SizeOfImage};
  }
  g_activeShipped.store(next, std::memory_order_release);
}

bool InShippedCode(uintptr_t address) {
  const int active = g_activeShipped.load(std::memory_order_acquire);
  if (active < 0)
    return false;
  const ShippedTable& t = g_shipped[active];
  for (size_t i = 0; i < t.count; ++i)
    if (address >= t.ranges[i].lo && address < t.ranges[i].hi)
      return true;
  return false;
}

bool IsFatalCode(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
  case EXCEPTION_DATATYPE_MISALIGNMENT:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_PRIV_INSTRUCTION:
  case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_INT_OVERFLOW:
  case EXCEPTION_STACK_OVERFLOW:
  case EXCEPTION_FLT_DENORMAL_OPERAND:
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
  case EXCEPTION_FLT_INEXACT_RESULT:
  case EXCEPTION_FLT_INVALID_OPERATION:
  case EXCEPTION_FLT_OVERFLOW:
  case EXCEPTION_FLT_UNDERFLOW:
    return true;
  default:
    return false;
  }
}

// Windows has no POSIX signals/execinfo. An unhandled SEH exception writes
// the crash file — the code, the faulting address and the faulting thread's
// stack walked from the exception's own context, each frame as module + RVA
// — and a minidump, then continues the search so the process still dies as
// before.
LONG WINAPI OnWindowsException(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  Out file;
  file.file = OpenForWrite(g_crashPathW);
  char reason[48] = "Windows exception ";
  {
    // The code, formatted without stdio.
    size_t n = std::strlen(reason);
    reason[n++] = '0';
    reason[n++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
      reason[n++] = "0123456789ABCDEF"[(code >> shift) & 0xf];
    reason[n] = '\0';
  }
  PutHeader(file, reason, ExceptionName(code));
  if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
      info->ExceptionRecord->NumberParameters >= 2) {
    // What was touched, as the POSIX handler's si_addr: all ones when the
    // address was not canonical (a general-protection fault).
    const ULONG_PTR how = info->ExceptionRecord->ExceptionInformation[0];
    file.Put("address: ");
    file.Hex(info->ExceptionRecord->ExceptionInformation[1]);
    file.Put(how == 0 ? " (read)\n" : how == 8 ? " (execute)\n" : " (write)\n");
  }
  file.Put("raw frames (module+offset):\n");
  const auto fault =
      reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  PutIndexedFrame(file, 0, fault);
#if defined(__x86_64__) || defined(_M_X64)
  CONTEXT ctx = *info->ContextRecord;
  for (int i = 1; i < kMaxCrashFrames; ++i) {
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
    if (fn) {
      void* handlerData = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx,
                       &handlerData, &establisher, nullptr);
    } else {
      // A leaf function: the return address is on top of the stack.
      if (ctx.Rsp == 0)
        break;
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
    }
    if (ctx.Rip == 0)
      break;
    PutIndexedFrame(file, i, static_cast<uintptr_t>(ctx.Rip));
  }
#else
  void* frames[kMaxCrashFrames];
  const USHORT n =
      RtlCaptureStackBackTrace(0, kMaxCrashFrames, frames, nullptr);
  for (USHORT i = 0; i < n; ++i)
    PutIndexedFrame(file, i + 1, reinterpret_cast<uintptr_t>(frames[i]));
#endif
  if (file.file != INVALID_HANDLE_VALUE)
    CloseHandle(file.file);
  WriteMinidump(info);

  WriteStderr("\n=== fatal Windows exception (code ");
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08lx", code);
  WriteStderr(buf);
  WriteStderr(") ===\n");
  return EXCEPTION_CONTINUE_SEARCH;
}

// A fault in code we ship, at first chance. Nothing there can handle it: GCC
// code has no __except, and the start-up filter mingw puts round main only
// calls a signal() handler, which is left at the default
// (cad/OccSignals.cpp). Waiting for the unhandled-exception filter loses it
// on every std::thread: mcfgthread's thread entry ends the process with
// RaiseFailFastException before any filter runs (__MCF_seh_top,
// mcfgthread src/xglobals.c), and a worker's crash left no report. So it is
// reported here, and the process ends with the exception's code. A fault in
// the system's own code is left alone -- that code may be probing memory
// under its own __try -- and reaches the filter as before. Registered last:
// OCC's catch scopes, whose handler runs first, take their faults before it.
LONG WINAPI OnFaultInShippedCode(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (!IsFatalCode(code) || !InShippedCode(reinterpret_cast<uintptr_t>(
                                info->ExceptionRecord->ExceptionAddress)))
    return EXCEPTION_CONTINUE_SEARCH;
  OnWindowsException(info);
  TerminateProcess(GetCurrentProcess(), code);
  return EXCEPTION_CONTINUE_SEARCH;
}

// abort() — and std::terminate through it — is a CRT signal on Windows, not
// an SEH exception, so the filter above never sees it.
void OnWindowsAbort(int) {
  signal(SIGABRT, SIG_DFL);
  Out file;
  file.file = OpenForWrite(g_crashPathW);
  PutHeader(file, "SIGABRT", nullptr);
  file.Put("raw frames (module+offset):\n");
  void* frames[kMaxCrashFrames];
  const USHORT n =
      RtlCaptureStackBackTrace(1, kMaxCrashFrames, frames, nullptr);
  for (USHORT i = 0; i < n; ++i)
    PutIndexedFrame(file, i, reinterpret_cast<uintptr_t>(frames[i]));
  if (file.file != INVALID_HANDLE_VALUE)
    CloseHandle(file.file);
  WriteMinidump(nullptr);
  WriteStderr("\n=== fatal signal SIGABRT ===\n");
}

// Ctrl+C, Ctrl+Break, the console closing, logoff and shutdown: somebody
// asked the process to stop, which is not a crash -- what SIGINT, SIGTERM and
// SIGHUP are on Linux. Runs on a thread of its own, which the system starts
// for the event. Kept in front of OCC's (cad/OccSignals.h), which swallows
// Ctrl+C and exit(254)s on the rest.
BOOL WINAPI OnConsoleControl(DWORD event) {
  const bool closing = event == CTRL_CLOSE_EVENT ||
                       event == CTRL_LOGOFF_EVENT ||
                       event == CTRL_SHUTDOWN_EVENT;
  using Handler = void (*)(int);
  const Handler stop = signal(SIGINT, SIG_DFL);
  if (stop == SIG_IGN) {
    signal(SIGINT, SIG_IGN);
    if (!closing)
      return TRUE;
  } else if (stop != SIG_DFL && stop != SIG_ERR) {
    // Headless and offscreen stop on SIGINT, and end the session the
    // ordinary way.
    stop(SIGINT);
    if (!closing)
      return TRUE;
    // The process is ended when this returns, and at most ~5 s after the
    // event: the loop's own exit, which ends this thread too, comes first.
    Sleep(4000);
  }
  // Where nothing stops cleanly (the windowed app): the session marker goes,
  // so the next start does not report a crash, and the process ends at once,
  // as the default signal action does on Linux.
  if (g_markerPathW[0])
    DeleteFileW(g_markerPathW);
  if (!closing)
    TerminateProcess(GetCurrentProcess(), STATUS_CONTROL_C_EXIT);
  return TRUE;
}
#endif

} // namespace

void InstallCrashHandler() {
  g_previousTerminate = std::set_terminate(OnTerminate);
#if defined(_WIN32)
  SetUnhandledExceptionFilter(OnWindowsException);
  RecordShippedModules();
  AddVectoredExceptionHandler(0, OnFaultInShippedCode);
  signal(SIGABRT, OnWindowsAbort);
  occsig::SetConsoleControlHandler(OnConsoleControl);
#else
  RecordModules();
  stack_t alt{};
  alt.ss_sp = g_altStack;
  alt.ss_size = sizeof g_altStack;
  const bool haveAlt = sigaltstack(&alt, nullptr) == 0;
  struct sigaction sa{};
  sa.sa_sigaction = OnFatalSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | (haveAlt ? SA_ONSTACK : 0);
  for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
    sigaction(sig, &sa, nullptr);
  for (int sig : {SIGTERM, SIGINT, SIGHUP})
    signal(sig, OnTerminationSignal);
  // Warm the unwinder: its first use can load libgcc_s, which must not
  // happen for the first time inside the handler.
  void* warm[4];
  (void)backtrace(warm, 4);
#endif
}

void SetCrashReportFiles(const std::filesystem::path& crashFile,
                         const std::filesystem::path& dumpFile,
                         const std::filesystem::path& markerFile) {
#if defined(_WIN32)
  // Wide, from the path itself: path::string() is the ANSI code page there,
  // which cannot spell every user name.
  const auto widen = [](wchar_t* dst, const std::filesystem::path& p) {
    const std::wstring w = p.wstring();
    const size_t n = std::min(w.size(), kPathMax - 1);
    std::wmemcpy(dst, w.data(), n);
    dst[n] = L'\0';
  };
  widen(g_crashPathW, crashFile);
  widen(g_dumpPathW, dumpFile);
  widen(g_markerPathW, markerFile);
  // dbghelp loads here, not inside the filter.
  (void)LoadLibraryW(L"dbghelp.dll");
  RecordShippedModules();
#else
  (void)dumpFile; // a core dump is the system's business on POSIX
  RecordModules();
#endif
  CopyInto(g_crashPath, sizeof g_crashPath, crashFile.string());
  CopyInto(g_markerPath, sizeof g_markerPath, markerFile.string());
}

void RefreshCrashModules() {
#if defined(_WIN32)
  RecordShippedModules();
#else
  RecordModules();
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

#if defined(HAVE_STDCXX_STACKTRACE) && !defined(__APPLE__)
#define HAVE_EXE_SYMBOLS 1
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
#if defined(HAVE_EXE_SYMBOLS)
  static ExeSymbols symbols;
#endif
  size_t shown = 0;
  for (size_t i = 0; i < g_frameCount; ++i) {
    const std::stacktrace_entry& e = g_frames[i];
    std::string name = e.description();
    if (name.empty())
      continue; // the signal trampoline, the end of the stack
    if (!e.source_file().empty())
      name += " at " + e.source_file() + ":" + std::to_string(e.source_line());
#if defined(HAVE_EXE_SYMBOLS)
    // A return address, so one back to stay inside the call instruction's
    // function when the call was its last.
    const std::string owner = WithoutClone(
        symbols.Owner(static_cast<uintptr_t>(e.native_handle()) - 1));
    if (!owner.empty() && BareName(e.description()) != BareName(owner))
      name += ", inlined in " + owner;
#endif
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
namespace {

// Windows has no signal to ask a thread for its own stack: the watchdog
// suspends the main thread, reads its registers and walks its stack from
// there with RtlVirtualUnwind, as the crash handler walks a fault's. The walk
// runs while the thread is stopped, so it must take no lock that thread
// might hold -- the heap's, or the loader's, which RtlLookupFunctionEntry
// and GetModuleHandleEx take. So the functions are looked up in each
// module's own exception table (.pdata), from a list of modules read and
// pinned before the suspend, and named after the resume.

HANDLE g_mainThread = nullptr;
ULONG_PTR g_stackLow = 0, g_stackHigh = 0;
constexpr int kMaxStallFrames = 64;

struct PeModule {
  HMODULE pinned = nullptr;
  uintptr_t base = 0, end = 0;
  const RUNTIME_FUNCTION* table = nullptr;
  DWORD count = 0;
};

std::vector<PeModule> PinModules() {
  std::vector<HMODULE> handles(1024);
  DWORD needed = 0;
  if (!K32EnumProcessModules(
          GetCurrentProcess(), handles.data(),
          static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed))
    return {};
  handles.resize(std::min<size_t>(handles.size(), needed / sizeof(HMODULE)));
  std::vector<PeModule> out;
  for (HMODULE h : handles) {
    PeModule m;
    // A reference of our own, so none unloads while the walk reads it.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(h), &m.pinned) ||
        m.pinned != h) {
      if (m.pinned)
        FreeLibrary(m.pinned);
      continue;
    }
    const auto base = reinterpret_cast<uintptr_t>(h);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    m.base = base;
    m.end = base + nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir.VirtualAddress != 0 && dir.Size >= sizeof(RUNTIME_FUNCTION)) {
      m.table =
          reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
      m.count = dir.Size / sizeof(RUNTIME_FUNCTION);
    }
    out.push_back(m);
  }
  std::sort(out.begin(), out.end(), [](const PeModule& a, const PeModule& b) {
    return a.base < b.base;
  });
  return out;
}

const PeModule* ModuleAt(const std::vector<PeModule>& modules, uintptr_t pc) {
  auto it = std::upper_bound(
      modules.begin(), modules.end(), pc,
      [](uintptr_t a, const PeModule& m) { return a < m.base; });
  if (it == modules.begin())
    return nullptr;
  --it;
  return pc < it->end ? &*it : nullptr;
}

// The function entry for `pc`, from its module's table (sorted by address,
// as the PE format requires): no lock, no allocation.
RUNTIME_FUNCTION* FunctionAt(const std::vector<PeModule>& modules, uintptr_t pc,
                             DWORD64* imageBase) {
  const PeModule* m = ModuleAt(modules, pc);
  if (m == nullptr || m->table == nullptr)
    return nullptr;
  const auto rva = static_cast<DWORD>(pc - m->base);
  size_t lo = 0, hi = m->count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const RUNTIME_FUNCTION& f = m->table[mid];
    if (rva < f.BeginAddress)
      hi = mid;
    else if (rva >= f.EndAddress)
      lo = mid + 1;
    else {
      *imageBase = m->base;
      auto* fn = const_cast<RUNTIME_FUNCTION*>(&f);
      // An entry that names another entry (the low bit set).
      if (fn->UnwindData & 1)
        fn = reinterpret_cast<RUNTIME_FUNCTION*>(m->base + fn->UnwindData - 1);
      return fn;
    }
  }
  return nullptr;
}

} // namespace

void InstallStallCapture() {
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                       GetCurrentProcess(), &g_mainThread,
                       THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                           THREAD_QUERY_INFORMATION,
                       FALSE, 0))
    g_mainThread = nullptr;
  // The stack's whole reservation: from its allocation base to the top the
  // TEB records (GetCurrentThreadStackLimits, without needing Windows 8).
  MEMORY_BASIC_INFORMATION region{};
  if (VirtualQuery(&region, &region, sizeof region) != 0) {
    g_stackLow = reinterpret_cast<ULONG_PTR>(region.AllocationBase);
    g_stackHigh = reinterpret_cast<ULONG_PTR>(
        reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
  }
}

std::string CaptureMainThreadStack(double) {
  if (g_mainThread == nullptr)
    return {};
  const std::vector<PeModule> modules = PinModules();
  uintptr_t frames[kMaxStallFrames];
  int n = 0;
  if (SuspendThread(g_mainThread) != static_cast<DWORD>(-1)) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(g_mainThread, &ctx)) {
      const auto onStack = [](DWORD64 sp) {
        return sp >= g_stackLow && sp + 8 <= g_stackHigh;
      };
      frames[n++] = static_cast<uintptr_t>(ctx.Rip);
      while (n < kMaxStallFrames && onStack(ctx.Rsp)) {
        const DWORD64 sp = ctx.Rsp;
        DWORD64 imageBase = 0;
        RUNTIME_FUNCTION* fn = FunctionAt(modules, ctx.Rip, &imageBase);
        if (fn != nullptr) {
          void* handlerData = nullptr;
          DWORD64 establisher = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx,
                           &handlerData, &establisher, nullptr);
        } else if (n == 1) {
          // Stopped in a leaf function: its return address is on top.
          ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
          ctx.Rsp += 8;
        } else {
          break; // code with no unwind information: the walk is lost
        }
        if (ctx.Rip == 0 || ctx.Rsp < sp || !onStack(ctx.Rsp))
          break;
        frames[n++] = static_cast<uintptr_t>(ctx.Rip);
      }
    }
    ResumeThread(g_mainThread);
  }
  // Named now the thread runs again, as the crash file does: module + RVA,
  // for tools/symbolize-crash.py.
  std::string out;
  for (int i = 0; i < n; ++i) {
    char line[64];
    std::string module;
    uintptr_t offset = frames[i];
    if (const PeModule* m = ModuleAt(modules, frames[i])) {
      wchar_t wide[MAX_PATH];
      const DWORD len = GetModuleFileNameW(m->pinned, wide, MAX_PATH);
      char name[MAX_PATH * 3];
      const int bytes =
          WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len), name,
                              sizeof name - 1, nullptr, nullptr);
      module.assign(name, bytes > 0 ? bytes : 0);
      offset -= m->base;
    }
    std::snprintf(line, sizeof line, "%s0x%llx", module.empty() ? "" : "+",
                  static_cast<unsigned long long>(offset));
    out += "  #" + std::to_string(i) + " " + module + line + "\n";
  }
  for (const PeModule& m : modules)
    FreeLibrary(m.pinned);
  return out;
}
#endif
