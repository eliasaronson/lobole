#include <catch2/catch_test_macros.hpp>

#include "core/Document.h"
#include "core/ExtrudeFeature.h"
#include "core/SketchFeature.h"

#include <array>
#include <string>

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
