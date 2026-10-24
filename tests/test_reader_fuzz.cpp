// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Reader fuzzing (docs/versioning/README.md#reader-fuzzing): every reader is
// handed damaged versions of a valid input (tests/mutate) and must either
// REFUSE, naming the file and what is wrong, or LOAD the file correctly --
// never crash, hang, or load a document that is silently missing content.
//
// In-process, one TEST_CASE per reader family so ctest runs them side by
// side. `[reader_fuzz]` is a fixed sample (every operator at least once per
// family, about 1/50 of the rest) for the inner loop; `[reader_fuzz][slow]`
// is every case. Before each case its id goes to stderr and to
// build/reader-fuzz-last-case.<family>, so a crash or a hang names it;
// `tools/reader-fuzz.py case <id>` writes that case out as a regression
// (tests/reader_cases/, test_reader_cases.cpp).

#include "TestPaths.h"
#include "mutate/Mutate.h"
#include "mutate/ReaderFuzz.h"

#include "core/Document.h"
#include "core/io/Serialization.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using readerfuzz::Family;

namespace {

// The sweep's base seed is fixed, so a ctest run is reproducible;
// tools/reader-fuzz.py runs other seeds on purpose.
uint64_t BaseSeed() {
  if (const char* s = std::getenv("LOBOLE_READER_FUZZ_SEED"))
    return std::strtoull(s, nullptr, 10);
  return 0;
}

void Run(Family family, bool everyCase, int shard = 0, int shards = 1) {
  readerfuzz::Sweep sweep(family, everyCase, BaseSeed(), shard, shards);
  const readerfuzz::Summary s = sweep.Run();
  std::cout << s.Text() << "\n";
  for (const std::string& f : s.failures)
    std::cout << "  FAILED " << f << "\n";
  CHECK(s.failures.empty());
  CHECK(s.cases > 0);
}

} // namespace

#define READER_FUZZ_FAMILY(name, family)                                       \
  TEST_CASE("Reader fuzz: " name, "[reader_fuzz]") { Run(family, false); }     \
  TEST_CASE("Reader fuzz (every case): " name, "[reader_fuzz][slow]") {        \
    Run(family, true);                                                         \
  }

// The two families whose every case takes minutes run as shards.
#define READER_FUZZ_SHARDED(name, family, n)                                   \
  TEST_CASE("Reader fuzz: " name, "[reader_fuzz]") { Run(family, false); }     \
  TEST_CASE("Reader fuzz (every case): " name " 1/" #n,                        \
            "[reader_fuzz][slow]") {                                           \
    Run(family, true, 0, n);                                                   \
  }                                                                            \
  TEST_CASE("Reader fuzz (every case): " name " 2/" #n,                        \
            "[reader_fuzz][slow]") {                                           \
    Run(family, true, 1, n);                                                   \
  }                                                                            \
  TEST_CASE("Reader fuzz (every case): " name " 3/" #n,                        \
            "[reader_fuzz][slow]") {                                           \
    Run(family, true, 2, n);                                                   \
  }                                                                            \
  TEST_CASE("Reader fuzz (every case): " name " 4/" #n,                        \
            "[reader_fuzz][slow]") {                                           \
    Run(family, true, 3, n);                                                   \
  }

READER_FUZZ_SHARDED("part files", Family::PartFile, 4)
READER_FUZZ_FAMILY("assembly files", Family::AssemblyFile)
READER_FUZZ_FAMILY("drawing files", Family::DrawingFile)
READER_FUZZ_FAMILY("job files", Family::JobFile)
READER_FUZZ_SHARDED("part directories", Family::PartDir, 4)
READER_FUZZ_FAMILY("assembly directories", Family::AssemblyDir)
READER_FUZZ_FAMILY("drawing directories", Family::DrawingDir)
READER_FUZZ_FAMILY("job directories", Family::JobDir)
READER_FUZZ_FAMILY("projects", Family::Project)
READER_FUZZ_FAMILY("settings", Family::Settings)
READER_FUZZ_FAMILY("machine and print profiles", Family::Profiles)
READER_FUZZ_FAMILY("crash reports", Family::CrashReports)
READER_FUZZ_FAMILY("instance registry", Family::Instances)
READER_FUZZ_FAMILY("autosave recovery", Family::Recovery)
READER_FUZZ_FAMILY("git vault and merge", Family::Vault)
READER_FUZZ_FAMILY("images", Family::Images)

