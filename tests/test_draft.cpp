// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/DraftFeature.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>

using Catch::Matchers::WithinRel;

namespace {
// The four vertical side faces of a box (normal in the XY plane, i.e. not a
// top/bottom cap) — those are the ones a +Z draft can taper.
std::vector<std::string> SideFaces(const Body& b) {
  std::vector<std::string> out;
  for (const auto& [idx, name] : b.elementMap.names) {
    glm::dvec3 c, n, u;
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
  doc.AddFeature(
      std::make_unique<DraftFeature>("Taper", id, refs, 5.0, PlaneType::XY));
  REQUIRE(doc.GetBodies().size() == 1);
  const double after = doc.GetBodies()[0].shape.Volume();
  CHECK(after < before);       // tapered inward going up
  CHECK(after > before * 0.6); // 5-degree taper, not collapsed
}

// The angle is the contract, so it is measured, not inferred from a volume
// that merely went down: every drafted face stands at 90 - 5 degrees to the
// top cap, by the measure tool, leaning in (its normal has a +Z component,
// so the part narrows going up the pull), and the solid is exactly the
// frustum a 5-degree taper from a 10 mm square at the neutral plane makes.
TEST_CASE("Draft turns each face by its angle as measured", "[draft]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));
  const uint64_t id = doc.GetFeature(0)->GetId();
  std::vector<ElementRef> refs;
  for (const std::string& f : SideFaces(doc.GetBodies()[0]))
    refs.push_back(ElementRef{id, f});
  REQUIRE(refs.size() == 4);
  doc.AddFeature(
      std::make_unique<DraftFeature>("Taper", id, refs, 5.0, PlaneType::XY));
  REQUIRE(doc.Regenerate());
  const Body& b = doc.GetBodies()[0];

  ElementRef top;
  std::vector<ElementRef> sides;
  for (const auto& [idx, name] : b.elementMap.names) {
    glm::dvec3 c, n, u;
    if (!b.shape.FacePlane(idx, c, n, u))
      continue;
    if (n.z > 0.99f)
      top = ElementRef{b.createdBy, name};
    else if (std::abs(n.z) < 0.5f) {
      sides.push_back(ElementRef{b.createdBy, name});
      INFO("side normal " << n.x << " " << n.y << " " << n.z);
      CHECK(n.z > 0.0f); // leaning in: the part narrows going up
    }
  }
  REQUIRE(top.IsSet());
  REQUIRE(sides.size() == 4);
  for (const ElementRef& s : sides) {
    const auto a = Measure::Angle(doc.GetBodies(), s, top);
    REQUIRE(a.has_value());
    CHECK_THAT(a->degrees, Catch::Matchers::WithinAbs(85.0, 1e-6));
  }
  const double inset = 10.0 * std::tan(5.0 * std::numbers::pi / 180.0);
  const double a1 = 100.0, a2 = (10.0 - 2.0 * inset) * (10.0 - 2.0 * inset);
  const double frustum = 10.0 / 3.0 * (a1 + a2 + std::sqrt(a1 * a2));
  CHECK_THAT(b.shape.Volume(), WithinRel(frustum, 1e-6));
}

TEST_CASE("Draft rejects a cap face perpendicular to the pull", "[draft]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));
  const uint64_t id = doc.GetFeature(0)->GetId();
  // Find the +Z top cap.
  std::string cap;
  for (const auto& [idx, name] : doc.GetBodies()[0].elementMap.names) {
    glm::dvec3 c, n, u;
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
      "D", id, std::vector<ElementRef>{ElementRef{id, f}}, 3.0, PlaneType::XY));
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
