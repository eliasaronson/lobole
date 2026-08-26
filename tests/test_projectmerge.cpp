#include <catch2/catch_test_macros.hpp>

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectMerge.h"
#include "core/io/Serialization.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// A scratch project directory that is also a git repo, so a real merge can
// be driven through it. Per-pid: ctest runs these in parallel.
struct ScratchProject {
  fs::path dir;

  explicit ScratchProject(const char* name) {
    dir = fs::temp_directory_path() / ("designer-merge-" + std::string(name) +
                                       "-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  // Never throws: git's background maintenance races the delete.
  ~ScratchProject() {
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec)
      fs::remove_all(dir, ec);
  }

  std::string path() const { return dir.string(); }

  void Write(const std::string& rel, const std::string& text) const {
    const fs::path p = dir / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
  }
  void WriteJson(const std::string& rel, const json& value) const {
    Write(rel, value.dump(2) + "\n");
  }
  bool Has(const std::string& rel) const { return fs::exists(dir / rel); }
  std::string Read(const std::string& rel) const {
    std::ifstream in(dir / rel);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }
};

// A one-part project: parts/Stand holding a REAL sketch + extrude, written
// by the real writer. Hand-rolled feature JSON was a trap here — it parsed
// and then failed to load for a missing `plane`, which made Verify look
// broken when it was the fixture that was.
void SeedProject(const ScratchProject& p) {
  Document doc;
  auto sketch =
      std::make_unique<SketchFeature>("Profile", SketchPlane(PlaneType::XY));
  sketch->AddEntity(sketch::Line{{0, 0}, {20, 0}});
  sketch->AddEntity(sketch::Line{{20, 0}, {20, 20}});
  sketch->AddEntity(sketch::Line{{20, 20}, {0, 20}});
  sketch->AddEntity(sketch::Line{{0, 20}, {0, 0}});
  SketchFeature* sketchPtr = sketch.get();
  doc.AddFeature(std::move(sketch));
  const uint64_t sketchId = sketchPtr->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Boss", sketchId, 10.0,
                                                  ExtrudeMode::NewBody));

  const fs::path partDir = p.dir / "parts/Stand";
  fs::create_directories(partDir);
  REQUIRE(DocumentIO::Save(doc, partDir.string()).has_value());

  p.WriteJson("project.json",
              json{{"format", "designer-project"},
                   {"version", 2},
                   {"active", 1},
                   {"documents", json::array({json{{"dir", "parts/Stand"},
                                                   {"id", 1},
                                                   {"name", "Stand"},
                                                   {"type", "part"}}})}});
}

// The ids the seeded part actually used, read back off disk — the tests that
// edit a feature need to name its file, and hard-coding 1 and 2 would be a
// guess about how AddFeature numbers things.
std::vector<uint64_t> SeededFeatureIds(const ScratchProject& p) {
  const json index = json::parse(p.Read("parts/Stand/index.json"));
  return index["features"].get<std::vector<uint64_t>>();
}

// The file the writer actually produced for `id` ("<id>-<kebab type>.json").
// Globbing beats assuming the suffix.
std::string FeatureFile(const ScratchProject& p, uint64_t id) {
  const fs::path dir = p.dir / "parts/Stand/features";
  for (const auto& entry : fs::directory_iterator(dir)) {
    const std::string filename = entry.path().filename().string();
    if (filename.rfind(std::to_string(id) + "-", 0) == 0)
      return "parts/Stand/features/" + filename;
  }
  return "";
}

// Change one field of a real feature, leaving the rest of the file exactly as
// the writer produced it — so the document still loads afterwards and the
// merge tests can end with a Verify().
void SetField(const ScratchProject& p, uint64_t id, const std::string& key,
              const json& value) {
  const std::string rel = FeatureFile(p, id);
  REQUIRE_FALSE(rel.empty());
  json item = json::parse(p.Read(rel));
  item[key] = value;
  p.WriteJson(rel, item);
}

} // namespace

// ---------------------------------------------------------------------------
// The union merge — the class with a right answer, and the one "pick a side"
// gets wrong. No git needed: it is a pure function of three texts.
// ---------------------------------------------------------------------------

TEST_CASE("UnionIndex keeps both sides' additions", "[merge]") {
  const std::string base =
      json{{"features", json::array({1, 2})}, {"version", 53}}.dump();
  const std::string ours =
      json{{"features", json::array({1, 2, 10})}, {"version", 53}}.dump();
  const std::string theirs =
      json{{"features", json::array({1, 2, 20})}, {"version", 53}}.dump();

  const json merged = json::parse(ProjectMerge::UnionIndex(base, ours, theirs));
  // Taking either side alone would drop the other's new feature while its
  // file sits on disk — the document silently loses it.
  CHECK(merged["features"] == json::array({1, 2, 10, 20}));
}

