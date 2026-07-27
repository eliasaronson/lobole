#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/ShellFeature.h"
#include "core/io/Serialization.h"

#include <filesystem>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {
// Name of the box face whose outward normal is +Z (the "top"): the box spans
// [0,s] on each axis, so the +Z face is the one whose centroid has max z.
// We just enumerate names and pick by resolving — but the test only needs
// *a* face to open, so grab the first.
std::string AnyFace(const Body& b) {
  return b.elementMap.names.begin()->second;
}
} // namespace

TEST_CASE("Shell with an open face hollows a box into a bin", "[shell]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0), 10.0, 10.0, 10.0));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const std::string face = AnyFace(doc.GetBodies()[0]);

  // Open one face of a 10^3 cube, wall 1: cavity is 8x8x9, so the solid is
  // 1000 - 576 = 424 (the same for any single face of a cube, by symmetry).
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Bin", id, std::vector<ElementRef>{ElementRef{id, face}}, 1.0));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(424.0, 1e-2));
}

TEST_CASE("Shell without an open face errors", "[shell]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 6, 6, 6));
  const uint64_t id = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Sealed", id, std::vector<ElementRef>{}, 1.0));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(1)->GetError().empty());
}

TEST_CASE("Shell errors on a missing body and round-trips (v20)", "[shell]") {
  Document doc;
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Bad", 999, std::vector<ElementRef>{}, 1.0));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(0)->GetError().empty());

  Document doc2;
  doc2.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 6, 6, 6));
  const uint64_t id = doc2.GetFeature(0)->GetId();
  const std::string f = AnyFace(doc2.GetBodies()[0]);
  auto sh = std::make_unique<ShellFeature>(
      "H", id, std::vector<ElementRef>{ElementRef{id, f}}, 1.0);
  doc2.AddFeature(std::move(sh));
  const double v = doc2.GetBodies()[0].shape.Volume();

  const auto path = std::filesystem::temp_directory_path() / "shell_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc2, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<ShellFeature*>(loaded->GetFeature(1));
  REQUIRE(rt != nullptr);
  CHECK(rt->bodyId == id);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(v, 1e-9));
  std::filesystem::remove(path);
}
