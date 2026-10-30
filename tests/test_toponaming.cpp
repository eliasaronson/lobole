// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "TestPaths.h"
#include "core/AxisRef.h"
#include "core/Document.h"
#include "core/ElementHistory.h"
#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/features/BooleanFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Wire.hxx>
#include <cstdio>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

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
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Boss", sk2, 5.0, ExtrudeMode::Add));
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
  doc.AddFeature(std::make_unique<ExtrudeFeature>("A", skAid, distA,
                                                  ExtrudeMode::NewBody));
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
      glm::dvec3 mn, mx;
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
  const fs::path path =
      fs::temp_directory_path() / "designer_faceref_test.json";
  {
    Document doc;
    uint64_t extA = 0;
    BuildStackedOnCap(doc, 3.0, extA);
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
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
  box.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0, 0, 0), 2.0, 2.0, 2.0));
  glm::dvec3 c, n, u;
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

// ---- T5: propagation through booleans, edge names
// ----------------------------

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
  const uint64_t extB =
      AddRectExtrude(doc, {2, 2}, {6, 6}, 3.0, ExtrudeMode::NewBody);
  doc.AddFeature(
      std::make_unique<BooleanFeature>("Fuse", extA, extB, BooleanOp::Union));
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
  const uint64_t extB =
      AddRectExtrude(doc, {2, 2}, {6, 6}, 3.0, ExtrudeMode::NewBody);
  doc.AddFeature(
      std::make_unique<BooleanFeature>("Fuse", extA, extB, BooleanOp::Union));
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

TEST_CASE("A slot cut splits a cap into pieces named by their neighbours",
          "[toponaming]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext); // 4x4x3
  // A slot crossing the whole footprint splits both caps in two.
  const uint64_t slotExt =
      AddRectExtrude(doc, {1.5f, -1.0f}, {2.5f, 5.0f}, 3.0, ExtrudeMode::Cut);
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);

  const ElementMap& map = Map(doc);
  const std::string base =
      std::to_string(ext) + "/cap1;M" + std::to_string(slotExt);
  std::vector<std::string> pieces;
  for (const auto& [idx, name] : map.names)
    if (topo::ParsePiece(name).of == base)
      pieces.push_back(name);
  REQUIRE(pieces.size() == 2);
  for (const std::string& name : pieces) {
    INFO(name);
    const topo::PieceName piece = topo::ParsePiece(name);
    CHECK(piece.qualified);
    CHECK_FALSE(piece.ordinal);
    CHECK(map.Resolve(name).has_value()); // by its own name, exactly
  }
  CHECK(*map.Resolve(pieces[0]) != *map.Resolve(pieces[1]));
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
  const uint64_t boss =
      AddRectExtrude(doc, {1, 1}, {3, 3}, 8.0, ExtrudeMode::Add);
  REQUIRE(doc.Regenerate());
  const std::string fusedCap =
      std::to_string(extA) + "/cap1;M" + std::to_string(boss);
  REQUIRE(Map(doc).Resolve(fusedCap).has_value());

  // Sketch on the (pierced) top cap of the fused body + a small pad on it.
  auto skC = std::make_unique<SketchFeature>("OnCap", kXY);
  skC->faceRef = ElementRef{extA, fusedCap}; // fused body: createdBy == extA
  skC->AddRectangle({3.2f, 3.2f}, {3.8f, 3.8f});
  doc.AddFeature(std::move(skC));
  const uint64_t skCid = doc.GetFeature(4)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", skCid, 1.0,
                                                  ExtrudeMode::NewBody));
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

// ---- Patterned bodies carry names -----------------------------------------