TEST_CASE("UnionIndex keeps a deletion deleted", "[merge]") {
  const std::string base = json{{"features", json::array({1, 2, 3})}}.dump();
  const std::string ours = json{{"features", json::array({1, 3})}}.dump();
  const std::string theirs =
      json{{"features", json::array({1, 2, 3, 4})}}.dump();

  const json merged = json::parse(ProjectMerge::UnionIndex(base, ours, theirs));
  // 2 was in the base and we removed it: a deletion, not their addition.
  // 4 was never in the base: their addition, not our deletion.
  CHECK(merged["features"] == json::array({1, 3, 4}));
}

TEST_CASE("UnionIndex puts an inserted feature where they put it", "[merge]") {
  // Order is not cosmetic: an extrude regenerates after the sketch it uses.
  const std::string base = json{{"features", json::array({1, 2, 3})}}.dump();
  const std::string ours = json{{"features", json::array({1, 2, 3, 9})}}.dump();
  const std::string theirs =
      json{{"features", json::array({1, 7, 8, 2, 3})}}.dump();

  const json merged = json::parse(ProjectMerge::UnionIndex(base, ours, theirs));
  CHECK(merged["features"] == json::array({1, 7, 8, 2, 3, 9}));
}

TEST_CASE("UnionIndex merges every id-keyed collection", "[merge]") {
  const std::string base = json{
      {"occurrences", json::array({1})},
      {"mates",
       json::array({5})}}.dump();
  const std::string ours = json{
      {"occurrences", json::array({1, 2})},
      {"mates",
       json::array({5})}}.dump();
  const std::string theirs = json{
      {"occurrences", json::array({1, 3})},
      {"mates",
       json::array({5, 6})}}.dump();

  const json merged = json::parse(ProjectMerge::UnionIndex(base, ours, theirs));
  CHECK(merged["occurrences"] == json::array({1, 2, 3}));
  CHECK(merged["mates"] == json::array({5, 6}));
}

TEST_CASE("UnionIndex takes the side that changed a scalar", "[merge]") {
  const std::string base =
      json{{"features", json::array({1})}, {"rollback", 0}}.dump();
  const std::string ours =
      json{{"features", json::array({1})}, {"rollback", 0}}.dump();
  const std::string theirs =
      json{{"features", json::array({1})}, {"rollback", 1}}.dump();

  const json merged = json::parse(ProjectMerge::UnionIndex(base, ours, theirs));
  CHECK(merged["rollback"] == 1); // only they moved it
}

// ---------------------------------------------------------------------------
// Scan — what a conflict IS, in the words the tabs use
// ---------------------------------------------------------------------------

TEST_CASE("Scan describes a feature conflict in CAD language", "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("describe");
  SeedProject(p);
  const uint64_t extrude = SeededFeatureIds(p).back();
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string main = GitVault::CurrentBranch(p.path());

  REQUIRE(GitVault::CreateBranch(p.path(), "alice", true));
  SetField(p, extrude, "distance", 12.0);
  REQUIRE(GitVault::CommitAll(p.path(), "alice: 12 mm"));

  REQUIRE(GitVault::SwitchBranch(p.path(), main));
  REQUIRE(GitVault::CreateBranch(p.path(), "bob", true));
  SetField(p, extrude, "distance", 18.0);
  REQUIRE(GitVault::CommitAll(p.path(), "bob: 18 mm"));

  REQUIRE(GitVault::SwitchBranch(p.path(), "alice"));
  std::string error;
  REQUIRE(GitVault::Merge(p.path(), "bob", &error) ==
          GitVault::MergeStatus::Conflicted);

  const ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  CHECK(report.inMerge);
  REQUIRE(report.conflicts.size() == 1);

  const ProjectMerge::Conflict& c = report.conflicts[0];
  CHECK(c.kind == ProjectMerge::ConflictKind::Item);
  // Not "features/2-extrude.json is conflicted".
  CHECK(c.itemKind == "Extrude");
  CHECK(c.name == "Boss");
  CHECK(c.document == "Stand");
  CHECK(c.documentKind == "part");
  CHECK(c.id == 2);
  REQUIRE(c.fields.size() == 1); // only `distance` differs
  CHECK(c.fields[0].field == "distance");
  CHECK(c.fields[0].ours == "12");
  CHECK(c.fields[0].theirs == "18");
  CHECK(c.fields[0].base == "10");
  CHECK(c.summary.find("Extrude \"Boss\" in part Stand") != std::string::npos);
  CHECK(c.summary.find("distance 12 vs 18") != std::string::npos);
}

