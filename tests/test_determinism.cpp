#include "TestPaths.h"
#include <catch2/catch_test_macros.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Determinism: the same recipe builds the same geometry, every time.
//
// This is the assumption with the most weight on it in the whole codebase and
// the one nothing asserted directly. Checkpoints and rollback replay a feature
// list and expect the shape back. The git merge machinery assumes two people
// opening the same file see the same model. Baked geometry (F3) is only valid
// because a regeneration would rebuild what was saved beside the recipe. The
// golden corpus compares a document against a RECORDED expectation, which is a
// different claim: it says "this file still loads to the numbers we wrote
// down", not "this file loads to the same thing twice".
//
// The difference matters, because the ways determinism breaks are invisible to
// a recorded expectation that was itself produced by one of the runs: a hash
// seeded from a pointer, iteration over an unordered container, a static that
// survives between documents, an id counter that depends on what was opened
// first. Each of those produces a document that is self-consistent and
// disagrees with the same file opened in a different order.
//
// Two properties, because they fail differently:
//
//   1. One document regenerated twice — catches state that a regeneration
//      leaves behind and reads back.
//   2. The same file loaded into two independent documents — catches GLOBAL
//      state, which the first property cannot see because both runs share it.
//
// The corpus is the golden files, so this grows automatically with every
// format bump instead of needing its own fixtures.
namespace {

// Everything about a body that a downstream consumer depends on, flattened to
// text so a mismatch names itself instead of being a bool.
//
// The element names are the load-bearing half. Volume agreeing while the face
// names differ is WORSE than an outright geometry change: every sketch on a
// face, every fillet, every mate connector and every CAM operation is stored
// as one of these names, so a document that renames its faces between loads
// silently repoints all of them (docs/modeling/toponaming.md).
std::string BodySignature(const Body& b) {
  std::ostringstream out;
  out.precision(12);
  if (!b.shape.IsValid()) {
    out << "invalid";
    return out.str();
  }
  glm::vec3 lo(0.0f), hi(0.0f);
  b.shape.GetBoundingBox(lo, hi);
  out << "createdBy=" << b.createdBy << " vol=" << b.shape.Volume() << " bbox=["
      << lo.x << "," << lo.y << "," << lo.z << ".." << hi.x << "," << hi.y
      << "," << hi.z << "]";
  // In the map's own order, deliberately: two documents whose names match as a
  // SET but not in order have a different element map, and an index-based
  // consumer would resolve them differently.
  out << " elements={";
  for (const auto& [index, name] : b.elementMap.names)
    out << index << ":" << name << ",";
  out << "}";
  return out.str();
}

std::vector<std::string> Signature(const Document& doc) {
  std::vector<std::string> sig;
  sig.reserve(doc.GetBodies().size());
  for (const Body& b : doc.GetBodies())
    sig.push_back(BodySignature(b));
  return sig;
}

// Every golden document, which is every format era we still load.
std::vector<fs::path> GoldenDocuments() {
  std::vector<fs::path> files;
  const fs::path dir = fs::path(TestSourceDir()) / "tests" / "golden";
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    // `.expect.json` files are the recorded expectations, not documents.
    if (entry.path().extension() != ".json")
      continue;
    if (name.find(".expect.") != std::string::npos)
      continue;
    files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

} // namespace

TEST_CASE("Determinism: regenerating a document twice changes nothing",
          "[determinism][serialization]") {
  const std::vector<fs::path> files = GoldenDocuments();
  REQUIRE(!files.empty());
  for (const fs::path& file : files) {
    INFO("golden " << file.filename().string());
    auto loaded = DocumentIO::Load(file.string());
    REQUIRE(loaded.has_value());
    Document& doc = **loaded;
    const std::vector<std::string> first = Signature(doc);
    // A second regeneration of the SAME document. The per-feature output cache
    // may make this cheap or even a no-op; what is asserted is that it cannot
    // make it different.
    doc.Regenerate();
    const std::vector<std::string> second = Signature(doc);
    REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); i++) {
      INFO("body " << i);
      CHECK(first[i] == second[i]);
    }
  }
}

TEST_CASE("Determinism: two independent loads agree, body for body",
          "[determinism][serialization]") {
  // The property the first test structurally cannot check: both regenerations
  // there share whatever global state exists, so a static that survives
  // between documents is invisible to it. Here the two documents are built
  // separately from the same bytes, and anything carried between them shows up
  // as a difference.
  const std::vector<fs::path> files = GoldenDocuments();
  REQUIRE(!files.empty());
  for (const fs::path& file : files) {
    INFO("golden " << file.filename().string());
    auto a = DocumentIO::Load(file.string());
    auto b = DocumentIO::Load(file.string());
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    const std::vector<std::string> sigA = Signature(**a);
    const std::vector<std::string> sigB = Signature(**b);
    REQUIRE(sigA.size() == sigB.size());
    for (size_t i = 0; i < sigA.size(); i++) {
      INFO("body " << i);
      CHECK(sigA[i] == sigB[i]);
    }
  }
}

TEST_CASE("Determinism: a save/load round trip rebuilds the same bodies",
          "[determinism][serialization]") {
  // The third way the promise is used: a document written out and read back is
  // the same model. This is what a checkpoint, a git commit and an autosave
  // all rely on, and it composes with the two above — a round trip that agrees
  // is only meaningful if a plain load is reproducible in the first place.
  const std::vector<fs::path> files = GoldenDocuments();
  REQUIRE(!files.empty());
  const fs::path tmp =
      fs::temp_directory_path() / "designer_determinism_roundtrip.json";
  for (const fs::path& file : files) {
    INFO("golden " << file.filename().string());
    auto original = DocumentIO::Load(file.string());
    REQUIRE(original.has_value());
    const std::vector<std::string> before = Signature(**original);
    REQUIRE(DocumentIO::Save(**original, tmp.string()).has_value());
    auto reloaded = DocumentIO::Load(tmp.string());
    REQUIRE(reloaded.has_value());
    const std::vector<std::string> after = Signature(**reloaded);
    REQUIRE(before.size() == after.size());
    for (size_t i = 0; i < before.size(); i++) {
      INFO("body " << i);
      CHECK(before[i] == after[i]);
    }
    std::error_code ec;
    fs::remove(tmp, ec);
  }
}
