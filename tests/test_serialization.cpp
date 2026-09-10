#include "ui/CreateActions.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include "cad/CADKernel.h"

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>

#include <fstream>
#include <unistd.h>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

static fs::path TempFile(const std::string& name) {
  return fs::temp_directory_path() / name;
}

TEST_CASE("Sphere and cylinder features produce bodies", "[feature]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<SphereFeature>("Ball", glm::vec3(1, 2, 3), 1.5));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Rod", glm::vec3(0), glm::vec3(0, 0, 1), 1.0, 4.0));

  REQUIRE(doc.GetBodies().size() == 2);
  const double sphereVol = 4.0 / 3.0 * 3.14159265358979 * 1.5 * 1.5 * 1.5;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(sphereVol, 1e-4));
  const double cylVol = 3.14159265358979 * 1.0 * 4.0;
  CHECK_THAT(doc.GetBodies()[1].shape.Volume(), WithinRel(cylVol, 1e-4));
}

TEST_CASE("ImportFeature loads a STEP file centered at origin",
          "[feature][io]") {
  // Author a box far from the origin and export it
  CADShape box = CADKernel::Instance().CreateBox(glm::vec3(10, 20, 30),
                                                 glm::vec3(12, 22, 32));
  fs::path stepPath = TempFile("designer_test_import.step");
  std::vector<CADShape> shapes{box};
  REQUIRE(CADKernel::Instance().SaveSTEP(stepPath.string(), shapes));

  Document doc;
  doc.AddFeature(std::make_unique<ImportFeature>("part", stepPath.string()));

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0, 1e-6));

  // The import should be re-centered on the origin
  glm::vec3 min, max;
  doc.GetBodies()[0].shape.GetBoundingBox(min, max);
  glm::vec3 center = (min + max) * 0.5f;
  CHECK_THAT(center.x, WithinAbs(0.0f, 0.1f));
  CHECK_THAT(center.y, WithinAbs(0.0f, 0.1f));
  CHECK_THAT(center.z, WithinAbs(0.0f, 0.1f));

  fs::remove(stepPath);
}

TEST_CASE("A seeded ImportFeature uses the caller's geometry once, then the "
          "file again",
          "[feature][io]") {
  // The GUI import worker parses a STEP to decide part-vs-assembly; a
  // single-product file would then be read a SECOND time by the feature's
  // first regen — the whole file twice, ~12 s each at 48 MB.
  CADShape box = CADKernel::Instance().CreateBox(glm::vec3(10, 20, 30),
                                                 glm::vec3(12, 22, 32));
  fs::path stepPath = TempFile("designer_test_seeded.step");
  std::vector<CADShape> shapes{box};
  REQUIRE(CADKernel::Instance().SaveSTEP(stepPath.string(), shapes));

  CADKernel::ImportedModel parsed;
  REQUIRE(CADKernel::Instance().LoadSTEPStructured(stepPath.string(), parsed));

  Document doc;
  auto feature = std::make_unique<ImportFeature>("part", stepPath.string());
  feature->SeedGeometry(std::move(parsed));
  doc.AddFeature(std::move(feature));

  // Seeded or not, the result is the same import: one body, centered.
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0, 1e-6));
  glm::vec3 mn, mx;
  doc.GetBodies()[0].shape.GetBoundingBox(mn, mx);
  CHECK_THAT(glm::length((mn + mx) * 0.5f), WithinAbs(0.0f, 0.1f));

  // The seed is consumed, not sticky: a re-execution goes back to the file, so
  // editing the source still updates the part. Deleting the file is how this
  // proves the feature READ it rather than reusing the seed — and the regen
  // prefix cache has to be dropped first, or Regenerate serves the bodies it
  // already has and never calls Execute at all.
  fs::remove(stepPath);
  doc.ClearRegenCache();
  CHECK_FALSE(doc.Regenerate());
  CHECK(doc.GetBodies().empty());
}

