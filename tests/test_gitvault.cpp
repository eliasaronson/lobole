#include <catch2/catch_test_macros.hpp>

#include "core/io/GitVault.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// A scratch repository. Per-case AND per-pid: ctest runs these in parallel
// and a shared directory is a race (test_autosaver learned this the hard
// way).
struct ScratchRepo {
  fs::path dir;

  explicit ScratchRepo(const char* name) {
    dir = fs::temp_directory_path() / ("designer-git-" + std::string(name) +
                                       "-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  // Never throws: git detaches a background `maintenance`/`gc --auto` run
  // once a repo has enough commits, and it creates and deletes
  // `.git/objects/maintenance.lock` while we are deleting the tree. The
  // throwing overload turned that race into a terminate() out of a
  // destructor — the F7 cases commit often enough to hit it every time.
  ~ScratchRepo() {
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec)
      fs::remove_all(dir, ec); // the lock is gone by now; a leak is harmless
  }

  std::string path() const { return dir.string(); }

  void Write(const std::string& rel, const std::string& text) const {
    const fs::path p = dir / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
  }
  bool Has(const std::string& rel) const { return fs::exists(dir / rel); }
};

} // namespace

TEST_CASE("GitVault: commit, log, name a version", "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("basic");

  REQUIRE(GitVault::Init(repo.path()));
  REQUIRE(GitVault::IsRepo(repo.path()));

  repo.Write("index.json", "{}");
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));

  // A clean save is not an error, and it does not add a version.
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));
  REQUIRE(GitVault::Log(repo.path()).size() == 1);

  // Naming a version: the name is the commit SUBJECT (prose, spaces and
  // all), and the tag is only an address — git would refuse a ref with
  // spaces in it.
  repo.Write("index.json", R"({"parts":1})");
  REQUIRE(GitVault::CommitAll(repo.path(), "before the hinge rework"));
  REQUIRE(GitVault::Tag(repo.path(), "before the hinge rework"));

  const auto log = GitVault::Log(repo.path());
  REQUIRE(log.size() == 2);
  CHECK(log[0].subject == "before the hinge rework");
  CHECK(log[0].unixTime > 0);
  CHECK(log[0].named);
  CHECK_FALSE(log[1].named); // an ordinary save

  // Writer and reader agree on the address.
  CHECK(GitVault::TagRef("before the hinge rework") ==
        "cp/before-the-hinge-rework");
  std::string error;
  CHECK(GitVault::RestoreTo(
      repo.path(), GitVault::TagRef("before the hinge rework"), &error));
}

// Naming a state that is already saved is the common case, and the name has
// to become a version of its own — it cannot rewrite the subject of the
// commit that is already there.
TEST_CASE("GitVault: naming an unchanged state still makes a version",
          "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("empty-commit");
  REQUIRE(GitVault::Init(repo.path()));
  repo.Write("index.json", "{}");
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));

  // Without allowEmpty this is a no-op and the name is lost.
  REQUIRE(GitVault::CommitAll(repo.path(), "gangway v2"));
  CHECK(GitVault::Log(repo.path()).size() == 1);

  REQUIRE(GitVault::CommitAll(repo.path(), "gangway v2", nullptr, true));
  const auto log = GitVault::Log(repo.path());
  REQUIRE(log.size() == 2);
  CHECK(log[0].subject == "gangway v2");
  // Same tree — restoring the name gives the state it was attached to.
  REQUIRE(GitVault::Tag(repo.path(), "gangway v2"));
  std::string error;
  REQUIRE(GitVault::RestoreTo(repo.path(), "cp/gangway-v2", &error));
  CHECK(repo.Has("index.json"));
  CHECK_FALSE(GitVault::Dirty(repo.path()));
}

TEST_CASE("GitVault: version names that are not ref-safe", "[git]") {
  CHECK(GitVault::TagRef("v2") == "cp/v2");
  CHECK(GitVault::TagRef("before the hinge rework") ==
        "cp/before-the-hinge-rework");
  // Runs collapse, edges are trimmed, and git's forbidden characters go.
  CHECK(GitVault::TagRef("  gangway   v2  ") == "cp/gangway-v2");
  CHECK(GitVault::TagRef("try ~this^ [now]?") == "cp/try-this-now");
  CHECK(GitVault::TagRef("a/b") == "cp/a-b"); // no nested refs
  // Nothing usable left: refused rather than silently tagging something else.
  CHECK(GitVault::TagRef("***").empty());
  CHECK(GitVault::TagRef("").empty());
}

