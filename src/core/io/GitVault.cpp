// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/GitVault.h"

#include "common/Logger.h"
#include "core/io/ProjectMerge.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include <cctype>
#include <map>
#include <mutex>
#include <set>

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

// Every git call starts with these: the two settings through which a
// repository's own config runs a command on the commands this app uses
// (diff, status, add, commit, merge), switched off whatever it says, and the
// `ext::` transport, whose URL is a command, refused even where a person's
// own config allows it. The rest of a repository's config is checked before
// git runs at all (UntrustedReason).
constexpr const char* kSafe =
    "-c core.fsmonitor=false -c core.hooksPath=" NULDEV
    " -c protocol.ext.allow=never";

// Git runs only in a project folder holding a repository this app accepts:
// never in one it does not, and never in a parent folder's, which git would
// otherwise find by searching upward (a project unpacked inside a crafted
// folder). `init` is the one command for a folder with none.
bool MayRun(const std::string& dir, const std::string& args,
            std::string* output) {
  if (args.starts_with("init"))
    return true;
  std::error_code ec;
  const std::string why =
      std::filesystem::is_directory(std::filesystem::path(dir) / ".git", ec)
          ? UntrustedReason(dir)
          : std::string("no repository here");
  if (why.empty())
    return true;
  if (output)
    *output = why;
  return false;
}

