// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/ChildProcess.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h first: psapi.h uses its types.
#include <psapi.h>
#include <wchar.h>
#else
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace proc {
namespace {

#if defined(_WIN32)
constexpr char kPathSep = ';';
#else
constexpr char kPathSep = ':';
#endif

bool IsExecutableFile(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec))
    return false;
#if defined(_WIN32)
  return true;
#else
  return ::access(p.c_str(), X_OK) == 0;
#endif
}

// A file of its own per child, so two exports never share one.
std::string StderrPath() {
  static std::atomic<unsigned> counter{0};
  std::error_code ec;
  // $TMPDIR can name a directory that is gone (a nix-shell's, after the
  // shell exits); the working directory is never the fallback, since that is
  // wherever the app was started — a source tree, as often as not.
  fs::path dir = fs::temp_directory_path(ec);
  if (ec || !fs::is_directory(dir, ec)) {
#if defined(_WIN32)
    dir = ".";
#else
    dir = "/tmp";
#endif
  }
  const long pid = CurrentPid();
  return (dir / ("designer-child-" + std::to_string(pid) + "-" +
                 std::to_string(counter.fetch_add(1)) + ".log"))
      .string();
}

// The last few lines of what the child said: an encoder's reason is at the
// end, after whatever it printed on the way.
std::string Tail(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  while (!text.empty() &&
         (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();
  constexpr int kLines = 6;
  constexpr size_t kMaxChars = 1200;
  size_t pos = text.size();
  for (int n = 0; n < kLines && pos != std::string::npos && pos > 0; n++)
    pos = text.rfind('\n', pos - 1);
  std::string tail =
      pos == std::string::npos || pos == 0 ? text : text.substr(pos + 1);
  if (tail.size() > kMaxChars)
    tail = tail.substr(tail.size() - kMaxChars);
  return tail;
}

#if defined(_WIN32)
// One argument for the C runtime's argv parser: double-quoted, with embedded
// quotes and the backslashes before them escaped.
std::string QuoteArg(const std::string& a) {
  std::string out = "\"";
  size_t backslashes = 0;
  for (char c : a) {
    if (c == '\\') {
      backslashes++;
      continue;
    }
    if (c == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out += '"';
    } else {
      out.append(backslashes, '\\');
      out += c;
    }
    backslashes = 0;
  }
  out.append(backslashes * 2, '\\');
  out += '"';
  return out;
}

std::wstring Wide(const std::string& s) {
  if (s.empty())
    return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
  return w;
}

std::string LastError() {
  const DWORD code = ::GetLastError();
  char* text = nullptr;
  ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<char*>(&text), 0,
                   nullptr);
  std::string m = text != nullptr ? text : "error " + std::to_string(code);
  if (text != nullptr)
    ::LocalFree(text);
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r' ||
                        m.back() == ' ' || m.back() == '.'))
    m.pop_back();
  return m;
}

// A file the child writes (truncated), as an inheritable handle.
HANDLE OpenForChild(const std::wstring& path) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  return ::CreateFileW(path.c_str(), GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// This process's environment with `env`'s keys set over it, as the
// double-NUL-terminated block CreateProcessW takes. Windows' keys are
// case-insensitive.
std::wstring
EnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& env) {
  std::vector<std::wstring> keys;
  for (const auto& kv : env)
    keys.push_back(Wide(kv.first));
  std::wstring block;
  if (wchar_t* all = ::GetEnvironmentStringsW(); all != nullptr) {
    for (const wchar_t* e = all; *e != L'\0'; e += wcslen(e) + 1) {
      bool replaced = false;
      for (const std::wstring& k : keys)
        if (_wcsnicmp(e, k.c_str(), k.size()) == 0 && e[k.size()] == L'=')
          replaced = true;
      if (!replaced) {
        block += e;
        block += L'\0';
      }
    }
    ::FreeEnvironmentStringsW(all);
  }
  for (const auto& [key, value] : env) {
    block += Wide(key + "=" + value);
    block += L'\0';
  }
  block += L'\0';
  return block;
}
#endif

} // namespace