TEST_CASE("GitVault: Dirty tracks the working tree", "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("dirty");

  // Not a repo yet: nothing to commit to, so "no uncommitted changes".
  CHECK_FALSE(GitVault::Dirty(repo.path()));

  REQUIRE(GitVault::Init(repo.path()));
  repo.Write("index.json", "{}");
  CHECK(GitVault::Dirty(repo.path()));
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));
  CHECK_FALSE(GitVault::Dirty(repo.path()));

  repo.Write("index.json", R"({"edited":true})");
  CHECK(GitVault::Dirty(repo.path()));
}

// The bug F6 fixed: `git checkout <commit> -- .` restores the paths that
// commit HAS and leaves every other tracked file alone, so restoring to
// before a part existed kept the part on disk and in the reopened project.
TEST_CASE("GitVault: restore deletes files added after the target", "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("restore");

  REQUIRE(GitVault::Init(repo.path()));
  repo.Write("parts/base/index.json", R"({"name":"Base"})");
  REQUIRE(GitVault::CommitAll(repo.path(), "one part"));
  const std::string first = GitVault::Log(repo.path())[0].hash;

  repo.Write("parts/lid/index.json", R"({"name":"Lid"})");
  repo.Write("parts/base/index.json", R"({"name":"Base","edited":true})");
  REQUIRE(GitVault::CommitAll(repo.path(), "two parts"));
  REQUIRE(repo.Has("parts/lid/index.json"));

  std::string error;
  REQUIRE(GitVault::RestoreTo(repo.path(), first, &error));
  CHECK(error.empty());
  CHECK(repo.Has("parts/base/index.json"));
  CHECK_FALSE(repo.Has("parts/lid/index.json")); // the whole point
  {
    std::ifstream in(repo.dir / "parts/base/index.json");
    std::string text((std::istreambuf_iterator<char>(in)), {});
    CHECK(text == R"({"name":"Base"})"); // and edits are rolled back
  }

  // History stays linear: the two commits are still there, and the restored
  // state becomes a third on the next save.
  CHECK(GitVault::Log(repo.path()).size() == 2);
  CHECK(GitVault::Dirty(repo.path()));
  REQUIRE(GitVault::CommitAll(repo.path(), "Restore \"one part\""));
  const auto log = GitVault::Log(repo.path());
  REQUIRE(log.size() == 3);
  CHECK(log[0].subject == "Restore \"one part\"");
}

TEST_CASE("GitVault: restore names an unknown version", "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("unknown");
  REQUIRE(GitVault::Init(repo.path()));
  repo.Write("index.json", "{}");
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));

  std::string error;
  CHECK_FALSE(GitVault::RestoreTo(repo.path(), "cp/no-such-version", &error));
  CHECK_FALSE(error.empty());
  CHECK(repo.Has("index.json")); // a failed restore changes nothing
}

TEST_CASE("GitVault: a non-repo directory answers, never crashes", "[git]") {
  ScratchRepo repo("norepo");
  CHECK_FALSE(GitVault::IsRepo(repo.path()));
  CHECK(GitVault::Log(repo.path()).empty());
  CHECK_FALSE(GitVault::Dirty(repo.path()));
  std::string error;
  CHECK_FALSE(GitVault::RestoreTo(repo.path(), "HEAD", &error));
  CHECK(error == "not a git project");
}

// ---------------------------------------------------------------------------
// Branch + merge (F7). Before this, GitVault was linear-only: there was no
// verb that could produce a conflict, so the conflict UI F5 and F6 both
// deferred had no state to serve. These cases build that state.
// ---------------------------------------------------------------------------

namespace {

// Commit `text` as index.json on `branch`, creating the branch off `from`.
void CommitOn(const ScratchRepo& repo, const std::string& branch,
              const std::string& from, const std::string& file,
              const std::string& text) {
  if (!from.empty())
    REQUIRE(GitVault::SwitchBranch(repo.path(), from));
  if (branch != GitVault::CurrentBranch(repo.path()))
    REQUIRE(GitVault::CreateBranch(repo.path(), branch, true));
  repo.Write(file, text);
  REQUIRE(GitVault::CommitAll(repo.path(), "save on " + branch));
}

} // namespace