// The conflict the collaboration round actually produced: two people place
// the same part in different spots. It used to read as a truncated JSON dump
// — `transform {"orientation":[0.0,0.0,0.0,1.0],"position":[50.0,0…` — which
// tells you a transform is involved and nothing about what to pick.
TEST_CASE("An occurrence conflict reads as a place, not as JSON", "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("occdescribe");
  SeedProject(p);
  const auto occFile = [&](double x, const json& orientation) {
    p.WriteJson(
        "assemblies/Frame/occurrences/4-pin.json",
        json{{"id", 4},
             {"name", "P1"},
             {"part_id", 1},
             {"transform", json{{"position", json::array({x, 0.0, 0.0})},
                                {"orientation", orientation}}}});
  };
  const json noTurn = json::array({0.0, 0.0, 0.0, 1.0});
  p.WriteJson("assemblies/Frame/index.json",
              json{{"occurrences", json::array({4})}});
  occFile(0.0, noTurn);
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string main = GitVault::CurrentBranch(p.path());

  REQUIRE(GitVault::CreateBranch(p.path(), "alice", true));
  occFile(50.0, noTurn);
  REQUIRE(GitVault::CommitAll(p.path(), "alice: moved it"));

  REQUIRE(GitVault::SwitchBranch(p.path(), main));
  REQUIRE(GitVault::CreateBranch(p.path(), "bob", true));
  // A quarter turn about Z, and somewhere else entirely.
  occFile(-12.5, json::array({0.0, 0.0, 0.70710678, 0.70710678}));
  REQUIRE(GitVault::CommitAll(p.path(), "bob: moved and turned it"));

  REQUIRE(GitVault::SwitchBranch(p.path(), "alice"));
  std::string error;
  REQUIRE(GitVault::Merge(p.path(), "bob", &error) ==
          GitVault::MergeStatus::Conflicted);

  const ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  REQUIRE(report.conflicts.size() == 1);
  const ProjectMerge::Conflict& c = report.conflicts[0];
  CHECK(c.document == "Frame");
  CHECK(c.documentKind == "assembly");
  REQUIRE(c.fields.size() == 1);
  CHECK(c.fields[0].field == "transform");
  CHECK(c.fields[0].ours == "(50, 0, 0)"); // no rotation: no noise about it
  CHECK(c.fields[0].theirs == "(-12.5, 0, 0) turned 90° about Z");
  CHECK(c.fields[0].base == "(0, 0, 0)");
  CHECK(c.summary.find("{") == std::string::npos); // no JSON anywhere in it
}

// A rename on one side and a dimension change on the other is NOT a
// conflict: one key per line, sorted, so git merges it itself. If this ever
// starts conflicting, the format's diffability has regressed.
TEST_CASE("Different fields of one feature never reach the panel", "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("disjointfields");
  SeedProject(p);
  const uint64_t extrude = SeededFeatureIds(p).back();
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string main = GitVault::CurrentBranch(p.path());

  REQUIRE(GitVault::CreateBranch(p.path(), "alice", true));
  SetField(p, extrude, "distance", 12.0);
  REQUIRE(GitVault::CommitAll(p.path(), "alice changes the distance"));

  REQUIRE(GitVault::SwitchBranch(p.path(), main));
  REQUIRE(GitVault::CreateBranch(p.path(), "bob", true));
  SetField(p, extrude, "name", "Mounting boss");
  REQUIRE(GitVault::CommitAll(p.path(), "bob renames it"));

  REQUIRE(GitVault::SwitchBranch(p.path(), "alice"));
  std::string error;
  CHECK(GitVault::Merge(p.path(), "bob", &error) ==
        GitVault::MergeStatus::Merged);

  const json merged = json::parse(p.Read(FeatureFile(p, extrude)));
  CHECK(merged["distance"] == 12.0);        // alice's
  CHECK(merged["name"] == "Mounting boss"); // bob's
  CHECK(ProjectMerge::Scan(p.path()).Empty());
  CHECK(ProjectMerge::Verify(p.path()).empty());
}

TEST_CASE("Scan finds the duplicate ids git cannot see", "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("dupes");
  SeedProject(p);
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string main = GitVault::CurrentBranch(p.path());

  // Both sides add a feature offline; nextId++ hands them both 15.
  REQUIRE(GitVault::CreateBranch(p.path(), "alice", true));
  p.WriteJson("parts/Stand/features/15-box.json",
              json{{"id", 15}, {"type", "Box"}, {"name", "Alice box"}});
  REQUIRE(GitVault::CommitAll(p.path(), "alice adds a box"));

  REQUIRE(GitVault::SwitchBranch(p.path(), main));
  REQUIRE(GitVault::CreateBranch(p.path(), "bob", true));
  p.WriteJson("parts/Stand/features/15-hole.json",
              json{{"id", 15}, {"type", "Hole"}, {"name", "Bob hole"}});
  REQUIRE(GitVault::CommitAll(p.path(), "bob adds a hole"));

  REQUIRE(GitVault::SwitchBranch(p.path(), "alice"));
  std::string error;
  // Two adds, no overlap: git is perfectly happy.
  REQUIRE(GitVault::Merge(p.path(), "bob", &error) ==
          GitVault::MergeStatus::Merged);
  REQUIRE(GitVault::Unmerged(p.path()).empty());

  const ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  CHECK_FALSE(report.inMerge); // the merge is DONE and still wrong
  REQUIRE(report.conflicts.size() == 1);
  const ProjectMerge::Conflict& c = report.conflicts[0];
  CHECK(c.kind == ProjectMerge::ConflictKind::DuplicateId);
  CHECK(c.id == 15);
  CHECK(c.document == "Stand");
  CHECK(c.summary.find("claim id 15") != std::string::npos);
  CHECK(report.NeedsDecision() == 1);
}

