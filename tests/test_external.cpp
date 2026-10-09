// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <filesystem>
#include <string>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;
using sketch::ExternalKind;

static const SketchPlane kXY(PlaneType::XY);

// A 4x4 rectangle extruded up 3 -> a box body (createdBy == extrude id) whose
// top cap ("<ext>/cap1") is parallel to XY.
static uint64_t BuildBox(Document& doc) {
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  sk->AddRectangle({0, 0}, {4, 4});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketchId, 3.0,
                                                  ExtrudeMode::NewBody));
  return doc.GetFeature(1)->GetId();
}

TEST_CASE("Projecting a face yields locked construction geometry",
          "[external]") {
  Document doc;
  const uint64_t ext = BuildBox(doc);
  auto sk2 = std::make_unique<SketchFeature>("Ref", kXY);
  sk2->AddExternalGeometry(ElementRef{ext, std::to_string(ext) + "/cap1"},
                           ExternalKind::Face);
  doc.AddFeature(std::move(sk2)); // regenerates -> projects the cap boundary

  auto* s2 = dynamic_cast<SketchFeature*>(doc.GetFeature(2));
  REQUIRE(s2 != nullptr);
  REQUIRE(s2->external.size() == 1);
  REQUIRE(s2->external[0].genIds.size() == 4); // 4 boundary edges of the cap

  // Every generated entity is a construction Line, and together they span the
  // 4x4 rectangle (projected onto XY).
  float minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
  for (uint32_t id : s2->external[0].genIds) {
    const size_t idx = s2->IndexOfEntity(id);
    REQUIRE(idx != SIZE_MAX);
    CHECK(sketch::IsConstruction(s2->entities[idx]));
    auto* l = std::get_if<sketch::Line>(&s2->entities[idx]);
    REQUIRE(l != nullptr);
    for (glm::vec2 p : {l->a, l->b}) {
      minX = std::min(minX, p.x);
      maxX = std::max(maxX, p.x);
      minY = std::min(minY, p.y);
      maxY = std::max(maxY, p.y);
    }
  }
  CHECK_THAT(minX, WithinAbs(0.0f, 1e-4));
  CHECK_THAT(maxX, WithinAbs(4.0f, 1e-4));
  CHECK_THAT(minY, WithinAbs(0.0f, 1e-4));
  CHECK_THAT(maxY, WithinAbs(4.0f, 1e-4));
}

TEST_CASE("Projected geometry keeps stable ids across regeneration",
          "[external]") {
  Document doc;
  const uint64_t ext = BuildBox(doc);
  auto sk2 = std::make_unique<SketchFeature>("Ref", kXY);
  sk2->AddExternalGeometry(ElementRef{ext, std::to_string(ext) + "/cap1"},
                           ExternalKind::Face);
  doc.AddFeature(std::move(sk2));
  auto* s2 = dynamic_cast<SketchFeature*>(doc.GetFeature(2));
  const std::vector<uint32_t> ids = s2->external[0].genIds;

  doc.Regenerate();
  // Same ids reused (so a user's constraints to them survive), no duplication.
  CHECK(s2->external[0].genIds == ids);
  CHECK(s2->entities.size() == 4); // no runaway growth
}

TEST_CASE("A missing external reference fails regeneration", "[external]") {
  Document doc;
  const uint64_t ext = BuildBox(doc);
  auto sk2 = std::make_unique<SketchFeature>("Ref", kXY);
  sk2->AddExternalGeometry(ElementRef{ext, "nope"}, ExternalKind::Face);
  doc.AddFeature(std::move(sk2));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("External geometry round-trips through JSON", "[external]") {
  const fs::path path =
      fs::temp_directory_path() / "designer_external_test.json";
  uint64_t ext = 0;
  {
    Document doc;
    ext = BuildBox(doc);
    auto sk2 = std::make_unique<SketchFeature>("Ref", kXY);
    sk2->AddExternalGeometry(ElementRef{ext, std::to_string(ext) + "/cap1"},
                             ExternalKind::Face);
    doc.AddFeature(std::move(sk2));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* s2 = dynamic_cast<SketchFeature*>(loaded->GetFeature(2));
  REQUIRE(s2 != nullptr);
  REQUIRE(s2->external.size() == 1);
  CHECK(s2->external[0].kind == ExternalKind::Face);
  CHECK(s2->external[0].ref.name == std::to_string(ext) + "/cap1");
  CHECK(s2->external[0].genIds.size() == 4); // re-projected on load
  fs::remove(path);
}