TEST_CASE("GitVault: branch, switch, and a merge that fast-forwards", "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("branch");
  REQUIRE(GitVault::Init(repo.path()));
  repo.Write("index.json", "{}");
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));

  const std::string main = GitVault::CurrentBranch(repo.path());
  REQUIRE_FALSE(main.empty());

  REQUIRE(GitVault::CreateBranch(repo.path(), "hinge-rework", true));
  CHECK(GitVault::CurrentBranch(repo.path()) == "hinge-rework");
  repo.Write("features/1-extrude.json", R"({"id":1,"distance":12})");
  REQUIRE(GitVault::CommitAll(repo.path(), "add an extrude"));

  REQUIRE(GitVault::SwitchBranch(repo.path(), main));
  CHECK_FALSE(repo.Has("features/1-extrude.json")); // the branch holds it

  // main has no commits of its own since the split, so this is a
  // fast-forward, not a merge commit.
  std::string error;
  CHECK(GitVault::Merge(repo.path(), "hinge-rework", &error) ==
        GitVault::MergeStatus::FastForward);
  CHECK(repo.Has("features/1-extrude.json"));

  // And merging it again changes nothing.
  CHECK(GitVault::Merge(repo.path(), "hinge-rework", &error) ==
        GitVault::MergeStatus::UpToDate);

  const auto branches = GitVault::Branches(repo.path());
  REQUIRE(branches.size() == 2);
  CHECK(branches[0] == main); // current first

  // A dirty tree refuses rather than letting git decide what to carry.
  repo.Write("index.json", R"({"dirty":true})");
  CHECK_FALSE(GitVault::SwitchBranch(repo.path(), "hinge-rework", &error));
  CHECK(error.find("save the project") != std::string::npos);
}

TEST_CASE("GitVault: disjoint edits merge with no conflict", "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("disjoint");
  REQUIRE(GitVault::Init(repo.path()));
  repo.Write("index.json", "{}");
  repo.Write("features/1-sketch.json", R"({"id":1})");
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));
  const std::string main = GitVault::CurrentBranch(repo.path());

  // The property F3 was designed for: two people touch different features,
  // so they touch different FILES and git has nothing to reconcile.
  CommitOn(repo, "alice", main, "features/2-extrude.json", R"({"id":2})");
  CommitOn(repo, "bob", main, "features/3-hole.json", R"({"id":3})");

  REQUIRE(GitVault::SwitchBranch(repo.path(), "alice"));
  std::string error;
  CHECK(GitVault::Merge(repo.path(), "bob", &error) ==
        GitVault::MergeStatus::Merged);
  CHECK(GitVault::Unmerged(repo.path()).empty());
  CHECK(repo.Has("features/2-extrude.json"));
  CHECK(repo.Has("features/3-hole.json"));
}

TEST_CASE("GitVault: a conflicted merge exposes its three stages", "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("conflict");
  REQUIRE(GitVault::Init(repo.path()));
  repo.Write("features/1-extrude.json", "{\n\"id\": 1,\n\"distance\": 10\n}\n");
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));
  const std::string main = GitVault::CurrentBranch(repo.path());

  // Same FIELD of the same feature on both sides — the narrow case that is
  // actually a conflict (git merges disjoint fields of one feature itself).
  CommitOn(repo, "alice", main, "features/1-extrude.json",
           "{\n\"id\": 1,\n\"distance\": 12\n}\n");
  CommitOn(repo, "bob", main, "features/1-extrude.json",
           "{\n\"id\": 1,\n\"distance\": 18\n}\n");

  REQUIRE(GitVault::SwitchBranch(repo.path(), "alice"));
  std::string error;
  REQUIRE(GitVault::Merge(repo.path(), "bob", &error) ==
          GitVault::MergeStatus::Conflicted);
  CHECK(GitVault::InMerge(repo.path()));

  const auto unmerged = GitVault::Unmerged(repo.path());
  REQUIRE(unmerged.size() == 1);
  CHECK(unmerged[0] == "features/1-extrude.json");

  GitVault::Stages stages;
  REQUIRE(GitVault::ReadStages(repo.path(), unmerged[0], &stages));
  CHECK(stages.hasBase);
  CHECK(stages.base.find("10") != std::string::npos);
  CHECK(stages.ours.find("12") != std::string::npos);
  CHECK(stages.theirs.find("18") != std::string::npos);

  // Committing while a path is still unmerged must fail — a half-resolved
  // tree committed as finished is exactly the silent loss F7 exists to stop.
  CHECK_FALSE(GitVault::CommitMerge(repo.path(), "merge", &error));
  CHECK(error.find("unresolved") != std::string::npos);

  REQUIRE(GitVault::ResolveWith(repo.path(), unmerged[0],
                                GitVault::MergeSide::Theirs, &error));
  CHECK(GitVault::Unmerged(repo.path()).empty());
  REQUIRE(GitVault::CommitMerge(repo.path(), "merge bob", &error));
  CHECK_FALSE(GitVault::InMerge(repo.path()));

  std::ifstream in(repo.dir / "features/1-extrude.json");
  const std::string merged((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
  CHECK(merged.find("18") != std::string::npos);
  CHECK(merged.find("<<<<<<<") == std::string::npos);
}

TEST_CASE("GitVault: abort puts the project back", "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("abort");
  REQUIRE(GitVault::Init(repo.path()));
  repo.Write("features/1-extrude.json", "{\n\"distance\": 10\n}\n");
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));
  const std::string main = GitVault::CurrentBranch(repo.path());
  CommitOn(repo, "alice", main, "features/1-extrude.json",
           "{\n\"distance\": 12\n}\n");
  CommitOn(repo, "bob", main, "features/1-extrude.json",
           "{\n\"distance\": 18\n}\n");

  REQUIRE(GitVault::SwitchBranch(repo.path(), "alice"));
  std::string error;
  REQUIRE(GitVault::Merge(repo.path(), "bob", &error) ==
          GitVault::MergeStatus::Conflicted);
  REQUIRE(GitVault::AbortMerge(repo.path(), &error));

  CHECK_FALSE(GitVault::InMerge(repo.path()));
  CHECK_FALSE(GitVault::Dirty(repo.path()));
  std::ifstream in(repo.dir / "features/1-extrude.json");
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  CHECK(text.find("12") != std::string::npos); // alice's side, untouched
}

