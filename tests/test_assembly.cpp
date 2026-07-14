#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Occurrence.h"
#include "core/Serialization.h"

#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <fstream>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

// A small assembly: three occurrences, two of them of the same part (id 2),
// with distinct transforms, visibility, and a fixed anchor.
AssemblyDocument SampleAssembly() {
  AssemblyDocument asmDoc;

  Occurrence base;
  base.name = "base";
  base.partId = 1;
  base.fixed = true; // grounded anchor
  asmDoc.AddOccurrence(base);

  Occurrence arm;
  arm.name = "arm";
  arm.partId = 2;
  arm.position = glm::vec3(10.0f, 0.0f, 0.0f);
  arm.orientation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
  asmDoc.AddOccurrence(arm);

  Occurrence arm2;
  arm2.name = "arm (2)";
  arm2.partId = 2; // same part → the instancing win
  arm2.position = glm::vec3(-5.0f, 3.0f, 1.0f);
  arm2.visible = false;
  arm2.partVersion = "abc123"; // git pin round-trips even before F5 uses it
  asmDoc.AddOccurrence(arm2);

  return asmDoc;
}

void ExpectSameAssembly(const AssemblyDocument& a, const AssemblyDocument& b) {
  REQUIRE(a.OccurrenceCount() == b.OccurrenceCount());
  const auto& oa = a.GetOccurrences();
  const auto& ob = b.GetOccurrences();
  for (size_t i = 0; i < oa.size(); i++) {
    CHECK(oa[i].id == ob[i].id); // order + ids preserved
    CHECK(oa[i].name == ob[i].name);
    CHECK(oa[i].partId == ob[i].partId);
    CHECK(oa[i].partVersion == ob[i].partVersion);
    CHECK(oa[i].visible == ob[i].visible);
    CHECK(oa[i].fixed == ob[i].fixed);
    for (int k = 0; k < 3; k++)
      CHECK_THAT(oa[i].position[k], WithinAbs(ob[i].position[k], 1e-6f));
    for (int k = 0; k < 4; k++)
      CHECK_THAT(oa[i].orientation[k], WithinAbs(ob[i].orientation[k], 1e-6f));
  }
}

} // namespace

TEST_CASE("AssemblyDocument assigns stable ids and finds/removes", "[assembly]") {
  AssemblyDocument asmDoc = SampleAssembly();
  REQUIRE(asmDoc.OccurrenceCount() == 3);
  // Ids grow from 1, unique.
  CHECK(asmDoc.GetOccurrences()[0].id == 1);
  CHECK(asmDoc.GetOccurrences()[1].id == 2);
  CHECK(asmDoc.GetOccurrences()[2].id == 3);

  REQUIRE(asmDoc.FindById(2) != nullptr);
  CHECK(asmDoc.FindById(2)->name == "arm");
  CHECK(asmDoc.FindById(999) == nullptr);

  REQUIRE(asmDoc.RemoveOccurrence(2));
  CHECK(asmDoc.OccurrenceCount() == 2);
  CHECK(asmDoc.FindById(2) == nullptr);
  CHECK_FALSE(asmDoc.RemoveOccurrence(2)); // already gone

  // A new add never reuses id 2 (F1 stable-id contract).
  Occurrence extra;
  extra.partId = 3;
  const uint64_t newId = asmDoc.AddOccurrence(extra);
  CHECK(newId == 4);
}

TEST_CASE("Assembly round-trips through the single-file form", "[assembly]") {
  AssemblyDocument original = SampleAssembly();
  const fs::path path =
      fs::temp_directory_path() / "designer_asm_roundtrip.json";
  std::string error;
  REQUIRE(DocumentIO::SaveAssembly(original, path.string(), &error));

  // Sanity: the file declares the assembly discriminator + shared version.
  {
    std::ifstream in(path);
    nlohmann::json j = nlohmann::json::parse(in);
    CHECK(j.at("format") == "designer-document");
    CHECK(j.at("kind") == "assembly");
    CHECK(j.at("occurrences").size() == 3);
  }

  auto reloaded = DocumentIO::LoadAssembly(path.string(), &error);
  REQUIRE(reloaded != nullptr);
  ExpectSameAssembly(original, *reloaded);
  fs::remove(path);
}