// Larger projects, opt-in: LOBOLE_READER_CORPUS=<dir> points at generator
// output (untracked, so it never decides whether the suite passes). With
// LOBOLE_READER_FUZZ_ALL set, every case of every project; without, the
// fixed sample.
TEST_CASE("Reader fuzz: LOBOLE_READER_CORPUS", "[.reader_corpus]") {
  if (std::getenv("LOBOLE_READER_CORPUS") == nullptr)
    SKIP("LOBOLE_READER_CORPUS is not set");
  Run(Family::ExternalCorpus, std::getenv("LOBOLE_READER_FUZZ_ALL") != nullptr);
}

// tools/reader-fuzz.py case <id>: LOBOLE_READER_FUZZ_CASE names the case,
// LOBOLE_READER_FUZZ_OUT where it goes.
TEST_CASE("Reader fuzz: write one case", "[.reader_fuzz_case]") {
  const char* id = std::getenv("LOBOLE_READER_FUZZ_CASE");
  const char* out = std::getenv("LOBOLE_READER_FUZZ_OUT");
  if (id == nullptr || out == nullptr)
    SKIP("LOBOLE_READER_FUZZ_CASE / LOBOLE_READER_FUZZ_OUT are not set");
  std::string error;
  const auto family = readerfuzz::WriteCase(id, BaseSeed(), out, &error);
  INFO(error);
  REQUIRE(family.has_value());
  if (!readerfuzz::IsDocumentFamily(*family)) {
    // A family outside documents: what its check made of the tree.
    std::cout << "family " << readerfuzz::FamilyName(*family) << "\n"
              << "check: " << error << "\n";
    return;
  }
  const readerfuzz::Outcome o = readerfuzz::ReadAs(*family, out);
  std::cout << "family " << readerfuzz::FamilyName(*family) << "\n"
            << (o.loaded ? "loaded" : "refused: " + o.message) << "\n";
  for (const auto& [what, n] : o.counts)
    std::cout << "count " << what << " " << n << "\n";
  for (const readctx::Drop& d : o.drops.drops)
    std::cout << "dropped " << d.path << " " << d.pointer << ": " << d.reason
              << "\n";
}

// ---- the mutation library's own test ---------------------------------------

