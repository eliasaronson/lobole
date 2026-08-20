#include "core/io/GitVault.h"

#include "common/Logger.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
// _pclose returns the child exit code directly; cmd.exe's null device is "nul".
#define NULDEV "nul"
#define GIT_EXIT_STATUS(s) (s)
#else
#include <sys/wait.h>
#define NULDEV "/dev/null"
#define GIT_EXIT_STATUS(s) WEXITSTATUS(s)
#endif

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
  const std::string cmd = "git -C " + Quote(dir) + " " + args + " 2>" NULDEV;
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
  return status == -1 ? -1 : GIT_EXIT_STATUS(status);
}

// Inline identity: commits must work on CI and fresh machines with no
// global git config.
constexpr const char* kIdentity =
    "-c user.name=Designer -c user.email=designer@local";

} // namespace

bool Available() {
  static const bool available = [] {
    return std::system("git --version >" NULDEV " 2>&1") == 0;
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
               std::string* error, bool allowEmpty) {
  if (!Available()) {
    if (error)
      *error = "git is not available";
    return false;
  }
  if (RunGit(dir, "add -A") != 0) {
    if (error)
      *error = "git add failed";
    return false;
  }
  std::string status;
  RunGit(dir, "status --porcelain", &status);
  if (status.empty() && !allowEmpty)
    return true; // nothing changed — a clean save is not an error
  if (RunGit(dir, std::string(kIdentity) + " commit -q" +
                      (allowEmpty ? " --allow-empty" : "") + " -m " +
                      Quote(message)) != 0) {
    if (error)
      *error = "git commit failed";
    return false;
  }
  return true;
}

bool Dirty(const std::string& dir) {
  if (!Available() || !IsRepo(dir))
    return false;
  std::string status;
  if (RunGit(dir, "status --porcelain", &status) != 0)
    return false;
  return !status.empty();
}

std::string TagRef(const std::string& name) {
  // Keep what git-check-ref-format allows and a person would recognise;
  // everything else (spaces above all) becomes '-', with runs collapsed.
  std::string slug;
  for (char c : name) {
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (keep)
      slug += c;
    else if (!slug.empty() && slug.back() != '-')
      slug += '-';
  }
  while (!slug.empty() && slug.back() == '-')
    slug.pop_back();
  return slug.empty() ? "" : "cp/" + slug;
}

bool Tag(const std::string& dir, const std::string& name) {
  const std::string ref = TagRef(name);
  return Available() && !ref.empty() &&
         RunGit(dir, "tag -f " + Quote(ref)) == 0;
}

namespace {

// `%D` decorations -> "is one of the tags ours?". Git formats the field as
// "HEAD -> main, tag: cp/before-hinges, origin/main". Only a "cp/" tag marks
// a named version; the name itself is the commit subject (see TagRef).
bool HasVersionTag(const std::string& decorations) {
  size_t pos = 0;
  while ((pos = decorations.find("tag: ", pos)) != std::string::npos) {
    pos += 5;
    if (decorations.compare(pos, 3, "cp/") == 0)
      return true;
  }
  return false;
}

} // namespace

std::vector<LogEntry> Log(const std::string& dir, int limit) {
  std::vector<LogEntry> out;
  if (!Available() || !IsRepo(dir))
    return out;
  std::string text;
  // Subject stays LAST: it is the only field that can contain a tab.
  if (RunGit(dir,
             "log -n " + std::to_string(limit) +
                 " --pretty=format:%H%x09%at%x09%D%x09%s",
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
    const size_t t3 = line.find('\t', t2 + 1);
    if (t1 == std::string::npos || t2 == std::string::npos ||
        t3 == std::string::npos)
      continue;
    out.push_back({line.substr(0, t1), line.substr(t3 + 1),
                   std::stoll(line.substr(t1 + 1, t2 - t1 - 1)),
                   HasVersionTag(line.substr(t2 + 1, t3 - t2 - 1))});
  }
  return out;
}

bool RestoreTo(const std::string& dir, const std::string& commit,
               std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return false;
  }
  // `read-tree --reset -u`, NOT `checkout <commit> -- .`: checkout restores
  // the paths that commit HAS and leaves every other tracked file in place,
  // so restoring past the creation of a part left that part on disk and the
  // reopened project still listed it. read-tree sets index + working tree to
  // the tree and deletes what is not in it, without moving HEAD.
  if (RunGit(dir, "read-tree --reset -u " + Quote(commit)) != 0) {
    if (error)
      *error = "git read-tree failed (unknown commit?)";
    return false;
  }
  return true;
}

} // namespace GitVault