TEST_CASE("ImportFeature fails cleanly on a missing file", "[feature][io]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<ImportFeature>("nope", "/does/not/exist.step"));
  CHECK(doc.GetBodies().empty());
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("Document JSON roundtrip preserves features and geometry",
          "[serialization]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::vec3(1, 0, -2), 2.0, 3.0, 4.0));
  doc.AddFeature(
      std::make_unique<SphereFeature>("Ball", glm::vec3(5, 0, 0), 1.5));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Rod", glm::vec3(-3, 0, 0), glm::vec3(0, 1, 0), 0.5, 2.0));

  fs::path path = TempFile("designer_test_doc.json");
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));

  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);

  REQUIRE(loaded->FeatureCount() == 3);
  REQUIRE(loaded->GetBodies().size() == 3);
  CHECK(loaded->GetFeature(0)->TypeName() == "Box");
  CHECK(loaded->GetFeature(1)->GetName() == "Ball");
  for (size_t i = 0; i < 3; i++) {
    CHECK_THAT(loaded->GetBodies()[i].shape.Volume(),
               WithinRel(doc.GetBodies()[i].shape.Volume(), 1e-9));
  }

  fs::remove(path);
}

TEST_CASE("Feature material round-trips and stamps its body",
          "[serialization]") {
  Document doc;
  auto box = std::make_unique<BoxFeature>("Base", glm::vec3(0), 2.0, 2.0, 2.0);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  // A second body left at the default (empty) material.
  doc.AddFeature(
      std::make_unique<SphereFeature>("Ball", glm::vec3(5, 0, 0), 1.0));

  // The creating feature stamps its material onto the body it produces.
  REQUIRE(doc.GetBodies().size() == 2);
  CHECK(doc.GetBodies()[0].material == "steel");
  CHECK(doc.GetBodies()[1].material.empty());

  fs::path path = TempFile("designer_test_material.json");
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));

  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  REQUIRE(loaded->FeatureCount() == 2);
  CHECK(loaded->GetFeature(0)->GetMaterial() == "steel");
  CHECK(loaded->GetFeature(1)->GetMaterial().empty());
  // And the material survives regeneration onto the rebuilt body.
  CHECK(loaded->GetBodies()[0].material == "steel");
  CHECK(loaded->GetBodies()[1].material.empty());

  fs::remove(path);
}

TEST_CASE("DistancePL dimension and its expression round-trip",
          "[serialization][sketch]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t l0 = sk->AddEntity(sketch::Line{{0, 0}, {4, 0}});
  const uint32_t p0 = sk->AddEntity(sketch::Point{{1, 2}});
  sketch::Constraint dim{sketch::ConstraintKind::DistancePL, {p0, 0}, {l0, 0}};
  dim.value = 2.0;
  dim.valueExpr = "4/2";
  dim.labelOffset = {0.5f, 0.0f};
  sk->constraints.push_back(dim);
  doc.AddFeature(std::move(sk));

  fs::path path = TempFile("designer_test_distance_pl.json");
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE((loaded != nullptr && error.empty()));

  auto* lsk = dynamic_cast<SketchFeature*>(loaded->GetFeature(0));
  REQUIRE(lsk != nullptr);
  REQUIRE(lsk->constraints.size() == 1);
  const sketch::Constraint& c = lsk->constraints[0];
  CHECK(c.kind == sketch::ConstraintKind::DistancePL);
  CHECK(c.a.entity == p0);
  CHECK(c.b.entity == l0);
  CHECK_THAT(c.value, WithinAbs(2.0, 1e-9));
  CHECK(c.valueExpr == "4/2");
  CHECK_THAT(c.labelOffset.x, WithinAbs(0.5f, 1e-6f));
  // The expression re-evaluated + solved on load keeps the point 2 off the
  // line (it already was — regeneration must not move it).
  const glm::vec2 p = std::get<sketch::Point>(lsk->entities[1]).p;
  CHECK_THAT(p.y, WithinAbs(2.0f, 1e-3f));

  fs::remove(path);
}

