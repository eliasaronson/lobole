#include "core/io/GitVault.h"

#include "common/Logger.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

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

// Like RunGit, but folds stderr into the captured output: for a merge or a
// branch switch the REASON git refused is the message the panel shows, and
// discarding it would leave "it did not work".
int RunGitVerbose(const std::string& dir, const std::string& args,
                  std::string* output) {
  const std::string cmd = "git -C " + Quote(dir) + " " + args + " 2>&1";
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

// Trailing newline off a one-line `git` answer.
std::string FirstLine(std::string s) {
  const size_t eol = s.find('\n');
  if (eol != std::string::npos)
    s.resize(eol);
  while (!s.empty() && (s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  return s;
}

// Split newline-separated `git` output, dropping the trailing blank.
std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t eol = text.find('\n', pos);
    const std::string line =
        text.substr(pos, eol == std::string::npos ? eol : eol - pos);
    pos = eol == std::string::npos ? text.size() : eol + 1;
    if (!line.empty())
      out.push_back(line);
  }
  return out;
}

// Space-separated token count of one line.
int CountTokens(const std::string& line) {
  int n = 0;
  size_t pos = 0;
  while (pos < line.size()) {
    while (pos < line.size() && line[pos] == ' ')
      ++pos;
    if (pos >= line.size())
      break;
    ++n;
    while (pos < line.size() && line[pos] != ' ')
      ++pos;
  }
  return n;
}

// The last-resort identity: commits must work on CI and on a fresh machine
// with no git config at all. Only used for a field nothing else supplies.
constexpr const char* kFallbackName = "Designer";
constexpr const char* kFallbackEmail = "designer@local";

// Set from Preferences (F9 C1). Empty = the person expressed no preference.
std::string g_preferredName;
std::string g_preferredEmail;

// Does git already know this? `config <key>` exits non-zero when unset, and
// checks the repo config then the global one — which is exactly the order a
// person would expect.
bool GitKnows(const std::string& dir, const char* key, std::string* value) {
  std::string out;
  if (RunGit(dir, std::string("config ") + key, &out) != 0)
    return false;
  const std::string trimmed = FirstLine(out);
  if (trimmed.empty())
    return false;
  if (value)
    *value = trimmed;
  return true;
}

// `-c user.name=... -c user.email=...` for ONLY the fields git lacks, so a
// person who set just one keeps it.
std::string IdentityArgs(const std::string& dir) {
  std::string args;
  if (!GitKnows(dir, "user.name", nullptr)) {
    args += " -c user.name=" +
            Quote(g_preferredName.empty() ? kFallbackName : g_preferredName);
  }
  if (!GitKnows(dir, "user.email", nullptr)) {
    args += " -c user.email=" +
            Quote(g_preferredEmail.empty() ? kFallbackEmail : g_preferredEmail);
  }
  return args;
}

} // namespace

void SetPreferredIdentity(const std::string& name, const std::string& email) {
  g_preferredName = name;
  g_preferredEmail = email;
}

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
  if (RunGit(dir, IdentityArgs(dir) + " commit -q" +
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

// ---------------------------------------------------------------------------
// Branch + merge (F7)
// ---------------------------------------------------------------------------

std::vector<std::string> Branches(const std::string& dir) {
  std::vector<std::string> out;
  if (!Available() || !IsRepo(dir))
    return out;
  std::string text;
  // `--sort=-HEAD` puts the checked-out branch first; the rest keep git's
  // own order, which is alphabetical.
  // The format MUST be quoted: `%(refname:short)` goes through a shell, and
  // bare parens are a syntax error there, not a format string.
  if (RunGit(dir,
             "for-each-ref --sort=-HEAD --format=" + Quote("%(refname:short)") +
                 " refs/heads/",
             &text) != 0)
    return out;
  return Lines(text);
}

std::string CurrentBranch(const std::string& dir) {
  if (!Available() || !IsRepo(dir))
    return "";
  std::string text;
  if (RunGit(dir, "symbolic-ref --quiet --short HEAD", &text) != 0)
    return ""; // detached, or no commit yet
  return FirstLine(text);
}

bool CreateBranch(const std::string& dir, const std::string& name,
                  bool switchTo, std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return false;
  }
  std::string out;
  if (RunGitVerbose(dir, "branch " + Quote(name), &out) != 0) {
    if (error)
      *error = out.empty() ? "could not create that branch" : FirstLine(out);
    return false;
  }
  return switchTo ? SwitchBranch(dir, name, error) : true;
}

bool SwitchBranch(const std::string& dir, const std::string& name,
                  std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return false;
  }
  // Refuse on a dirty tree rather than letting git decide: it carries
  // changes across when it can and refuses when it cannot, and neither is
  // what someone thinking in "versions" expects. Save first.
  if (Dirty(dir)) {
    if (error)
      *error = "save the project before switching branch";
    return false;
  }
  std::string out;
  if (RunGitVerbose(dir, "checkout -q " + Quote(name), &out) != 0) {
    if (error)
      *error = out.empty() ? "could not switch branch" : FirstLine(out);
    return false;
  }
  return true;
}

