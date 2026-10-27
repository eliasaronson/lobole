// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/Document.h"
#include "core/Feature.h" // BoxFeature
#include "core/io/GitVault.h"
#include "core/io/Serialization.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/ProjectActions.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>

// `.lob` is the project extension; `.dsgn`, the one before the rename, is
// opened and never written: saving the project opened from one writes the
// `.lob` beside it, history and all
// (docs/versioning/README.md#the-dsgn-extension).

namespace fs = std::filesystem;

namespace {

// git may still be running `maintenance --auto` in the tree (see
// test_project_trust.cpp): a second pass finds its lock gone.
void RemoveTree(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);
  if (ec)
    fs::remove_all(p, ec);
}

fs::path Scratch(const char* name) {
  const fs::path p = fs::temp_directory_path() /
                     ("lobole-ext-" + std::to_string(::getpid()) + "-" + name);
  RemoveTree(p);
  fs::create_directories(p);
  return p;
}

ui::AppState StateWithBox(bool twoParts) {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  if (twoParts) // a lone part saves flat; two need project.json
    ui::AddPart(state, "Second");
  // Into the active part, which is the one an open makes active again.
  state.document->AddFeature(
      std::make_unique<BoxFeature>("Box", glm::vec3(0), 10, 10, 10));
  return state;
}

// Every file under `dir`, relative, with its bytes: "the tree as it was".
std::map<std::string, std::string> Snapshot(const fs::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file())
      continue;
    std::ifstream in(e.path(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    out[fs::relative(e.path(), dir).string()] = ss.str();
  }
  return out;
}

bool HasFeature(const ui::AppState& state, const std::string& name) {
  for (const auto& f : state.document->GetFeatures())
    if (f->GetName() == name)
      return true;
  return false;
}

// What 0.1.5 left on disk: a project folder named .dsgn, with a history.
// Written as a .lob (the only form the app writes now) and renamed.
fs::path LegacyProject(const fs::path& root, bool twoParts) {
  ui::AppState state = StateWithBox(twoParts);
  const fs::path lob = root / "old.lob";
  std::string error;
  REQUIRE(ui::SaveProject(state, lob.string(), &error));
  ui::MarkProjectSaved(state, lob.string(), "made in 0.1.5");
  const fs::path dsgn = root / "old.dsgn";
  fs::rename(lob, dsgn);
  return dsgn;
}

} // namespace

TEST_CASE("A project path is a .lob, a .dsgn, a .json or a folder", "[io]") {
  CHECK(DocumentIO::IsProjectFolderName("/p/bracket.lob"));
  CHECK(DocumentIO::IsProjectFolderName("/p/bracket.lob/"));
  CHECK(DocumentIO::IsProjectFolderName("/p/bracket.dsgn"));
  CHECK_FALSE(DocumentIO::IsProjectFolderName("/p/bracket.json"));
  CHECK_FALSE(DocumentIO::IsProjectFolderName("/p/bracket.lobe"));
  CHECK(ui::IsProjectPath("/nowhere/bracket.lob"));
  CHECK(ui::IsProjectPath("/nowhere/bracket.dsgn"));
  CHECK(ui::IsProjectPath("/nowhere/bracket.json"));
  CHECK_FALSE(ui::IsProjectPath("/nowhere/bracket.step"));
  // A pick inside either folder is the folder.
  CHECK(ui::NormalizeProjectPick("/p/b.lob/parts/A/index.json") == "/p/b.lob");
  CHECK(ui::NormalizeProjectPick("/p/b.dsgn/project.json") == "/p/b.dsgn");
  // A save dialog's name lands on .lob, whatever was typed.
  CHECK(DocumentIO::ProjectSaveTarget("/p/b.dsgn") == "/p/b.lob");
  CHECK(ui::ProjectWriteTarget("/p/b.dsgn") == "/p/b.lob");
  CHECK(ui::ProjectWriteTarget("/p/b.lob") == "/p/b.lob");
  CHECK(ui::ProjectWriteTarget("/p/b.json") == "/p/b.json");
}

