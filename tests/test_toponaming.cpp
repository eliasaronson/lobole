#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/ExtrudeFeature.h"
#include "core/Feature.h"
#include "core/SketchFeature.h"
#include "core/Serialization.h"

#include <array>
#include <filesystem>
#include <string>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

static const SketchPlane kXY(PlaneType::XY);

namespace {

// A rectangle sketch (4 lines) + a NewBody extrude. Returns the 4 line ids;
// outputs the extrude feature's id.
std::array<uint32_t, 4> BuildRectPrism(Document& doc, uint64_t& extrudeId,
                                       glm::vec2 c1 = {0, 0},
                                       glm::vec2 c2 = {4, 4}) {
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  std::array<uint32_t, 4> lines = sk->AddRectangle(c1, c2);
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketchId, 3.0,
                                                  ExtrudeMode::NewBody));
  extrudeId = doc.GetFeature(1)->GetId();
  return lines;
}

std::string SideName(uint64_t extrudeId, uint32_t entityId) {
  return std::to_string(extrudeId) + "/g" + std::to_string(entityId);
}

const ElementMap& Map(const Document& doc) {
  return doc.GetBodies().at(0).elementMap;
}

SketchFeature& Sketch(Document& doc) {
  return *dynamic_cast<SketchFeature*>(doc.GetFeature(0));
}

} // namespace

TEST_CASE("Extrude names its side faces and caps", "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  auto lines = BuildRectPrism(doc, ext);
  const ElementMap& map = Map(doc);

  // A rectangle prism has 6 faces: 4 named sides + 2 caps.
  CHECK(map.names.size() == 6);
  for (uint32_t line : lines) {
    const auto idx = map.Resolve(SideName(ext, line));
    INFO("side face for line " << line);
    REQUIRE(idx.has_value());
    // Round-trips: the index maps back to the same name.
    REQUIRE(map.NameOf(*idx) != nullptr);
    CHECK(*map.NameOf(*idx) == SideName(ext, line));
  }
  CHECK(map.Resolve(std::to_string(ext) + "/cap0").has_value());
  CHECK(map.Resolve(std::to_string(ext) + "/cap1").has_value());
}

TEST_CASE("Side-face names survive a geometry edit (the headline)",
          "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  auto lines = BuildRectPrism(doc, ext);
  const std::string name = SideName(ext, lines[0]);
  const int before = *Map(doc).Resolve(name);

  // Scale every rectangle line about the origin — a real geometry change that
  // keeps the entities (and their ids) intact.
  for (auto& e : Sketch(doc).entities) {
    if (auto* line = std::get_if<sketch::Line>(&e)) {
      line->a *= 2.0f;
      line->b *= 2.0f;
    }
  }
  REQUIRE(doc.Regenerate());

  // The face for line 0 still resolves to its name, whatever its index now is.
  const auto after = Map(doc).Resolve(name);
  REQUIRE(after.has_value());
  CHECK(*Map(doc).NameOf(*after) == name);
  (void)before;
}

TEST_CASE("Names survive an added hole and caps keep their names",
          "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  auto lines = BuildRectPrism(doc, ext);
  const std::string sideName = SideName(ext, lines[0]);
  const std::string cap0 = std::to_string(ext) + "/cap0";
  REQUIRE(Map(doc).Resolve(sideName).has_value());

  // Add an interior hole — reorders faces (a new cylindrical side appears).
  const uint32_t hole = Sketch(doc).AddEntity(sketch::Circle{{2, 2}, 1.0});
  REQUIRE(doc.Regenerate());

  // Rectangle side + caps keep their names despite the reindexing...
  CHECK(Map(doc).Resolve(sideName).has_value());
  CHECK(Map(doc).Resolve(cap0).has_value());
  // ...and the hole's cylindrical face is named from its entity id.
  CHECK(Map(doc).Resolve(SideName(ext, hole)).has_value());
}

TEST_CASE("Deleting a referenced entity makes its name stop resolving",
          "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  auto lines = BuildRectPrism(doc, ext);
  // Add a hole so deleting it leaves a valid profile (the rectangle alone).
  const uint32_t hole = Sketch(doc).AddEntity(sketch::Circle{{2, 2}, 1.0});
  REQUIRE(doc.Regenerate());
  const std::string holeName = SideName(ext, hole);
  REQUIRE(Map(doc).Resolve(holeName).has_value());

  Sketch(doc).DeleteEntity(hole);
  REQUIRE(doc.Regenerate());

  // The toponaming contract: the deleted element's name reports gone rather
  // than silently pointing at a renumbered face.
  CHECK_FALSE(Map(doc).Resolve(holeName).has_value());
  // The rectangle sides are unaffected.
  CHECK(Map(doc).Resolve(SideName(ext, lines[0])).has_value());
}

TEST_CASE("Only NewBody extrudes carry an element map", "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext); // NewBody -> a body with a full map
  REQUIRE_FALSE(Map(doc).Empty());

  // A second sketch + an Add extrude fusing into the first body: the fused
  // body's map is empty (a boolean renumbers faces — that's T5).
  auto sk = std::make_unique<SketchFeature>("Sketch2", kXY);
  sk->AddRectangle({1, 1}, {3, 3});
  doc.AddFeature(std::move(sk));
  const uint64_t sk2 = doc.GetFeature(2)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Boss", sk2, 5.0,
                                                  ExtrudeMode::Add));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].elementMap.Empty());
}