TEST_CASE("An index conflict resolves by union, not by a pick", "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("indexunion");
  SeedProject(p);
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string main = GitVault::CurrentBranch(p.path());

  REQUIRE(GitVault::CreateBranch(p.path(), "alice", true));
  p.WriteJson("parts/Stand/index.json",
              json{{"format", "designer-document"},
                   {"version", 53},
                   {"features", json::array({1, 2, 10})}});
  p.WriteJson("parts/Stand/features/10-fillet.json",
              json{{"id", 10}, {"type", "Fillet"}, {"name", "Edge break"}});
  REQUIRE(GitVault::CommitAll(p.path(), "alice adds a fillet"));

  REQUIRE(GitVault::SwitchBranch(p.path(), main));
  REQUIRE(GitVault::CreateBranch(p.path(), "bob", true));
  p.WriteJson("parts/Stand/index.json",
              json{{"format", "designer-document"},
                   {"version", 53},
                   {"features", json::array({1, 2, 20})}});
  p.WriteJson("parts/Stand/features/20-chamfer.json",
              json{{"id", 20}, {"type", "Chamfer"}, {"name", "Lead-in"}});
  REQUIRE(GitVault::CommitAll(p.path(), "bob adds a chamfer"));

  REQUIRE(GitVault::SwitchBranch(p.path(), "alice"));
  std::string error;
  REQUIRE(GitVault::Merge(p.path(), "bob", &error) ==
          GitVault::MergeStatus::Conflicted);

  ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  REQUIRE(report.conflicts.size() == 1);
  const ProjectMerge::Conflict& c = report.conflicts[0];
  CHECK(c.kind == ProjectMerge::ConflictKind::Order);
  CHECK(c.autoResolvable); // nobody should be asked this
  CHECK(c.document == "Stand");

  REQUIRE(ProjectMerge::AutoResolve(p.path(), c, &error));
  REQUIRE(GitVault::CommitMerge(p.path(), "merge bob", &error));

  const json index = json::parse(p.Read("parts/Stand/index.json"));
  // Both features exist on disk; both must be listed, or one silently
  // vanishes from the document.
  const auto ids = index["features"].get<std::vector<uint64_t>>();
  CHECK(std::find(ids.begin(), ids.end(), 10u) != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), 20u) != ids.end());
  CHECK(p.Has("parts/Stand/features/10-fillet.json"));
  CHECK(p.Has("parts/Stand/features/20-chamfer.json"));
}

TEST_CASE("TakeField keeps the other side's untouched edits", "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("takefield");
  SeedProject(p);
  const uint64_t extrude = SeededFeatureIds(p).back();
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string main = GitVault::CurrentBranch(p.path());

  // Both sides change BOTH fields, so git cannot merge it and every field is
  // genuinely contested.
  REQUIRE(GitVault::CreateBranch(p.path(), "alice", true));
  SetField(p, extrude, "name", "Alice boss");
  SetField(p, extrude, "distance", 12.0);
  REQUIRE(GitVault::CommitAll(p.path(), "alice"));

  REQUIRE(GitVault::SwitchBranch(p.path(), main));
  REQUIRE(GitVault::CreateBranch(p.path(), "bob", true));
  SetField(p, extrude, "name", "Bob boss");
  SetField(p, extrude, "distance", 18.0);
  REQUIRE(GitVault::CommitAll(p.path(), "bob"));

  REQUIRE(GitVault::SwitchBranch(p.path(), "alice"));
  std::string error;
  REQUIRE(GitVault::Merge(p.path(), "bob", &error) ==
          GitVault::MergeStatus::Conflicted);

  const ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  REQUIRE(report.conflicts.size() == 1);
  CHECK(report.conflicts[0].fields.size() == 2);

  // Take THEIR distance; keep OUR name. Neither whole-side pick can do this.
  REQUIRE(ProjectMerge::TakeField(p.path(), report.conflicts[0], "distance",
                                  GitVault::MergeSide::Theirs, &error));
  REQUIRE(GitVault::CommitMerge(p.path(), "merge", &error));

  const json merged = json::parse(p.Read(FeatureFile(p, extrude)));
  CHECK(merged["distance"] == 18.0);
  CHECK(merged["name"] == "Alice boss");
  // The bar: not "git exited 0" but "the project still works".
  CHECK(ProjectMerge::Verify(p.path()).empty());
}

// ---------------------------------------------------------------------------
// Duplicate ids — the class that actually ate work
// ---------------------------------------------------------------------------

