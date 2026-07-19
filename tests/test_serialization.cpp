#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/io/Serialization.h"

#include "CADKernel.h"

#include <filesystem>
#include <fstream>

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

TEST_CASE("ImportFeature loads a STEP file centered at origin", "[feature][io]") {
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
  doc.AddFeature(std::make_unique<BoxFeature>("Base", glm::vec3(1, 0, -2),
                                              2.0, 3.0, 4.0));
  doc.AddFeature(
      std::make_unique<SphereFeature>("Ball", glm::vec3(5, 0, 0), 1.5));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Rod", glm::vec3(-3, 0, 0), glm::vec3(0, 1, 0), 0.5, 2.0));

  fs::path path = TempFile("designer_test_doc.json");
  std::string error;
  REQUIRE(DocumentIO::Save(doc, path.string(), &error));

  auto loaded = DocumentIO::Load(path.string(), &error);
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

TEST_CASE("Feature material round-trips and stamps its body", "[serialization]") {
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
  REQUIRE(DocumentIO::Save(doc, path.string(), &error));

  auto loaded = DocumentIO::Load(path.string(), &error);
  REQUIRE(loaded != nullptr);
  REQUIRE(loaded->FeatureCount() == 2);
  CHECK(loaded->GetFeature(0)->GetMaterial() == "steel");
  CHECK(loaded->GetFeature(1)->GetMaterial().empty());
  // And the material survives regeneration onto the rebuilt body.
  CHECK(loaded->GetBodies()[0].material == "steel");
  CHECK(loaded->GetBodies()[1].material.empty());

  fs::remove(path);
}

TEST_CASE("Loading rejects non-document JSON", "[serialization]") {
  fs::path path = TempFile("designer_test_bad.json");
  {
    std::ofstream out(path);
    out << R"({"format": "something-else", "features": []})";
  }

  std::string error;
  CHECK(DocumentIO::Load(path.string(), &error) == nullptr);
  CHECK_FALSE(error.empty());

  fs::remove(path);
}

TEST_CASE("Loading rejects unknown feature types", "[serialization]") {
  fs::path path = TempFile("designer_test_unknown.json");
  {
    std::ofstream out(path);
    out << R"({"format": "designer-document", "version": 1,
               "features": [{"type": "Warp", "name": "x"}]})";
  }

  std::string error;
  CHECK(DocumentIO::Load(path.string(), &error) == nullptr);
  CHECK(error.find("Warp") != std::string::npos);

  fs::remove(path);
}

// ---- FEM studies (v29) ----------------------------------------------------

namespace {
Document DocWithStudy() {
  Document doc;
  doc.SetParameter("load_n", "1000");
  doc.AddFeature(std::make_unique<BoxFeature>("Base", glm::vec3(0, 0, 0),
                                              10.0, 4.0, 4.0));
  fem::Study& s = doc.AddStudy("Static 1");
  s.meshSize = 5.0;
  s.meshSizeExpr = "";
  s.refinements.push_back(
      {ElementRef{2, "2/g1"}, 1.0, std::string{}});
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
}  // namespace

TEST_CASE("Study round-trips through the single-file format", "[serialization][fem]") {
  Document doc = DocWithStudy();
  fs::path path = TempFile("designer_test_study.json");
  std::string error;
  REQUIRE(DocumentIO::Save(doc, path.string(), &error));
  auto loaded = DocumentIO::Load(path.string(), &error);
  INFO(error);
  REQUIRE(loaded != nullptr);
  CheckStudyMatches(*loaded);
  fs::remove(path);
}

TEST_CASE("Study round-trips through the directory format", "[serialization][fem]") {
  Document doc = DocWithStudy();
  fs::path dir = fs::temp_directory_path() / "designer_test_study.dsgn";
  fs::remove_all(dir);
  std::string error;
  REQUIRE(DocumentIO::Save(doc, dir.string(), &error));
  CHECK(fs::exists(dir / "studies"));
  auto loaded = DocumentIO::Load(dir.string(), &error);
  INFO(error);
  REQUIRE(loaded != nullptr);
  CheckStudyMatches(*loaded);
  // A freshly allocated id must not collide with the loaded study/load ids
  // (the high-water mark was restored on load).
  const fem::Study& s = loaded->GetStudies()[0];
  uint64_t maxId = s.id;
  for (const fem::Load& l : s.loads) maxId = std::max(maxId, l.id);
  CHECK(loaded->AllocateId() > maxId);
  fs::remove_all(dir);
}

TEST_CASE("A document with no studies writes no studies key or dir",
          "[serialization][fem]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Base", glm::vec3(0, 0, 0),
                                              1.0, 1.0, 1.0));
  // Single-file: no "studies" key.
  fs::path path = TempFile("designer_test_nostudy.json");
  std::string error;
  REQUIRE(DocumentIO::Save(doc, path.string(), &error));
  std::ifstream in(path);
  const std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  CHECK(body.find("studies") == std::string::npos);
  fs::remove(path);

  // Directory: no studies/ subdir.
  fs::path dir = fs::temp_directory_path() / "designer_test_nostudy.dsgn";
  fs::remove_all(dir);
  REQUIRE(DocumentIO::Save(doc, dir.string(), &error));
  CHECK_FALSE(fs::exists(dir / "studies"));
  fs::remove_all(dir);
}

TEST_CASE("Modal analysis type + gravity round-trip (v32)", "[serialization][fem]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0),
                                              100.0, 10.0, 10.0));
  fem::Study& modal = doc.AddStudy("Modes");
  modal.type = fem::AnalysisType::Modal;
  modal.numModes = 8;
  modal.meshSize = 5.0;
  fem::Study& grav = doc.AddStudy("Self weight");
  grav.gravity = glm::dvec3(0, 0, -9.81);
  grav.meshSize = 5.0;

  fs::path path = TempFile("designer_test_modal.json");
  std::string error;
  REQUIRE(DocumentIO::Save(doc, path.string(), &error));
  // A static study with no gravity writes none of the new keys (byte-identity).
  {
    Document plain;
    plain.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0, 0, 0), 1, 1, 1));
    plain.AddStudy("S");
    fs::path p2 = TempFile("designer_test_plainstudy.json");
    REQUIRE(DocumentIO::Save(plain, p2.string(), &error));
    std::ifstream in(p2);
    const std::string body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(body.find("analysis") == std::string::npos);
    CHECK(body.find("gravity") == std::string::npos);
    fs::remove(p2);
  }

  auto loaded = DocumentIO::Load(path.string(), &error);
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
