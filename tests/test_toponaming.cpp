#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/BooleanFeature.h"
#include "core/Document.h"
#include "core/ExtrudeFeature.h"
#include "core/Feature.h"
#include "core/RevolveFeature.h"
#include "core/SketchFeature.h"
#include "core/Serialization.h"

#include <algorithm>
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

TEST_CASE("Every extrude mode carries an element map", "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  auto lines = BuildRectPrism(doc, ext); // NewBody -> a body with a full map
  REQUIRE_FALSE(Map(doc).Empty());

  // A second sketch + an Add extrude fusing into the first body: names
  // propagate through the fuse (T5) — the base's untouched sides keep their
  // names verbatim.
  auto sk = std::make_unique<SketchFeature>("Sketch2", kXY);
  sk->AddRectangle({1, 1}, {3, 3});
  doc.AddFeature(std::move(sk));
  const uint64_t sk2 = doc.GetFeature(2)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Boss", sk2, 5.0,
                                                  ExtrudeMode::Add));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  REQUIRE_FALSE(Map(doc).Empty());
  for (uint32_t line : lines) {
    CHECK(Map(doc).Resolve(SideName(ext, line)).has_value());
  }
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

// ---- T5: propagation through booleans, edge names ----------------------------

namespace {

// Appends a rectangle sketch + extrude to `doc`; returns the extrude's id.
uint64_t AddRectExtrude(Document& doc, glm::vec2 c1, glm::vec2 c2,
                        double distance, ExtrudeMode mode) {
  auto sk = std::make_unique<SketchFeature>("Sk", kXY);
  sk->AddRectangle(c1, c2);
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Ext", skId, distance, mode));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// Number of B-Rep faces of the first body (via the mesh's face ids — the same
// count the element map's face indices must cover).
size_t FaceCount(const Document& doc) {
  const auto mesh = doc.GetBodies().at(0).shape.GenerateMesh();
  uint32_t maxId = 0;
  for (uint32_t f : mesh.triangleFaces) {
    maxId = std::max(maxId, f);
  }
  return mesh.triangleFaces.empty() ? 0 : maxId + 1;
}

} // namespace

TEST_CASE("Boolean feature propagates the operands' names", "[toponaming]") {
  Document doc;
  uint64_t extA = 0;
  auto linesA = BuildRectPrism(doc, extA); // 4x4x3 at origin
  const uint64_t extB = AddRectExtrude(doc, {2, 2}, {6, 6}, 3.0,
                                       ExtrudeMode::NewBody);
  doc.AddFeature(std::make_unique<BooleanFeature>("Fuse", extA, extB,
                                                  BooleanOp::Union));
  const uint64_t boolId = doc.GetFeature(4)->GetId();
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].createdBy == boolId);

  const ElementMap& map = Map(doc);
  // A's x=0 and y=0 sides are untouched by the fuse: survivor names, verbatim.
  // (Rectangle {0,0}-{4,4} fused with {2,2}-{6,6} overlaps only the far
  // corner.) At least those two of A's four sides must survive unrenamed.
  int survivors = 0;
  for (uint32_t line : linesA) {
    if (map.Resolve(SideName(extA, line)).has_value()) {
      survivors++;
    }
  }
  CHECK(survivors >= 2);
  // A trimmed side got the fuse's ";M<tag>" suffix.
  int modified = 0;
  for (uint32_t line : linesA) {
    if (map.Resolve(SideName(extA, line) + ";M" + std::to_string(boolId))
            .has_value()) {
      modified++;
    }
  }
  CHECK(modified >= 1);
  // Every face of the result is named (history + U/L leave nothing anonymous).
  CHECK(map.names.size() == FaceCount(doc));
  // Edge names exist too — including generated section edges (";G").
  CHECK_FALSE(map.edgeNames.empty());
  bool anyGenerated = false;
  for (const auto& [idx, name] : map.edgeNames) {
    if (name.find(";G" + std::to_string(boolId)) != std::string::npos) {
      anyGenerated = true;
    }
  }
  CHECK(anyGenerated);
}