TEST_CASE("Renumbering a duplicate keeps both features", "[merge]") {
  ScratchProject p("renumber");
  SeedProject(p);
  // The state a clean git merge leaves: two files, one id, and an index
  // listing that id twice.
  p.WriteJson("parts/Stand/index.json",
              json{{"format", "designer-document"},
                   {"version", 53},
                   {"features", json::array({1, 2, 15, 15})}});
  p.WriteJson("parts/Stand/features/15-box.json",
              json{{"id", 15}, {"type", "Box"}, {"name", "Alice box"}});
  p.WriteJson("parts/Stand/features/15-hole.json",
              json{{"id", 15}, {"type", "Hole"}, {"name", "Bob hole"}});

  ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  REQUIRE(report.conflicts.size() == 1);
  REQUIRE(report.conflicts[0].kind == ProjectMerge::ConflictKind::DuplicateId);

  std::string error;
  REQUIRE(
      ProjectMerge::RenumberDuplicate(p.path(), report.conflicts[0], &error));

  // Neither person's work is gone: one file kept 15, the other got a fresh
  // id, and the index lists both exactly once.
  const json index = json::parse(p.Read("parts/Stand/index.json"));
  const auto ids = index["features"].get<std::vector<uint64_t>>();
  CHECK(ids.size() == 4);
  CHECK(std::set<uint64_t>(ids.begin(), ids.end()).size() == 4);
  CHECK(std::count(ids.begin(), ids.end(), 15u) == 1);

  int featureFiles = 0;
  for (const auto& entry :
       fs::directory_iterator(p.dir / "parts/Stand/features"))
    if (entry.path().extension() == ".json")
      ++featureFiles;
  CHECK(featureFiles == 4); // 1, 2, 15, and the renumbered one

  // And the project is clean afterwards.
  CHECK(ProjectMerge::Scan(p.path()).Empty());
}

// The honest half: when something still points at the contested id, which
// feature it meant is unknowable, and guessing would quietly re-parent
// geometry.
TEST_CASE("Renumbering refuses while the id is still referenced", "[merge]") {
  ScratchProject p("refparent");
  SeedProject(p);
  const uint64_t extrude = SeededFeatureIds(p).back();
  p.WriteJson("parts/Stand/index.json",
              json{{"format", "designer-document"},
                   {"version", 53},
                   {"features", json::array({1, 2, 15, 15, 16})}});
  p.WriteJson("parts/Stand/features/15-sketch.json",
              json{{"id", 15}, {"type", "Sketch"}, {"name", "Alice profile"}});
  p.WriteJson("parts/Stand/features/15-box.json",
              json{{"id", 15}, {"type", "Box"}, {"name", "Bob box"}});
  // An extrude built on "sketch 15" — but there are two of those now.
  p.WriteJson("parts/Stand/features/16-extrude.json", json{{"id", 16},
                                                           {"type", "Extrude"},
                                                           {"name", "Boss"},
                                                           {"sketch_id", 15},
                                                           {"distance", 4.0}});

  ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  REQUIRE(report.conflicts.size() == 1);

  std::string error;
  CHECK_FALSE(
      ProjectMerge::RenumberDuplicate(p.path(), report.conflicts[0], &error));
  CHECK(error.find("16-extrude.json") != std::string::npos);
  CHECK(error.find("which of the two") != std::string::npos);
}

// A sketch entity numbered 15 is not a reference to feature 15: the id
// spaces are different, and treating them as one would refuse every
// renumber in a document that has sketches.
TEST_CASE("A sketch's own entity ids are not feature references", "[merge]") {
  ScratchProject p("entityspace");
  SeedProject(p);
  p.WriteJson("parts/Stand/index.json",
              json{{"format", "designer-document"},
                   {"version", 53},
                   {"features", json::array({1, 2, 15, 15})}});
  p.WriteJson("parts/Stand/features/15-box.json",
              json{{"id", 15}, {"type", "Box"}, {"name", "Alice box"}});
  p.WriteJson("parts/Stand/features/15-hole.json",
              json{{"id", 15}, {"type", "Hole"}, {"name", "Bob hole"}});
  // A sketch whose own entity numbering happens to include 15.
  p.WriteJson(
      "parts/Stand/features/30-sketch.json",
      json{
          {"id", 30},
          {"type", "Sketch"},
          {"name", "Other profile"},
          {"next_entity_id", 20},
          {"entities", json::array({json{{"entity_id", 15}, {"kind", "line"}},
                                    json{{"entity_id", 16}, {"kind", "arc"}}})},
          {"constraints",
           json::array({json{{"entity_id", 15}, {"kind", "horizontal"}}})}});

  ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  REQUIRE(report.conflicts.size() == 1);
  std::string error;
  CHECK(ProjectMerge::RenumberDuplicate(p.path(), report.conflicts[0], &error));
}

// ---------------------------------------------------------------------------
// The integrity pass — "the resolved project loads and regenerates", which is
// the bar. A resolution can parse perfectly and still be broken.
// ---------------------------------------------------------------------------

TEST_CASE("Verify passes a healthy project", "[merge]") {
  ScratchProject p("verifyok");
  SeedProject(p);
  CHECK(ProjectMerge::Verify(p.path()).empty());
}

TEST_CASE("Verify catches a dangling reference a pick left behind", "[merge]") {
  ScratchProject p("verifydangle");
  SeedProject(p);
  const uint64_t extrude = SeededFeatureIds(p).back();
  // Exactly what "take their side" produces when they deleted the sketch our
  // extrude was built on: every file is valid JSON, the index is consistent,
  // and the document is broken.
  const uint64_t sketch = SeededFeatureIds(p).front();
  fs::remove(p.dir / FeatureFile(p, sketch));
  json index = json::parse(p.Read("parts/Stand/index.json"));
  index["features"] = json::array({extrude});
  p.WriteJson("parts/Stand/index.json", index);

  const auto broken = ProjectMerge::Verify(p.path());
  REQUIRE(broken.size() == 1);
  CHECK(broken[0].document == "Stand");
  CHECK_FALSE(broken[0].reason.empty());
}

