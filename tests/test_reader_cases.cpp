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
#include "core/io/ReadContext.h"
#include "core/io/Serialization.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/ProjectActions.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
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
        // Named: items() over value()'s temporary iterated freed memory on
        // GCC 13, before P2718 (test_conventions.cpp's range-for scan).
        const json counts = expect.value("counts", json::object());
        for (const auto& [what, n] : counts.items()) {
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

// A save prunes a collection that became EMPTY, not only one that shrank:
// the last configuration or study deleted left its file behind, and the
// next open reported it as a file it could not place -- a lossy load of a
// project nobody damaged (two scratch projects did this).
TEST_CASE("Reader cases: a save prunes a collection emptied since the last",
          "[reader_fuzz][serialization]") {
  const fs::path dir =
      fs::temp_directory_path() /
      ("lobole-emptied-" + std::to_string(::getpid()) + ".dsgn");
  fs::remove_all(dir);
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Box", glm::dvec3(0.0), 10.0, 10.0, 10.0));
  const uint64_t cfg = doc.AddConfiguration("Long").id;
  const uint64_t study = doc.AddStudy("Pull").id;
  REQUIRE(DocumentIO::Save(doc, dir.string()).has_value());
  REQUIRE(doc.RemoveConfiguration(cfg));
  REQUIRE(doc.RemoveStudy(study));
  REQUIRE(DocumentIO::Save(doc, dir.string()).has_value());
  readctx::CollectDrops collect;
  REQUIRE(DocumentIO::Load(dir.string()).has_value());
  const readctx::LoadReport report = collect.Take();
  INFO(report.Summary());
  CHECK(report.Empty());
  fs::remove_all(dir);
}

// nlohmann copies and prints a value recursively: ten thousand nested
// brackets parsed, then ran the first copy off the stack under ASan. A
// strict parse refuses past kMaxJsonDepth, before a DOM of it exists.
TEST_CASE("Reader cases: a value nested past the limit is refused",
          "[reader_fuzz][serialization]") {
  const auto nest = [](size_t n) {
    return "{\"a\":" + std::string(n, '[') + std::string(n, ']') + "}";
  };
  CHECK_NOTHROW(readctx::ParseStrict(nest(readctx::kMaxJsonDepth - 1)));
  try {
    (void)readctx::ParseStrict(nest(10000));
    FAIL("a value ten thousand levels deep parsed");
  } catch (const std::runtime_error& e) {
    CHECK(std::string(e.what()).find("nests deeper than") != std::string::npos);
  }
}

namespace {
// This process's resident set now, and its peak since the last
// ResetPeakRss, in kB (/proc/self/status: VmRSS, VmHWM).
size_t StatusKb(const std::string& key) {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line))
    if (line.rfind(key + ":", 0) == 0)
      return std::stoul(line.substr(key.size() + 1));
  return 0;
}
// Writing 5 to clear_refs resets the peak (VmHWM) to the resident set now.
bool ResetPeakRss() {
  std::ofstream out("/proc/self/clear_refs");
  out << "5";
  out.flush();
  return static_cast<bool>(out);
}
} // namespace

