// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/PtySession.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace term {

namespace {

// The child's environment. TERM must name something the program can look up;
// COLORTERM is what makes truecolor programs (Claude Code among them) emit
// 24-bit SGR instead of falling back to 256 colours.
void ApplyChildEnv(const PtySession::Options& opts) {
  setenv("TERM", "xterm-256color", 1);
  setenv("COLORTERM", "truecolor", 1);
  // We are not a terminal multiplexer and we are not xterm; leave anything
  // that claims otherwise behind so a program does not probe for features we
  // do not have.
  unsetenv("TERM_PROGRAM");
  unsetenv("TERM_PROGRAM_VERSION");
  for (const auto& [k, v] : opts.env)
    setenv(k.c_str(), v.c_str(), 1);
}

} // namespace

PtySession::~PtySession() { Terminate(); }

bool PtySession::Start(const Options& opts, std::string* error) {
  if (m_pid > 0) {
    if (error != nullptr)
      *error = "session already running";
    return false;
  }

  std::string program = opts.program;
  if (program.empty()) {
    const char* shell = std::getenv("SHELL");
    program = (shell != nullptr && *shell != '\0') ? shell : "/bin/sh";
  }

  m_rows = opts.rows > 0 ? opts.rows : 24;
  m_cols = opts.cols > 0 ? opts.cols : 80;
  winsize ws = {};
  ws.ws_row = static_cast<unsigned short>(m_rows);
  ws.ws_col = static_cast<unsigned short>(m_cols);

  int fd = -1;
  const pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
  if (pid < 0) {
    if (error != nullptr)
      *error = std::string("could not open a pseudo-terminal: ") +
               std::strerror(errno);
    return false;
  }

  if (pid == 0) {
    // --- child ---------------------------------------------------------
    if (!opts.cwd.empty()) {
      if (chdir(opts.cwd.c_str()) != 0) {
        // Not fatal: better to land in the wrong directory with a shell than
        // to exit with nothing on screen.
      }
    }
    ApplyChildEnv(opts);
    // A child of a GUI app inherits its signal dispositions; a shell with
    // SIGINT ignored cannot be interrupted with Ctrl+C.
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : opts.args)
      argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(program.c_str(), argv.data());
    // Only reached if exec failed. Say so ON THE TERMINAL — the parent has
    // no other channel, and a panel that just goes blank reads as a crash.
    const std::string msg = "lobole: could not run '" + program +
                            "': " + std::strerror(errno) + "\r\n";
    ssize_t ignored = write(STDERR_FILENO, msg.data(), msg.size());
    (void)ignored;
    _exit(127);
  }

  // --- parent ------------------------------------------------------------
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
  m_fd = fd;
  m_pid = static_cast<int>(pid);
  m_exitStatus = -1;
  return true;
}

std::string PtySession::Poll(size_t limit) {
  std::string out;
  if (m_fd < 0)
    return out;

  char buf[8192];
  while (out.size() < limit) {
    const ssize_t n = read(m_fd, buf, sizeof buf);
    if (n > 0) {
      out.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0)
      break; // EOF: the child closed the slave side
    if (errno == EINTR)
      continue;
    break; // EAGAIN — nothing more this frame
  }

  if (m_pid > 0) {
    int status = 0;
    const pid_t r = waitpid(static_cast<pid_t>(m_pid), &status, WNOHANG);
    if (r == static_cast<pid_t>(m_pid)) {
      if (WIFEXITED(status))
        m_exitStatus = WEXITSTATUS(status);
      else if (WIFSIGNALED(status))
        m_exitStatus = 128 + WTERMSIG(status);
      else
        m_exitStatus = -1;
      m_pid = -1;
      // The fd stays open until Terminate(): output written just before exit
      // is still in the buffer, and dropping it loses the last screen — which
      // is exactly the screen that says why the program stopped.
    }
  }
  return out;
}

void PtySession::Write(const char* bytes, size_t n) {
  if (m_fd < 0)
    return;
  size_t off = 0;
  while (off < n) {
    const ssize_t w = write(m_fd, bytes + off, n - off);
    if (w > 0) {
      off += static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR)
      continue;
    break; // EAGAIN with a full buffer, or the child is gone
  }
}

void PtySession::Resize(int rows, int cols) {
  rows = rows > 0 ? rows : 1;
  cols = cols > 0 ? cols : 1;
  if (rows == m_rows && cols == m_cols)
    return;
  m_rows = rows;
  m_cols = cols;
  if (m_fd < 0)
    return;
  winsize ws = {};
  ws.ws_row = static_cast<unsigned short>(rows);
  ws.ws_col = static_cast<unsigned short>(cols);
  ioctl(m_fd, TIOCSWINSZ, &ws);
  // forkpty gave the child its own session, so the kernel delivers SIGWINCH
  // to the foreground process group on TIOCSWINSZ; no explicit kill needed.
}

std::string PtySession::ForegroundName() const {
  if (m_fd < 0)
    return {};
  const pid_t pg = tcgetpgrp(m_fd);
  if (pg <= 0)
    return {};
  // /proc is Linux-specific and so is this whole file; a platform without it
  // simply gets no name, and the caller falls back to the session's label.
  const std::string path = "/proc/" + std::to_string(pg) + "/comm";
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr)
    return {};
  char buf[64] = {};
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  std::string name(buf, n);
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
    name.pop_back();
  return name;
}

void PtySession::Terminate() {
  if (m_pid > 0) {
    kill(static_cast<pid_t>(m_pid), SIGHUP);
    // Give it a moment to leave on its own, then insist. This runs on the UI
    // thread, so the wait is bounded hard rather than being a loop with a
    // generous timeout.
    for (int i = 0; i < 20 && m_pid > 0; i++) {
      int status = 0;
      const pid_t r = waitpid(static_cast<pid_t>(m_pid), &status, WNOHANG);
      if (r == static_cast<pid_t>(m_pid)) {
        m_pid = -1;
        break;
      }
      usleep(5000);
    }
    if (m_pid > 0) {
      kill(static_cast<pid_t>(m_pid), SIGKILL);
      int status = 0;
      waitpid(static_cast<pid_t>(m_pid), &status, 0);
      m_pid = -1;
    }
  }
  if (m_fd >= 0) {
    close(m_fd);
    m_fd = -1;
  }
}

} // namespace term