TEST_CASE("Centered rectangle's symmetric-about-point anchor round-trips",
          "[serialization][sketch]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const auto ids = sk->AddRectangleCentered({1.0f, 0.5f}, {2.0f, 1.0f});
  doc.AddFeature(std::move(sk));

  fs::path path = TempFile("designer_test_center_rect.json");
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE((loaded != nullptr && error.empty()));

  auto* lsk = dynamic_cast<SketchFeature*>(loaded->GetFeature(0));
  REQUIRE(lsk != nullptr);
  REQUIRE(lsk->entities.size() == 5); // 4 sides + the centre point
  bool anchored = false;
  for (const auto& c : lsk->constraints)
    if (c.kind == sketch::ConstraintKind::Symmetric && c.c.entity == ids[4] &&
        c.a.entity == ids[0] && c.b.entity == ids[2])
      anchored = true;
  CHECK(anchored); // the point-centre third ref survived the round trip

  fs::remove(path);
}

TEST_CASE("Loading rejects non-document JSON", "[serialization]") {
  fs::path path = TempFile("designer_test_bad.json");
  {
    std::ofstream out(path);
    out << R"({"format": "something-else", "features": []})";
  }

  const auto r = DocumentIO::Load(path.string());
  CHECK_FALSE(r.has_value());
  CHECK_FALSE(r.error().empty());

  fs::remove(path);
}

TEST_CASE("Loading rejects unknown feature types", "[serialization]") {
  fs::path path = TempFile("designer_test_unknown.json");
  {
    std::ofstream out(path);
    out << R"({"format": "designer-document", "version": 1,
               "features": [{"type": "Warp", "name": "x"}]})";
  }

  const auto r = DocumentIO::Load(path.string());
  CHECK_FALSE(r.has_value());
  CHECK(r.error().find("Warp") != std::string::npos);

  fs::remove(path);
}

// ---- FEM studies (v29) ----------------------------------------------------

namespace {
Document DocWithStudy() {
  Document doc;
  doc.SetParameter("load_n", "1000");
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::vec3(0, 0, 0), 10.0, 4.0, 4.0));
  fem::Study& s = doc.AddStudy("Static 1");
  s.meshSize = 5.0;
  s.meshSizeExpr = "";
  s.refinements.push_back({ElementRef{2, "2/g1"}, 1.0, std::string{}});
  fem::Load fixed;
  fixed.id = doc.AllocateId();
  fixed.type = fem::LoadType::Fixed;
  fixed.face = ElementRef{2, "2/cap0"};
  s.loads.push_back(fixed);
  fem::Load force;
  force.id = doc.AllocateId();
  force.type = fem::LoadType::Force;
  force.face = ElementRef{2, "2/cap1"};
  force.magnitude = 1000.0;
  force.magnitudeExpr = "load_n";
  force.direction = glm::dvec3(0, 0, -1);
  s.loads.push_back(force);
  return doc;
}

void CheckStudyMatches(const Document& loaded) {
  REQUIRE(loaded.GetStudies().size() == 1);
  const fem::Study& s = loaded.GetStudies()[0];
  CHECK(s.name == "Static 1");
  CHECK(s.meshSize == 5.0);
  REQUIRE(s.refinements.size() == 1);
  CHECK(s.refinements[0].face.name == "2/g1");
  CHECK(s.refinements[0].size == 1.0);
  REQUIRE(s.loads.size() == 2);
  CHECK(s.loads[0].type == fem::LoadType::Fixed);
  CHECK(s.loads[0].face.name == "2/cap0");
  CHECK(s.loads[1].type == fem::LoadType::Force);
  CHECK(s.loads[1].magnitude == 1000.0);
  CHECK(s.loads[1].magnitudeExpr == "load_n");
  CHECK(s.loads[1].direction.z == -1.0);
}
} // namespace

TEST_CASE("Study round-trips through the single-file format",
          "[serialization][fem]") {
  Document doc = DocWithStudy();
  fs::path path = TempFile("designer_test_study.json");
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  INFO(error);
  REQUIRE(loaded != nullptr);
  CheckStudyMatches(*loaded);
  fs::remove(path);
}

TEST_CASE("Study round-trips through the directory format",
          "[serialization][fem]") {
  Document doc = DocWithStudy();
  fs::path dir = fs::temp_directory_path() / "designer_test_study.dsgn";
  fs::remove_all(dir);
  std::string error;
  REQUIRE(testio::Save(doc, dir.string()));
  CHECK(fs::exists(dir / "studies"));
  auto loaded = testio::Load(dir.string());
  INFO(error);
  REQUIRE(loaded != nullptr);
  CheckStudyMatches(*loaded);
  // A freshly allocated id must not collide with the loaded study/load ids
  // (the high-water mark was restored on load).
  const fem::Study& s = loaded->GetStudies()[0];
  uint64_t maxId = s.id;
  for (const fem::Load& l : s.loads)
    maxId = std::max(maxId, l.id);
  CHECK(loaded->AllocateId() > maxId);
  fs::remove_all(dir);
}