TEST_CASE("A patterned body's instances are named, suffixed per instance",
          "[toponaming]") {
  // A face with no name cannot be picked, threaded, filleted or fitted, and
  // `list_elements` does not show it — so a patterned body used to be
  // invisible to everything downstream. Every part of the grill that repeats
  // (the grate's bars, the warming rack's) was in that state.
  Document doc;
  const uint64_t ext =
      AddRectExtrude(doc, {0, 0}, {4, 4}, 3.0, ExtrudeMode::NewBody);
  auto pat = std::make_unique<PatternFeature>("Bars", PatternKind::Linear, ext);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 3;
  pat->spacing = 10.0; // disjoint: three separate bodies
  doc.AddFeature(std::move(pat));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 3);

  // Every face of every instance is named.
  for (const Body& b : doc.GetBodies()) {
    const auto mesh = b.shape.GenerateMesh();
    uint32_t maxId = 0;
    for (uint32_t f : mesh.triangleFaces) {
      maxId = std::max(maxId, f);
    }
    const size_t faces = mesh.triangleFaces.empty() ? 0 : maxId + 1;
    CHECK(b.elementMap.names.size() == faces);
    CHECK_FALSE(b.elementMap.Empty());
  }

  // The seed keeps its names; the copies carry the instance suffix, so a face
  // of the third bar is as referenceable as one of the first.
  const auto suffixCount = [&doc](const std::string& suffix) {
    int n = 0;
    for (const Body& b : doc.GetBodies()) {
      for (const auto& [idx, name] : b.elementMap.names) {
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) ==
                0) {
          n++;
        }
      }
    }
    return n;
  };
  CHECK(suffixCount("#1") > 0);
  CHECK(suffixCount("#2") > 0);
  // And the names are distinct across instances — the whole point of the
  // suffix is that two bars' matching faces are different references.
  std::vector<std::string> all;
  for (const Body& b : doc.GetBodies()) {
    for (const auto& [idx, name] : b.elementMap.names) {
      all.push_back(name);
    }
  }
  const size_t total = all.size();
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  CHECK(all.size() == total);
}

TEST_CASE("A mirrored body is named too", "[toponaming]") {
  Document doc;
  const uint64_t ext =
      AddRectExtrude(doc, {2, 2}, {6, 6}, 3.0, ExtrudeMode::NewBody);
  auto pat =
      std::make_unique<PatternFeature>("Mirrored", PatternKind::Mirror, ext);
  pat->basePlane = PlaneType::YZ;
  doc.AddFeature(std::move(pat));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 2);
  for (const Body& b : doc.GetBodies()) {
    CHECK_FALSE(b.elementMap.Empty());
  }
  // The reflection is suffixed "#m", not "#1": it is not the first of a
  // series, and a reference must be able to say which side it means.
  bool mirrored = false;
  for (const Body& b : doc.GetBodies()) {
    for (const auto& [idx, name] : b.elementMap.names) {
      if (name.find("#m") != std::string::npos) {
        mirrored = true;
      }
    }
  }
  CHECK(mirrored);
}

// ---- Split pieces named by what bounds them
// (docs/modeling/toponaming.md#split-pieces) --------------------------------
//
// A 40x20x10 block with a 4 mm deep slot across its top at x = 12..16: the
// top splits in two. A sketch is attached to the RIGHT piece and a fillet
// rounds that piece's front edge; then the cut is edited, and each case
// asserts by centroid where both land -- or that they refuse.

namespace {

struct SlotBlock {
  Document doc;
  uint64_t base = 0, cut = 0, sketch = 0, fillet = 0;
  std::string topName, edgeName;
};

SketchFeature& SketchById(Document& doc, uint64_t id) {
  return *dynamic_cast<SketchFeature*>(doc.FindFeatureById(id));
}

glm::dvec3 FaceMeshCentroid(const Body& body, int face) {
  const auto mesh = body.shape.GenerateMesh();
  glm::dvec3 sum(0.0);
  int n = 0;
  for (size_t t = 0; t < mesh.triangleFaces.size(); t++) {
    if (static_cast<int>(mesh.triangleFaces[t]) != face)
      continue;
    for (int k = 0; k < 3; k++) {
      sum += glm::dvec3(mesh.vertices[mesh.indices[t * 3 + k]].position);
      n++;
    }
  }
  return n == 0 ? glm::dvec3(-1e9) : sum / static_cast<double>(n);
}

// Every face name and every edge name in the map is held by one element.
bool NamesUnique(const ElementMap& map) {
  std::set<std::string> faces, edges;
  for (const auto& [i, n] : map.names)
    if (!faces.insert(n).second)
      return false;
  for (const auto& [i, n] : map.edgeNames)
    if (!edges.insert(n).second)
      return false;
  return true;
}

SlotBlock BuildSlotBlock() {
  SlotBlock s;
  Document& doc = s.doc;
  auto baseSk = std::make_unique<SketchFeature>("Base", kXY);
  baseSk->AddRectangle({0, 0}, {40, 20});
  doc.AddFeature(std::move(baseSk));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Block", doc.GetFeature(0)->GetId(), 10.0, ExtrudeMode::NewBody));
  s.base = doc.GetFeature(1)->GetId();
  auto slotSk = std::make_unique<SketchFeature>(
      "SlotSketch", SketchPlane(PlaneType::XY, glm::dvec3(0, 0, 6)));
  slotSk->AddRectangle({12, -1}, {16, 21});
  doc.AddFeature(std::move(slotSk));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Slot", doc.GetFeature(2)->GetId(), 4.0, ExtrudeMode::Cut));
  s.cut = doc.GetFeature(3)->GetId();
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);

