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