MergeStatus Merge(const std::string& dir, const std::string& branch,
                  std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return MergeStatus::Failed;
  }
  if (Dirty(dir)) {
    if (error)
      *error = "save the project before merging";
    return MergeStatus::Failed;
  }
  std::string headBefore;
  RunGit(dir, "rev-parse HEAD", &headBefore);
  const std::string before = FirstLine(headBefore);
  std::string out;
  const int code = RunGitVerbose(
      dir, IdentityArgs(dir) + " merge --no-edit " + Quote(branch), &out);
  if (code == 0) {
    std::string after;
    RunGit(dir, "rev-parse HEAD", &after);
    if (FirstLine(after) == before)
      return MergeStatus::UpToDate;
    // A fast-forward moves HEAD to an existing commit; a real merge makes a
    // commit with TWO parents. `rev-list --parents -n 1` prints
    // "<commit> <parent>..." on one line, so three tokens means a merge.
    std::string parents;
    RunGit(dir, "rev-list --parents -n 1 HEAD", &parents);
    return CountTokens(FirstLine(parents)) >= 3 ? MergeStatus::Merged
                                                : MergeStatus::FastForward;
  }
  // Exit 1 with unmerged paths is the conflict we came for, not a failure.
  if (InMerge(dir) && !Unmerged(dir).empty())
    return MergeStatus::Conflicted;
  if (error)
    *error = out.empty() ? "git merge failed" : FirstLine(out);
  return MergeStatus::Failed;
}

bool InMerge(const std::string& dir) {
  if (!Available() || !IsRepo(dir))
    return false;
  std::error_code ec;
  return std::filesystem::exists(
      std::filesystem::path(dir) / ".git" / "MERGE_HEAD", ec);
}

std::vector<std::string> Unmerged(const std::string& dir) {
  std::vector<std::string> out;
  if (!Available() || !IsRepo(dir))
    return out;
  std::string text;
  if (RunGit(dir, "diff --name-only --diff-filter=U", &text) != 0)
    return out;
  return Lines(text);
}

bool ShowFile(const std::string& dir, const std::string& rev,
              const std::string& path, std::string* out) {
  if (!Available() || !IsRepo(dir))
    return false;
  std::string text;
  if (RunGit(dir, "show " + Quote(rev + ":" + path), &text) != 0)
    return false;
  if (out)
    *out = text;
  return true;
}

bool ReadStages(const std::string& dir, const std::string& path, Stages* out) {
  if (!out || !Available() || !IsRepo(dir))
    return false;
  *out = Stages{};
  // ":1:" base, ":2:" ours, ":3:" theirs — a stage git did not record (an
  // add/add has no base) simply fails, which is how absence is detected.
  out->hasBase = ShowFile(dir, ":1", path, &out->base);
  out->hasOurs = ShowFile(dir, ":2", path, &out->ours);
  out->hasTheirs = ShowFile(dir, ":3", path, &out->theirs);
  return out->hasBase || out->hasOurs || out->hasTheirs;
}

std::vector<std::string> ListTree(const std::string& dir,
                                  const std::string& rev) {
  std::vector<std::string> out;
  if (!Available() || !IsRepo(dir))
    return out;
  std::string text;
  if (RunGit(dir, "ls-tree -r --name-only " + Quote(rev), &text) != 0)
    return out;
  return Lines(text);
}

