// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/RibFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <cmath>
#include <filesystem>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {
uint64_t AddSketch(Document& doc, const SketchPlane& plane, const char* name,
                   const std::vector<sketch::Entity>& ents) {
  auto sk = std::make_unique<SketchFeature>(name, plane);
  for (const auto& e : ents)
    sk->AddEntity(e);
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// Box [0,10]^3 + an open line profile on a plane at z=15 facing the box
// (normal -Z), spanning x in [2,8] at y=5.
uint64_t BoxAndProfile(Document& doc, const glm::vec3& normal) {
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::vec3(0), 10.0, 10.0, 10.0));
  return AddSketch(doc,
                   SketchPlane(glm::vec3(5, 5, 15), normal, glm::vec3(1, 0, 0)),
                   "RibProfile", {sketch::Line{{-3, 0}, {3, 0}}});
}

// A 6-long, `t`-thick slab contour with round (arc-joined) ends, `h` tall:
// the exact volume the rib between the profile plane and the box top adds.
double StadiumVolume(double span, double t, double h) {
  return (span * t + M_PI * (t / 2.0) * (t / 2.0)) * h;
}
} // namespace

TEST_CASE("Rib fills from the profile down to the box top", "[rib]") {
  Document doc;
  const uint64_t sk = BoxAndProfile(doc, glm::vec3(0, 0, -1));
  doc.AddFeature(std::make_unique<RibFeature>("Rib", sk, 1.0));

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetFeature(2)->GetError().empty());
  // Box 1000 + stadium slab (6 x 1, round ends) over the 5-unit gap.
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(1000.0 + StadiumVolume(6.0, 1.0, 5.0), 1e-3));

  // Deterministic: a full regeneration reproduces the same volume.
  const double v1 = doc.GetBodies()[0].shape.Volume();
  doc.ClearRegenCache();
  REQUIRE(doc.Regenerate());
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(v1, 1e-9));
}

TEST_CASE("Rib that never reaches the body errors without killing the doc",
          "[rib]") {
  Document doc;
  // Normal +Z points AWAY from the box: the slab grows into empty space.
  const uint64_t sk = BoxAndProfile(doc, glm::vec3(0, 0, 1));
  doc.AddFeature(std::make_unique<RibFeature>("Rib", sk, 1.0));

  CHECK(!doc.GetFeature(2)->GetError().empty());
  // The document still regenerated past the failed feature: box untouched.
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(1000.0, 1e-9));
}

TEST_CASE("Rib flipDirection grows against the plane normal", "[rib]") {
  Document doc;
  // Same away-facing plane as the error case; flip recovers it.
  const uint64_t sk = BoxAndProfile(doc, glm::vec3(0, 0, 1));
  doc.AddFeature(
      std::make_unique<RibFeature>("Rib", sk, 1.0, /*flipDirection=*/true));

  CHECK(doc.GetFeature(2)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(1000.0 + StadiumVolume(6.0, 1.0, 5.0), 1e-3));
}

TEST_CASE("Rib round-trips through serialization", "[rib][serialization]") {
  namespace fs = std::filesystem;
  const fs::path path = fs::temp_directory_path() / "designer_rib_test.json";
  double v0 = 0.0;
  uint64_t skId = 0;
  {
    Document doc;
    skId = BoxAndProfile(doc, glm::vec3(0, 0, -1));
    auto rib = std::make_unique<RibFeature>("Rib", skId, 1.0);
    rib->thicknessExpr = "1.0";
    doc.AddFeature(std::move(rib));
    v0 = doc.GetBodies()[0].shape.Volume();
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rb = dynamic_cast<RibFeature*>(loaded->GetFeature(2));
  REQUIRE(rb != nullptr);
  CHECK(rb->sketchId == skId);
  CHECK(rb->thickness == 1.0);
  CHECK(rb->thicknessExpr == "1.0");
  CHECK_FALSE(rb->flipDirection);
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(v0, 1e-9));
  fs::remove(path);
}
