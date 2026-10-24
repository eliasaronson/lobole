// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Damage to a valid input, seeded and reproducible
// (docs/versioning/README.md#reader-fuzzing).
//
// One valid file (or a directory tree) in, a list of damaged versions out.
// Every case carries a stable id, `<corpus>/<file>:<operator>:<n>`, and is a
// pure function of the input, that id and the base seed: the RNG
// (xorshift64) is seeded by a hash of the id, so one case replays without
// generating the others. Five levels of operator:
//
//   bytes      truncation, bytes flipped/zeroed/set, runs dropped/repeated/
//              swapped, garbage appended, the file doubled/reversed/emptied
//   text       lines dropped/repeated/swapped/replaced, a word replaced by
//              another of the file's, numbers replaced by liars
//   json       a key deleted or written twice, keys reordered, a value's
//              type swapped, a number lied about, an enum string changed,
//              `version` changed, a value wrapped deep, an array grown
//   model      an id reference pointed nowhere, at itself or forward; two
//              items given one id; id 0; items reordered
//   directory  a file deleted, emptied, duplicated under another id or
//              unlisted; one document swapped for another kind; a name
//              that leaves the project; a symlink out of it; a result's
//              offset or count lying
//
// The operators know nothing about the readers; the harness
// (tests/test_reader_fuzz.cpp) decides what a reader must do with each.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mutate {

// FNV-1a, 64-bit: what seeds a case's RNG from its id.
uint64_t Hash(std::string_view s);

// xorshift64. Never seeded with 0 (which it would keep forever).
class Rng {
public:
  explicit Rng(uint64_t seed);
  uint64_t Next();
  // Uniform in [0, n); n == 0 gives 0.
  size_t Below(size_t n);

private:
  uint64_t m_state;
};

struct Options {
  uint64_t seed = 0;
  // The format version the `version` operator lies around (kVersion).
  int version = 0;
  // At most this many cases per operator that pick their target at random
  // (a flipped byte, a deleted key); 0 = no cap. Deterministic sweeps
  // (truncation, every operator's fixed list) ignore it.
  size_t perOperator = 6;
  // Truncation lengths: every length of a file under `truncateAll` bytes,
  // else the first and last `truncateEnds` and `truncateSpread` spaced
  // evenly between.
  size_t truncateAll = 1024, truncateEnds = 64, truncateSpread = 1024;
  // The length an array is grown to.
  size_t growTo = 1000000;
  // Byte-level operators only (a binary file: a .brep, a .bin, an image).
  bool binary = false;
  // Which cases to make, by id (null: all). A case not wanted is never
  // built, so a sample costs what it runs, not what it skips.
  std::function<bool(const std::string& id)> want;
};

// One damaged file: the bytes that replace it.
struct FileCase {
  std::string id; // "<corpus>/<file>:<operator>:<n>"
  std::string op;
  std::string bytes;
};

// The cases for one file. `corpus` and `file` only make the ids.
std::vector<FileCase> FileCases(const std::string& corpus,
                                const std::string& file,
                                const std::string& bytes, const Options& opt);

// One edit to a directory tree.
struct Edit {
  enum class Kind { Write, Remove, Symlink };
  Kind kind = Kind::Write;
  std::string rel;    // relative to the tree's root, '/'-separated
  std::string bytes;  // Write
  std::string target; // Symlink: what the link points at
};

// One damaged tree: the edits that make it.
struct TreeCase {
  std::string id; // "<corpus>/<file>:<operator>:<n>"; file "." for the tree
  std::string op;
  std::vector<Edit> edits;
};

struct TreeOptions {
  Options file;   // for each file's own cases
  Options binary; // for each binary file's
  // Another document's index.json, of a different kind, for `swap_kind`
  // ("" = none).
  std::string foreignIndex;
  // A file outside the tree for `symlink_out` to point at ("" = none).
  std::string outsideFile;
  // Each file's own cases on every file of the tree; otherwise on every
  // file that is no collection's item, and on one item of each kind per
  // directory (the single-file families already damage every item's
  // content; a tree is here for what only the directory form reads).
  bool everyFile = false;
};

// The cases for a directory form: every file's own cases as Write edits,
// and the directory operators.
std::vector<TreeCase> TreeCases(const std::string& corpus,
                                const std::filesystem::path& root,
                                const TreeOptions& opt);

// Every operator name, by level, for the library's own test.
std::vector<std::string> FileOperators();
std::vector<std::string> TreeOperators();

// The liars the number operators use, as JSON text (the values
// hostile_params sends: docs/scripting/rpc.md#the-hostile-sweep).
const std::vector<std::string>& Liars();

} // namespace mutate