  // The right top piece, and its front edge, by geometry.
  const Body& body = doc.GetBodies().at(0);
  for (const auto& [idx, name] : body.elementMap.names) {
    glm::dvec3 c, n, u;
    if (body.shape.FacePlane(idx, c, n, u) && n.z > 0.99 &&
        std::abs(c.z - 10) < 1e-6 && c.x > 16)
      s.topName = name;
  }
  const auto polys = body.shape.GenerateEdgePolylines();
  for (const auto& [idx, name] : body.elementMap.edgeNames) {
    if (idx < 0 || static_cast<size_t>(idx) >= polys.size())
      continue;
    const auto& poly = polys[static_cast<size_t>(idx)];
    bool front = !poly.empty();
    double x = 0;
    for (const glm::vec3& p : poly) {
      front = front && std::abs(p.y) < 1e-4 && std::abs(p.z - 10) < 1e-4;
      x += p.x;
    }
    if (front && x / static_cast<double>(poly.size()) > 16)
      s.edgeName = name;
  }
  REQUIRE_FALSE(s.topName.empty());
  REQUIRE_FALSE(s.edgeName.empty());

  auto onTop = std::make_unique<SketchFeature>("OnTop", kXY);
  onTop->faceRef = ElementRef{s.base, s.topName};
  onTop->AddRectangle({-1, -1}, {1, 1});
  doc.AddFeature(std::move(onTop));
  s.sketch = doc.GetFeature(4)->GetId();
  doc.AddFeature(std::make_unique<FilletFeature>(
      "Round", std::vector<ElementRef>{{s.base, s.edgeName}}, 1.0));
  s.fillet = doc.GetFeature(5)->GetId();
  REQUIRE(doc.Regenerate());
  return s;
}

// Where the sketch sits (its plane origin, the face's centroid) and where
// the fillet's face is, in x.
double SketchX(SlotBlock& s) {
  return SketchById(s.doc, s.sketch).plane.GetOrigin().x;
}
double FilletX(const SlotBlock& s) {
  const Body& body = s.doc.GetBodies().at(0);
  const std::string g = ";G" + std::to_string(s.fillet);
  for (const auto& [idx, name] : body.elementMap.names)
    if (name.size() > g.size() &&
        name.compare(name.size() - g.size(), g.size(), g) == 0)
      return FaceMeshCentroid(body, idx).x;
  return -1e9;
}

// Applies `edit` to the slot sketch's lines (by line index 0..3).
void EditSlot(SlotBlock& s, const std::function<void(sketch::Line&)>& edit) {
  SketchFeature& sk = SketchById(s.doc, s.cut - 1);
  for (auto& e : sk.entities)
    if (auto* line = std::get_if<sketch::Line>(&e))
      edit(*line);
}

void ExpectOnRightPiece(SlotBlock& s, double from) {
  const Feature* sk = s.doc.FindFeatureById(s.sketch);
  const Feature* fi = s.doc.FindFeatureById(s.fillet);
  INFO("sketch error: " << sk->GetError() << " warning: " << sk->GetWarning());
  INFO("fillet error: " << fi->GetError() << " warning: " << fi->GetWarning());
  CHECK(sk->GetError().empty());
  CHECK(fi->GetError().empty());
  CHECK(sk->GetWarning().empty());
  CHECK(fi->GetWarning().empty());
  CHECK(SketchX(s) > from);
  CHECK(SketchX(s) < 40);
  CHECK(FilletX(s) > from);
  CHECK(FilletX(s) < 40);
}

} // namespace