// "Theirs deleted it" has to be expressible as a resolution — `git checkout
// --ours` fails on a side that has no file, which is why ResolveWith is built
// on the stages instead.
TEST_CASE("GitVault: delete/modify resolves either way", "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("delmod");
  REQUIRE(GitVault::Init(repo.path()));
  repo.Write("features/1-extrude.json", "{\n\"distance\": 10\n}\n");
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));
  const std::string main = GitVault::CurrentBranch(repo.path());

  CommitOn(repo, "alice", main, "features/1-extrude.json",
           "{\n\"distance\": 12\n}\n");
  REQUIRE(GitVault::SwitchBranch(repo.path(), main));
  REQUIRE(GitVault::CreateBranch(repo.path(), "bob", true));
  std::filesystem::remove(repo.dir / "features/1-extrude.json");
  REQUIRE(GitVault::CommitAll(repo.path(), "bob deletes it"));

  REQUIRE(GitVault::SwitchBranch(repo.path(), "alice"));
  std::string error;
  REQUIRE(GitVault::Merge(repo.path(), "bob", &error) ==
          GitVault::MergeStatus::Conflicted);

  GitVault::Stages stages;
  REQUIRE(
      GitVault::ReadStages(repo.path(), "features/1-extrude.json", &stages));
  CHECK(stages.hasOurs);
  CHECK_FALSE(stages.hasTheirs); // bob has no such file

  REQUIRE(GitVault::ResolveWith(repo.path(), "features/1-extrude.json",
                                GitVault::MergeSide::Theirs, &error));
  CHECK_FALSE(repo.Has("features/1-extrude.json"));
  REQUIRE(GitVault::CommitMerge(repo.path(), "take the deletion", &error));
}

// The class git CANNOT see: two sides each add a different file claiming the
// same id. No overlap, two adds, a clean merge — and the project that comes
// out is the one that used to open as ['Bob box', 'Bob box'].
TEST_CASE("GitVault: same-id different-file merges clean (the blind spot)",
          "[git]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchRepo repo("blindspot");
  REQUIRE(GitVault::Init(repo.path()));
  repo.Write("index.json", R"({"features":[1]})");
  REQUIRE(GitVault::CommitAll(repo.path(), "save"));
  const std::string main = GitVault::CurrentBranch(repo.path());

  CommitOn(repo, "alice", main, "features/15-box.json", R"({"id":15})");
  CommitOn(repo, "bob", main, "features/15-hole.json", R"({"id":15})");

  REQUIRE(GitVault::SwitchBranch(repo.path(), "alice"));

  // Taken BEFORE the merge: afterwards bob is an ancestor of alice, so the
  // merge base would be bob itself. This is the tree both sides diverged
  // from, and what ProjectMerge diffs each side against to find their adds.
  const std::string base = GitVault::MergeBase(repo.path(), "alice", "bob");
  REQUIRE_FALSE(base.empty());
  CHECK(GitVault::ListTree(repo.path(), base).size() == 1); // index.json only

  std::string error;
  CHECK(GitVault::Merge(repo.path(), "bob", &error) ==
        GitVault::MergeStatus::Merged);
  CHECK(GitVault::Unmerged(repo.path()).empty()); // git saw nothing wrong
  CHECK(repo.Has("features/15-box.json"));
  CHECK(repo.Has("features/15-hole.json"));
}
