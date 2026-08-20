#include <catch2/catch_test_macros.hpp>

#include "core/io/GitVault.h"

#include <filesystem>
#include <fstream>
#include <string>
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
  ~ScratchRepo() { fs::remove_all(dir); }

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