namespace {
std::string GoldenBytes(const char* name) {
  std::ifstream in(fs::path(TestSourceDir()) / "tests" / "golden" / name,
                   std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("Mutate: cases are seeded, reproducible and cover every operator",
          "[reader_fuzz]") {
  const std::string bytes = GoldenBytes("v17-full.json");
  REQUIRE(!bytes.empty());
  mutate::Options opt;
  opt.version = 188;
  const auto a = mutate::FileCases("part", "v17-full.json", bytes, opt);
  const auto b = mutate::FileCases("part", "v17-full.json", bytes, opt);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); i++) {
    CHECK(a[i].id == b[i].id);
    CHECK(a[i].bytes == b[i].bytes);
  }
  // Another seed, other damage (the deterministic sweeps aside).
  mutate::Options other = opt;
  other.seed = 1;
  const auto c = mutate::FileCases("part", "v17-full.json", bytes, other);
  size_t differ = 0;
  for (size_t i = 0; i < std::min(a.size(), c.size()); i++)
    differ += a[i].id == c[i].id && a[i].bytes != c[i].bytes;
  CHECK(differ > 10);
  // Ids are unique, and one case is made alone by asking for its id.
  std::set<std::string> ids;
  for (const auto& k : a)
    CHECK(ids.insert(k.id).second);
  const std::string pick = a[a.size() / 2].id;
  mutate::Options one = opt;
  one.want = [&](const std::string& id) { return id == pick; };
  const auto alone = mutate::FileCases("part", "v17-full.json", bytes, one);
  REQUIRE(alone.size() == 1);
  CHECK(alone[0].bytes == a[a.size() / 2].bytes);
  // Truncation goes down to empty.
  CHECK(std::any_of(a.begin(), a.end(), [](const mutate::FileCase& k) {
    return k.op == "truncate" && k.bytes.empty();
  }));
  // Every operator makes at least one case on a small golden.
  std::map<std::string, size_t> perOp;
  for (const auto& k : a)
    perOp[k.op]++;
  for (const std::string& op : mutate::FileOperators()) {
    INFO(op);
    CHECK(perOp[op] > 0);
  }
  // A 100 KB file stays under a few thousand cases.
  nlohmann::json big = nlohmann::json::parse(bytes);
  for (int i = 0; big.dump(2).size() < 100 * 1024; i++)
    big["features"].push_back(big["features"][i % 3]);
  const auto many =
      mutate::FileCases("part", "big.json", big.dump(2) + "\n", opt);
  CHECK(many.size() < 4000);
}

TEST_CASE("Mutate: a tree's directory operators", "[reader_fuzz]") {
  const fs::path dir = fs::temp_directory_path() /
                       ("lobole-mutate-tree-" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir / "features");
  std::ofstream(dir / "index.json")
      << R"({"features": [1, 2], "format": "designer-document"})" << "\n";
  std::ofstream(dir / "features" / "1-box.json") << R"({"id": 1})" << "\n";
  std::ofstream(dir / "features" / "2-box.json") << R"({"id": 2})" << "\n";
  std::ofstream(dir / "features" / "2-box.result.json")
      << R"({"bin_bytes": 8, "a": {"type": "f32", "offset": 0, "count": 2}})"
      << "\n";
  mutate::TreeOptions opt;
  opt.file.version = 188;
  opt.binary = opt.file;
  opt.binary.binary = true;
  opt.foreignIndex = R"({"kind": "drawing"})";
  opt.outsideFile = (dir.parent_path() / "outside.json").string();
  std::map<std::string, size_t> perOp;
  for (const auto& c : mutate::TreeCases("t", dir, opt))
    perOp[c.op]++;
  for (const std::string& op : mutate::TreeOperators()) {
    if (op == "escape") // needs a `dir`, `asset` or `brep` name
      continue;
    INFO(op);
    CHECK(perOp[op] > 0);
  }
  fs::remove_all(dir);
}

// ---- the harness's own test ------------------------------------------------

TEST_CASE("Reader fuzz: the harness catches what it claims to",
          "[reader_fuzz]") {
  const fs::path golden =
      fs::path(TestSourceDir()) / "tests" / "golden" / "v01-plain-box.json";
  const std::string original = GoldenBytes("v01-plain-box.json");
  const auto bytesOf = [](const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  };
  std::shared_ptr<Document> last;
  // The real reader, for the original and as the base of each plant.
  const auto realRead = [&](const fs::path& p) {
    readerfuzz::Outcome o;
    auto r = DocumentIO::Load(p.string());
    if (!r) {
      o.message = r.error();
      return o;
    }
    last = std::shared_ptr<Document>(std::move(*r));
    o.loaded = true;
    o.counts = {{"features", last->FeatureCount()}};
    return o;
  };
  const auto realSave = [&](const fs::path& to) {
    return DocumentIO::Save(*last, to.string()).has_value();
  };

  SECTION("a reader that drops a key fails the round trip") {
    // The tolerant reader this sweep exists to catch: a key it does not
    // know is ignored rather than refused. Handed the file with one key a
    // newer build might write, it loads -- and saves without it.
    nlohmann::json withKey = nlohmann::json::parse(original);
    withKey["features"][0]["colour"] = "red";
    const fs::path cleaned = fs::temp_directory_path() /
                             ("lobole-planted-" + std::to_string(::getpid())) /
                             golden.filename();
    fs::create_directories(cleaned.parent_path());
    readerfuzz::Planted p{[&](const fs::path& path) {
                            nlohmann::json j =
                                nlohmann::json::parse(bytesOf(path));
                            j["features"][0].erase("colour");
                            std::ofstream(cleaned) << j.dump(2) << "\n";
                            return realRead(cleaned);
                          },
                          realSave};
    const auto s = readerfuzz::RunPlanted(
        golden, p, 0, {{"part/v01-plain-box.json:given:0", withKey.dump(2)}});
    fs::remove_all(cleaned.parent_path());
    REQUIRE(s.loaded == 1);
    CHECK(std::any_of(s.failures.begin(), s.failures.end(),
                      [](const std::string& f) {
                        return f.find("saved without") != std::string::npos &&
                               f.find("colour") != std::string::npos;
                      }));
  }
  SECTION("a refusal without the file's name fails the naming rule") {
    readerfuzz::Planted p{[&](const fs::path& path) {
                            if (bytesOf(path) == original)
                              return realRead(path);
                            readerfuzz::Outcome o;
                            o.message = "[json.exception.type_error.302] type "
                                        "must be number, but is string";
                            return o;
                          },
                          realSave};
    const auto s = readerfuzz::RunPlanted(golden, p, 20);
    REQUIRE(s.refused > 0);
    CHECK(std::any_of(
        s.failures.begin(), s.failures.end(), [](const std::string& f) {
          return f.find("does not name the file") != std::string::npos;
        }));
  }
  SECTION("a reader that sleeps fails its budget") {
    readerfuzz::Planted p{[&](const fs::path& path) {
                            if (bytesOf(path) != original)
                              std::this_thread::sleep_for(
                                  std::chrono::duration<double>(
                                      readerfuzz::Budget() + 0.3));
                            return realRead(path);
                          },
                          realSave};
    const auto s = readerfuzz::RunPlanted(golden, p, 1);
    CHECK(std::any_of(s.failures.begin(), s.failures.end(),
                      [](const std::string& f) {
                        return f.find("past its budget") != std::string::npos;
                      }));
  }
}
