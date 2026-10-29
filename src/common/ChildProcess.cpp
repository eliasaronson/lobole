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
#include <process.h>
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
#if defined(_WIN32)
  const long pid = static_cast<long>(_getpid());
#else
  const long pid = static_cast<long>(::getpid());
#endif
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
// One argument for cmd.exe + the C runtime's argv parser: double-quoted, with
// embedded quotes and the backslashes before them escaped.
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
#endif

} // namespace

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
  m_stderrPath = StderrPath();
  // cmd.exe carries what posix_spawn's file actions do: the directory, the
  // environment and where stdout goes, ahead of the program.
  std::string cmd;
  if (!options.cwd.empty())
    cmd += "cd /d " + QuoteArg(options.cwd) + " && ";
  for (const auto& [key, value] : options.env)
    cmd += "set \"" + key + "=" + value + "\" && ";
  cmd += QuoteArg(program);
  for (const std::string& a : args)
    cmd += " " + QuoteArg(a);
  if (!options.stdoutPath.empty())
    cmd += " >" + QuoteArg(options.stdoutPath);
  cmd += " 2>" + QuoteArg(m_stderrPath);
  // cmd.exe /c strips the first and last quote of a line that starts with
  // one; an outer pair gives it those to strip.
  cmd = "\"" + cmd + "\"";
  FILE* f = ::_popen(cmd.c_str(), "wb");
  if (f == nullptr) {
    if (error)
      *error = "could not start " + program + ": " + std::strerror(errno);
    return false;
  }
  m_pipe = f;
  m_started = true;
  return true;
}

bool PipedChild::Write(const void* data, size_t n) {
  if (!Running() || m_pipe == nullptr)
    return false;
  FILE* f = static_cast<FILE*>(m_pipe);
  return std::fwrite(data, 1, n, f) == n;
}

int PipedChild::Finish(std::string* stderrTail) {
  if (m_pipe != nullptr) {
    m_exitCode = ::_pclose(static_cast<FILE*>(m_pipe));
    m_pipe = nullptr;
  }
  m_finished = true;
  if (stderrTail)
    *stderrTail = Tail(m_stderrPath);
  return m_exitCode;
}

bool PipedChild::Poll(int* code) {
  // _popen gives no process to ask: waiting is the only way to know.
  const int c = Running() ? Finish(nullptr) : m_exitCode;
  if (code)
    *code = c;
  return true;
}

long PipedChild::Pid() const { return 0; }

void PipedChild::Kill() {
  // _popen gives no process handle: closing the input ends an encoder, which
  // stops at the end of its input, and _pclose waits for that.
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