TEST_CASE("A split piece is named by what bounds it, and keeps it",
          "[toponaming]") {
  SlotBlock s = BuildSlotBlock();
  // Named by the block's right wall and the slot's wall, nothing else: the
  // front and back walls bound both pieces, so they tell nothing apart.
  const std::string b = std::to_string(s.base), c = std::to_string(s.cut);
  for (const std::string& name : {s.topName, s.edgeName}) {
    INFO(name);
    const topo::PieceName piece = topo::ParsePiece(name);
    CHECK(piece.qualified);
    CHECK_FALSE(piece.ordinal);
    REQUIRE(piece.boundedBy.size() == 2);
    CHECK(piece.boundedBy[0].rfind(b + "/g", 0) == 0);
    CHECK(piece.boundedBy[0].find(';') == std::string::npos);
    CHECK(piece.boundedBy[1].rfind(c + "/g", 0) == 0);
    CHECK(piece.boundedBy[1].ends_with(";M" + c));
  }
  CHECK(s.topName.rfind(b + "/cap1;M" + c + "[", 0) == 0);
  CHECK(NamesUnique(s.doc.GetBodies().at(0).elementMap));
  ExpectOnRightPiece(s, 16);

  SECTION("the slot moved") {
    EditSlot(s, [](sketch::Line& l) {
      l.a.x += 16;
      l.b.x += 16;
    });
    s.doc.Regenerate();
    ExpectOnRightPiece(s, 32);
  }
  SECTION("the slot resized") {
    EditSlot(s, [](sketch::Line& l) {
      if (std::abs(l.a.x - 16) < 1e-9)
        l.a.x = 18;
      if (std::abs(l.b.x - 16) < 1e-9)
        l.b.x = 18;
    });
    s.doc.Regenerate();
    ExpectOnRightPiece(s, 18);
  }
  SECTION("a second slot on the far side") {
    SketchById(s.doc, s.cut - 1).AddRectangle({4, -1}, {6, 21});
    s.doc.Regenerate();
    ExpectOnRightPiece(s, 16);
  }
  CHECK(NamesUnique(s.doc.GetBodies().at(0).elementMap));
}

TEST_CASE("A second slot through the piece refuses, naming both pieces",
          "[toponaming]") {
  SlotBlock s = BuildSlotBlock();
  SketchById(s.doc, s.cut - 1).AddRectangle({36, -1}, {38, 21});
  CHECK_FALSE(s.doc.Regenerate());
  CHECK(NamesUnique(s.doc.GetBodies().at(0).elementMap));
  for (uint64_t id : {s.sketch, s.fillet}) {
    const std::string& error = s.doc.FindFeatureById(id)->GetError();
    INFO(error);
    // Today this landed on the 2 mm sliver at x = 38..40 and said nothing.
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("split by Slot (" + std::to_string(s.cut) + ")") !=
          std::string::npos);
    CHECK(error.find("pick the one you meant") != std::string::npos);
    // Both candidates: the sliver and the middle piece, each quoted.
    size_t quotes = 0;
    for (char c : error)
      quotes += c == '\'' ? 1 : 0;
    CHECK(quotes >= 6);
  }
}

TEST_CASE("A base drawn from the other corner never lands on the left piece",
          "[toponaming]") {
  SlotBlock s = BuildSlotBlock();
  // The same four line ids, the rectangle turned half a turn: each line now
  // lies where the opposite one did.
  SketchFeature& baseSk = *dynamic_cast<SketchFeature*>(s.doc.GetFeature(0));
  for (auto& e : baseSk.entities)
    if (auto* l = std::get_if<sketch::Line>(&e)) {
      l->a = glm::dvec2(40, 20) - glm::dvec2(l->a);
      l->b = glm::dvec2(40, 20) - glm::dvec2(l->b);
    }
  s.doc.Regenerate();
  CHECK(NamesUnique(s.doc.GetBodies().at(0).elementMap));
  const Feature* sk = s.doc.FindFeatureById(s.sketch);
  const Feature* fi = s.doc.FindFeatureById(s.fillet);
  if (sk->GetError().empty())
    CHECK(SketchX(s) > 16);
  if (fi->GetError().empty())
    CHECK(FilletX(s) > 16);
}

TEST_CASE("A stored positional piece name resolves as it did, and warns",
          "[toponaming]") {
  SlotBlock s = BuildSlotBlock();
  // What the build before qualified names saved for the right piece: the
  // first piece in result order.
  auto* sk = dynamic_cast<SketchFeature*>(s.doc.FindFeatureById(s.sketch));
  sk->faceRef.name =
      std::to_string(s.base) + "/cap1;M" + std::to_string(s.cut) + ".0";
  s.doc.Regenerate();
  INFO(sk->GetError());
  CHECK(sk->GetError().empty());
  CHECK(SketchX(s) > 16);
  CHECK(sk->GetWarning().find("position") != std::string::npos);
}