TEST_CASE("A document with no studies writes no studies key or dir",
          "[serialization][fem]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::vec3(0, 0, 0), 1.0, 1.0, 1.0));
  // Single-file: no "studies" key.
  fs::path path = TempFile("designer_test_nostudy.json");
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  std::ifstream in(path);
  const std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  CHECK(body.find("studies") == std::string::npos);
  fs::remove(path);

  // Directory: no studies/ subdir.
  fs::path dir = fs::temp_directory_path() / "designer_test_nostudy.dsgn";
  fs::remove_all(dir);
  REQUIRE(testio::Save(doc, dir.string()));
  CHECK_FALSE(fs::exists(dir / "studies"));
  fs::remove_all(dir);
}

TEST_CASE("Modal analysis type + gravity round-trip (v32)",
          "[serialization][fem]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 100.0,
                                              10.0, 10.0));
  fem::Study& modal = doc.AddStudy("Modes");
  modal.type = fem::AnalysisType::Modal;
  modal.numModes = 8;
  modal.meshSize = 5.0;
  fem::Study& grav = doc.AddStudy("Self weight");
  grav.gravity = glm::dvec3(0, 0, -9.81);
  grav.meshSize = 5.0;

  fs::path path = TempFile("designer_test_modal.json");
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  // A static study with no gravity writes none of the new keys (byte-identity).
  {
    Document plain;
    plain.AddFeature(
        std::make_unique<BoxFeature>("B", glm::vec3(0, 0, 0), 1, 1, 1));
    plain.AddStudy("S");
    fs::path p2 = TempFile("designer_test_plainstudy.json");
    REQUIRE(testio::Save(plain, p2.string()));
    std::ifstream in(p2);
    const std::string body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(body.find("analysis") == std::string::npos);
    CHECK(body.find("gravity") == std::string::npos);
    fs::remove(p2);
  }

  auto loaded = testio::Load(path.string());
  INFO(error);
  REQUIRE(loaded != nullptr);
  REQUIRE(loaded->GetStudies().size() == 2);
  const fem::Study& m = loaded->GetStudies()[0];
  CHECK(m.type == fem::AnalysisType::Modal);
  CHECK(m.numModes == 8);
  const fem::Study& g = loaded->GetStudies()[1];
  CHECK(g.type == fem::AnalysisType::Static);
  CHECK(g.gravity.z == -9.81);
  fs::remove(path);
}

// --- Merge hazards ----------------------------------------------------------
//
// The directory format merges well on purpose — one file per feature named by
// a stable id, one key per line — so people WILL merge these projects. Two
// cases that git resolves "cleanly" are not clean at all, and both used to
// load without a word: two files claiming one id (the id counter hands the
// same number to two people working offline, and git keeps both files when
// the type suffix differs), and an index that lists one id twice. Either way
// the document ends up with two features sharing an id, every reference to it
// resolves ambiguously, and one person's work is silently replaced by a copy
// of the other's.
namespace {
fs::path MergeCaseProject(const std::string& name) {
  const fs::path dir =
      fs::temp_directory_path() /
      ("designer_merge_" + name + "_" + std::to_string(::getpid()) + ".dsgn");
  fs::remove_all(dir);
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 2, 2, 2));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 2, 2, 2));
  REQUIRE(testio::Save(doc, dir.string())); // .dsgn ⇒ the directory format
  return dir;
}
} // namespace