TEST_CASE("A .lob folder and a .json file round-trip", "[io][open]") {
  const fs::path root = Scratch("roundtrip");
  for (bool twoParts : {false, true}) {
    INFO((twoParts ? "two parts" : "one part"));
    ui::AppState state = StateWithBox(twoParts);
    const fs::path lob = root / (twoParts ? "two.lob" : "one.lob");
    std::string error;
    REQUIRE(ui::SaveProject(state, lob.string(), &error));
    ui::MarkProjectSaved(state, lob.string());
    CHECK(fs::is_directory(lob));
    CHECK(fs::exists(lob / (twoParts ? "project.json" : "index.json")));
    ui::AppState back = StateWithBox(false);
    REQUIRE(ui::OpenProject(back, lob.string(), &error));
    CHECK(back.parts.size() == (twoParts ? 2u : 1u));
    if (GitVault::Available())
      CHECK(GitVault::Log(lob.string(), 5).size() == 1);
  }
  // The single-file form is still .json, and only for one document.
  ui::AppState one = StateWithBox(false);
  const fs::path json = root / "one.json";
  std::string error;
  REQUIRE(ui::SaveProject(one, json.string(), &error));
  CHECK(fs::is_regular_file(json));
  ui::AppState back = StateWithBox(false);
  REQUIRE(ui::OpenProject(back, json.string(), &error));
  CHECK(HasFeature(back, "Box"));
  RemoveTree(root);
}

TEST_CASE("Saving a project opened from a .dsgn writes the .lob beside it",
          "[io][open]") {
  const fs::path root = Scratch("migrate");
  for (bool twoParts : {false, true}) {
    INFO((twoParts ? "two parts" : "one part"));
    const fs::path dsgn = LegacyProject(root, twoParts);
    const auto before = Snapshot(dsgn);

    ui::AppState state = StateWithBox(false);
    std::string error;
    REQUIRE(ui::OpenProject(state, dsgn.string(), &error));
    ui::MarkProjectOpened(state, dsgn.string());
    CHECK(HasFeature(state, "Box")); // a .dsgn opens
    state.document->AddFeature(
        std::make_unique<BoxFeature>("Added", glm::vec3(20), 5, 5, 5));

    REQUIRE(ui::SaveProject(state, state.projectPath, &error));
    ui::MarkProjectSaved(state, state.projectPath, "after the rename");
    const fs::path lob = root / "old.lob";
    CHECK(state.projectPath == lob.string()); // the app tracks the .lob now
    CHECK(state.savedAsNotice.find(lob.string()) != std::string::npos);
    CHECK(fs::is_directory(lob));
    // The .dsgn is exactly as it was: not one file written, moved or gone.
    CHECK(Snapshot(dsgn) == before);
    // The .lob holds the edit, and carries the history on.
    ui::AppState back = StateWithBox(false);
    REQUIRE(ui::OpenProject(back, lob.string(), &error));
    CHECK(HasFeature(back, "Added"));
    if (GitVault::Available()) {
      const auto log = GitVault::Log(lob.string(), 5);
      REQUIRE(log.size() == 2);
      CHECK(log[0].subject == "after the rename");
      CHECK(log[1].subject == "made in 0.1.5");
      CHECK(GitVault::Log(dsgn.string(), 5).size() == 1);
    }
    RemoveTree(lob);
    RemoveTree(dsgn);
  }
  RemoveTree(root);
}

TEST_CASE("A .dsgn is never written", "[io]") {
  const fs::path root = Scratch("refuse");
  const fs::path dsgn = LegacyProject(root, false);
  const auto before = Snapshot(dsgn);

  SECTION("a save to a .dsgn that is not the open project") {
    ui::AppState state = StateWithBox(false);
    const fs::path other = root / "new.dsgn";
    std::string error;
    CHECK_FALSE(ui::SaveProject(state, other.string(), &error));
    CHECK(error.find((root / "new.lob").string()) != std::string::npos);
    CHECK_FALSE(fs::exists(other));
    CHECK_FALSE(fs::exists(root / "new.lob"));
  }
  SECTION("a .lob already beside it is not saved over") {
    fs::create_directories(root / "old.lob");
    std::ofstream(root / "old.lob" / "mine.txt") << "someone's";
    ui::AppState state = StateWithBox(false);
    std::string error;
    REQUIRE(ui::OpenProject(state, dsgn.string(), &error));
    ui::MarkProjectOpened(state, dsgn.string());
    CHECK_FALSE(ui::SaveProject(state, state.projectPath, &error));
    CHECK(error.find("already exists") != std::string::npos);
    CHECK(state.projectPath == dsgn.string());
    CHECK(Snapshot(root / "old.lob").size() == 1);
  }
  CHECK(Snapshot(dsgn) == before);
  RemoveTree(root);
}