TEST_CASE("Pieces nothing tells apart are numbered, and say so",
          "[toponaming]") {
  Document cyl;
  cyl.AddFeature(std::make_unique<CylinderFeature>(
      "C", glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 10.0, 10.0));
  REQUIRE(cyl.Regenerate());
  const Body& body = cyl.GetBodies().at(0);
  // A slab through the axis whose faces carry no names (an unnamed tool):
  // the two halves of the wall touch the same two caps and nothing else that
  // is named, so only their order tells them apart.
  const CADShape slab(
      BRepPrimAPI_MakeBox(gp_Pnt(-1, -15, -1), 2.0, 30.0, 12.0).Shape());
  ElementMap out;
  const CADShape cut = topo::BooleanNamed(BooleanOp::Subtract, body.shape,
                                          body.elementMap, slab, {}, 9, out);
  REQUIRE(cut.IsValid());
  CHECK(NamesUnique(out));
  int walls = 0;
  for (const auto& [idx, name] : out.names) {
    glm::dvec3 c, n, u;
    if (cut.FacePlane(idx, c, n, u))
      continue; // a cap half, or the slab's (unnamed) walls
    INFO(name);
    const topo::ElementOrigin o = topo::DecodeElementName(name);
    CHECK(o.piece.piece);
    CHECK(o.piece.ordinal); // what list_elements reports as `ordinal: true`
    CHECK_FALSE(o.piece.qualified);
    walls++;
  }
  CHECK(walls >= 2); // OCC may split the half holding the seam once more
}

namespace {

// Every edge name of `shape`'s map, by where the edge is: its parameter
// midpoint, rounded to a micrometre.
std::map<std::string, std::array<long, 3>>
EdgeNamesByPlace(const TopoDS_Shape& shape, const ElementMap& map) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  std::map<std::string, std::array<long, 3>> out;
  for (const auto& [idx, name] : map.edgeNames) {
    const BRepAdaptor_Curve c(TopoDS::Edge(edges(idx + 1)));
    const gp_Pnt p = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
    out[name] = {std::lround(p.X() * 1e3), std::lround(p.Y() * 1e3),
                 std::lround(p.Z() * 1e3)};
  }
  return out;
}

// One planar face through `pts` in order, its wire starting at pts[start].
TopoDS_Face PolygonFace(const std::vector<gp_Pnt>& pts, size_t start) {
  BRepBuilderAPI_MakePolygon poly;
  for (size_t i = 0; i < pts.size(); i++) {
    poly.Add(pts[(start + i) % pts.size()]);
  }
  poly.Close();
  return BRepBuilderAPI_MakeFace(poly.Wire(), true).Face();
}

} // namespace

// The order a builder hands a face's edges back follows shape addresses
// (docs/modeling/toponaming.md#how-each-operation-names-its-output): the same
// face built twice can list its edges from another start. A U name must name
// the same edge either way, so it is numbered from the edges' geometry, not
// their wire order.
TEST_CASE("A U name follows the edge's geometry, not the wire's order",
          "[toponaming]") {
  const std::vector<gp_Pnt> pts{
      {0, 0, 0}, {5, 0, 0}, {6, 3, 0}, {2, 4, 0}, {-1, 2, 0}};
  std::map<std::string, std::array<long, 3>> first;
  for (size_t start = 0; start < pts.size(); start++) {
    const TopoDS_Face face = PolygonFace(pts, start);
    ElementMap map;
    map.names[0] = "9/face0";
    topo::ApplyFallbackPasses(CADShape(face), map);
    const auto named = EdgeNamesByPlace(face, map);
    REQUIRE(named.size() == pts.size());
    if (start == 0) {
      first = named;
    }
    INFO("wire starting at point " << start);
    CHECK(named == first);
  }
}

// Two edges at the same place cannot be told apart by where they are, so
// neither is numbered: a guess would be the address order again.
TEST_CASE("Coincident boundary edges take no U name", "[toponaming]") {
  const gp_Pnt a(0, 0, 0), b(4, 0, 0), c(4, 4, 0), d(0, 4, 0);
  const TopoDS_Edge twin1 = BRepBuilderAPI_MakeEdge(a, b).Edge();
  const TopoDS_Edge twin2 = BRepBuilderAPI_MakeEdge(a, b).Edge();
  BRep_Builder bb;
  TopoDS_Wire wire;
  bb.MakeWire(wire);
  bb.Add(wire, twin1);
  bb.Add(wire, twin2);
  bb.Add(wire, BRepBuilderAPI_MakeEdge(b, c).Edge());
  bb.Add(wire, BRepBuilderAPI_MakeEdge(c, d).Edge());
  bb.Add(wire, BRepBuilderAPI_MakeEdge(d, a).Edge());
  TopoDS_Face face = BRepBuilderAPI_MakeFace(gp_Pln()).Face();
  TopoDS_Face plate = TopoDS::Face(face.EmptyCopied());
  bb.Add(plate, wire);
  ElementMap map;
  map.names[0] = "3/face0";
  topo::ApplyFallbackPasses(CADShape(plate), map);
  CHECK(map.EdgeNameOf(topo::EdgeIndexOf(plate, twin1)) == nullptr);
  CHECK(map.EdgeNameOf(topo::EdgeIndexOf(plate, twin2)) == nullptr);
  CHECK(map.edgeNames.size() == 3);
}