TEST_CASE("Two feature files claiming one id are refused", "[io][merge]") {
  const fs::path dir = MergeCaseProject("dupfile");
  // What a merge leaves behind: a second file with the same id prefix. The
  // app never writes this; git does, and it used to win silently.
  fs::path first;
  for (const auto& e : fs::directory_iterator(dir / "features"))
    if (e.path().filename().string().starts_with("2-"))
      first = e.path();
  REQUIRE(!first.empty());
  fs::copy_file(first, dir / "features" / "2-hole.json");

  auto loaded = DocumentIO::Load(dir.string());
  REQUIRE_FALSE(loaded); // refused, not "last file wins"
  const std::string err = loaded.error();
  INFO(err);
  CHECK(err.find("claim id 2") != std::string::npos);
  CHECK(err.find("2-hole.json") != std::string::npos); // names BOTH files
  fs::remove_all(dir);
}

TEST_CASE("An index listing one id twice is refused", "[io][merge]") {
  const fs::path dir = MergeCaseProject("dupindex");
  nlohmann::json index;
  {
    std::ifstream in(dir / "index.json");
    index = nlohmann::json::parse(in);
  }
  index["features"].push_back(index["features"].back()); // both sides kept
  std::ofstream(dir / "index.json", std::ios::trunc) << index.dump(2);

  auto loaded = DocumentIO::Load(dir.string());
  REQUIRE_FALSE(loaded);
  const std::string err = loaded.error();
  INFO(err);
  CHECK(err.find("twice") != std::string::npos);
  fs::remove_all(dir);
}

TEST_CASE("Unresolved conflict markers name the file", "[io][merge]") {
  const fs::path dir = MergeCaseProject("markers");
  fs::path victim;
  for (const auto& e : fs::directory_iterator(dir / "features"))
    if (e.path().filename().string().starts_with("1-"))
      victim = e.path();
  REQUIRE(!victim.empty());
  const std::string body = [&] {
    std::ifstream in(victim);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }();
  std::ofstream(victim, std::ios::trunc) << "<<<<<<< HEAD\n"
                                         << body << "=======\n"
                                         << body << ">>>>>>> theirs\n";

  auto loaded = DocumentIO::Load(dir.string());
  REQUIRE_FALSE(loaded);
  const std::string err = loaded.error();
  INFO(err);
  // A line and column number is not a diagnosis; the file and the cause are.
  CHECK(err.find(victim.filename().string()) != std::string::npos);
  CHECK(err.find("conflict markers") != std::string::npos);
  fs::remove_all(dir);
}

// A project saved from the GUI must always be the directory form. The flat
// single-file form still LOADS, but saving one created a project File > Open
// Project could not return: that dialog picks folders, so a flat `foo.json`
// was reachable only through Open Recent.
TEST_CASE("A picked save target is always the directory form", "[io]") {
  CHECK(DocumentIO::ProjectSaveTarget("/tmp/hinge.dsgn") == "/tmp/hinge.dsgn");
  CHECK(DocumentIO::ProjectSaveTarget("/tmp/hinge") == "/tmp/hinge.dsgn");
  // The flat form is replaced, not appended to.
  CHECK(DocumentIO::ProjectSaveTarget("/tmp/hinge.json") == "/tmp/hinge.dsgn");
  // A dot inside the NAME is not an extension to overwrite — "hinge v2" must
  // not come back as "hinge v.dsgn".
  CHECK(DocumentIO::ProjectSaveTarget("/tmp/hinge v2") == "/tmp/hinge v2.dsgn");
  CHECK(DocumentIO::ProjectSaveTarget("/tmp/hinge v1.2") ==
        "/tmp/hinge v1.2.dsgn");
  // Nothing picked stays nothing.
  CHECK(DocumentIO::ProjectSaveTarget("").empty());
}

// And what it produces is openable by the path the folder picker returns.
TEST_CASE("The directory form round-trips through a folder pick", "[io]") {
  namespace fs = std::filesystem;
  const fs::path picked =
      fs::temp_directory_path() /
      ("designer-savetarget-" + std::to_string(::getpid()) + ".json");
  fs::remove_all(fs::path(DocumentIO::ProjectSaveTarget(picked.string())));

  const std::string target = DocumentIO::ProjectSaveTarget(picked.string());
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::vec3(0), 10.0, 10.0, 10.0));
  REQUIRE(DocumentIO::Save(doc, target));
  // A DIRECTORY: what a folder picker can hand back, unlike a flat file.
  CHECK(fs::is_directory(target));
  auto loaded = DocumentIO::Load(target);
  REQUIRE(loaded);
  CHECK((*loaded)->FeatureCount() == 1);
  fs::remove_all(target);
}