std::vector<std::string> Parents(const std::string& dir,
                                 const std::string& rev) {
  std::vector<std::string> out;
  if (!Available() || !IsRepo(dir))
    return out;
  std::string text;
  if (RunGit(dir, "rev-list --parents -n 1 " + Quote(rev), &text) != 0)
    return out;
  // "<commit> <parent>..." on one line — the first token is the commit.
  const std::string line = FirstLine(text);
  size_t pos = 0;
  bool first = true;
  while (pos < line.size()) {
    while (pos < line.size() && line[pos] == ' ')
      ++pos;
    const size_t start = pos;
    while (pos < line.size() && line[pos] != ' ')
      ++pos;
    if (pos > start) {
      if (first)
        first = false;
      else
        out.push_back(line.substr(start, pos - start));
    }
  }
  return out;
}

std::string MergeBase(const std::string& dir, const std::string& a,
                      const std::string& b) {
  if (!Available() || !IsRepo(dir))
    return "";
  std::string text;
  if (RunGit(dir, "merge-base " + Quote(a) + " " + Quote(b), &text) != 0)
    return "";
  return FirstLine(text);
}

bool ResolveWithContent(const std::string& dir, const std::string& path,
                        const std::string& content, std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return false;
  }
  const std::filesystem::path full = std::filesystem::path(dir) / path;
  std::error_code ec;
  std::filesystem::create_directories(full.parent_path(), ec);
  {
    std::ofstream file(full, std::ios::binary | std::ios::trunc);
    file << content;
    if (!file) {
      if (error)
        *error = "could not write " + path;
      return false;
    }
  }
  if (RunGit(dir, "add -- " + Quote(path)) != 0) {
    if (error)
      *error = "could not stage " + path;
    return false;
  }
  return true;
}

bool ResolveAsDeleted(const std::string& dir, const std::string& path,
                      std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return false;
  }
  // `--ignore-unmatch`: the file may already be absent from the working tree
  // (the side that deleted it is checked out), and removing it again is not
  // an error.
  if (RunGit(dir, "rm -q -f --ignore-unmatch -- " + Quote(path)) != 0) {
    if (error)
      *error = "could not remove " + path;
    return false;
  }
  return true;
}

bool ResolveWith(const std::string& dir, const std::string& path,
                 MergeSide side, std::string* error) {
  Stages stages;
  if (!ReadStages(dir, path, &stages)) {
    if (error)
      *error = path + " is not a conflicted file";
    return false;
  }
  const bool has = side == MergeSide::Ours ? stages.hasOurs : stages.hasTheirs;
  // "That side deleted it" is a real answer, and `git checkout --ours` would
  // have failed here rather than expressing it.
  if (!has)
    return ResolveAsDeleted(dir, path, error);
  return ResolveWithContent(
      dir, path, side == MergeSide::Ours ? stages.ours : stages.theirs, error);
}

bool AbortMerge(const std::string& dir, std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return false;
  }
  std::string out;
  if (RunGitVerbose(dir, "merge --abort", &out) != 0) {
    if (error)
      *error = out.empty() ? "could not abort the merge" : FirstLine(out);
    return false;
  }
  return true;
}

bool CommitMerge(const std::string& dir, const std::string& message,
                 std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return false;
  }
  // The guard that stops a half-resolved tree being committed as finished.
  const auto left = Unmerged(dir);
  if (!left.empty()) {
    if (error)
      *error = std::to_string(left.size()) +
               (left.size() == 1 ? " conflict is" : " conflicts are") +
               " still unresolved";
    return false;
  }
  if (RunGit(dir, "add -A") != 0) {
    if (error)
      *error = "git add failed";
    return false;
  }
  std::string out;
  if (RunGitVerbose(
          dir, IdentityArgs(dir) + " commit -q --no-edit -m " + Quote(message),
          &out) != 0) {
    if (error)
      *error = out.empty() ? "git commit failed" : FirstLine(out);
    return false;
  }
  return true;
}

} // namespace GitVault
