#pragma once

#include <string>
#include <utility>
#include <vector>

// A child process on the other end of a pseudo-terminal
// (docs/ui/terminal-panel.md).
//
// POSIX only, and deliberately the ONLY platform-specific piece of the
// terminal: the screen and the panel above it never touch a file descriptor,
// so a future Windows port replaces this class (ConPTY) and nothing else.
//
// No background thread. The master fd is non-blocking and drained once per
// frame, which is enough at 60 Hz, keeps libvterm single-threaded, and means
// a runaway `yes` cannot outrun the UI — it just gets a bounded slice.
namespace term {

class PtySession {
public:
  struct Options {
    std::string program; // "" = $SHELL, then /bin/sh
    std::vector<std::string> args;
    std::string cwd; // "" = inherit
    std::vector<std::pair<std::string, std::string>> env;
    int rows = 24;
    int cols = 80;
  };

  PtySession() = default;
  ~PtySession();
  PtySession(const PtySession&) = delete;
  PtySession& operator=(const PtySession&) = delete;

  // Forks and execs. Returns false with a caller-grade message: "no such
  // program 'claude'" is the whole answer for a user who has not installed it.
  bool Start(const Options& opts, std::string* error);

  // True until the child exits AND has been reaped (Poll does the reaping).
  [[nodiscard]] bool Running() const { return m_pid > 0; }
  // Exit status once it has exited, or -1 while it runs. 128+n for a signal,
  // as a shell reports it.
  [[nodiscard]] int ExitStatus() const { return m_exitStatus; }

  // Drains whatever the child has written, up to `limit` bytes, and reaps it
  // if it has exited. Returns the bytes read this call.
  std::string Poll(size_t limit = 1 << 18);

  void Write(const char* bytes, size_t n);
  void Write(std::string_view s) { Write(s.data(), s.size()); }

  // Tells the child its new size (TIOCSWINSZ + SIGWINCH). Without this a
  // resized panel keeps a program drawing at the old width.
  void Resize(int rows, int cols);

  // SIGHUP, then SIGKILL if it is still there. Safe to call twice.
  void Terminate();

private:
  int m_fd = -1;
  int m_pid = -1;
  int m_exitStatus = -1;
  int m_rows = 24;
  int m_cols = 80;
};

} // namespace term