// --- kFeatureTypes is the authority, not a comment ------------------------
//
// The table beside FeatureFromJson used to carry "Keep in lockstep with the
// FeatureFromJson chain below" and nothing enforced it. A type could gain a
// `type ==` branch without a row and load, save and regenerate perfectly
// while being invisible to the `describe` RPC — an agent reading describe
// would never learn the feature exists. FeatureFromJson now rejects any type
// missing from the table before reaching the chain, which makes a
// branch-without-a-row unreachable; these pin both directions.

TEST_CASE("every documented feature type reaches a FeatureFromJson branch",
          "[serialization]") {
  const auto types = DocumentIO::FeatureTypes();
  REQUIRE(!types.empty());
  for (const DocumentIO::FeatureTypeInfo& info : types) {
    // A bare {type, name} is missing whatever fields the branch requires, so
    // most types throw — but they must throw about a FIELD, never about the
    // TYPE. "Unknown feature type" here would mean the table lists something
    // the chain cannot build.
    nlohmann::json j = {{"type", info.type}, {"name", "probe"}};
    try {
      auto f = DocumentIO::FeatureFromJson(j);
      REQUIRE(f != nullptr);
      CHECK(f->TypeName() == std::string(info.type));
    } catch (const std::exception& e) {
      const std::string msg = e.what();
      INFO("type " << info.type << " reported: " << msg);
      CHECK(msg.find("Unknown feature type") == std::string::npos);
      CHECK(msg.find("no branch for it") == std::string::npos);
    }
  }
}

TEST_CASE("a feature type outside the table is refused by name",
          "[serialization]") {
  nlohmann::json j = {{"type", "Sprocket"}, {"name", "nope"}};
  REQUIRE_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                      Catch::Matchers::ContainsSubstring("Unknown feature "
                                                         "type: Sprocket") &&
                          // decision-complete: the rejection names the
                          // vocabulary, so the fix needs no second round-trip
                          Catch::Matchers::ContainsSubstring("Known types:") &&
                          Catch::Matchers::ContainsSubstring("Extrude"));
}

TEST_CASE("describe's feature table covers every type a document can hold",
          "[serialization]") {
  // The reverse direction, through the real serializer: build one of every
  // feature the GUI can create, round-trip it, and require its TypeName to be
  // in the table. FeatureToJson would throw "Unserializable feature type" for
  // anything it cannot write, so this also pins To/From agreement.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 1, 1, 1));
  doc.AddFeature(std::make_unique<SphereFeature>("S", glm::vec3(0), 1));
  doc.AddFeature(std::make_unique<CylinderFeature>("C", glm::vec3(0),
                                                   glm::vec3(0, 1, 0), 1, 1));

  const auto types = DocumentIO::FeatureTypes();
  for (size_t i = 0; i < doc.FeatureCount(); i++) {
    const Feature* f = doc.GetFeature(i);
    const std::string name = f->TypeName();
    const bool listed =
        std::any_of(types.begin(), types.end(),
                    [&](const DocumentIO::FeatureTypeInfo& info) {
                      return name == info.type;
                    });
    INFO("feature type " << name << " is missing a kFeatureTypes row");
    CHECK(listed);
  }
}

namespace {
// One entity wrapped in the smallest Sketch that will carry it. EntityFromJson
// is file-local in SerializeFeatures.cpp, so the entity dialect is tested
// through the same door an agent comes in by.
nlohmann::json SketchWith(const nlohmann::json& entity) {
  return {{"type", "Sketch"},
          {"name", "probe"},
          {"plane",
           {{"origin", {0.0, 0.0, 0.0}},
            {"normal", {0.0, 0.0, 1.0}},
            {"uAxis", {1.0, 0.0, 0.0}}}},
          {"entities", nlohmann::json::array({entity})}};
}
} // namespace

// --- kSketchEntityTypes is the authority too -------------------------------
//
// One level below the feature table, and it had the same hole for longer: the
// entity list was bare NAMES, so `describe` could tell an agent that "arc"
// exists and not one field it takes. A sketch arc's angles are radians, in an
// API whose other angles are all named `_deg`, and nothing anywhere said so —
// `end_angle: 90` is a legal 90 RADIANS and silently sweeps 116.6 degrees.