// An edge two named faces share takes its U name by a fixed rule, never by
// the result's face order: a builder can hand its faces back in
// shape-address order, so another process named the edge from the other
// face and a saved reference to it found nothing. The same box, its faces
// in two orders, names every edge alike -- and the other face's U name
// still resolves to it, as a document saved under the face order holds.
TEST_CASE("A shared edge's U name does not follow the result's face order",
          "[toponaming]") {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 20, 30).Shape();
  std::vector<TopoDS_Shape> faces;
  for (TopExp_Explorer f(box, TopAbs_FACE); f.More(); f.Next())
    faces.push_back(f.Current());
  // A face's name by its outward normal: the same in either order.
  const auto nameOf = [](const TopoDS_Shape& f) {
    const gp_Pln pl = BRepAdaptor_Surface(TopoDS::Face(f)).Plane();
    gp_Dir n = pl.Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED)
      n.Reverse();
    const char* axis = std::abs(n.X()) > 0.5   ? "x"
                       : std::abs(n.Y()) > 0.5 ? "y"
                                               : "z";
    const double s = n.X() + n.Y() + n.Z();
    return std::string("9/") + (s > 0 ? "+" : "-") + axis;
  };
  const auto named = [&](const std::vector<TopoDS_Shape>& order) {
    BRep_Builder bb;
    TopoDS_Shell shell;
    bb.MakeShell(shell);
    for (const TopoDS_Shape& f : order)
      bb.Add(shell, f);
    ElementMap map;
    for (size_t i = 0; i < order.size(); i++)
      map.names[static_cast<int>(i)] = nameOf(order[i]);
    const CADShape s(shell);
    topo::ApplyFallbackPasses(s, map);
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shell, TopAbs_EDGE, edges);
    std::map<std::string, std::string> byEdgeMid; // midpoint -> name
    for (int i = 1; i <= edges.Extent(); i++) {
      BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
      const gp_Pnt m = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
      char key[64];
      std::snprintf(key, sizeof key, "%.3f,%.3f,%.3f", m.X(), m.Y(), m.Z());
      const std::string* n = map.EdgeNameOf(i - 1);
      REQUIRE(n != nullptr);
      byEdgeMid[key] = *n;
    }
    return std::make_pair(byEdgeMid, map);
  };
  std::vector<TopoDS_Shape> reversed(faces.rbegin(), faces.rend());
  const auto [a, mapA] = named(faces);
  const auto [b, mapB] = named(reversed);
  CHECK(a.size() == 12);
  CHECK(a == b);
  // Every face's U name for each of its edges resolves, whichever face
  // named it.
  for (const auto& [idx, faceName] : mapB.names) {
    (void)idx;
    for (int k = 0; k < 4; k++)
      CHECK(mapB.ResolveEdge(faceName + ";U" + std::to_string(k)));
  }
}
TEST_CASE("The v216 golden's hole rim is a U name of its bore",
          "[toponaming][golden]") {
  // tests/golden/v216-u-name-by-fewer-edges.json: a plate with a hole, a
  // Fillet on the top rim by the name the bore gives it (its three edges
  // against the cap's five); the cap's U name for the rim is an alias.
  auto doc = testio::Load(std::string(TestSourceDir()) +
                          "/tests/golden/v216-u-name-by-fewer-edges.json");
  REQUIRE(doc != nullptr);
  INFO(doc->GetFeature(2)->GetError());
  CHECK(doc->GetFeature(2)->GetError().empty());
  const std::vector<Body>* before = doc->CachedBodiesAfter(1); // the plate
  REQUIRE(before != nullptr);
  const Body& b = before->at(0);
  const auto rim = b.elementMap.ResolveEdge("2/g3031518093;U1");
  REQUIRE(rim);
  CHECK(b.elementMap.ResolveEdge("2/cap1;U1") == rim);
}