TEST_CASE("Assembly round-trips through the directory form", "[assembly]") {
  AssemblyDocument original = SampleAssembly();
  const fs::path dir = fs::temp_directory_path() / "designer_asm_roundtrip.dsgn";
  fs::remove_all(dir);
  std::string error;
  REQUIRE(DocumentIO::SaveAssembly(original, dir.string(), &error));

  // One file per occurrence, named by stable id; index lists the order.
  CHECK(fs::exists(dir / "index.json"));
  CHECK(fs::exists(dir / "occurrences" / "1-base.json"));
  CHECK(fs::exists(dir / "occurrences" / "2-arm.json"));

  auto reloaded = DocumentIO::LoadAssembly(dir.string(), &error);
  REQUIRE(reloaded != nullptr);
  ExpectSameAssembly(original, *reloaded);
  fs::remove_all(dir);
}

// A FROZEN golden (docs/versioning/format.md): hardcoded keys/values, never
// regenerated. Loading it pins the exact v23 on-disk schema, so a silent key
// rename (which a save→load round-trip would not catch) fails here.
TEST_CASE("Frozen v23 assembly golden loads at the exact schema", "[assembly][golden]") {
  // Assembly goldens live in a subdirectory so the part golden harness
  // (which scans tests/golden/ non-recursively for {features,bodies,volumes})
  // does not try to load them as parts.
  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" /
                          "assembly" / "v23-assembly.json";
  REQUIRE(fs::exists(golden));
  std::string error;
  auto asmDoc = DocumentIO::LoadAssembly(golden.string(), &error);
  REQUIRE(asmDoc != nullptr);
  REQUIRE(asmDoc->OccurrenceCount() == 3);

  const Occurrence* base = asmDoc->FindById(1);
  REQUIRE(base != nullptr);
  CHECK(base->partId == 1);
  CHECK(base->fixed);
  CHECK_FALSE(base->partVersion.has_value());

  const Occurrence* arm = asmDoc->FindById(2);
  REQUIRE(arm != nullptr);
  CHECK(arm->partId == 2);
  CHECK_THAT(arm->position.x, WithinAbs(10.0f, 1e-6f));
  CHECK_THAT(arm->orientation.z, WithinAbs(0.70710677f, 1e-6f)); // 90° about Z

  const Occurrence* arm2 = asmDoc->FindById(3);
  REQUIRE(arm2 != nullptr);
  CHECK(arm2->partId == 2); // same part as id 2 → the instancing win
  CHECK_FALSE(arm2->visible);
  REQUIRE(arm2->partVersion.has_value());
  CHECK(*arm2->partVersion == "abc123");
}

// ---- Mates (A3, v24) ------------------------------------------------------

namespace {

// Adds a revolute mate between the two arm occurrences (a face anchor on the
// base, an origin anchor on the arm) to SampleAssembly. Returns the mate id.
uint64_t AddSampleMate(AssemblyDocument& asmDoc) {
  Mate m;
  m.name = "hinge";
  m.type = MateType::Revolute;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{4, "4/cap1"};
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = true;
  m.rotation = 0.7853981f;
  m.offset = 2.0f;
  return asmDoc.AddMate(m);
}

void ExpectSameMates(const AssemblyDocument& a, const AssemblyDocument& b) {
  REQUIRE(a.MateCount() == b.MateCount());
  const auto& ma = a.GetMates();
  const auto& mb = b.GetMates();
  for (size_t i = 0; i < ma.size(); i++) {
    CHECK(ma[i].id == mb[i].id);
    CHECK(ma[i].name == mb[i].name);
    CHECK(ma[i].type == mb[i].type);
    CHECK(ma[i].a.occurrenceId == mb[i].a.occurrenceId);
    CHECK(ma[i].a.anchor.origin == mb[i].a.anchor.origin);
    CHECK(ma[i].a.anchor.face.featureId == mb[i].a.anchor.face.featureId);
    CHECK(ma[i].a.anchor.face.name == mb[i].a.anchor.face.name);
    CHECK(ma[i].b.occurrenceId == mb[i].b.occurrenceId);
    CHECK(ma[i].b.anchor.origin == mb[i].b.anchor.origin);
    CHECK(ma[i].type == mb[i].type);
    CHECK(ma[i].flipped == mb[i].flipped);
    CHECK_THAT(ma[i].rotation, WithinAbs(mb[i].rotation, 1e-6f));
    CHECK_THAT(ma[i].offset, WithinAbs(mb[i].offset, 1e-6f));
    CHECK_THAT(ma[i].dx, WithinAbs(mb[i].dx, 1e-6f));
    CHECK_THAT(ma[i].dy, WithinAbs(mb[i].dy, 1e-6f));
    for (int k = 0; k < 4; k++)
      CHECK_THAT(ma[i].ballRotation[k], WithinAbs(mb[i].ballRotation[k], 1e-6f));
  }
}

} // namespace