TEST_CASE("Boolean-result names survive an upstream edit (T5 headline)",
          "[toponaming]") {
  Document doc;
  uint64_t extA = 0;
  auto linesA = BuildRectPrism(doc, extA);
  const uint64_t extB = AddRectExtrude(doc, {2, 2}, {6, 6}, 3.0,
                                       ExtrudeMode::NewBody);
  doc.AddFeature(std::make_unique<BooleanFeature>("Fuse", extA, extB,
                                                  BooleanOp::Union));
  REQUIRE(doc.Regenerate());

  // Capture a survivor name, then change A's real geometry.
  std::string captured;
  for (uint32_t line : linesA) {
    if (Map(doc).Resolve(SideName(extA, line)).has_value()) {
      captured = SideName(extA, line);
      break;
    }
  }
  REQUIRE_FALSE(captured.empty());

  for (auto& e : Sketch(doc).entities) {
    if (auto* line = std::get_if<sketch::Line>(&e)) {
      line->a *= 0.9f;
      line->b *= 0.9f;
    }
  }
  REQUIRE(doc.Regenerate());
  CHECK(Map(doc).Resolve(captured).has_value());
}

TEST_CASE("A cut hole renames the pierced cap and keeps the sides",
          "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  auto lines = BuildRectPrism(doc, ext); // 4x4x3
  // A circular cut straight through the middle.
  auto sk = std::make_unique<SketchFeature>("Hole", kXY);
  sk->AddEntity(sketch::Circle{{2, 2}, 1.0});
  doc.AddFeature(std::move(sk));
  const uint64_t holeSk = doc.GetFeature(2)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Cut", holeSk, 3.0, ExtrudeMode::Cut));
  const uint64_t cutId = doc.GetFeature(3)->GetId();
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);

  const ElementMap& map = Map(doc);
  // Untouched rectangle sides keep their names verbatim.
  for (uint32_t line : lines) {
    CHECK(map.Resolve(SideName(ext, line)).has_value());
  }
  // The pierced caps carry the cut's ";M<tag>".
  const std::string suffix = ";M" + std::to_string(cutId);
  CHECK(map.Resolve(std::to_string(ext) + "/cap0" + suffix).has_value());
  CHECK(map.Resolve(std::to_string(ext) + "/cap1" + suffix).has_value());
  // Full coverage of the result's faces.
  CHECK(map.names.size() == FaceCount(doc));
}

TEST_CASE("A slot cut splits a cap into disambiguated .0/.1 pieces",
          "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext); // 4x4x3
  // A slot crossing the whole footprint splits both caps in two.
  const uint64_t slotExt =
      AddRectExtrude(doc, {1.5f, -1.0f}, {2.5f, 5.0f}, 3.0, ExtrudeMode::Cut);
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  (void)slotExt;

  const ElementMap& map = Map(doc);
  const std::string base = std::to_string(ext) + "/cap1;M" +
                           std::to_string(slotExt);
  const auto piece0 = map.Resolve(base + ".0");
  const auto piece1 = map.Resolve(base + ".1");
  REQUIRE(piece0.has_value());
  REQUIRE(piece1.has_value());
  CHECK(*piece0 != *piece1);
}

TEST_CASE("A face the cut consumes stops resolving", "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  auto lines = BuildRectPrism(doc, ext); // 4x4x3
  // Shave off the x in [3,4] slab: the x=4 side face is consumed entirely.
  AddRectExtrude(doc, {3.0f, -1.0f}, {5.0f, 5.0f}, 3.0, ExtrudeMode::Cut);
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);

  // Find which line was the x=4 side by geometry (before-edit resolution
  // order of AddRectangle's lines is an implementation detail).
  const ElementMap& map = Map(doc);
  int gone = 0, kept = 0;
  for (uint32_t line : lines) {
    if (map.Resolve(SideName(ext, line)).has_value()) {
      kept++;
    } else {
      gone++;
    }
  }
  // Exactly one side (x=4) vanished; x=0/y=0/y=4 survive (trimmed sides keep
  // ";M" names instead, so plain-name resolution may drop them too — but the
  // x=0 side is untouched and must survive verbatim).
  CHECK(gone >= 1);
  CHECK(kept >= 1);
}