long CurrentPid() {
#if defined(_WIN32)
  return static_cast<long>(::GetCurrentProcessId());
#else
  return static_cast<long>(::getpid());
#endif
}

bool ProcessAlive(long pid) {
  if (pid <= 0)
    return false;
#if defined(_WIN32)
  HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
  if (h == nullptr)
    return ::GetLastError() != ERROR_INVALID_PARAMETER; // no such process
  DWORD code = 0;
  const bool running =
      ::GetExitCodeProcess(h, &code) == 0 || code == STILL_ACTIVE;
  ::CloseHandle(h);
  return running;
#else
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

std::string FindOnPath(const std::string& name) {
  const char* env = std::getenv("PATH");
  if (env == nullptr)
    return {};
  const std::string path(env);
#if defined(_WIN32)
  const std::string file =
      fs::path(name).has_extension() ? name : name + ".exe";
#else
  const std::string& file = name;
#endif
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find(kPathSep, start);
    if (end == std::string::npos)
      end = path.size();
    const std::string dir = path.substr(start, end - start);
    if (!dir.empty()) {
      const fs::path candidate = fs::path(dir) / file;
      if (IsExecutableFile(candidate))
        return candidate.string();
    }
    start = end + 1;
  }
  return {};
}

PipedChild::~PipedChild() {
  if (Running())
    Kill();
  if (!m_stderrPath.empty()) {
    std::error_code ec;
    fs::remove(m_stderrPath, ec);
  }
}

#if defined(_WIN32)

bool PipedChild::Start(const std::string& program,
                       const std::vector<std::string>& args, std::string* error,
                       const ChildOptions& options) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  m_stderrPath = StderrPath();
  // Its input: a pipe whose read end it inherits and whose write end it
  // must not, or it would hold its own input open and never see the end.
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE inRead = nullptr, inWrite = nullptr;
  if (!::CreatePipe(&inRead, &inWrite, &sa, 0))
    return fail("could not make a pipe: " + LastError());
  ::SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
  HANDLE out = OpenForChild(options.stdoutPath.empty()
                                ? std::wstring(L"NUL")
                                : fs::path(options.stdoutPath).wstring());
  HANDLE err = OpenForChild(fs::path(m_stderrPath).wstring());
  const auto closeAll = [&] {
    for (HANDLE h : {inRead, inWrite, out, err})
      if (h != nullptr && h != INVALID_HANDLE_VALUE)
        ::CloseHandle(h);
  };
  if (out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE) {
    const std::string why = LastError();
    closeAll();
    return fail("could not open the output of " + program + ": " + why);
  }

  // Only these three handles reach the child: another thread starting a
  // child at the same moment must not hand it ours.
  HANDLE inherit[3] = {inRead, out, err};
  SIZE_T attrSize = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  std::vector<char> attrStore(attrSize);
  auto* attrs =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStore.data());
  if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) ||
      !::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherit, sizeof(inherit), nullptr,
                                   nullptr)) {
    const std::string why = LastError();
    closeAll();
    return fail("could not start " + program + ": " + why);
  }
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = inRead;
  si.StartupInfo.hStdOutput = out;
  si.StartupInfo.hStdError = err;
  si.lpAttributeList = attrs;

  std::string cmd = QuoteArg(program);
  for (const std::string& a : args)
    cmd += " " + QuoteArg(a);
  std::wstring cmdLine = Wide(cmd);
  std::wstring env = EnvironmentBlock(options.env);
  const std::wstring app = fs::path(program).wstring();
  const std::wstring cwd =
      options.cwd.empty() ? std::wstring() : fs::path(options.cwd).wstring();
  PROCESS_INFORMATION pi{};
  const BOOL ok = ::CreateProcessW(
      app.c_str(), cmdLine.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
          EXTENDED_STARTUPINFO_PRESENT,
      env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
  const std::string why = ok ? std::string() : LastError();
  ::DeleteProcThreadAttributeList(attrs);
  // The child has its copies; ours of its ends would keep the files open
  // (a run directory could not be removed) and its input from ending.
  ::CloseHandle(inRead);
  ::CloseHandle(out);
  ::CloseHandle(err);
  if (!ok) {
    ::CloseHandle(inWrite);
    return fail("could not start " + program + ": " + why);
  }
  ::CloseHandle(pi.hThread);
  m_process = pi.hProcess;
  m_pid = static_cast<long>(pi.dwProcessId);
  m_stdin = inWrite;
  m_started = true;
  return true;
}

