// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The reader-fuzz harness (docs/versioning/README.md#reader-fuzzing): the
// corpus, each reader's real entry point, and the rules every case is held
// to. Shared by tests/test_reader_fuzz.cpp (the sweep), test_reader_cases.cpp
// (the committed regressions) and tools/reader-fuzz.py (which writes a case
// out through the test binary).

#include "core/io/ReadContext.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace readerfuzz {

enum class Family {
  PartFile,
  AssemblyFile,
  DrawingFile,
  JobFile,
  PartDir,
  AssemblyDir,
  DrawingDir,
  JobDir,
  Project,
  Settings,
  Profiles,
  CrashReports,
  Instances,
  Recovery,
  Vault,
  Images,
  ExternalCorpus,
};

// The family's name as a case id starts with it ("part", "part-dir", ...).
const char* FamilyName(Family f);
std::optional<Family> FamilyFromName(const std::string& name);

// What a read produced: refused with a message, or loaded with the counts
// a load must keep and what it reported dropping.
struct Outcome {
  bool loaded = false;
  std::string message;
  std::map<std::string, size_t> counts;
  readctx::LoadReport drops;
};

// A document family: one whose reader loads a document (a file, a
// directory form, a project). The others check a fixture tree.
bool IsDocumentFamily(Family f);

// Reads `path` (a file or a tree) as `family` reads it, through the real
// entry point. A family outside documents runs its check on the tree:
// `loaded` when every rule held, else `message` says which did not.
Outcome ReadAs(Family family, const std::filesystem::path& path);

struct Summary {
  std::string family;
  size_t cases = 0, refused = 0, loaded = 0;
  double seconds = 0.0, slowest = 0.0;
  std::string slowestId;
  std::map<std::string, size_t> perOperator;
  std::vector<std::string> failures;
  std::string Text() const;
};

class Sweep {
public:
  // `every`: every case; otherwise the fixed sample. `shard` of `shards`:
  // the inputs whose name hashes to it, so one family's every case runs as
  // several ctest tests side by side.
  Sweep(Family family, bool every, uint64_t seed, int shard = 0,
        int shards = 1);
  ~Sweep();
  Summary Run();

  struct Impl; // ReaderFuzz.cpp

private:
  Impl* m;
};

// The harness's own test: the first `limit` cases of one part file, read by
// a planted reader instead of the real one, so the rules are seen to catch
// what they claim (a reader that drops a key, one that refuses without
// naming the file, one that sleeps past its budget).
struct Planted {
  // Reads a file: refused, or loaded with counts.
  std::function<Outcome(const std::filesystem::path&)> read;
  // Saves what the last read loaded to `to`.
  std::function<bool(const std::filesystem::path& to)> save;
};
// The seconds one read may take (at least; a heavy input gets four times
// its undamaged read): larger in a sanitized build.
double Budget();

// `cases` (id, bytes), when given, are run in place of the generated ones.
Summary
RunPlanted(const std::filesystem::path& file, const Planted& planted,
           size_t limit,
           const std::vector<std::pair<std::string, std::string>>& cases = {});

// Writes the case `id` (made with `seed`) to `out`: the damaged file, or
// the damaged tree under `out`. Returns the family, or nullopt with
// `error` set.
std::optional<Family> WriteCase(const std::string& id, uint64_t seed,
                                const std::filesystem::path& out,
                                std::string* error);

} // namespace readerfuzz