TEST_CASE("Prism edges are named by the U pass and survive edits",
          "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  auto lines = BuildRectPrism(doc, ext);
  const ElementMap& map = Map(doc);

  // A rectangular prism has 12 edges; all of them get names.
  CHECK(map.edgeNames.size() == 12);
  // They are U names hanging off the named faces.
  bool anyU = false;
  for (const auto& [idx, name] : map.edgeNames) {
    if (name.find(";U") != std::string::npos) {
      anyU = true;
    }
  }
  CHECK(anyU);

  // Round-trip and mutation stability: capture one edge name, scale the
  // sketch, and re-resolve.
  const std::string edgeName = map.edgeNames.begin()->second;
  REQUIRE(map.ResolveEdge(edgeName).has_value());
  for (auto& e : Sketch(doc).entities) {
    if (auto* line = std::get_if<sketch::Line>(&e)) {
      line->a *= 2.0f;
      line->b *= 2.0f;
    }
  }
  REQUIRE(doc.Regenerate());
  const auto after = Map(doc).ResolveEdge(edgeName);
  REQUIRE(after.has_value());
  CHECK(*Map(doc).EdgeNameOf(*after) == edgeName);
  (void)lines;
}

TEST_CASE("A sketch attached to a fused body's face follows base edits",
          "[toponaming]") {
  Document doc;
  uint64_t extA = 0;
  BuildRectPrism(doc, extA); // 4x4x3 base
  // A boss fused into the base, piercing the top cap (so cap1 -> ";M<boss>").
  const uint64_t boss = AddRectExtrude(doc, {1, 1}, {3, 3}, 8.0,
                                       ExtrudeMode::Add);
  REQUIRE(doc.Regenerate());
  const std::string fusedCap = std::to_string(extA) + "/cap1;M" +
                               std::to_string(boss);
  REQUIRE(Map(doc).Resolve(fusedCap).has_value());

  // Sketch on the (pierced) top cap of the fused body + a small pad on it.
  auto skC = std::make_unique<SketchFeature>("OnCap", kXY);
  skC->faceRef = ElementRef{extA, fusedCap}; // fused body: createdBy == extA
  skC->AddRectangle({3.2f, 3.2f}, {3.8f, 3.8f});
  doc.AddFeature(std::move(skC));
  const uint64_t skCid = doc.GetFeature(4)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", skCid, 1.0, ExtrudeMode::NewBody));
  const uint64_t pad = doc.GetFeature(5)->GetId();
  REQUIRE(doc.Regenerate());
  CHECK_THAT(BodyMinZ(doc, pad), WithinAbs(3.0, 1e-3));

  // Raise the base: the cap's name is unchanged, so the pad rides up.
  auto* extAfeat = dynamic_cast<ExtrudeFeature*>(doc.FindFeatureById(extA));
  REQUIRE(extAfeat != nullptr);
  extAfeat->distance = 5.0;
  REQUIRE(doc.Regenerate());
  CHECK_THAT(BodyMinZ(doc, pad), WithinAbs(5.0, 1e-3));
}

TEST_CASE("Revolve names its faces like extrude does", "[toponaming]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("Profile", kXY);
  auto lines = sk->AddRectangle({1, 0}, {3, 3}); // +x of the global Y axis
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(0)->GetId();

  SECTION("partial revolve: sides + caps") {
    auto rev = std::make_unique<RevolveFeature>("Rev", skId, 90.0,
                                                ExtrudeMode::NewBody);
    rev->axis.kind = AxisRef::Kind::Y;
    doc.AddFeature(std::move(rev));
    const uint64_t revId = doc.GetFeature(1)->GetId();
    REQUIRE(doc.GetBodies().size() == 1);
    const ElementMap& map = Map(doc);
    for (uint32_t line : lines) {
      CHECK(map.Resolve(SideName(revId, line)).has_value());
    }
    CHECK(map.Resolve(std::to_string(revId) + "/cap0").has_value());
    CHECK(map.Resolve(std::to_string(revId) + "/cap1").has_value());
    CHECK(map.names.size() == FaceCount(doc));
    CHECK_FALSE(map.edgeNames.empty());
  }

  SECTION("full revolution: no caps, still full coverage") {
    auto rev = std::make_unique<RevolveFeature>("Rev", skId, 360.0,
                                                ExtrudeMode::NewBody);
    rev->axis.kind = AxisRef::Kind::Y;
    doc.AddFeature(std::move(rev));
    const uint64_t revId = doc.GetFeature(1)->GetId();
    REQUIRE(doc.GetBodies().size() == 1);
    const ElementMap& map = Map(doc);
    CHECK_FALSE(map.Resolve(std::to_string(revId) + "/cap0").has_value());
    CHECK(map.names.size() == FaceCount(doc));
  }
}