TEST_CASE("Verify names the document that will not load", "[merge]") {
  ScratchProject p("verifyparse");
  SeedProject(p);
  const uint64_t extrude = SeededFeatureIds(p).back();
  // Unresolved markers left in a file — the state the loader has refused
  // since the merge-safety work, now reported per document instead of as one
  // opaque open failure.
  p.Write(FeatureFile(p, extrude),
          "<<<<<<< ours\n{}\n=======\n{}\n>>>>>>> theirs\n");

  const auto broken = ProjectMerge::Verify(p.path());
  REQUIRE(broken.size() == 1);
  CHECK(broken[0].document == "Stand");
  CHECK(broken[0].reason.find("conflict markers") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Duplicate ids, attributed by merge side (F9 C3)
//
// The refusal is safe but, for a team, would become routine manual work: two
// people adding features offline both get the next id, and each one's new
// feature usually references their OTHER new feature. After a merge we know
// which side contributed which file, so those references are not ambiguous
// at all — their extrude meant their sketch.
// ---------------------------------------------------------------------------

namespace {

// Two people, offline, both adding a sketch + something built on it. The id
// counter hands them both the same pair of numbers.
void DivergeWithSameIds(ScratchProject& p, const std::string& main) {
  REQUIRE(GitVault::CreateBranch(p.path(), "alice", true));
  p.WriteJson("parts/Stand/features/15-sketch.json",
              json{{"id", 15}, {"type", "Sketch"}, {"name", "Alice profile"}});
  p.WriteJson("parts/Stand/features/16-extrude.json",
              json{{"id", 16},
                   {"type", "Extrude"},
                   {"name", "Alice boss"},
                   {"sketch_id", 15},
                   {"distance", 4.0}});
  json index = json::parse(p.Read("parts/Stand/index.json"));
  index["features"].push_back(15);
  index["features"].push_back(16);
  p.WriteJson("parts/Stand/index.json", index);
  REQUIRE(GitVault::CommitAll(p.path(), "alice adds a boss"));

  REQUIRE(GitVault::SwitchBranch(p.path(), main));
  REQUIRE(GitVault::CreateBranch(p.path(), "bob", true));
  p.WriteJson("parts/Stand/features/15-circle.json",
              json{{"id", 15}, {"type", "Sketch"}, {"name", "Bob profile"}});
  p.WriteJson("parts/Stand/features/16-hole.json", json{{"id", 16},
                                                        {"type", "Hole"},
                                                        {"name", "Bob hole"},
                                                        {"sketch_id", 15},
                                                        {"diameter", 3.0}});
  json bobIndex = json::parse(p.Read("parts/Stand/index.json"));
  bobIndex["features"].push_back(15);
  bobIndex["features"].push_back(16);
  p.WriteJson("parts/Stand/index.json", bobIndex);
  REQUIRE(GitVault::CommitAll(p.path(), "bob adds a hole"));

  REQUIRE(GitVault::SwitchBranch(p.path(), "alice"));
}

} // namespace

TEST_CASE("A duplicate id renumbers, and its own side's references follow",
          "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("attributed");
  SeedProject(p);
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string main = GitVault::CurrentBranch(p.path());
  DivergeWithSameIds(p, main);

  std::string error;
  // The index conflicts (both appended); the FEATURE files do not — different
  // type suffixes, so git takes all four without a word.
  const GitVault::MergeStatus status = GitVault::Merge(p.path(), "bob", &error);
  if (status == GitVault::MergeStatus::Conflicted) {
    for (const auto& c : ProjectMerge::Scan(p.path()).conflicts)
      if (c.autoResolvable)
        REQUIRE(ProjectMerge::AutoResolve(p.path(), c, &error));
    REQUIRE(GitVault::CommitMerge(p.path(), "merge bob", &error));
  }

  ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  // Two duplicate ids: 15 and 16.
  REQUIRE(report.conflicts.size() == 2);
  for (const auto& c : report.conflicts)
    CHECK(c.kind == ProjectMerge::ConflictKind::DuplicateId);

  // This is the case that used to refuse: 16-hole references 15, and there
  // are two 15s.
  for (const auto& c : ProjectMerge::Scan(p.path()).conflicts)
    REQUIRE(ProjectMerge::RenumberDuplicate(p.path(), c, &error));

  CHECK(ProjectMerge::Scan(p.path()).Empty());

  // Nobody's work was lost, and — the whole point — nobody's reference was
  // re-parented onto the other person's sketch.
  const auto readByName = [&](const std::string& name) {
    for (const auto& entry :
         fs::directory_iterator(p.dir / "parts/Stand/features")) {
      const json item = json::parse(
          p.Read("parts/Stand/features/" + entry.path().filename().string()));
      if (item.value("name", "") == name)
        return item;
    }
    return json();
  };
  const json aliceSketch = readByName("Alice profile");
  const json aliceBoss = readByName("Alice boss");
  const json bobSketch = readByName("Bob profile");
  const json bobHole = readByName("Bob hole");
  REQUIRE(aliceSketch.is_object());
  REQUIRE(bobSketch.is_object());
  CHECK(aliceSketch["id"] != bobSketch["id"]);
  CHECK(aliceBoss["sketch_id"] == aliceSketch["id"]);
  CHECK(bobHole["sketch_id"] == bobSketch["id"]);
}

// The scan's ORDER is load-bearing. `directory_iterator` hands files back in
// filesystem order, so the same merge resolved on two machines took two
// different orders and only one of them worked: renumbering an id renames its
// file, and a reference always points backwards at a LOWER id, so the highest
// id first renames a referrer whose side can then no longer be attributed.
// This is the bug CI caught while the same test passed on the dev machine.
TEST_CASE("Duplicate ids are reported lowest first, and that order resolves",
          "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("ordered");
  SeedProject(p);
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string main = GitVault::CurrentBranch(p.path());
  DivergeWithSameIds(p, main);

  std::string error;
  const GitVault::MergeStatus status = GitVault::Merge(p.path(), "bob", &error);
  if (status == GitVault::MergeStatus::Conflicted) {
    for (const auto& c : ProjectMerge::Scan(p.path()).conflicts)
      if (c.autoResolvable)
        REQUIRE(ProjectMerge::AutoResolve(p.path(), c, &error));
    REQUIRE(GitVault::CommitMerge(p.path(), "merge bob", &error));
  }

  const ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  REQUIRE(report.conflicts.size() == 2);
  CHECK(report.conflicts[0].id < report.conflicts[1].id);

  // Taken the other way round — the order a filesystem could hand back, and
  // the order a person can still click in — the second one REFUSES, and the
  // refusal names the way out instead of sending them to a text editor.
  REQUIRE(
      ProjectMerge::RenumberDuplicate(p.path(), report.conflicts[1], &error));
  CHECK_FALSE(
      ProjectMerge::RenumberDuplicate(p.path(), report.conflicts[0], &error));
  CHECK(error.find("LOWEST id first") != std::string::npos);
}

// The honest half survives: with no merge to attribute against — a project
// that arrived by rsync, a zip, or a hand-edit — a reference still cannot say
// which of the two it meant.
TEST_CASE("Without a merge to attribute against, it still refuses", "[merge]") {
  ScratchProject p("unattributed");
  SeedProject(p);
  p.WriteJson("parts/Stand/index.json",
              json{{"format", "designer-document"},
                   {"version", 53},
                   {"features", json::array({1, 2, 15, 15, 16})}});
  p.WriteJson("parts/Stand/features/15-sketch.json",
              json{{"id", 15}, {"type", "Sketch"}, {"name", "One"}});
  p.WriteJson("parts/Stand/features/15-circle.json",
              json{{"id", 15}, {"type", "Sketch"}, {"name", "Two"}});
  p.WriteJson("parts/Stand/features/16-extrude.json", json{{"id", 16},
                                                           {"type", "Extrude"},
                                                           {"name", "Boss"},
                                                           {"sketch_id", 15},
                                                           {"distance", 4.0}});

  ProjectMerge::Report report = ProjectMerge::Scan(p.path());
  REQUIRE(report.conflicts.size() == 1);
  std::string error;
  CHECK_FALSE(
      ProjectMerge::RenumberDuplicate(p.path(), report.conflicts[0], &error));
  CHECK(error.find("which of the two") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Describing a change (F11 G1)
//
// The History panel showed sixty rows saying "save". These are the strings
// that replace them — a pure function of a commit's file list, so the cases
// live here rather than behind git.
// ---------------------------------------------------------------------------

TEST_CASE("A change summary reads as the tabs do", "[merge]") {
  using ProjectMerge::SummarizePaths;

  // The common save: one feature, and the caller read it to name it.
  CHECK(SummarizePaths({"parts/Stand/features/15-extrude.json"}, "Extrude",
                       "Boss") == "Extrude \"Boss\" in part Stand");
  // The same file without a name read is still better than "save".
  CHECK(SummarizePaths({"parts/Stand/features/15-extrude.json"}) ==
        "1 feature in part Stand");

  // Several in one document.
  CHECK(SummarizePaths({"parts/Dock/features/1-sketch.json",
                        "parts/Dock/features/2-extrude.json",
                        "parts/Dock/features/3-hole.json"}) ==
        "3 features in part Dock");

  // The order changed, which is a real thing to have done.
  CHECK(SummarizePaths({"parts/Dock/index.json"}) == "the order in part Dock");

  // Mixed collections, and an assembly reads as an assembly.
  CHECK(SummarizePaths({"assemblies/Set/occurrences/4-body.json",
                        "assemblies/Set/mates/7-fastened.json"}) ==
        "1 mate, 1 occurrence in assembly Set");

  // Two documents join; more than two would be a wall.
  CHECK(SummarizePaths(
            {"parts/A/features/1-box.json", "drawings/D/dims/2-linear.json"}) ==
        "1 feature in part A and 1 dimension in drawing D");
  CHECK(SummarizePaths(
            {"parts/A/features/1-box.json", "parts/B/features/1-box.json",
             "parts/C/features/1-box.json"}) == "3 documents changed");
}

// Baked geometry and the repository hints are not changes anyone made, and a
// row that said "3 files in Dock" every time you saved would be worse than
// "save".
TEST_CASE("A change summary ignores derived files", "[merge]") {
  using ProjectMerge::SummarizePaths;
  CHECK(SummarizePaths({"parts/Stand/geometry/0.brep",
                        "parts/Stand/geometry/index.json"}) == "");
  CHECK(SummarizePaths({".gitattributes", ".gitignore"}) == "");
  CHECK(SummarizePaths({"parts/Stand/features/1-box.json",
                        "parts/Stand/geometry/0.brep"}) ==
        "1 feature in part Stand");
  // Which documents the project HAS is a change, and says so.
  CHECK(SummarizePaths({"project.json"}) == "the project");
}

// Only one changed item is worth a blob read to name; anything else is
// counted from the paths alone, which is what keeps a 60-row page to one
// `git log`.
TEST_CASE("Only a lone item is worth naming", "[merge]") {
  using ProjectMerge::IsSingleItem;
  CHECK(IsSingleItem({"parts/Stand/features/15-extrude.json"}));
  CHECK(IsSingleItem({"features/15-extrude.json"})); // a lone document
  CHECK_FALSE(IsSingleItem({"parts/Stand/index.json"}));
  CHECK_FALSE(IsSingleItem({"project.json"}));
  CHECK_FALSE(IsSingleItem(
      {"parts/Stand/features/1-box.json", "parts/Stand/features/2-box.json"}));
}

// ---------------------------------------------------------------------------
// Diffing two versions (F11 G2)
//
// Not a merge: two revisions are two revisions. The field-level layer is
// DiffFields, unchanged from the Conflicts panel — two blobs are two blobs.
// ---------------------------------------------------------------------------

TEST_CASE("A diff between versions reads as items and fields", "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("diffversions");
  SeedProject(p);
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string first = GitVault::Log(p.path())[0].hash;
  const uint64_t extrude = SeededFeatureIds(p).back();

  // Change one, add one, remove one — the three things a diff has to say.
  SetField(p, extrude, "distance", 18.0);
  p.WriteJson("parts/Stand/features/40-fillet.json",
              json{{"id", 40}, {"type", "Fillet"}, {"name", "Edge break"}});
  const uint64_t sketch = SeededFeatureIds(p).front();
  fs::remove(p.dir / FeatureFile(p, sketch));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string second = GitVault::Log(p.path())[0].hash;

  const auto changes = ProjectMerge::DiffVersions(p.path(), first, second);
  REQUIRE(changes.size() >= 3);

  const auto find = [&](ProjectMerge::ItemChange::How how) {
    for (const auto& c : changes)
      if (c.how == how && c.itemKind != "Order")
        return c;
    return ProjectMerge::ItemChange{};
  };

  const auto changed = find(ProjectMerge::ItemChange::How::Changed);
  CHECK(changed.itemKind == "Extrude");
  CHECK(changed.document == "Stand");
  REQUIRE(changed.fields.size() == 1);
  CHECK(changed.fields[0].field == "distance");
  // "to", not "vs" — a diff has a direction where a conflict has two sides.
  CHECK(changed.summary.find("distance 10 to 18") != std::string::npos);
  CHECK(changed.summary.find("in part Stand") != std::string::npos);

  CHECK(find(ProjectMerge::ItemChange::How::Added).summary.find("added") !=
        std::string::npos);
  CHECK(find(ProjectMerge::ItemChange::How::Removed).summary.find("removed") !=
        std::string::npos);
}

// "What have I changed since that version" — the working tree is the other
// side, and it has no revision to name.
TEST_CASE("A diff can end at the working tree", "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("diffworking");
  SeedProject(p);
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string saved = GitVault::Log(p.path())[0].hash;
  const uint64_t extrude = SeededFeatureIds(p).back();

  CHECK(ProjectMerge::DiffVersions(p.path(), saved, "").empty());

  SetField(p, extrude, "distance", 33.0); // unsaved, uncommitted
  const auto changes = ProjectMerge::DiffVersions(p.path(), saved, "");
  REQUIRE(changes.size() == 1);
  CHECK(changes[0].itemKind == "Extrude");
  CHECK(changes[0].summary.find("distance 10 to 33") != std::string::npos);
}

// Baked geometry is not a change anyone made.
TEST_CASE("A diff ignores derived files", "[merge]") {
  if (!GitVault::Available())
    SKIP("git is not on PATH");
  ScratchProject p("diffderived");
  SeedProject(p);
  REQUIRE(GitVault::Init(p.path()));
  REQUIRE(GitVault::CommitAll(p.path(), "save"));
  const std::string saved = GitVault::Log(p.path())[0].hash;

  p.Write("parts/Stand/geometry/0.brep", "rebuilt");
  p.Write("parts/Stand/geometry/index.json", R"({"chain":"x"})");
  CHECK(ProjectMerge::DiffVersions(p.path(), saved, "").empty());
}
