#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Git-backed project history (versioning F5 v1): the `.dsgn` project
// directory IS the repository. Shells out to the `git` binary (this
// product's audience has git; embedding libgit2 buys nothing here). All
// operations no-op gracefully when git is unavailable.
namespace GitVault {

struct LogEntry {
  std::string hash;
  std::string subject;
  int64_t unixTime = 0;
};

bool Available();                    // `git` on PATH
bool IsRepo(const std::string& dir); // dir/.git exists
bool Init(const std::string& dir);   // git init -q

// Stage everything and commit (inline identity — works on CI and fresh
// machines without git config). True when committed OR nothing changed.
bool CommitAll(const std::string& dir, const std::string& message,
               std::string* error = nullptr);

// Force-tag the current HEAD (used for checkpoints: "cp/<name>").
bool Tag(const std::string& dir, const std::string& name);

std::vector<LogEntry> Log(const std::string& dir, int limit = 20);

// Check out `commit`'s tree over the working directory (the tree is clean
// by the commit-on-save policy); history stays linear — the restored state
// becomes a new commit on the next save.
bool RestoreTo(const std::string& dir, const std::string& commit,
               std::string* error = nullptr);

} // namespace GitVault