TEST_CASE("Mates share the occurrence id counter and find/remove", "[assembly]") {
  AssemblyDocument asmDoc = SampleAssembly(); // occurrences 1,2,3
  const uint64_t mateId = AddSampleMate(asmDoc);
  CHECK(mateId == 4); // one id space: next after occurrence 3

  // A subsequent occurrence add continues past the mate.
  Occurrence extra;
  extra.partId = 9;
  CHECK(asmDoc.AddOccurrence(extra) == 5);

  REQUIRE(asmDoc.FindMateById(4) != nullptr);
  CHECK(asmDoc.FindMateById(4)->name == "hinge");
  CHECK(asmDoc.FindMateById(2) == nullptr); // that id is an occurrence

  REQUIRE(asmDoc.RemoveMate(4));
  CHECK(asmDoc.MateCount() == 0);
  CHECK_FALSE(asmDoc.RemoveMate(4));
}

TEST_CASE("Assembly with mates round-trips (single-file + directory)", "[assembly]") {
  AssemblyDocument original = SampleAssembly();
  AddSampleMate(original);

  SECTION("single file") {
    const fs::path path = fs::temp_directory_path() / "designer_asm_mates.json";
    std::string error;
    REQUIRE(DocumentIO::SaveAssembly(original, path.string(), &error));
    {
      std::ifstream in(path);
      nlohmann::json j = nlohmann::json::parse(in);
      CHECK(j.at("mates").size() == 1);
      CHECK(j.at("mates")[0].at("mate_type") == "revolute");
      CHECK(j.at("mates")[0].at("a").at("anchor").at("face").at("element") ==
            "4/cap1");
      CHECK(j.at("mates")[0].at("b").at("anchor").at("origin") == true);
    }
    auto reloaded = DocumentIO::LoadAssembly(path.string(), &error);
    REQUIRE(reloaded != nullptr);
    ExpectSameAssembly(original, *reloaded);
    ExpectSameMates(original, *reloaded);
    fs::remove(path);
  }

  SECTION("directory") {
    const fs::path dir = fs::temp_directory_path() / "designer_asm_mates.dsgn";
    fs::remove_all(dir);
    std::string error;
    REQUIRE(DocumentIO::SaveAssembly(original, dir.string(), &error));
    CHECK(fs::exists(dir / "mates" / "4-hinge.json"));
    auto reloaded = DocumentIO::LoadAssembly(dir.string(), &error);
    REQUIRE(reloaded != nullptr);
    ExpectSameAssembly(original, *reloaded);
    ExpectSameMates(original, *reloaded);
    // The id counter is pinned past the mate: a new add is 5, not a reuse.
    CHECK(reloaded->AddOccurrence(Occurrence{}) == 5);
    fs::remove_all(dir);
  }
}

TEST_CASE("A v23 assembly (no mates) still loads under v24", "[assembly]") {
  // Additive bump: the frozen v23 golden must still load, with zero mates.
  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" /
                          "assembly" / "v23-assembly.json";
  std::string error;
  auto asmDoc = DocumentIO::LoadAssembly(golden.string(), &error);
  REQUIRE(asmDoc != nullptr);
  CHECK(asmDoc->OccurrenceCount() == 3);
  CHECK(asmDoc->MateCount() == 0);
}

TEST_CASE("Frozen v24 assembly golden loads at the exact schema", "[assembly][golden]") {
  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" /
                          "assembly" / "v24-assembly.json";
  REQUIRE(fs::exists(golden));
  std::string error;
  auto asmDoc = DocumentIO::LoadAssembly(golden.string(), &error);
  REQUIRE(asmDoc != nullptr);
  REQUIRE(asmDoc->OccurrenceCount() == 2);
  REQUIRE(asmDoc->MateCount() == 1);

  const Mate* hinge = asmDoc->FindMateById(3);
  REQUIRE(hinge != nullptr);
  CHECK(hinge->type == MateType::Revolute);
  CHECK(hinge->name == "hinge");
  CHECK(hinge->a.occurrenceId == 1);
  CHECK_FALSE(hinge->a.anchor.origin);
  CHECK(hinge->a.anchor.face.featureId == 4);
  CHECK(hinge->a.anchor.face.name == "4/cap1");
  CHECK(hinge->b.occurrenceId == 2);
  CHECK(hinge->b.anchor.origin);
  CHECK(hinge->flipped);
  CHECK_THAT(hinge->rotation, WithinAbs(0.7853981f, 1e-6f));
}