TEST_CASE("every documented sketch entity type reaches a branch",
          "[serialization]") {
  const auto types = DocumentIO::SketchEntityTypes();
  REQUIRE(!types.empty());
  for (const DocumentIO::SketchEntityInfo& info : types) {
    try {
      DocumentIO::FeatureFromJson(SketchWith({{"type", info.type}}));
    } catch (const std::exception& e) {
      const std::string msg = e.what();
      INFO("entity " << info.type << " reported: " << msg);
      // It may complain about a missing FIELD; it must never say the TYPE is
      // unknown or undocumented.
      CHECK(msg.find("Unknown sketch entity type") == std::string::npos);
      CHECK(msg.find("no branch for it") == std::string::npos);
    }
    INFO("entity " << info.type << " documents no fields");
    CHECK(std::strlen(info.params) > 0);
  }
}

TEST_CASE("a sketch entity outside the table is refused by name",
          "[serialization]") {
  REQUIRE_THROWS_WITH(
      DocumentIO::FeatureFromJson(SketchWith({{"type", "squircle"}})),
      Catch::Matchers::ContainsSubstring("Unknown sketch entity type: "
                                         "squircle") &&
          Catch::Matchers::ContainsSubstring("Known types:") &&
          Catch::Matchers::ContainsSubstring("arc"));
}

TEST_CASE("a sketch arc takes degrees or radians, never both",
          "[serialization]") {
  const auto arc = [](nlohmann::json extra) {
    nlohmann::json e = {
        {"type", "arc"}, {"center", {0.0, 0.0}}, {"radius", 40.0}};
    e.update(extra);
    return SketchWith(e);
  };

  // The mistake this whole change exists for: 90 in a radian field is a legal
  // 90 radians, which folds to 116.6 degrees — a plausible arc and the wrong
  // part. Refused on geometry (more than a full turn is a circle), and the
  // message names the cause.
  REQUIRE_THROWS_WITH(
      DocumentIO::FeatureFromJson(
          arc({{"start_angle", 0.0}, {"end_angle", 90.0}})),
      Catch::Matchers::ContainsSubstring("more than one full turn") &&
          Catch::Matchers::ContainsSubstring("RADIANS") &&
          Catch::Matchers::ContainsSubstring("end_angle_deg"));

  // The same number in the _deg field is exactly what was meant.
  REQUIRE_NOTHROW(DocumentIO::FeatureFromJson(
      arc({{"start_angle_deg", 0.0}, {"end_angle_deg", 90.0}})));

  // Radians still work, and a quarter turn is not degenerate either way.
  REQUIRE_NOTHROW(DocumentIO::FeatureFromJson(
      arc({{"start_angle", 0.0}, {"end_angle", 1.5707963}})));

  // Two spellings of one angle have no right answer, so neither is picked.
  REQUIRE_THROWS_WITH(DocumentIO::FeatureFromJson(arc({{"start_angle", 0.0},
                                                       {"start_angle_deg", 0.0},
                                                       {"end_angle", 1.0}})),
                      Catch::Matchers::ContainsSubstring("not both"));

  // A missing angle names BOTH spellings, so the fix needs no round-trip.
  REQUIRE_THROWS_WITH(
      DocumentIO::FeatureFromJson(arc({{"end_angle", 1.0}})),
      Catch::Matchers::ContainsSubstring("start_angle") &&
          Catch::Matchers::ContainsSubstring("start_angle_deg"));
}

