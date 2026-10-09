// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/io/ProjectStamp.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct ScratchDir {
  fs::path dir;

  explicit ScratchDir(const char* name) {
    dir = fs::temp_directory_path() / ("designer-stamp-" + std::string(name) +
                                       "-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~ScratchDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  std::string path() const { return dir.string(); }
  void Write(const std::string& rel, const std::string& text) const {
    const fs::path p = dir / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc) << text;
  }
};

// The stamp's resolution is the filesystem's mtime tick. A test that writes
// twice in the same tick is testing the clock, not the code.
void NextTick() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }

void Seed(const ScratchDir& d) {
  d.Write("index.json", R"({"features":[1,2]})");
  d.Write("features/1-sketch.json", R"({"id":1})");
  d.Write("features/2-extrude.json", R"({"id":2,"distance":10})");
}

} // namespace

TEST_CASE("A stamp of an unchanged project matches itself", "[stamp]") {
  ScratchDir d("stable");
  Seed(d);
  const auto first = DocumentIO::StampProject(d.path());
  REQUIRE(first.valid);
  CHECK(first.fileCount == 3);
  CHECK(first.Matches(DocumentIO::StampProject(d.path())));
}

TEST_CASE("An external edit moves the stamp", "[stamp]") {
  ScratchDir d("edited");
  Seed(d);
  const auto before = DocumentIO::StampProject(d.path());
  NextTick();
  // Someone else's write: a different value, a different size.
  d.Write("features/2-extrude.json", R"({"id":2,"distance":18})");
  CHECK_FALSE(DocumentIO::StampProject(d.path()).Matches(before));
}

TEST_CASE("Adding and removing a file both move the stamp", "[stamp]") {
  ScratchDir d("count");
  Seed(d);
  const auto before = DocumentIO::StampProject(d.path());
  d.Write("features/3-hole.json", R"({"id":3})");
  const auto added = DocumentIO::StampProject(d.path());
  CHECK_FALSE(added.Matches(before));
  CHECK(added.fileCount == 4);

  std::error_code ec;
  fs::remove(d.dir / "features/3-hole.json", ec);
  CHECK_FALSE(DocumentIO::StampProject(d.path()).Matches(added));
}

// .git churns on its own — the index, the reflog, and git's own detached
// `maintenance`/`gc --auto` runs, which write while we are reading. A stamp
// that noticed those would cry wolf on every save.
TEST_CASE("Churn inside .git is not a change to the project", "[stamp]") {
  ScratchDir d("gitchurn");
  Seed(d);
  const auto before = DocumentIO::StampProject(d.path());
  NextTick();
  d.Write(".git/index", "whatever git puts here");
  d.Write(".git/logs/HEAD", "reflog line");
  d.Write(".git/objects/maintenance.lock", "");
  CHECK(DocumentIO::StampProject(d.path()).Matches(before));
}

// F7 settled that baked geometry is derived: the chain hash decides at load,
// so either side of a merge does. A guard that fired on it would contradict
// that — the stamp covers the recipe.
TEST_CASE("Baked geometry is the build, not the recipe", "[stamp]") {
  ScratchDir d("derived");
  Seed(d);
  d.Write("geometry/index.json", R"({"chain":"abc"})");
  d.Write("geometry/0.brep", "binary-ish");
  const auto before = DocumentIO::StampProject(d.path());
  NextTick();
  d.Write("geometry/0.brep", "rebuilt, different, longer");
  CHECK(DocumentIO::StampProject(d.path()).Matches(before));

  // ...and it is skipped at every depth, not only at the root: a
  // multi-document project has one geometry/ per document.
  d.Write("parts/Stand/geometry/0.brep", "another body");
  const auto nested = DocumentIO::StampProject(d.path());
  NextTick();
  d.Write("parts/Stand/geometry/0.brep", "rebuilt again, longer still");
  CHECK(DocumentIO::StampProject(d.path()).Matches(nested));
}

TEST_CASE("A stamp with nothing to compare never matches", "[stamp]") {
  const DocumentIO::ProjectStamp none;
  CHECK_FALSE(none.valid);
  // "We have no idea" must never read as "unchanged".
  CHECK_FALSE(none.Matches(none));
  CHECK_FALSE(DocumentIO::StampProject("").valid);
  CHECK_FALSE(DocumentIO::StampProject("/nonexistent/designer/project").valid);
}

TEST_CASE("A legacy single-file project stamps as itself", "[stamp]") {
  ScratchDir d("flat");
  d.Write("part.json", R"({"features":[]})");
  const std::string file = (d.dir / "part.json").string();
  const auto before = DocumentIO::StampProject(file);
  REQUIRE(before.valid);
  CHECK(before.fileCount == 1);
  NextTick();
  d.Write("part.json", R"({"features":[1,2,3]})");
  CHECK_FALSE(DocumentIO::StampProject(file).Matches(before));
}