bool PipedChild::Write(const void* data, size_t n) {
  if (!Running() || m_stdin == nullptr)
    return false;
  const char* p = static_cast<const char*>(data);
  while (n > 0) {
    const DWORD chunk = static_cast<DWORD>(n > (1u << 30) ? (1u << 30) : n);
    DWORD written = 0;
    // Fails (ERROR_BROKEN_PIPE, ERROR_NO_DATA) once the child is gone.
    if (!::WriteFile(static_cast<HANDLE>(m_stdin), p, chunk, &written, nullptr))
      return false;
    p += written;
    n -= written;
  }
  return true;
}

// Ends the wait on the process (blocking when `block`), keeping its exit
// code and peak working set. False when it has not ended (only without
// `block`).
bool PipedChild::Reap(bool block) {
  if (m_process == nullptr)
    return true;
  HANDLE h = static_cast<HANDLE>(m_process);
  if (::WaitForSingleObject(h, block ? INFINITE : 0) != WAIT_OBJECT_0)
    return false;
  DWORD code = 0;
  if (!::GetExitCodeProcess(h, &code))
    m_exitCode = -1;
  else
    // An NTSTATUS failure (0xC0000005, an access violation) is the
    // process dying, as a signal is on POSIX.
    m_exitCode = code >= 0xC0000000u ? -1 : static_cast<int>(code);
  PROCESS_MEMORY_COUNTERS pmc{};
  if (::GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
    m_peakBytes = static_cast<uint64_t>(pmc.PeakWorkingSetSize);
  ::CloseHandle(h);
  m_process = nullptr;
  m_pid = 0;
  return true;
}

int PipedChild::Finish(std::string* stderrTail) {
  if (m_stdin != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(m_stdin));
    m_stdin = nullptr;
  }
  Reap(true);
  m_finished = true;
  if (stderrTail)
    *stderrTail = Tail(m_stderrPath);
  return m_exitCode;
}

bool PipedChild::Poll(int* code) {
  if (m_process != nullptr) {
    if (!Reap(false))
      return false;
    if (m_stdin != nullptr) {
      ::CloseHandle(static_cast<HANDLE>(m_stdin));
      m_stdin = nullptr;
    }
    m_finished = true;
  }
  if (code)
    *code = m_exitCode;
  return true;
}

long PipedChild::Pid() const { return m_process != nullptr ? m_pid : 0; }

void PipedChild::Kill() {
  if (m_process != nullptr)
    ::TerminateProcess(static_cast<HANDLE>(m_process), 1);
  Finish(nullptr);
}

#else // POSIX

