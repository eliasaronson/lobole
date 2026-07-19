#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/LoftFeature.h"
#include "core/io/Serialization.h"
#include "core/features/SketchFeature.h"

#include <filesystem>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {
// A square sketch of half-size `h` centered on (0,0) of a plane at z=`z`.
uint64_t SquareAt(Document& doc, double z, double h, const char* name) {
  SketchPlane plane(glm::vec3(0, 0, z), glm::vec3(0, 0, 1),
                    glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>(name, plane);
  sk->AddRectangle({-h, -h}, {h, h});
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}
} // namespace

TEST_CASE("Loft between two squares makes a frustum", "[loft]") {
  Document doc;
  // Bottom square 4x4 (half 2) at z=0, top square 2x2 (half 1) at z=6.
  const uint64_t a = SquareAt(doc, 0.0, 2.0, "Bottom");
  const uint64_t b = SquareAt(doc, 6.0, 1.0, "Top");
  doc.AddFeature(std::make_unique<LoftFeature>(
      "Frustum", std::vector<uint64_t>{a, b}));
  REQUIRE(doc.GetBodies().size() == 1);
  // Prismatoid: V = h/6 (A_bottom + 4 A_mid + A_top). A_bottom=16, A_top=4,
  // mid square half=1.5 -> side 3 -> A_mid=9. V = 6/6 (16 + 36 + 4) = 56.
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(56.0, 1e-3));
}

TEST_CASE("Loft needs at least two sections", "[loft]") {
  Document doc;
  const uint64_t a = SquareAt(doc, 0.0, 2.0, "Only");
  doc.AddFeature(std::make_unique<LoftFeature>(
      "Bad", std::vector<uint64_t>{a}));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
}

TEST_CASE("Loft round-trips through serialization", "[loft]") {
  Document doc;
  const uint64_t a = SquareAt(doc, 0.0, 2.0, "B");
  const uint64_t b = SquareAt(doc, 5.0, 2.0, "T");
  doc.AddFeature(std::make_unique<LoftFeature>(
      "L", std::vector<uint64_t>{a, b}));
  const double v = doc.GetBodies()[0].shape.Volume(); // straight box: 4*4*5=80
  CHECK_THAT(v, WithinRel(80.0, 1e-3));

  const auto path = std::filesystem::temp_directory_path() / "loft_rt.json";
  std::string error;
  REQUIRE(DocumentIO::Save(doc, path.string(), &error));
  auto loaded = DocumentIO::Load(path.string(), &error);
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<LoftFeature*>(loaded->GetFeature(2));
  REQUIRE(rt != nullptr);
  CHECK(rt->sketchIds.size() == 2);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(v, 1e-9));
  std::filesystem::remove(path);
}
