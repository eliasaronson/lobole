// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/io/RecentProjects.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

// The list lives in the XDG state dir; point that at a scratch directory so a
// test never touches (or reads) the developer's real history.
struct ScratchState {
  fs::path dir;
  std::string previous;
  bool had = false;

  explicit ScratchState(const char* name) {
    dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (const char* old = std::getenv("XDG_STATE_HOME")) {
      previous = old;
      had = true;
    }
    setenv("XDG_STATE_HOME", dir.string().c_str(), 1);
    RecentProjects::Clear();
  }
  ~ScratchState() {
    if (had)
      setenv("XDG_STATE_HOME", previous.c_str(), 1);
    else
      unsetenv("XDG_STATE_HOME");
    fs::remove_all(dir);
  }
};

} // namespace

TEST_CASE("Recent projects: newest first, no duplicates", "[recent]") {
  ScratchState scratch("designer-recent-basic");

  RecentProjects::Remember("/tmp/one.dsgn");
  RecentProjects::Remember("/tmp/two.dsgn");
  RecentProjects::Remember("/tmp/three.dsgn");
  auto list = RecentProjects::List();
  REQUIRE(list.size() == 3);
  CHECK(list[0] == "/tmp/three.dsgn");
  CHECK(list[2] == "/tmp/one.dsgn");

  // Re-opening an old project moves it to the front instead of adding a
  // second entry — otherwise the list fills with one project.
  RecentProjects::Remember("/tmp/one.dsgn");
  list = RecentProjects::List();
  REQUIRE(list.size() == 3);
  CHECK(list[0] == "/tmp/one.dsgn");

  // The same file spelled differently is the same file.
  RecentProjects::Remember("/tmp/./two.dsgn");
  list = RecentProjects::List();
  CHECK(list.size() == 3);
  CHECK(list[0] == "/tmp/two.dsgn");

  // The unsaved slot has no path and must not appear.
  RecentProjects::Remember("");
  CHECK(RecentProjects::List().size() == 3);
}

TEST_CASE("Recent projects: capped, and it survives a restart", "[recent]") {
  ScratchState scratch("designer-recent-cap");
  for (int i = 0; i < 25; i++)
    RecentProjects::Remember("/tmp/p" + std::to_string(i) + ".dsgn");
  auto list = RecentProjects::List();
  CHECK(list.size() == RecentProjects::kMax);
  CHECK(list.front() == "/tmp/p24.dsgn"); // the newest survives
  CHECK(std::find(list.begin(), list.end(), "/tmp/p0.dsgn") == list.end());

  // Reading it back is what the next launch does.
  CHECK(RecentProjects::List() == list);
  RecentProjects::Forget(list[1]);
  CHECK(RecentProjects::List().size() == RecentProjects::kMax - 1);
}

// A project on an unplugged drive is still a project you had open. Dropping
// it silently would make the menu lie about your own history; the UI shows it
// disabled instead, which it can only do if the list keeps it.
TEST_CASE("Recent projects keeps entries whose file is gone", "[recent]") {
  ScratchState scratch("designer-recent-missing");
  const fs::path real = scratch.dir / "here.dsgn";
  std::ofstream(real) << "{}";
  RecentProjects::Remember(real.string());
  RecentProjects::Remember("/nowhere/gone.dsgn");
  CHECK(RecentProjects::List().size() == 2);
  fs::remove(real);
  CHECK(RecentProjects::List().size() == 2);
}

TEST_CASE("A corrupt recent list costs a menu, not a launch", "[recent]") {
  ScratchState scratch("designer-recent-corrupt");
  RecentProjects::Remember("/tmp/x.dsgn");
  std::ofstream(RecentProjects::FilePath(), std::ios::trunc) << "{not json";
  CHECK(RecentProjects::List().empty()); // no throw, no crash
  RecentProjects::Remember("/tmp/y.dsgn");
  REQUIRE(RecentProjects::List().size() == 1);
  CHECK(RecentProjects::List()[0] == "/tmp/y.dsgn");
}