// Runs `git -C <dir> <args>`, capturing stdout. Returns exit code.
int RunGit(const std::string& dir, const std::string& args,
           std::string* output = nullptr) {
  if (!MayRun(dir, args, nullptr))
    return 128;
  const std::string cmd =
      "git -C " + Quote(dir) + " " + kSafe + " " + args + " 2>" NULDEV;
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
  if (!MayRun(dir, args, output))
    return 128;
  const std::string cmd =
      "git -C " + Quote(dir) + " " + kSafe + " " + args + " 2>&1";
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

// Runs a NETWORK git command. Two differences from RunGitVerbose, both about
// never hanging a frame:
//   GIT_TERMINAL_PROMPT=0  git asks on the terminal for a username/password
//                          when it has no credential helper. There is no
//                          terminal here, so without this the child waits
//                          forever and the app waits with it.
//   GIT_ASKPASS / SSH_ASKPASS empty + SSH BatchMode
//                          the same for the graphical and ssh prompts.
// The result is that unconfigured auth FAILS, quickly, with git's own message
// — which someone who understands git can act on.
int RunGitNetwork(const std::string& dir, const std::string& args,
                  std::string* output) {
  // A clone runs from the parent folder, into a new one; everything else in
  // the project's own repository.
  if (!args.starts_with("clone") && !MayRun(dir, args, output))
    return 128;
  const std::string cmd = "GIT_TERMINAL_PROMPT=0 GIT_ASKPASS= SSH_ASKPASS= "
                          "GIT_SSH_COMMAND='ssh -o BatchMode=yes' "
                          "git -C " +
                          Quote(dir) + " " + kSafe + " " + args + " 2>&1";
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

// The last-resort identity: commits must work on CI and on a fresh machine
// with no git config at all. Only used for a field nothing else supplies.
constexpr const char* kFallbackName = "Lobole";
constexpr const char* kFallbackEmail = "lobole@local";

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

// A saved study or motion result stays out of the history
// (docs/versioning/README.md#saved-results-stay-out-of-the-history). It is
// reproducible from its inputs and named by their hash, so a version needs
// none; and a few runs of a big study would hold gigabytes in a history that
// keeps every blob for good. The newest result of each study is the one in
// the working tree, which git leaves alone: restoring a version keeps it,
// and the reopen reads it current when the restored inputs hash the same,
// stale when they do not. The rule goes in the repository's own exclude
// file, and a result an earlier build committed leaves the index here.
constexpr const char* kResultsExcluded =
    "# Written by Lobole: saved study results are derived (their input "
    "hash names them)";
// The study and motion results (v153), and the drawing views' saved
// projections (v162): derived, each named by its input hash.
constexpr const char* kDerivedPatterns[] = {
    "*.result.json", "*.result.bin", "*.projection.json", "*.projection.bin"};

void KeepResultsOutOfHistory(const std::string& dir) {
  std::string out;
  if (RunGit(dir, "rev-parse --git-path info/exclude", &out) != 0)
    return;
  std::filesystem::path exclude = FirstLine(out);
  if (exclude.empty())
    return;
  if (exclude.is_relative())
    exclude = std::filesystem::path(dir) / exclude;
  std::string current;
  {
    std::ifstream in(exclude);
    if (in)
      current.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
  }
  // Each pattern on its own line, added when that line is missing: a
  // repository whose exclude file an earlier build wrote has the study
  // results' lines and not the drawing projections' (v162).
  std::string missing;
  if (current.find(kResultsExcluded) == std::string::npos)
    missing += std::string(kResultsExcluded) + "\n";
  for (const char* pattern : kDerivedPatterns)
    if (current.find(std::string("\n") + pattern + "\n") == std::string::npos &&
        current.rfind(std::string(pattern) + "\n", 0) != 0)
      missing += std::string(pattern) + "\n";
  if (!missing.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(exclude.parent_path(), ec);
    std::ofstream app(exclude, std::ios::app);
    if (!current.empty() && current.back() != '\n')
      app << "\n";
    app << missing;
  }
  std::string untrack = "rm -r -q --cached --ignore-unmatch --";
  for (const char* pattern : kDerivedPatterns)
    untrack += " " + Quote(pattern);
  RunGit(dir, untrack);
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

namespace {

// The local config keys this app writes, git writes for it (init, clone,
// remote add, a push that sets an upstream) or a person plausibly set by hand
// — none of which makes the commands this app runs execute anything. A name
// is matched whole, or by the prefix before its first or last dot.
bool HarmlessConfigKey(const std::string& key) {
  static const std::set<std::string> kExact = {"core.repositoryformatversion",
                                               "core.filemode",
                                               "core.bare",
                                               "core.logallrefupdates",
                                               "core.ignorecase",
                                               "core.precomposeunicode",
                                               "core.symlinks",
                                               "core.autocrlf",
                                               "core.safecrlf",
                                               "core.eol",
                                               "core.quotepath",
                                               "core.untrackedcache",
                                               "core.splitindex",
                                               "core.commitgraph",
                                               "core.multipackindex",
                                               "core.compression",
                                               "pull.rebase",
                                               "pull.ff",
                                               "push.default",
                                               "push.autosetupremote",
                                               "merge.ff",
                                               "merge.conflictstyle",
                                               "fetch.prune",
                                               "fetch.fsckobjects",
                                               "transfer.fsckobjects",
                                               "receive.fsckobjects",
                                               "gc.auto",
                                               "commit.gpgsign",
                                               "tag.gpgsign",
                                               "gpg.format",
                                               "init.defaultbranch",
                                               "index.version",
                                               "feature.manyfiles"};
  if (kExact.contains(key))
    return true;
  const size_t first = key.find('.');
  const size_t last = key.rfind('.');
  const std::string section = key.substr(0, first);
  const std::string variable = key.substr(last + 1);
  if (section == "user" || section == "extensions" || section == "lfs" ||
      section == "author" || section == "committer")
    return true;
  // remote.<name>.<variable>, branch.<name>.<variable>: the subsection is a
  // name; the variable decides (`remote.x.uploadpack` runs a program).
  if (section == "remote")
    return variable == "url" || variable == "pushurl" || variable == "fetch" ||
           variable == "push" || variable == "tagopt" || variable == "prune" ||
           variable == "mirror";
  if (section == "branch")
    return variable == "remote" || variable == "merge" ||
           variable == "rebase" || variable == "pushremote" ||
           variable == "description";
  return false;
}

struct TrustCache {
  std::mutex mutex;
  // dir -> (the config file's write time, the answer).
  std::map<std::string, std::pair<std::filesystem::file_time_type, std::string>>
      answers;
};

TrustCache& Trust() {
  static TrustCache cache;
  return cache;
}

} // namespace

std::string UntrustedReason(const std::string& dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path gitPath = fs::path(dir) / ".git";
  if (!fs::exists(gitPath, ec))
    return {};
  if (!fs::is_directory(gitPath, ec))
    return "its .git is a file, which points git at another repository";
  const fs::path config = gitPath / "config";
  const auto stamp = fs::last_write_time(config, ec);
  {
    std::lock_guard<std::mutex> lock(Trust().mutex);
    auto it = Trust().answers.find(dir);
    if (it != Trust().answers.end() && it->second.first == stamp)
      return it->second.second;
  }
  // `--file` reads that one file as text: no include is followed, nothing
  // runs, and no repository is set up.
  std::string why;
  const std::string cmd = "git config --file " + Quote(config.string()) +
                          " --name-only --list 2>" NULDEV;
  FILE* pipe = fs::exists(config, ec) ? ::popen(cmd.c_str(), "r") : nullptr;
  if (pipe) {
    std::array<char, 4096> chunk{};
    std::string keys;
    size_t n = 0;
    while ((n = ::fread(chunk.data(), 1, chunk.size(), pipe)) > 0)
      keys.append(chunk.data(), n);
    if (::pclose(pipe) != 0)
      why = "its .git/config cannot be read";
    for (std::string key : Lines(keys)) {
      if (!why.empty())
        break;
      std::transform(key.begin(), key.end(), key.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (!HarmlessConfigKey(key))
        why = "its .git/config sets '" + key +
              "', which can make git run a command; remove it with `git "
              "config --local --unset " +
              key + "` if you trust it";
    }
  }
  if (!why.empty())
    LOG_WARN("Not using the history in {}: {}", dir, why);
  std::lock_guard<std::mutex> lock(Trust().mutex);
  Trust().answers[dir] = {stamp, why};
  return why;
}

bool IsRepo(const std::string& dir) {
  std::error_code ec;
  return std::filesystem::is_directory(std::filesystem::path(dir) / ".git",
                                       ec) &&
         UntrustedReason(dir).empty();
}

bool Init(const std::string& dir) {
  if (!Available())
    return false;
  std::error_code ec;
  if (std::filesystem::exists(std::filesystem::path(dir) / ".git", ec))
    return false; // reinitializing would keep, and then use, its config
  const bool ok = RunGit(dir, "init -q") == 0;
  if (ok) {
    KeepResultsOutOfHistory(dir);
    LOG_INFO("Initialized git repository in {}", dir);
  }
  return ok;
}

bool CommitAll(const std::string& dir, const std::string& message,
               std::string* error, bool allowEmpty) {
  if (!Available()) {
    if (error)
      *error = "git is not available";
    return false;
  }
  KeepResultsOutOfHistory(dir);
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
  const std::string base = TagRef(name);
  if (!Available() || base.empty())
    return false;
  // NOT `tag -f`. Two people naming a version the same thing is ordinary —
  // "before the hinge rework" is what anyone would call it — and forcing
  // would silently move theirs onto our commit, locally and then on push.
  //
  // F6 already separated the two halves this needs: the NAME is the commit
  // subject, and the ref is only an address. So on collision we disambiguate
  // the ADDRESS and leave the person's prose alone; both names survive,
  // each pointing at its own commit.
  // ANNOTATED, not lightweight. Two reasons: `push --follow-tags` carries
  // annotated tags along with their commits and silently ignores lightweight
  // ones, so a name would never reach a teammate; and an annotated tag
  // records who named the version and when, which is the same question C1
  // answers for commits. The message is the person's prose.
  const std::string annotate =
      IdentityArgs(dir) + " tag -a -m " + Quote(name) + " --end-of-options ";
  if (RunGit(dir, annotate + Quote(base)) == 0)
    return true;
  std::string head;
  if (RunGit(dir, "rev-parse --short HEAD", &head) != 0)
    return false;
  const std::string unique = base + "-" + FirstLine(head);
  // Already ours (naming the same state twice) is success, not a collision.
  std::string existing;
  if (RunGit(dir, "rev-parse --end-of-options " + Quote(unique), &existing) ==
      0)
    return true;
  return RunGit(dir, annotate + Quote(unique)) == 0;
}

namespace {

// `%D` decorations -> the "cp/" ref on this commit, or "". Git formats the
// field as "HEAD -> main, tag: cp/before-hinges, origin/main". Only a "cp/"
// tag marks a named version; the name itself is the commit subject (TagRef).
std::string VersionTagRef(const std::string& decorations) {
  size_t pos = 0;
  while ((pos = decorations.find("tag: ", pos)) != std::string::npos) {
    pos += 5;
    if (decorations.compare(pos, 3, "cp/") != 0)
      continue;
    const size_t end = decorations.find_first_of(",", pos);
    std::string ref =
        decorations.substr(pos, end == std::string::npos ? end : end - pos);
    while (!ref.empty() && (ref.back() == ' ' || ref.back() == '\r'))
      ref.pop_back();
    return ref;
  }
  return "";
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
    // By NAME, not position: a braced list here re-binds every field after
    // any member later added to LogEntry, silently.
    LogEntry entry;
    entry.hash = line.substr(0, t1);
    entry.subject = line.substr(t3 + 1);
    entry.unixTime = std::stoll(line.substr(t1 + 1, t2 - t1 - 1));
    entry.tagRef = VersionTagRef(line.substr(t2 + 1, t3 - t2 - 1));
    entry.named = !entry.tagRef.empty();
    out.push_back(std::move(entry));
  }
  return out;
}

std::vector<LogEntry> LogWithChanges(const std::string& dir, int limit,
                                     int maxNamed) {
  std::vector<LogEntry> out = Log(dir, limit);
  if (out.empty())
    return out;

  // One process for the page. `-m --first-parent` makes a merge commit report
  // its files too, rather than silently listing nothing.
  std::string text;
  if (RunGit(
          dir,
          "log -n " + std::to_string(limit) +
              " --first-parent --name-only --pretty=format:" + Quote("\x01%H"),
          &text) != 0)
    return out;

  std::map<std::string, std::vector<std::string>> byCommit;
  std::string current;
  for (const std::string& line : Lines(text)) {
    if (!line.empty() && line[0] == '\x01') {
      current = line.substr(1);
      continue;
    }
    if (!current.empty())
      byCommit[current].push_back(line);
  }

  for (LogEntry& entry : out) {
    const auto it = byCommit.find(entry.hash);
    if (it == byCommit.end())
      continue;
    std::string kind, name;
    // Worth one more process only when a single item changed — then the row
    // can say WHICH extrude instead of "1 feature" — and only for the rows
    // someone is likely to be looking at.
    if (maxNamed > 0 && ProjectMerge::IsSingleItem(it->second)) {
      maxNamed--;
      std::string blob;
      if (ShowFile(dir, entry.hash, it->second[0], &blob)) {
        try {
          const nlohmann::json item = nlohmann::json::parse(blob);
          kind = item.value("type", "");
          name = item.value("name", "");
        } catch (const std::exception&) {
          // Not an item we can read; the path-only summary still applies.
        }
      }
    }
    entry.changed = ProjectMerge::SummarizePaths(it->second, kind, name);
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
  if (RunGit(dir, "read-tree --reset -u --end-of-options " + Quote(commit)) !=
      0) {
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
  if (RunGitVerbose(dir, "branch --end-of-options " + Quote(name), &out) != 0) {
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
  // `switch`, not `checkout`: before git 2.44 checkout reads
  // `--end-of-options` as a path.
  if (RunGitVerbose(dir, "switch -q --end-of-options " + Quote(name), &out) !=
      0) {
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
      dir,
      IdentityArgs(dir) + " merge --no-edit --end-of-options " + Quote(branch),
      &out);
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
  if (RunGit(dir, "show --end-of-options " + Quote(rev + ":" + path), &text) !=
      0)
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

bool DiffNames(const std::string& dir, const std::string& from,
               const std::string& to, std::string* out) {
  if (!Available() || !IsRepo(dir))
    return false;
  std::string args = "diff --name-status --end-of-options " + Quote(from);
  if (!to.empty())
    args += " " + Quote(to);
  return RunGit(dir, args, out) == 0;
}

std::vector<std::string> ListTree(const std::string& dir,
                                  const std::string& rev) {
  std::vector<std::string> out;
  if (!Available() || !IsRepo(dir))
    return out;
  std::string text;
  if (RunGit(dir, "ls-tree -r --name-only --end-of-options " + Quote(rev),
             &text) != 0)
    return out;
  return Lines(text);
}

std::vector<std::string> Parents(const std::string& dir,
                                 const std::string& rev) {
  std::vector<std::string> out;
  if (!Available() || !IsRepo(dir))
    return out;
  std::string text;
  if (RunGit(dir, "rev-list --parents -n 1 --end-of-options " + Quote(rev),
             &text) != 0)
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

bool ExtractSubtree(const std::string& dir, const std::string& rev,
                    const std::string& subdir, const std::string& destDir,
                    std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return false;
  }
  std::error_code ec;
  const std::filesystem::path dest(destDir);
  if (std::filesystem::exists(dest, ec)) {
    if (error)
      *error = "there is already something at " + destDir;
    return false;
  }
  const std::string prefix = subdir.empty() ? "" : subdir + "/";
  int written = 0;
  for (const std::string& path : ListTree(dir, rev)) {
    if (!prefix.empty() && path.rfind(prefix, 0) != 0)
      continue;
    std::string blob;
    if (!ShowFile(dir, rev, path, &blob))
      continue;
    // A tree from someone else's history can name `../x` or an absolute
    // path; git's own checks are off by default (transfer.fsckObjects).
    const std::filesystem::path rel(path.substr(prefix.size()));
    if (rel.empty() || rel.is_absolute() || rel.has_root_name() ||
        std::any_of(rel.begin(), rel.end(), [](const auto& part) {
          return part == ".." || part == ".";
        }))
      continue;
    const std::filesystem::path out = dest / rel;
    std::filesystem::create_directories(out.parent_path(), ec);
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    file << blob;
    if (!file) {
      if (error)
        *error = "could not write " + out.string();
      return false;
    }
    written++;
  }
  if (written == 0) {
    if (error)
      *error = subdir.empty() ? "that version is empty"
                              : subdir + " did not exist in that version";
    return false;
  }
  return true;
}

std::string MergeBase(const std::string& dir, const std::string& a,
                      const std::string& b) {
  if (!Available() || !IsRepo(dir))
    return "";
  std::string text;
  if (RunGit(dir, "merge-base --end-of-options " + Quote(a) + " " + Quote(b),
             &text) != 0)
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
  KeepResultsOutOfHistory(dir);
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

// ---------------------------------------------------------------------------
// Remotes (F9 C2)
// ---------------------------------------------------------------------------

std::vector<Remote> Remotes(const std::string& dir) {
  std::vector<Remote> out;
  if (!Available() || !IsRepo(dir))
    return out;
  std::string text;
  if (RunGit(dir, "remote -v", &text) != 0)
    return out;
  // "origin\tURL (fetch)" / "origin\tURL (push)" — the fetch line is the one
  // that matters, and listing each remote twice would be a UI bug.
  for (const std::string& line : Lines(text)) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos)
      continue;
    const size_t space = line.find(' ', tab);
    if (space == std::string::npos ||
        line.compare(space + 1, 7, "(fetch)") != 0)
      continue;
    out.push_back({line.substr(0, tab), line.substr(tab + 1, space - tab - 1)});
  }
  return out;
}

// A remote's name, a URL and a branch are typed, pasted or read from a
// repository's config, and one starting with `-` would be read as an option
// (`--upload-pack=<command>`). Each follows `--`, as a revision follows
// `--end-of-options` everywhere above.
bool AddRemote(const std::string& dir, const std::string& name,
               const std::string& url, std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return false;
  }
  std::string out;
  if (RunGitVerbose(dir, "remote add -- " + Quote(name) + " " + Quote(url),
                    &out) != 0) {
    if (error)
      *error = out.empty() ? "could not add that remote" : FirstLine(out);
    return false;
  }
  return true;
}

bool RemoveRemote(const std::string& dir, const std::string& name,
                  std::string* error) {
  std::string out;
  if (!Available() || !IsRepo(dir) ||
      RunGitVerbose(dir, "remote remove -- " + Quote(name), &out) != 0) {
    if (error)
      *error = out.empty() ? "could not remove that remote" : FirstLine(out);
    return false;
  }
  return true;
}

std::string Upstream(const std::string& dir) {
  if (!Available() || !IsRepo(dir))
    return "";
  std::string text;
  if (RunGit(dir, "rev-parse --abbrev-ref --symbolic-full-name @{u}", &text) !=
      0)
    return "";
  return FirstLine(text);
}

Divergence AheadBehind(const std::string& dir) {
  Divergence out;
  if (Upstream(dir).empty())
    return out;
  out.hasUpstream = true;
  std::string text;
  // "<behind>\t<ahead>" — left is the upstream side.
  if (RunGit(dir, "rev-list --left-right --count @{u}...HEAD", &text) != 0)
    return out;
  const std::string line = FirstLine(text);
  const size_t tab = line.find('\t');
  if (tab == std::string::npos)
    return out;
  try {
    out.behind = std::stoi(line.substr(0, tab));
    out.ahead = std::stoi(line.substr(tab + 1));
  } catch (const std::exception&) {
    out.behind = out.ahead = 0;
  }
  return out;
}

bool Fetch(const std::string& dir, const std::string& remote,
           std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return false;
  }
  std::string out;
  // --tags: a named version is a tag, and a teammate's names should arrive
  // with their commits (F9 C5).
  if (RunGitNetwork(dir, "fetch --tags --quiet -- " + Quote(remote), &out) !=
      0) {
    if (error)
      *error = out.empty() ? "could not reach " + remote : FirstLine(out);
    return false;
  }
  return true;
}

PushResult Push(const std::string& dir, const std::string& remote,
                const std::string& branch, bool setUpstream,
                std::string* error) {
  if (!Available() || !IsRepo(dir)) {
    if (error)
      *error = "not a git project";
    return PushResult::Failed;
  }
  std::string out;
  const std::string args = std::string("push --follow-tags --porcelain ") +
                           (setUpstream ? "--set-upstream " : "") + "-- " +
                           Quote(remote) + " " + Quote(branch);
  const int code = RunGitNetwork(dir, args, &out);
  if (code == 0) {
    // --porcelain marks an unchanged ref with "=".
    return out.find("\n=") != std::string::npos ||
                   out.find("Everything up-to-date") != std::string::npos
               ? PushResult::UpToDate
               : PushResult::Pushed;
  }
  // The upstream moved. Not a failure — it means get their work first, and
  // merging on the user's behalf here would be a surprise.
  if (out.find("[rejected]") != std::string::npos ||
      out.find("non-fast-forward") != std::string::npos ||
      out.find("fetch first") != std::string::npos) {
    if (error)
      *error = "someone else has pushed since you last got their changes";
    return PushResult::Rejected;
  }
  if (error)
    *error = out.empty() ? "could not push to " + remote : FirstLine(out);
  return PushResult::Failed;
}

bool Clone(const std::string& url, const std::string& dir, std::string* error) {
  if (!Available()) {
    if (error)
      *error = "git is not available";
    return false;
  }
  std::error_code ec;
  if (std::filesystem::exists(dir, ec)) {
    if (error)
      *error = "there is already something at " + dir;
    return false;
  }
  // `-C .` is meaningless for clone, so run it from the parent and name the
  // target explicitly.
  const std::filesystem::path target(dir);
  std::filesystem::create_directories(target.parent_path(), ec);
  std::string out;
  if (RunGitNetwork(target.parent_path().string(),
                    "clone --quiet -- " + Quote(url) + " " + Quote(dir),
                    &out) != 0) {
    if (error)
      *error = out.empty() ? "could not clone " + url : FirstLine(out);
    return false;
  }
  return true;
}

} // namespace GitVault