bool PipedChild::Start(const std::string& program,
                       const std::vector<std::string>& args, std::string* error,
                       const ChildOptions& options) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0)
    return fail(std::string("could not make a pipe: ") + std::strerror(errno));
  m_stderrPath = StderrPath();

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO);
  if (options.stdoutPath.empty())
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
  else
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                     options.stdoutPath.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                   m_stderrPath.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0600);
  // The file actions run in order, after the child exists: the output files
  // above are opened where this process stands, and only then does the
  // child move to its own directory.
  if (!options.cwd.empty())
    posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str());
  // The child starts with default signal handling, whatever this process
  // set: an encoder that inherits SIGPIPE ignored behaves differently.
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  sigaddset(&defaults, SIGINT);
  sigaddset(&defaults, SIGTERM);
  posix_spawnattr_setsigdefault(&attr, &defaults);
  sigset_t none;
  sigemptyset(&none);
  posix_spawnattr_setsigmask(&attr, &none);
  posix_spawnattr_setflags(&attr,
                           POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(program.c_str()));
  for (const std::string& a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  // This process's environment with the options' keys set over it.
  std::vector<std::string> envStore;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    const std::string entry(*e);
    bool replaced = false;
    for (const auto& [key, value] : options.env)
      if (entry.size() > key.size() && entry.compare(0, key.size(), key) == 0 &&
          entry[key.size()] == '=')
        replaced = true;
    if (!replaced)
      envStore.push_back(entry);
  }
  for (const auto& [key, value] : options.env)
    envStore.push_back(key + "=" + value);
  std::vector<char*> envp;
  for (std::string& e : envStore)
    envp.push_back(e.data());
  envp.push_back(nullptr);
  pid_t pid = -1;
  const int rc = ::posix_spawn(&pid, program.c_str(), &actions, &attr,
                               argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  ::close(fds[0]);
  if (rc != 0) {
    ::close(fds[1]);
    return fail("could not start " + program + ": " + std::strerror(rc));
  }
  m_stdin = fds[1];
  m_pid = pid;
  m_started = true;
  return true;
}

bool PipedChild::Write(const void* data, size_t n) {
  if (!Running() || m_stdin < 0)
    return false;
  // A child that has exited turns the next write into SIGPIPE, which would
  // end THIS process. Blocked on the writing thread, the write fails with
  // EPIPE instead, and the pending signal is dropped before the mask goes
  // back.
  sigset_t pipeOnly, previous;
  sigemptyset(&pipeOnly);
  sigaddset(&pipeOnly, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &pipeOnly, &previous);
  const char* p = static_cast<const char*>(data);
  bool ok = true;
  while (n > 0) {
    const ssize_t w = ::write(m_stdin, p, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      ok = false;
      break;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  if (!ok) {
    const struct timespec zero{0, 0};
    sigtimedwait(&pipeOnly, nullptr, &zero); // consume it, if it is pending
  }
  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  return ok;
}

namespace {

// Reaps `pid` (waiting when `block`), keeping its exit code and the most
// memory it held. False when it has not ended (only without `block`).
bool Reap(int pid, bool block, int& exitCode, uint64_t& peakBytes) {
  int status = 0;
  struct rusage usage{};
  pid_t r;
  do {
    r = ::wait4(pid, &status, block ? 0 : WNOHANG, &usage);
  } while (r < 0 && errno == EINTR);
  if (r == 0)
    return false;
  exitCode = r == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (r == pid)
    peakBytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024; // KiB on Linux
  return true;
}

} // namespace

int PipedChild::Finish(std::string* stderrTail) {
  if (m_stdin >= 0) {
    ::close(m_stdin);
    m_stdin = -1;
  }
  if (m_pid > 0) {
    Reap(m_pid, true, m_exitCode, m_peakBytes);
    m_pid = -1;
  }
  m_finished = true;
  if (stderrTail)
    *stderrTail = Tail(m_stderrPath);
  return m_exitCode;
}

bool PipedChild::Poll(int* code) {
  if (m_pid > 0) {
    if (!Reap(m_pid, false, m_exitCode, m_peakBytes))
      return false;
    m_pid = -1;
    if (m_stdin >= 0) {
      ::close(m_stdin);
      m_stdin = -1;
    }
    m_finished = true;
  }
  if (code)
    *code = m_exitCode;
  return true;
}

long PipedChild::Pid() const { return m_pid > 0 ? m_pid : 0; }

void PipedChild::Kill() {
  if (m_pid > 0)
    ::kill(m_pid, SIGTERM);
  Finish(nullptr);
}

#endif

} // namespace proc