// ---- T3/T4: ElementRef, sketch-on-face, error state -------------------------

namespace {

// Extrude A (a 4x4 rect at distance H), then a sketch attached to A's TOP CAP
// by a face ElementRef, then extrude B on it. Returns B's producing-feature id;
// outputs the extrude-A feature id.
uint64_t BuildStackedOnCap(Document& doc, double distA, uint64_t& extAid) {
  auto skA = std::make_unique<SketchFeature>("SkA", kXY);
  skA->AddRectangle({0, 0}, {4, 4});
  doc.AddFeature(std::move(skA));
  const uint64_t skAid = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("A", skAid, distA, ExtrudeMode::NewBody));
  extAid = doc.GetFeature(1)->GetId();

  // Sketch on A's top cap (cap1 = LastShape). The constructor plane is a
  // placeholder; Execute re-derives it from the resolved face.
  auto skB = std::make_unique<SketchFeature>("SkB", kXY);
  skB->faceRef = ElementRef{extAid, std::to_string(extAid) + "/cap1"};
  skB->AddRectangle({0, 0}, {1, 1});
  doc.AddFeature(std::move(skB));
  const uint64_t skBid = doc.GetFeature(2)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("B", skBid, 2.0, ExtrudeMode::NewBody));
  return doc.GetFeature(3)->GetId();
}

double BodyMinZ(const Document& doc, uint64_t createdBy) {
  for (const auto& b : doc.GetBodies()) {
    if (b.createdBy == createdBy) {
      glm::vec3 mn, mx;
      b.shape.GetBoundingBox(mn, mx);
      return mn.z;
    }
  }
  return -1e9;
}

} // namespace

TEST_CASE("A face-attached sketch follows edits to the base (the payoff)",
          "[toponaming]") {
  Document doc;
  uint64_t extA = 0;
  const uint64_t extB = BuildStackedOnCap(doc, 3.0, extA);
  REQUIRE(doc.GetBodies().size() == 2); // base + the feature on top
  // B sits on A's top cap at z = 3.
  CHECK_THAT(BodyMinZ(doc, extB), WithinAbs(3.0, 1e-3));

  // Raise A's height; B rides up with the cap it's attached to.
  auto* extAfeat = dynamic_cast<ExtrudeFeature*>(doc.FindFeatureById(extA));
  REQUIRE(extAfeat != nullptr);
  extAfeat->distance = 5.0;
  REQUIRE(doc.Regenerate());
  CHECK_THAT(BodyMinZ(doc, extB), WithinAbs(5.0, 1e-3));
}

TEST_CASE("A dangling face ref errors the sketch", "[toponaming]") {
  Document doc;
  uint64_t extA = 0;
  BuildStackedOnCap(doc, 3.0, extA);
  auto* skB = dynamic_cast<SketchFeature*>(doc.GetFeature(2));
  REQUIRE(skB != nullptr);

  // Point the face ref at a name that no longer exists.
  skB->faceRef.name = "nonsense";
  CHECK_FALSE(doc.Regenerate());
  CHECK_FALSE(skB->GetError().empty()); // surfaces the loss, not a silent snap
}

TEST_CASE("A face-attached sketch round-trips through JSON", "[toponaming]") {
  const fs::path path = fs::temp_directory_path() / "designer_faceref_test.json";
  {
    Document doc;
    uint64_t extA = 0;
    BuildStackedOnCap(doc, 3.0, extA);
    REQUIRE(DocumentIO::Save(doc, path.string()));
  }
  const auto loaded = DocumentIO::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* skB = dynamic_cast<SketchFeature*>(loaded->GetFeature(2));
  REQUIRE(skB != nullptr);
  CHECK(skB->faceRef.IsSet());
  CHECK(skB->faceRef.name.find("/cap1") != std::string::npos);
  REQUIRE(loaded->GetBodies().size() == 2);
  fs::remove(path);
}

TEST_CASE("CADShape::FacePlane extracts planar faces and rejects curved ones",
          "[toponaming]") {
  Document box;
  box.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0, 0, 0), 2.0, 2.0,
                                              2.0));
  glm::vec3 c, n, u;
  // A box's faces are all planar.
  CHECK(box.GetBodies()[0].shape.FacePlane(0, c, n, u));
  CHECK_THAT(glm::length(n), WithinAbs(1.0f, 1e-5f)); // unit normal

  Document cyl;
  cyl.AddFeature(std::make_unique<CylinderFeature>(
      "C", glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 1.0, 2.0));
  const CADShape& shape = cyl.GetBodies()[0].shape;
  bool anyNonPlanar = false;
  for (int i = 0; i < 5; i++) {
    if (!shape.FacePlane(i, c, n, u))
      anyNonPlanar = true; // the lateral surface is a cylinder, not a plane
  }
  CHECK(anyNonPlanar);
}

TEST_CASE("A feature that fails records an error message", "[toponaming]") {
  Document doc;
  // An extrude referencing a missing sketch fails without a specific message,
  // so Regenerate stamps the generic one.
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Bad", 9999, 1.0, ExtrudeMode::NewBody));
  CHECK_FALSE(doc.Regenerate());
  CHECK_FALSE(doc.GetFeature(0)->GetError().empty());
}