TEST_CASE("Frozen v25 golden: cylindrical/planar/ball mate schema", "[assembly][golden]") {
  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" /
                          "assembly" / "v25-assembly.json";
  REQUIRE(fs::exists(golden));
  std::string error;
  auto asmDoc = DocumentIO::LoadAssembly(golden.string(), &error);
  REQUIRE(asmDoc != nullptr);
  REQUIRE(asmDoc->MateCount() == 2);

  const Mate* planar = asmDoc->FindMateById(3);
  REQUIRE(planar != nullptr);
  CHECK(planar->type == MateType::Planar);
  CHECK_THAT(planar->dx, WithinAbs(3.0f, 1e-6f));
  CHECK_THAT(planar->dy, WithinAbs(4.0f, 1e-6f));
  CHECK_THAT(planar->rotation, WithinAbs(0.5f, 1e-6f));

  const Mate* ball = asmDoc->FindMateById(4);
  REQUIRE(ball != nullptr);
  CHECK(ball->type == MateType::Ball);
  CHECK_THAT(ball->ballRotation.z, WithinAbs(0.3826834f, 1e-6f)); // 45° about Z
  CHECK_THAT(ball->ballRotation.w, WithinAbs(0.9238795f, 1e-6f));

  // Round-trips (write at v25, reload) without losing the new DOF fields.
  const fs::path tmp = fs::temp_directory_path() / "designer_v25_rt.json";
  REQUIRE(DocumentIO::SaveAssembly(*asmDoc, tmp.string(), &error));
  auto again = DocumentIO::LoadAssembly(tmp.string(), &error);
  REQUIRE(again != nullptr);
  CHECK(again->FindMateById(3)->type == MateType::Planar);
  CHECK_THAT(again->FindMateById(3)->dx, WithinAbs(3.0f, 1e-6f));
  CHECK(again->FindMateById(4)->type == MateType::Ball);
  fs::remove(tmp);
}

TEST_CASE("Connector inference snap edge round-trips (v26)", "[assembly]") {
  AssemblyDocument asmDoc = SampleAssembly();
  Mate m;
  m.name = "bolt";
  m.type = MateType::Fastened;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{4, "4/cap1"};
  m.a.anchor.snapEdge = ElementRef{4, "4/g7;M1"}; // snapped to a hole center
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  asmDoc.AddMate(m);

  const fs::path path = fs::temp_directory_path() / "designer_snap_rt.json";
  std::string error;
  REQUIRE(DocumentIO::SaveAssembly(asmDoc, path.string(), &error));
  {
    std::ifstream in(path);
    nlohmann::json j = nlohmann::json::parse(in);
    const auto& a = j.at("mates")[0].at("a").at("anchor");
    CHECK(a.at("snap_edge").at("element") == "4/g7;M1");
  }
  auto reloaded = DocumentIO::LoadAssembly(path.string(), &error);
  REQUIRE(reloaded != nullptr);
  const Mate* rm = reloaded->GetMates()[0].id ? &reloaded->GetMates()[0] : nullptr;
  REQUIRE(rm != nullptr);
  CHECK(rm->a.anchor.snapEdge.featureId == 4);
  CHECK(rm->a.anchor.snapEdge.name == "4/g7;M1");
  CHECK_FALSE(rm->b.anchor.snapEdge.IsSet()); // origin anchor: no snap
  fs::remove(path);
}

TEST_CASE("Frozen v26 golden: connector snap-edge schema", "[assembly][golden]") {
  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" /
                          "assembly" / "v26-assembly.json";
  REQUIRE(fs::exists(golden));
  std::string error;
  auto asmDoc = DocumentIO::LoadAssembly(golden.string(), &error);
  REQUIRE(asmDoc != nullptr);
  REQUIRE(asmDoc->MateCount() == 1);

  const Mate* bolt = asmDoc->FindMateById(3);
  REQUIRE(bolt != nullptr);
  CHECK(bolt->a.anchor.face.name == "4/cap1");
  CHECK(bolt->a.anchor.snapEdge.featureId == 4);
  CHECK(bolt->a.anchor.snapEdge.name == "4/g7;M1");
  CHECK(asmDoc->GetExplodedViews().empty()); // pre-v27: none

  // Round-trips at the current version; a view-less assembly writes NO
  // `exploded` key (the v27 bump is invisible to v26 content).
  const fs::path tmp = fs::temp_directory_path() / "designer_v26_rt.json";
  REQUIRE(DocumentIO::SaveAssembly(*asmDoc, tmp.string(), &error));
  {
    std::ifstream in(tmp);
    nlohmann::json j = nlohmann::json::parse(in);
    CHECK_FALSE(j.contains("exploded"));
    CHECK(j.at("mates")[0].at("a").at("anchor").at("snap_edge").at("element") ==
          "4/g7;M1");
  }
  auto again = DocumentIO::LoadAssembly(tmp.string(), &error);
  REQUIRE(again != nullptr);
  CHECK(again->FindMateById(3)->a.anchor.snapEdge.name == "4/g7;M1");
  fs::remove(tmp);
}