TEST_CASE("an angle CONSTRAINT reads the same unit through either door",
          "[serialization][sketch][units]") {
  // The other half of the same bug, and the one that survived the arc fix:
  // add_constraint read `value` as DEGREES while this path — add_feature with
  // a sketch spec, and every file load — read the same field as RADIANS, with
  // get_state echoing the radians back. Set 45, read 0.785, and both were
  // "right".
  const auto sketchWith = [](const nlohmann::json& constraint) {
    nlohmann::json sk =
        SketchWith({{"type", "line"}, {"a", {0.0, 0.0}}, {"b", {10.0, 0.0}}});
    sk["entities"].push_back(
        {{"type", "line"}, {"a", {0.0, 0.0}}, {"b", {10.0, 10.0}}});
    sk["constraints"] = nlohmann::json::array({constraint});
    return sk;
  };
  const auto angle = [&](const nlohmann::json& extra) {
    nlohmann::json c = {{"kind", "angle"}, {"a", {1, 0}}, {"b", {2, 0}}};
    c.update(extra);
    return sketchWith(c);
  };

  // Radians is what the file stores and the solver works in, so the bare name
  // means radians here exactly as it does for an arc.
  REQUIRE_NOTHROW(
      DocumentIO::FeatureFromJson(angle({{"value", 0.7853981634}})));
  // ...and the _deg twin is the same angle said the other way.
  REQUIRE_NOTHROW(DocumentIO::FeatureFromJson(angle({{"value_deg", 45.0}})));

  // 45 in the radians field is what the old wire meant by 45 degrees.
  REQUIRE_THROWS_WITH(DocumentIO::FeatureFromJson(angle({{"value", 45.0}})),
                      Catch::Matchers::ContainsSubstring("RADIANS") &&
                          Catch::Matchers::ContainsSubstring("value_deg"));

  REQUIRE_THROWS_WITH(DocumentIO::FeatureFromJson(
                          angle({{"value", 0.785}, {"value_deg", 45.0}})),
                      Catch::Matchers::ContainsSubstring("not both"));

  // A millimetre dimension is untouched: there is nothing to confuse, and 45
  // is a perfectly ordinary distance.
  const nlohmann::json dist = {
      {"kind", "distance"}, {"a", {1, 0}}, {"b", {1, 1}}, {"value", 45.0}};
  REQUIRE_NOTHROW(DocumentIO::FeatureFromJson(sketchWith(dist)));
}

// --- The loader refuses an enum string it does not recognise ---------------
//
// It used to be split: eight parsers threw, six silently kept the default. The
// silent half produced a document that LOADED, looked right and was wrong —
// an A2 sheet as A4, a revolute mate as fastened (solving fine, with the wrong
// degrees of freedom), a modal study as static (the wrong physics, reported
// with the same confidence as the right physics).
//
// Two of the six had already been fixed for exactly this reason — the
// dimension-kind loader, and ParseSheetSizeOrThrow on the RPC side, whose
// comment calls it "the silent A3-stays-A4 bug class". These pin the rest, so
// the sweep cannot go half-done again.
//
// The directory format is deliberately one hand-editable file per item, so a
// bad string is a normal-workflow risk here, not a corner case.

TEST_CASE("an unknown mate type is refused, not demoted to fastened",
          "[serialization][enums]") {
  nlohmann::json j = {
      {"id", 1},
      {"mate_type", "revolut"}, // one character off
      {"a", {{"occurrence", 1}, {"anchor", {{"origin", true}}}}},
      {"b", {{"occurrence", 2}, {"anchor", {{"origin", true}}}}}};
  REQUIRE_THROWS_WITH(
      DocumentIO::MateFromJson(j),
      Catch::Matchers::ContainsSubstring("unknown mate type 'revolut'") &&
          Catch::Matchers::ContainsSubstring("revolute"));
}

TEST_CASE("every real mate type still round-trips", "[serialization][enums]") {
  // The refusal must not have swallowed a legitimate value — including
  // "fastened", which used to be the fall-through and is now explicit.
  for (const char* name :
       {"fastened", "revolute", "slider", "cylindrical", "planar", "ball"}) {
    nlohmann::json j = {
        {"id", 1},
        {"mate_type", name},
        {"a", {{"occurrence", 1}, {"anchor", {{"origin", true}}}}},
        {"b", {{"occurrence", 2}, {"anchor", {{"origin", true}}}}}};
    INFO("mate type " << name);
    REQUIRE_NOTHROW(DocumentIO::MateFromJson(j));
    // Round-trip through the writer, so the pair cannot drift apart.
    CHECK(DocumentIO::MateToJson(DocumentIO::MateFromJson(j))
              .at("mate_type")
              .get<std::string>() == std::string(name));
  }
}
