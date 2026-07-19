#include "core/io/GitVault.h"

#include "Logger.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace GitVault {

namespace {

// POSIX-shell single-quote escaping: ' -> '\'' inside a quoted string.
std::string Quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

// Runs `git -C <dir> <args>`, capturing stdout. Returns exit code.
int RunGit(const std::string& dir, const std::string& args,
           std::string* output = nullptr) {
  const std::string cmd =
      "git -C " + Quote(dir) + " " + args + " 2>/dev/null";
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (!pipe)
    return -1;
  std::array<char, 4096> chunk{};
  std::string captured;
  size_t n = 0;
  while ((n = ::fread(chunk.data(), 1, chunk.size(), pipe)) > 0)
    captured.append(chunk.data(), n);
  const int status = ::pclose(pipe);
  if (output)
    *output = captured;
  return status == -1 ? -1 : WEXITSTATUS(status);
}

// Inline identity: commits must work on CI and fresh machines with no
// global git config.
constexpr const char* kIdentity =
    "-c user.name=Designer -c user.email=designer@local";

} // namespace

bool Available() {
  static const bool available = [] {
    return std::system("git --version >/dev/null 2>&1") == 0;
  }();
  return available;
}

bool IsRepo(const std::string& dir) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(dir) / ".git", ec);
}

bool Init(const std::string& dir) {
  if (!Available())
    return false;
  const bool ok = RunGit(dir, "init -q") == 0;
  if (ok)
    LOG_INFO("Initialized git repository in {}", dir);
  return ok;
}

bool CommitAll(const std::string& dir, const std::string& message,
               std::string* error) {
  if (!Available()) {
    if (error) *error = "git is not available";
    return false;
  }
  if (RunGit(dir, "add -A") != 0) {
    if (error) *error = "git add failed";
    return false;
  }
  std::string status;
  RunGit(dir, "status --porcelain", &status);
  if (status.empty())
    return true; // nothing changed — a clean save is not an error
  if (RunGit(dir, std::string(kIdentity) + " commit -q -m " +
                      Quote(message)) != 0) {
    if (error) *error = "git commit failed";
    return false;
  }
  return true;
}

bool Tag(const std::string& dir, const std::string& name) {
  return Available() && RunGit(dir, "tag -f " + Quote(name)) == 0;
}

std::vector<LogEntry> Log(const std::string& dir, int limit) {
  std::vector<LogEntry> out;
  if (!Available() || !IsRepo(dir))
    return out;
  std::string text;
  if (RunGit(dir,
             "log -n " + std::to_string(limit) +
                 " --pretty=format:%H%x09%at%x09%s",
             &text) != 0)
    return out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t eol = text.find('\n', pos);
    const std::string line =
        text.substr(pos, eol == std::string::npos ? eol : eol - pos);
    pos = eol == std::string::npos ? text.size() : eol + 1;
    const size_t t1 = line.find('\t');
    const size_t t2 = line.find('\t', t1 + 1);
    if (t1 == std::string::npos || t2 == std::string::npos)
      continue;
    out.push_back({line.substr(0, t1),
                   line.substr(t2 + 1),
                   std::stoll(line.substr(t1 + 1, t2 - t1 - 1))});
  }
  return out;
}

bool RestoreTo(const std::string& dir, const std::string& commit,
               std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error) *error = "not a git project";
    return false;
  }
  if (RunGit(dir, "checkout -q " + Quote(commit) + " -- .") != 0) {
    if (error) *error = "git checkout failed (unknown commit?)";
    return false;
  }
  return true;
}

} // namespace GitVault
