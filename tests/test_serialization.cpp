#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/Serialization.h"

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
