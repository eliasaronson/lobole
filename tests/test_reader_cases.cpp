// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The reader-fuzz regressions (docs/versioning/README.md#reader-fuzzing):
// every directory under tests/reader_cases/ holds one damaged input, as
// bytes, and the expect.json `tools/reader-fuzz.py case <id>` wrote for it:
//
//   {"family": "part", "input": "v17-full.json",
//    "expect": "refused", "message": ["v17-full.json", "unknown key"]}
//   {"family": "part-dir", "input": "tree.dsgn",
//    "expect": "loads", "counts": {"features": 3}, "dropped": ["features/"]}
//   {"family": "settings", "input": "tree.dsgn", "expect": "passes"}
//
// `refused`: the reader refuses it, and the message holds every substring.
// `loads`: it loads, with those counts, and each `dropped` substring is in
// the path of something the load reported dropping (none when absent).
// `passes`: a family outside documents runs its check on the tree and every
// rule holds. A case stores the bytes, not the case id, so a changed
// operator never silently retargets an old regression.

#include "TestPaths.h"
#include "mutate/ReaderFuzz.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/io/GitVault.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/ProjectActions.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
using nlohmann::json;

TEST_CASE("Reader cases: every committed regression reads as it should",
          "[reader_fuzz][serialization]") {
  const fs::path root = fs::path(TestSourceDir()) / "tests" / "reader_cases";
  REQUIRE(fs::is_directory(root));
  std::vector<fs::path> cases;
  for (const auto& e : fs::directory_iterator(root))
    if (e.is_directory() && fs::exists(e.path() / "expect.json"))
      cases.push_back(e.path());
  std::sort(cases.begin(), cases.end());
  REQUIRE(!cases.empty());

  // A case reads a COPY: a reader that wrote beside what it read (crash
  // reports move a dead session's marker into a report) must not change
  // the committed bytes.
  const fs::path scratch =
      fs::temp_directory_path() /
      ("lobole-reader-cases-" + std::to_string(::getpid()));
  for (const fs::path& dir : cases) {
    DYNAMIC_SECTION(dir.filename().string()) {
      json expect;
      std::ifstream(dir / "expect.json") >> expect;
      const auto family =
          readerfuzz::FamilyFromName(expect.at("family").get<std::string>());
      REQUIRE(family.has_value());
      const fs::path input =
          dir / "input" / expect.at("input").get<std::string>();
      REQUIRE(fs::exists(fs::symlink_status(input)));
      fs::remove_all(scratch);
      fs::create_directories(scratch);
      // The whole input/ directory: a case may hold a file beside its tree
      // (what a link out of the project points at).
      fs::copy(dir / "input", scratch / "input",
               fs::copy_options::recursive | fs::copy_options::copy_symlinks);
      const fs::path copy = scratch / "input" / input.filename();
      const readerfuzz::Outcome o = readerfuzz::ReadAs(*family, copy);
      const std::string want = expect.at("expect").get<std::string>();
      INFO("message: " << o.message);
      if (want == "refused") {
        CHECK_FALSE(o.loaded);
        for (const auto& sub : expect.value("message", json::array()))
          CHECK(o.message.find(sub.get<std::string>()) != std::string::npos);
      } else if (want == "passes") {
        CHECK(o.loaded);
      } else {
        REQUIRE(want == "loads");
        REQUIRE(o.loaded);
        for (const auto& [what, n] :
             expect.value("counts", json::object()).items()) {
          INFO(what);
          const auto it = o.counts.find(what);
          REQUIRE(it != o.counts.end());
          CHECK(it->second == n.get<size_t>());
        }
        const json dropped = expect.value("dropped", json::array());
        CHECK(o.drops.drops.size() >= dropped.size());
        if (dropped.empty())
          CHECK(o.drops.Empty());
        for (const auto& sub : dropped) {
          const std::string s = sub.get<std::string>();
          INFO("dropped: " << s << "\n" << o.drops.Summary());
          CHECK(std::any_of(o.drops.drops.begin(), o.drops.drops.end(),
                            [&](const readctx::Drop& d) {
                              return d.path.find(s) != std::string::npos;
                            }));
        }
      }
    }
  }
  fs::remove_all(scratch);
}

// Compare-with-revision reads a version out of the history and loads it.
// One that does not load says WHY, naming the file in that version: "that
// version of this part does not load" alone sent the user looking.
TEST_CASE("Reader cases: comparing with a damaged version says why it does "
          "not load",
          "[reader_fuzz][history]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  const fs::path dir =
      fs::temp_directory_path() /
      ("lobole-compare-damaged-" + std::to_string(::getpid()) + ".dsgn");
  fs::remove_all(dir);
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  state.document->AddFeature(
      std::make_unique<BoxFeature>("Box", glm::dvec3(0.0), 10.0, 10.0, 10.0));
  std::string error;
  REQUIRE(ui::SaveProject(state, dir.string(), &error));
  state.projectPath = dir.string();
  REQUIRE(GitVault::Init(dir.string()));
  REQUIRE(GitVault::CommitAll(dir.string(), "box", &error));
  // A version whose feature file holds a key no build reads.
  fs::path feature;
  for (const auto& e : fs::directory_iterator(dir / "features"))
    feature = e.path();
  REQUIRE(!feature.empty());
  json j;
  std::ifstream(feature) >> j;
  j["colour"] = "red";
  std::ofstream(feature) << j.dump(2) << "\n";
  REQUIRE(GitVault::CommitAll(dir.string(), "damaged", &error));
  const auto log = GitVault::Log(dir.string(), 1);
  REQUIRE(!log.empty());
  const ui::GeometryChange change =
      ui::CompareVersionGeometry(state, log.front().hash, "");
  INFO(change.summary);
  CHECK_FALSE(change.valid);
  CHECK(change.summary.find("does not load") != std::string::npos);
  CHECK(change.summary.find(feature.filename().string()) != std::string::npos);
  CHECK(change.summary.find("unknown key 'colour'") != std::string::npos);
  fs::remove_all(dir);
}
