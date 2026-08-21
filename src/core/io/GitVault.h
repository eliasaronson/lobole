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

// ---------------------------------------------------------------------------
// Branch + merge (F7). Local branches only: remotes drag in SSH keys, tokens
// and host-key prompts, and one person with two machines is served by two
// clones of a folder. Everything below is the machinery a conflict needs to
// EXIST — before F7 the engine was linear, so the conflict UI F5 and F6 both
// deferred had no state to serve.
// ---------------------------------------------------------------------------

// Local branch names, current one first. Empty for a non-repo, or for a repo
// with no commit yet (git has a branch name but no ref to list).
std::vector<std::string> Branches(const std::string& dir);

// The checked-out branch, or "" when detached / not a repo.
std::string CurrentBranch(const std::string& dir);

// Create `name` at HEAD (and switch to it when `switchTo`). Fails when the
// name exists or git rejects it as a ref.
bool CreateBranch(const std::string& dir, const std::string& name,
                  bool switchTo, std::string* error = nullptr);

// Check out an existing branch. Refuses with a message when the tree is
// dirty — git would either refuse or carry the changes across, and both are
// surprises for someone who thinks of these as versions.
bool SwitchBranch(const std::string& dir, const std::string& name,
                  std::string* error = nullptr);

enum class MergeStatus {
  UpToDate,    // already contains `branch` — nothing happened
  FastForward, // no divergence; the tree just moved forward
  Merged,      // a real merge, committed, no conflicts
  Conflicted,  // stopped with unmerged paths; the tree is mid-merge
  Failed,      // git refused (dirty tree, unknown branch, …) — see `error`
};

// Merge `branch` into the current one. A Conflicted result is NOT a failure:
// it is the state the Conflicts panel exists to resolve, and the working
// tree is left mid-merge on purpose.
MergeStatus Merge(const std::string& dir, const std::string& branch,
                  std::string* error = nullptr);

// A merge is in progress (MERGE_HEAD exists). True after a conflicted merge
// — including one started from a terminal, which until F7 was the only way
// a project could get into this state at all.
bool InMerge(const std::string& dir);

// Paths git could not merge (`--diff-filter=U`), repo-relative.
// NOTE what this does NOT contain: two sides that each added a DIFFERENT
// file claiming the SAME document id (`15-box.json` and `15-hole.json`) is a
// clean merge to git — no overlap, two adds. That class is invisible here
// and is found by ProjectMerge::FindDuplicateIds instead.
std::vector<std::string> Unmerged(const std::string& dir);

// The three sides of a conflicted path. A side that does not exist (an
// add/add conflict has no base; a delete/modify has no ours or theirs) has
// its `has*` flag false — "deleted on that side" is a resolution the UI must
// be able to offer, so absence has to be representable.
struct Stages {
  std::string base, ours, theirs;
  bool hasBase = false, hasOurs = false, hasTheirs = false;
};
bool ReadStages(const std::string& dir, const std::string& path, Stages* out);

// `git show <rev>:<path>`. False when the path is absent in that revision.
bool ShowFile(const std::string& dir, const std::string& rev,
              const std::string& path, std::string* out);

// Paths in a revision's tree (`ls-tree -r --name-only`).
std::vector<std::string> ListTree(const std::string& dir,
                                  const std::string& rev);

// The merge base of two revisions, or "" when they share no history.
std::string MergeBase(const std::string& dir, const std::string& a,
                      const std::string& b);

enum class MergeSide { Ours, Theirs };

// Resolve one conflicted path by taking a whole side. Implemented over
// ReadStages rather than `checkout --ours`, so that "that side deleted it"
// resolves to a deletion instead of failing on a missing file.
bool ResolveWith(const std::string& dir, const std::string& path,
                 MergeSide side, std::string* error = nullptr);

// Resolve one conflicted path with content the caller computed — the union
// merge of an index, or a feature rebuilt field by field. Writing an empty
// `content` is a real resolution only via ResolveAsDeleted.
bool ResolveWithContent(const std::string& dir, const std::string& path,
                        const std::string& content,
                        std::string* error = nullptr);

// Resolve by removing the path.
bool ResolveAsDeleted(const std::string& dir, const std::string& path,
                      std::string* error = nullptr);

// Throw the merge away: working tree and index back to pre-merge HEAD.
bool AbortMerge(const std::string& dir, std::string* error = nullptr);

// Conclude a merge whose conflicts are all resolved. Fails while any path is
// still unmerged, which is the guard that stops a half-resolved tree from
// being committed as if it were finished.
bool CommitMerge(const std::string& dir, const std::string& message,
                 std::string* error = nullptr);

} // namespace GitVault
