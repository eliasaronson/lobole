#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/features/DraftFeature.h"
#include "core/Feature.h"
#include "core/io/Serialization.h"
#include "TestIO.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

using Catch::Matchers::WithinRel;

namespace {
// The four vertical side faces of a box (normal in the XY plane, i.e. not a
// top/bottom cap) — those are the ones a +Z draft can taper.
std::vector<std::string> SideFaces(const Body& b) {
  std::vector<std::string> out;
  for (const auto& [idx, name] : b.elementMap.names) {
    glm::vec3 c, n, u;
    if (b.shape.FacePlane(idx, c, n, u) && std::abs(n.z) < 0.5)
      out.push_back(name);
  }
  std::sort(out.begin(), out.end());
  return out;
}
} // namespace

TEST_CASE("Draft tapers a box's side faces (volume drops)", "[draft]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const double before = doc.GetBodies()[0].shape.Volume();

  std::vector<ElementRef> refs;
  for (const std::string& f : SideFaces(doc.GetBodies()[0]))
    refs.push_back(ElementRef{id, f});
  REQUIRE(refs.size() == 4);
  doc.AddFeature(std::make_unique<DraftFeature>("Taper", id, refs, 5.0,
                                                PlaneType::XY));
  REQUIRE(doc.GetBodies().size() == 1);
  const double after = doc.GetBodies()[0].shape.Volume();
  CHECK(after < before);       // tapered inward going up
  CHECK(after > before * 0.6); // 5-degree taper, not collapsed
}

TEST_CASE("Draft rejects a cap face perpendicular to the pull", "[draft]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));
  const uint64_t id = doc.GetFeature(0)->GetId();
  // Find the +Z top cap.
  std::string cap;
  for (const auto& [idx, name] : doc.GetBodies()[0].elementMap.names) {
    glm::vec3 c, n, u;
    if (doc.GetBodies()[0].shape.FacePlane(idx, c, n, u) && n.z > 0.9)
      cap = name;
  }
  doc.AddFeature(std::make_unique<DraftFeature>(
      "Bad", id, std::vector<ElementRef>{ElementRef{id, cap}}, 5.0,
      PlaneType::XY));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(1)->GetError().empty());
}

TEST_CASE("Draft errors on a missing body and round-trips (v22)", "[draft]") {
  Document doc;
  doc.AddFeature(std::make_unique<DraftFeature>(
      "Bad", 999, std::vector<ElementRef>{ElementRef{999, "x"}}, 5.0,
      PlaneType::XY));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(0)->GetError().empty());

  Document doc2;
  doc2.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 8, 8, 8));
  const uint64_t id = doc2.GetFeature(0)->GetId();
  const std::string f = SideFaces(doc2.GetBodies()[0]).front();
  doc2.AddFeature(std::make_unique<DraftFeature>(
      "D", id, std::vector<ElementRef>{ElementRef{id, f}}, 3.0,
      PlaneType::XY));
  const double v = doc2.GetBodies()[0].shape.Volume();

  const auto path = std::filesystem::temp_directory_path() / "draft_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc2, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<DraftFeature*>(loaded->GetFeature(1));
  REQUIRE(rt != nullptr);
  CHECK(rt->bodyId == id);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(v, 1e-9));
  std::filesystem::remove(path);
}