TEST_CASE("Exploded views round-trip (v27, single-file + directory)",
          "[assembly]") {
  AssemblyDocument asmDoc = SampleAssembly();
  ExplodedView view;
  view.name = "service";
  view.steps.push_back({1, glm::vec3(0.0f, 0.0f, 30.0f)});
  view.steps.push_back({2, glm::vec3(-15.0f, 0.0f, 10.0f)});
  const uint64_t viewId = asmDoc.AddExplodedView(view);
  CHECK(viewId >= 3); // shares the occurrence/mate id counter

  std::string error;

  SECTION("single JSON file") {
    const fs::path path = fs::temp_directory_path() / "designer_explode_rt.json";
    REQUIRE(DocumentIO::SaveAssembly(asmDoc, path.string(), &error));
    auto reloaded = DocumentIO::LoadAssembly(path.string(), &error);
    REQUIRE(reloaded != nullptr);
    REQUIRE(reloaded->GetExplodedViews().size() == 1);
    const ExplodedView* v = reloaded->FindExplodedViewById(viewId);
    REQUIRE(v != nullptr);
    CHECK(v->name == "service");
    REQUIRE(v->steps.size() == 2);
    CHECK(v->steps[0].occurrenceId == 1);
    CHECK_THAT(v->steps[0].offset.z, WithinAbs(30.0f, 1e-6f));
    CHECK(v->steps[1].occurrenceId == 2);
    CHECK_THAT(v->steps[1].offset.x, WithinAbs(-15.0f, 1e-6f));
    // The id counter is pinned past the view: no reuse after reload.
    CHECK(reloaded->AddOccurrence(Occurrence{}) == viewId + 1);
    fs::remove(path);
  }

  SECTION("directory form: file-per-view under exploded/") {
    const fs::path dir = fs::temp_directory_path() / "designer_explode_rt.dsgn";
    fs::remove_all(dir);
    REQUIRE(DocumentIO::SaveAssembly(asmDoc, dir.string(), &error));
    CHECK(fs::exists(dir / "exploded" /
                     (std::to_string(viewId) + "-service.json")));
    auto reloaded = DocumentIO::LoadAssembly(dir.string(), &error);
    REQUIRE(reloaded != nullptr);
    REQUIRE(reloaded->GetExplodedViews().size() == 1);
    CHECK(reloaded->FindExplodedViewById(viewId)->steps.size() == 2);

    // Removing the view removes its file on the next save (F3 discipline),
    // and the index drops the `exploded` key entirely.
    REQUIRE(reloaded->RemoveExplodedView(viewId));
    REQUIRE(DocumentIO::SaveAssembly(*reloaded, dir.string(), &error));
    CHECK_FALSE(fs::exists(dir / "exploded" /
                           (std::to_string(viewId) + "-service.json")));
    {
      std::ifstream in(dir / "index.json");
      nlohmann::json index = nlohmann::json::parse(in);
      CHECK_FALSE(index.contains("exploded"));
    }
    fs::remove_all(dir);
  }
}

TEST_CASE("A part loader rejects an assembly and vice versa", "[assembly]") {
  AssemblyDocument asmDoc = SampleAssembly();
  const fs::path path = fs::temp_directory_path() / "designer_asm_kind.json";
  std::string error;
  REQUIRE(DocumentIO::SaveAssembly(asmDoc, path.string(), &error));

  // Loading an assembly as a part must fail cleanly, not throw or misparse.
  error.clear();
  auto asPart = DocumentIO::Load(path.string(), &error);
  CHECK(asPart == nullptr);
  CHECK_FALSE(error.empty());
  fs::remove(path);
}
