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
// POSIX spawns with posix_spawn and a pipe; Windows runs the resolved program
// through _popen with the arguments quoted for cmd.exe.

#include <string>
#include <vector>

namespace proc {

// The full path of `name` on PATH — `name.exe` on Windows — or empty when it
// is not there. Read at every call: PATH is the environment's, not the
// build's.
std::string FindOnPath(const std::string& name);

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
             std::string* error);
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
  std::string m_stderrPath;
#if defined(_WIN32)
  void* m_pipe = nullptr; // FILE*
#else
  int m_stdin = -1;
  int m_pid = -1;
#endif
};

} // namespace proc
