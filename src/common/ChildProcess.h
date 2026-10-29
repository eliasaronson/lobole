// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Running an external program whose STANDARD INPUT we write: the motion video
// export streams raw frames into ffmpeg this way
// (docs/assemblies/README.md#motion-video).
//
// Not a shell command line. The program is found on PATH here, by us, and run
// with an argument vector — so a path with a quote or a space in it is one
// argument, and "is ffmpeg installed?" has one answer that `ping` and the
// export both read. What the child writes to stderr is kept (in a temporary
// file, so a chatty child can never fill a pipe nobody is reading and stall)
// and its TAIL is handed back, because a non-zero exit with no words is the
// refusal nobody can act on.
//
// POSIX spawns with posix_spawn and a pipe; Windows with CreateProcessW and a
// pipe, the arguments quoted for the C runtime's argv parser. Either way there
// is a process to poll and to end, so a solver's deadline and its cancel
// interrupt it on both (docs/simulation/README.md#calculix).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace proc {

// The full path of `name` on PATH — `name.exe` on Windows — or empty when it
// is not there. Read at every call: PATH is the environment's, not the
// build's.
std::string FindOnPath(const std::string& name);

// This process's id, as Pid() reports a child's.
long CurrentPid();
// Whether a process with this id is running now. A pid the system no longer
// knows is not; one that exists but this user may not inspect is.
bool ProcessAlive(long pid);

// How a child is started beyond its arguments: a solver that writes its
// files beside its input runs in a directory of its own, says what it has
// to say on stdout, and takes its thread count from its environment
// (docs/simulation/README.md#calculix).
struct ChildOptions {
  std::string cwd;        // empty: this process's
  std::string stdoutPath; // empty: /dev/null; else this file, truncated
  // Set in the child's environment, over this process's.
  std::vector<std::pair<std::string, std::string>> env;
};

class PipedChild {
public:
  PipedChild() = default;
  PipedChild(const PipedChild&) = delete;
  PipedChild& operator=(const PipedChild&) = delete;
  // A child still running is killed and reaped.
  ~PipedChild();

  // Start `program` (a full path, as FindOnPath returns) with `args`.
  // False, with `error`, when it cannot be started at all.
  bool Start(const std::string& program, const std::vector<std::string>& args,
             std::string* error, const ChildOptions& options = {});
  // Whether it has ended, without waiting: true once it has, with its exit
  // code in `code` (-1 when it died of a signal, or on Windows of an
  // exception), and it is then reaped as Finish would.
  bool Poll(int* code);
  // The process id while it runs (0 when there is none).
  long Pid() const;
  // The most memory it held at once, in bytes, once it has been reaped by
  // Poll or Finish: its peak resident set, on Windows its peak working set
  // (0 where the platform does not say, as Wine does not).
  uint64_t PeakBytes() const { return m_peakBytes; }
  // Write all of `n` bytes to its stdin. False once the child has gone away
  // (it exited, or closed its input): the caller stops and reads Finish.
  // Blocks while the child is behind; call it off the main thread.
  bool Write(const void* data, size_t n);
  // Close its stdin, wait for it, and return its exit code (-1 when it could
  // not be waited for or died of a signal). `stderrTail` gets the last few
  // lines it wrote there.
  int Finish(std::string* stderrTail);
  // Stop it now (a cancel): no more input, and the process is ended.
  void Kill();

  bool Running() const { return m_started && !m_finished; }

private:
  bool m_started = false;
  bool m_finished = false;
  int m_exitCode = -1;
  uint64_t m_peakBytes = 0;
  std::string m_stderrPath;
#if defined(_WIN32)
  void* m_stdin = nullptr;   // HANDLE: our end of its input pipe
  void* m_process = nullptr; // HANDLE
  long m_pid = 0;
  bool Reap(bool block);
#else
  int m_stdin = -1;
  int m_pid = -1;
#endif
};

} // namespace proc
