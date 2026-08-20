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
  // True when a "cp/" tag points at this commit: the user NAMED this version
  // (F6), and `subject` is the name they typed. Ordinary saves have none.
  bool named = false;
};

bool Available();                    // `git` on PATH
bool IsRepo(const std::string& dir); // dir/.git exists
bool Init(const std::string& dir);   // git init -q

// Uncommitted changes in the working tree. False for a non-repo — "nothing
// to commit" is the honest answer when there is nowhere to commit to.
bool Dirty(const std::string& dir);

// Stage everything and commit (inline identity — works on CI and fresh
// machines without git config). True when committed OR nothing changed.
// `allowEmpty` commits anyway when nothing changed: naming a version marks a
// state, and that state is usually already committed by the save before it.
bool CommitAll(const std::string& dir, const std::string& message,
               std::string* error = nullptr, bool allowEmpty = false);

// The git ref a named version is addressable by: "cp/" + a slug of `name`.
// Git refuses spaces (and ~^:?*[\ and a few sequences) in a ref, but a
// version name is user prose — so the NAME lives in the commit subject and
// the ref is only an address. Both the writer and the reader call this, so
// they agree. Empty when `name` slugs to nothing.
std::string TagRef(const std::string& name);

// Force-tag the current HEAD as a named version. `name` is the user's text;
// the ref it lands on is TagRef(name).
bool Tag(const std::string& dir, const std::string& name);

std::vector<LogEntry> Log(const std::string& dir, int limit = 20);

// Set the working tree to `commit`'s tree — including DELETING tracked files
// that commit did not have (a `git checkout <commit> -- .` would leave them,
// so restoring past a part's creation kept the part). HEAD is left alone, so
// history stays linear: the restored state becomes a new commit on the next
// save. Uncommitted changes are discarded; ask Dirty() first if that matters.
bool RestoreTo(const std::string& dir, const std::string& commit,
               std::string* error = nullptr);

} // namespace GitVault