// A v1 box grown to 104 857 copies of its one feature was read, and its
// rebuild ran past 8 GB (one attempt reached 23 GB before it was killed): a
// file that takes the machine with it. A part holds at most
// kMaxPartFeatures, and the count is checked before one feature is built,
// so the refusal costs a parse and nothing more.
TEST_CASE("Reader cases: a part past the feature limit is refused before it "
          "is built",
          "[reader_fuzz][serialization]") {
  const fs::path input = fs::path(TestSourceDir()) / "tests" / "reader_cases" /
                         "part-grown-past-the-feature-limit" / "input" /
                         "v01-plain-box.json";
  REQUIRE(fs::exists(input));
  const size_t rssBefore = StatusKb("VmRSS");
  const bool peakReset = ResetPeakRss();
  // A cost check, so CPU time (docs/ui/testing.md#tests-that-time-things).
  const std::clock_t t0 = std::clock();
  const auto doc = DocumentIO::Load(input.string());
  const double seconds = double(std::clock() - t0) / CLOCKS_PER_SEC;
  REQUIRE_FALSE(doc.has_value());
  const std::string& error = doc.error();
  INFO(error);
  CHECK(error.find("v01-plain-box.json") != std::string::npos);
  CHECK(error.find("a part holds at most 50 000 features; this one lists "
                   "104 857") != std::string::npos);
  // Milliseconds of CPU: a bound a rebuild could never meet. The sanitizers
  // multiply the parse's cost (6 s of CPU under ASan, where the bound is
  // 5 s), so it takes the reader-fuzz budget's factor of ten
  // (ReaderFuzz.cpp's Budget()) there, as the RSS check below is left out:
  // still far below the minutes a rebuild of 104 857 features would cost.
#ifdef LOBOLE_READER_FUZZ_SANITIZED
  constexpr double kCpuScale = 10.0;
#else
  constexpr double kCpuScale = 1.0;
#endif
  INFO("CPU seconds " << seconds);
  CHECK(seconds < 5.0 * kCpuScale);
  // The parse is what it costs: about 90 MB for 8 MB of JSON. Not under
  // the sanitizers, whose allocator keeps what was freed.
#ifndef LOBOLE_READER_FUZZ_SANITIZED
  if (peakReset) {
    const size_t peakKb = StatusKb("VmHWM");
    const size_t grewKb = peakKb - std::min(rssBefore, peakKb);
    INFO("peak grew " << grewKb / 1024 << " MB");
    CHECK(grewKb < 100 * 1024);
  }
#else
  (void)peakReset;
  (void)rssBefore;
#endif
}

// The directory form's index is checked the same way, before the
// directory is scanned.
TEST_CASE("Reader cases: a part index past the feature limit is refused",
          "[reader_fuzz][serialization]") {
  const fs::path dir =
      fs::temp_directory_path() /
      ("lobole-feature-limit-" + std::to_string(::getpid()) + ".dsgn");
  fs::remove_all(dir);
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Box", glm::dvec3(0.0), 10.0, 10.0, 10.0));
  REQUIRE(DocumentIO::Save(doc, dir.string()).has_value());
  json index;
  std::ifstream(dir / "index.json") >> index;
  // At the limit it is not the count that refuses (the files are missing).
  for (const size_t n :
       {readctx::kMaxPartFeatures, readctx::kMaxPartFeatures + 1}) {
    index["features"] = json::array();
    for (size_t i = 1; i <= n; i++)
      index["features"].push_back(i);
    std::ofstream(dir / "index.json") << index.dump(2) << "\n";
    const auto loaded = DocumentIO::Load(dir.string());
    REQUIRE_FALSE(loaded.has_value());
    const std::string& error = loaded.error();
    INFO(error);
    const bool countRefused =
        error.find("a part holds at most 50 000 features") != std::string::npos;
    CHECK(countRefused == (n > readctx::kMaxPartFeatures));
    if (countRefused) {
      CHECK(error.find("index.json") != std::string::npos);
      CHECK(error.find("lists 50 001") != std::string::npos);
    }
  }
  fs::remove_all(dir);
}

TEST_CASE("Reader cases: the feature limit refuses an add past it",
          "[reader_fuzz][serialization]") {
  CHECK(readctx::PartFeatureLimitRefusal(0, 1).empty());
  CHECK(readctx::PartFeatureLimitRefusal(readctx::kMaxPartFeatures - 1, 1)
            .empty());
  CHECK(readctx::PartFeatureLimitRefusal(readctx::kMaxPartFeatures, 1) ==
        "a part holds at most 50 000 features, and this one has 50 000");
  CHECK_FALSE(readctx::PartFeatureLimitRefusal(readctx::kMaxPartFeatures - 1, 2)
                  .empty());
  CHECK(readctx::GroupedCount(0) == "0");
  CHECK(readctx::GroupedCount(999) == "999");
  CHECK(readctx::GroupedCount(1000) == "1 000");
  CHECK(readctx::GroupedCount(104857) == "104 857");
  CHECK(readctx::GroupedCount(1234567) == "1 234 567");
}
