// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The direct edits: Delete, Offset, Move and Replace Face
// (docs/modeling/README.md#direct-edit), on a 40x30x20 block with the
// features a bought part arrives with.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestPaths.h"
#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/DeleteFaceFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/MoveFaceFeature.h"
#include "core/features/OffsetFaceFeature.h"
#include "core/features/ReplaceFaceFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using nlohmann::json;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBlock = 40.0 * 30.0 * 20.0; // 24000

const Body* BodyOf(const Document& doc, uint64_t createdBy) {
  for (const Body& b : doc.GetBodies())
    if (b.createdBy == createdBy)
      return &b;
  return nullptr;
}

// The name of the face (or edge) of `body` nearest `p`, within 1e-3 mm.
std::string ElementAt(const Body& body, bool face, const glm::dvec3& p) {
  const auto& names = face ? body.elementMap.names : body.elementMap.edgeNames;
  std::string best;
  double bestD = std::numeric_limits<double>::infinity();
  for (const auto& [idx, name] : names) {
    const auto d = Measure::DistanceToElementAt(body, face, idx, p, 1e-3);
    if (d && *d < bestD) {
      bestD = *d;
      best = name;
    }
  }
  REQUIRE_FALSE(best.empty());
  return best;
}

ElementRef FaceRef(const Body& body, const glm::dvec3& p) {
  return ElementRef{body.createdBy, ElementAt(body, true, p)};
}

// A 40x30x20 block on the origin corner; returns its body's createdBy.
uint64_t AddBlock(Document& doc) {
  doc.AddFeature(
      std::make_unique<BoxFeature>("Block", glm::dvec3(0.0), 40, 30, 20));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// Combines the block (target) with a tool primitive; the result's createdBy.
uint64_t Combine(Document& doc, uint64_t target, BooleanOp op) {
  const uint64_t tool = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<BooleanFeature>("Op", target, tool, op));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// The block with a through bore of D10 at (20, 15) along Z.
uint64_t BlockWithBore(Document& doc, double x = 20.0) {
  const uint64_t block = AddBlock(doc);
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::dvec3(x, 15, -1), glm::dvec3(0, 0, 1), 5.0, 22.0));
  return Combine(doc, block, BooleanOp::Subtract);
}

double VolumeOf(const Document& doc, uint64_t body) {
  const Body* b = BodyOf(doc, body);
  REQUIRE(b != nullptr);
  return b->shape.Volume();
}

Feature* Last(Document& doc) { return doc.GetFeature(doc.FeatureCount() - 1); }

} // namespace

TEST_CASE("Delete Face fills a through bore", "[direct_edit]") {
  Document doc;
  const uint64_t body = BlockWithBore(doc);
  const Body& b = *BodyOf(doc, body);
  REQUIRE_THAT(b.shape.Volume(), WithinRel(kBlock - kPi * 25 * 20, 1e-9));
  doc.AddFeature(std::make_unique<DeleteFaceFeature>(
      "Fill", body, std::vector<ElementRef>{FaceRef(b, {25, 15, 10})}));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  CHECK_THAT(VolumeOf(doc, body), WithinRel(kBlock, 1e-9));
}

TEST_CASE("Delete Face fills a blind hole, bore and floor", "[direct_edit]") {
  Document doc;
  const uint64_t block = AddBlock(doc);
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::dvec3(20, 15, 12), glm::dvec3(0, 0, 1), 5.0, 9.0));
  const uint64_t body = Combine(doc, block, BooleanOp::Subtract);
  const Body& b = *BodyOf(doc, body);
  REQUIRE_THAT(b.shape.Volume(), WithinRel(kBlock - kPi * 25 * 8, 1e-9));
  doc.AddFeature(std::make_unique<DeleteFaceFeature>(
      "Fill", body,
      std::vector<ElementRef>{FaceRef(b, {25, 15, 16}),
                              FaceRef(b, {20, 15, 12})}));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  CHECK_THAT(VolumeOf(doc, body), WithinRel(kBlock, 1e-9));
}

TEST_CASE("Delete Face levels a boss", "[direct_edit]") {
  Document doc;
  const uint64_t block = AddBlock(doc);
  doc.AddFeature(
      std::make_unique<BoxFeature>("Boss", glm::dvec3(15, 10, 20), 10, 10, 5));
  const uint64_t body = Combine(doc, block, BooleanOp::Union);
  const Body& b = *BodyOf(doc, body);
  REQUIRE_THAT(b.shape.Volume(), WithinRel(kBlock + 500, 1e-9));
  std::vector<ElementRef> boss{
      FaceRef(b, {20, 15, 25}), FaceRef(b, {15, 15, 22}),
      FaceRef(b, {25, 15, 22}), FaceRef(b, {20, 10, 22}),
      FaceRef(b, {20, 20, 22})};
  doc.AddFeature(std::make_unique<DeleteFaceFeature>("Level", body, boss));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  CHECK_THAT(VolumeOf(doc, body), WithinRel(kBlock, 1e-9));
}

TEST_CASE("Delete Face gives a fillet and a chamfer back their sharp edge",
          "[direct_edit]") {
  for (const EdgeBlendOp op : {EdgeBlendOp::Fillet, EdgeBlendOp::Chamfer}) {
    Document doc;
    const uint64_t block = AddBlock(doc);
    const Body& b0 = *BodyOf(doc, block);
    doc.AddFeature(std::make_unique<FilletFeature>(
        "Round",
        std::vector<ElementRef>{
            ElementRef{block, ElementAt(b0, false, {20, 30, 20})}},
        op == EdgeBlendOp::Fillet ? 3.0 : 2.0, op));
    REQUIRE(Last(doc)->GetError().empty());
    const Body& b = *BodyOf(doc, block);
    REQUIRE(b.shape.Volume() < kBlock - 1.0);
    // The blend's face: the one nearest the old edge's corner, inside.
    const glm::dvec3 mid = op == EdgeBlendOp::Fillet
                               ? glm::dvec3(20, 30 - 3 + 3 / std::sqrt(2.0),
                                            20 - 3 + 3 / std::sqrt(2.0))
                               : glm::dvec3(20, 29, 19);
    doc.AddFeature(std::make_unique<DeleteFaceFeature>(
        "Sharpen", block, std::vector<ElementRef>{FaceRef(b, mid)}));
    INFO(Last(doc)->GetError());
    REQUIRE(Last(doc)->GetError().empty());
    CHECK_THAT(VolumeOf(doc, block), WithinRel(kBlock, 1e-9));
  }
}

TEST_CASE("Delete Face refuses the block's own top, naming it",
          "[direct_edit]") {
  Document doc;
  const uint64_t block = AddBlock(doc);
  const Body& b = *BodyOf(doc, block);
  const ElementRef top = FaceRef(b, {20, 15, 20});
  doc.AddFeature(std::make_unique<DeleteFaceFeature>(
      "Top", block, std::vector<ElementRef>{top}));
  const std::string error = Last(doc)->GetError();
  INFO(error);
  CHECK(error.find("`" + top.name + "`") != std::string::npos);
  CHECK_THAT(VolumeOf(doc, block), WithinRel(kBlock, 1e-12));
}

TEST_CASE("Offset Face moves the top out and in, and refuses through the "
          "body",
          "[direct_edit]") {
  for (const auto& [d, want] : std::vector<std::pair<double, double>>{
           {5.0, 30000.0}, {-5.0, 18000.0}}) {
    Document doc;
    const uint64_t block = AddBlock(doc);
    const Body& b = *BodyOf(doc, block);
    const ElementRef top = FaceRef(b, {20, 15, 20});
    doc.AddFeature(std::make_unique<OffsetFaceFeature>(
        "Off", block, std::vector<ElementRef>{top}, d));
    INFO(Last(doc)->GetError());
    REQUIRE(Last(doc)->GetError().empty());
    CHECK_THAT(VolumeOf(doc, block), WithinRel(want, 1e-9));
    // The moved face reads ";M<id>", the others keep their names.
    const Body& after = *BodyOf(doc, block);
    const std::string moved =
        top.name + ";M" + std::to_string(Last(doc)->GetId());
    CHECK(after.elementMap.Resolve(moved).has_value());
    CHECK(after.elementMap.Resolve(ElementAt(b, true, {20, 15, 0})));
  }
  Document doc;
  const uint64_t block = AddBlock(doc);
  const Body& b = *BodyOf(doc, block);
  const ElementRef top = FaceRef(b, {20, 15, 20});
  doc.AddFeature(std::make_unique<OffsetFaceFeature>(
      "Off", block, std::vector<ElementRef>{top}, -20.0));
  const std::string error = Last(doc)->GetError();
  INFO(error);
  CHECK(error.find("consumes the body") != std::string::npos);
  CHECK(error.find("20 mm thick") != std::string::npos);
}

TEST_CASE("Offset Face changes a cylinder's radius and a bore's",
          "[direct_edit]") {
  {
    Document doc;
    doc.AddFeature(std::make_unique<CylinderFeature>(
        "Rod", glm::dvec3(0), glm::dvec3(0, 0, 1), 10.0, 20.0));
    const uint64_t rod = Last(doc)->GetId();
    const Body& b = *BodyOf(doc, rod);
    doc.AddFeature(std::make_unique<OffsetFaceFeature>(
        "Fatter", rod, std::vector<ElementRef>{FaceRef(b, {10, 0, 10})}, 2.0));
    INFO(Last(doc)->GetError());
    REQUIRE(Last(doc)->GetError().empty());
    CHECK_THAT(VolumeOf(doc, rod), WithinRel(kPi * 144 * 20, 1e-9));
  }
  Document doc;
  const uint64_t body = BlockWithBore(doc);
  const Body& b = *BodyOf(doc, body);
  doc.AddFeature(std::make_unique<OffsetFaceFeature>(
      "Bigger", body, std::vector<ElementRef>{FaceRef(b, {25, 15, 10})}, -1.0));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  CHECK_THAT(VolumeOf(doc, body), WithinRel(kBlock - kPi * 36 * 20, 1e-9));
}

TEST_CASE("Offset Face refuses a face beside a tangent fillet, and builds "
          "the tangent chain together",
          "[direct_edit]") {
  Document doc;
  const uint64_t block = AddBlock(doc);
  const Body& b0 = *BodyOf(doc, block);
  doc.AddFeature(std::make_unique<FilletFeature>(
      "Round",
      std::vector<ElementRef>{
          ElementRef{block, ElementAt(b0, false, {20, 30, 20})}},
      3.0));
  REQUIRE(Last(doc)->GetError().empty());
  const Body& b = *BodyOf(doc, block);
  const double before = b.shape.Volume();
  const ElementRef top = FaceRef(b, {20, 10, 20});
  const ElementRef side = FaceRef(b, {20, 30, 10});
  const ElementRef round =
      FaceRef(b, {20, 27 + 3 / std::sqrt(2.0), 17 + 3 / std::sqrt(2.0)});
  // The fillet is tangent to the top AND the side: each refusal names the
  // one left behind.
  for (const auto& [picks, named] :
       std::vector<std::pair<std::vector<ElementRef>, std::string>>{
           {{top}, round.name}, {{top, round}, side.name}}) {
    doc.AddFeature(
        std::make_unique<OffsetFaceFeature>("Alone", block, picks, 2.0));
    const std::string error = Last(doc)->GetError();
    INFO(error);
    CHECK(error.find("`" + named + "`") != std::string::npos);
    CHECK(error.find("tangentially") != std::string::npos);
    doc.RemoveFeature(doc.FeatureCount() - 1);
    doc.Regenerate();
  }
  doc.AddFeature(std::make_unique<OffsetFaceFeature>(
      "Together", block, std::vector<ElementRef>{top, round, side}, 2.0));
  INFO(Last(doc)->GetError());
  CHECK(Last(doc)->GetError().empty());
  // Top 40x27, side 40x17 and the quarter round R3 -> R5, each 2 out.
  const double grown = 40 * 27 * 2 + 40 * 17 * 2 + 40 * kPi / 4 * (25 - 9);
  CHECK_THAT(VolumeOf(doc, block) - before, WithinRel(grown, 1e-6));
}

TEST_CASE("Move Face moves a bore", "[direct_edit]") {
  Document doc;
  const uint64_t body = BlockWithBore(doc, 12.0);
  const Body& b = *BodyOf(doc, body);
  const ElementRef bore = FaceRef(b, {17, 15, 10});
  doc.AddFeature(std::make_unique<MoveFaceFeature>(
      "Shift", body, std::vector<ElementRef>{bore}, glm::dvec3(15, 0, 0)));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  const Body& after = *BodyOf(doc, body);
  CHECK_THAT(after.shape.Volume(), WithinRel(kBlock - kPi * 25 * 20, 1e-9));
  const std::string moved =
      bore.name + ";M" + std::to_string(Last(doc)->GetId());
  const auto at = after.elementMap.Resolve(moved);
  REQUIRE(at.has_value());
  const auto info = Measure::DescribeFaceAt(after, *at);
  REQUIRE(info.has_value());
  CHECK(info->kind == "cylinder");
  CHECK_THAT(info->centroid.x, WithinAbs(27.0, 1e-6));
  CHECK_THAT(info->centroid.y, WithinAbs(15.0, 1e-6));
}

TEST_CASE("Move Face refuses a move past the side, naming it",
          "[direct_edit]") {
  Document doc;
  const uint64_t body = BlockWithBore(doc, 12.0);
  const Body& b = *BodyOf(doc, body);
  const ElementRef bore = FaceRef(b, {17, 15, 10});
  const std::string side = ElementAt(b, true, {40, 15, 10});
  doc.AddFeature(std::make_unique<MoveFaceFeature>(
      "Shift", body, std::vector<ElementRef>{bore}, glm::dvec3(27, 0, 0)));
  const std::string error = Last(doc)->GetError();
  INFO(error);
  CHECK(error.find("`" + side + "`") != std::string::npos);
  CHECK(error.find("4 mm past") != std::string::npos);
}

TEST_CASE("Replace Face puts the top onto planes", "[direct_edit]") {
  const auto through = [](Document& doc, glm::dvec3 a, glm::dvec3 b,
                          glm::dvec3 c) {
    auto dp = std::make_unique<DatumPlaneFeature>(
        "Target", DatumPlaneKind::ThroughPoints);
    dp->p0 = a;
    dp->p1 = b;
    dp->p2 = c;
    doc.AddFeature(std::move(dp));
    return Last(doc)->GetId();
  };
  {
    Document doc;
    const uint64_t block = AddBlock(doc);
    const uint64_t datum = through(doc, {0, 0, 25}, {1, 0, 25}, {0, 1, 25});
    const Body& b = *BodyOf(doc, block);
    auto rp = std::make_unique<ReplaceFaceFeature>("Up", block,
                                                   FaceRef(b, {20, 15, 20}));
    rp->targetKind = ReplaceTargetKind::Plane;
    rp->targetId = datum;
    doc.AddFeature(std::move(rp));
    INFO(Last(doc)->GetError());
    REQUIRE(Last(doc)->GetError().empty());
    CHECK_THAT(VolumeOf(doc, block), WithinRel(30000.0, 1e-9));
  }
  {
    // Tilted 10 degrees about the top's centre line along Y.
    Document doc;
    const uint64_t block = AddBlock(doc);
    const double t = std::tan(10.0 * kPi / 180.0);
    const uint64_t datum =
        through(doc, {20, 0, 20}, {20, 1, 20}, {21, 0, 20 + t});
    const Body& b = *BodyOf(doc, block);
    auto rp = std::make_unique<ReplaceFaceFeature>("Tilt", block,
                                                   FaceRef(b, {20, 15, 20}));
    rp->targetKind = ReplaceTargetKind::Plane;
    rp->targetId = datum;
    doc.AddFeature(std::move(rp));
    INFO(Last(doc)->GetError());
    REQUIRE(Last(doc)->GetError().empty());
    CHECK_THAT(VolumeOf(doc, block), WithinRel(kBlock, 1e-9));
    glm::dvec3 lo, hi;
    BodyOf(doc, block)->shape.GetBoundingBox(lo, hi, true);
    CHECK_THAT(hi.z, WithinAbs(20 + 20 * t, 1e-6));
  }
  {
    // An R50 cylinder across the block, along X, its axis at z = 20 + 50 -
    // 2: it dips 2 mm into the top at y = 15.
    Document doc;
    const uint64_t block = AddBlock(doc);
    doc.AddFeature(std::make_unique<CylinderFeature>(
        "Drum", glm::dvec3(-10, 15, 68), glm::dvec3(1, 0, 0), 50.0, 60.0));
    const uint64_t drum = Last(doc)->GetId();
    const Body& b = *BodyOf(doc, block);
    const Body& d = *BodyOf(doc, drum);
    auto rp = std::make_unique<ReplaceFaceFeature>("Saddle", block,
                                                   FaceRef(b, {20, 15, 20}));
    rp->targetKind = ReplaceTargetKind::Face;
    rp->targetFace = FaceRef(d, {20, 15, 18});
    doc.AddFeature(std::move(rp));
    INFO(Last(doc)->GetError());
    REQUIRE(Last(doc)->GetError().empty());
    // The whole top is the drum's surface now: under z = 68 - sqrt(r^2 -
    // y'^2) across |y'| <= 15, times the 40 along X. (It dips 2 mm into the
    // old top in the middle and stands 0.3 mm proud of it at the sides.)
    const double r = 50.0, half = 15.0;
    const double under =
        68.0 * 2 * half - (2 * (half / 2 * std::sqrt(r * r - half * half) +
                                r * r / 2 * std::asin(half / r)));
    CHECK_THAT(VolumeOf(doc, block), WithinRel(under * 40.0, 1e-7));
  }
}

TEST_CASE("Offset Face keeps a drafted box drafted", "[direct_edit]") {
  // tests/reference_parts/22-draft-box: 40x40x30, the four sides drafted 5
  // degrees about the bottom. Its top 5 up: the neighbours follow their own
  // tilted planes, so the body is the taller frustum, not walls extruded
  // square.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Box", glm::dvec3(-20, -20, 0), 40, 40, 30));
  const uint64_t box = Last(doc)->GetId();
  const Body& b0 = *BodyOf(doc, box);
  doc.AddFeature(std::make_unique<DraftFeature>(
      "Taper", box,
      std::vector<ElementRef>{
          FaceRef(b0, {0, -20, 15}), FaceRef(b0, {20, 0, 15}),
          FaceRef(b0, {0, 20, 15}), FaceRef(b0, {-20, 0, 15})},
      5.0, PlaneType::XY));
  REQUIRE(Last(doc)->GetError().empty());
  const Body& b = *BodyOf(doc, box);
  doc.AddFeature(std::make_unique<OffsetFaceFeature>(
      "Taller", box, std::vector<ElementRef>{FaceRef(b, {0, 0, 30})}, 5.0));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  const double h = 35.0, t = std::tan(5.0 * kPi / 180.0);
  const double a1 = 40.0 * 40.0, s2 = 40.0 - 2 * h * t, a2 = s2 * s2;
  CHECK_THAT(VolumeOf(doc, box),
             WithinRel(h / 3 * (a1 + a2 + std::sqrt(a1 * a2)), 1e-9));
}

TEST_CASE("Faces a direct edit leaves alone keep their names, and what is "
          "built on it follows an upstream edit",
          "[direct_edit]") {
  Document doc;
  const uint64_t body = BlockWithBore(doc, 12.0);
  const size_t boreAt = 1; // the cylinder primitive
  const Body& b = *BodyOf(doc, body);
  std::map<std::string, int> before;
  for (const auto& [i, n] : b.elementMap.names)
    before[n] = i;
  const ElementRef bore = FaceRef(b, {17, 15, 10});
  const ElementRef top = FaceRef(b, {30, 25, 20});
  const ElementRef side = FaceRef(b, {40, 15, 10});
  doc.AddFeature(std::make_unique<MoveFaceFeature>(
      "Shift", body, std::vector<ElementRef>{bore}, glm::dvec3(15, 0, 0)));
  REQUIRE(Last(doc)->GetError().empty());
  const uint64_t move = Last(doc)->GetId();
  // The side the move never touched keeps its name verbatim.
  const Body& a = *BodyOf(doc, body);
  for (const auto& [i, n] : a.elementMap.names)
    UNSCOPED_INFO("after the move: " << n);
  CHECK(a.elementMap.Resolve(side.name).has_value());
  // Sketches on an untouched face and on the moved one.
  auto onSide =
      std::make_unique<SketchFeature>("OnSide", SketchPlane(PlaneType::XY));
  onSide->faceRef = side;
  doc.AddFeature(std::move(onSide));
  auto onBore =
      std::make_unique<SketchFeature>("OnTop", SketchPlane(PlaneType::XY));
  // The top was trimmed where the bore left and where it landed.
  onBore->faceRef = ElementRef{body, top.name + ";M" + std::to_string(move)};
  doc.AddFeature(std::move(onBore));
  REQUIRE(doc.Regenerate());
  // An upstream edit: the bore's radius.
  auto* cyl = dynamic_cast<CylinderFeature*>(doc.GetFeature(boreAt));
  REQUIRE(cyl != nullptr);
  cyl->radius = 4.0;
  CHECK(doc.Regenerate());
  for (size_t i = 0; i < doc.FeatureCount(); i++) {
    INFO(doc.GetFeature(i)->GetName() << ": " << doc.GetFeature(i)->GetError());
    CHECK(doc.GetFeature(i)->GetError().empty());
  }
  CHECK_THAT(VolumeOf(doc, body), WithinRel(kBlock - kPi * 16 * 20, 1e-9));
}

TEST_CASE("A face gone upstream is an error, never skipped", "[direct_edit]") {
  Document doc;
  const uint64_t block = AddBlock(doc);
  doc.AddFeature(std::make_unique<DeleteFaceFeature>(
      "Gone", block,
      std::vector<ElementRef>{
          ElementRef{block, std::to_string(block) + "/face99"}}));
  const std::string error = Last(doc)->GetError();
  INFO(error);
  CHECK(error.find("no longer exists") != std::string::npos);
  CHECK_THAT(VolumeOf(doc, block), WithinRel(kBlock, 1e-12));
}

namespace {

// Writes `shape` as BREP under the test's temp directory.
std::string WriteBrep(const CADShape& shape, const std::string& name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("lobole-direct-edit-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  const std::string path = (dir / name).string();
  REQUIRE(CADKernel::Instance().SaveBREP(path, shape));
  return path;
}

} // namespace

TEST_CASE("An imported face that is no longer the one picked fails naming "
          "both",
          "[direct_edit]") {
  CADShape block =
      CADKernel::Instance().CreateBox(glm::dvec3(0), glm::dvec3(40, 30, 20));
  CADShape drilled = block.Difference(CADKernel::Instance().CreateCylinder(
      glm::dvec3(20, 15, -1), glm::dvec3(0, 0, 1), 5.0, 22.0));
  const std::string path = WriteBrep(drilled, "part.brep");
  Document doc;
  {
    auto imp = std::make_unique<ImportFeature>("Part", path);
    imp->keepOrigin = true; // where the file put it, not centred
    doc.AddFeature(std::move(imp));
  }
  const uint64_t imp = Last(doc)->GetId();
  REQUIRE(Last(doc)->GetError().empty());
  const Body& b = *BodyOf(doc, imp);
  const ElementRef bore = FaceRef(b, {25, 15, 10});
  CHECK(topo::DecodeElementName(bore.name).role ==
        topo::ElementRole::Positional);
  doc.AddFeature(std::make_unique<DeleteFaceFeature>(
      "Fill", imp, std::vector<ElementRef>{bore}));
  REQUIRE(Last(doc)->GetError().empty());
  auto* del = dynamic_cast<DeleteFaceFeature*>(Last(doc));
  REQUIRE(del->fingerprints.count(bore.name) == 1);
  // The file changes: the same face index is now a different face (a bigger
  // bore).
  CADShape wider = block.Difference(CADKernel::Instance().CreateCylinder(
      glm::dvec3(20, 15, -1), glm::dvec3(0, 0, 1), 6.0, 22.0));
  WriteBrep(wider, "part.brep");
  doc.Regenerate(); // the file's content hash moved: Import reads it again
  const std::string error = Last(doc)->GetError();
  INFO(error);
  CHECK(error.find("is no longer the face that was picked") !=
        std::string::npos);
  CHECK(error.find("re-pick it") != std::string::npos);
}

TEST_CASE("Delete Face heals a pocket in a face whose edges lie off its "
          "plane, keeping the host's own surface",
          "[direct_edit]") {
  // An imported part's host face: a plane at z = 20 whose boundary edges
  // sit 5e-5 mm off it, alternately above and below.
  const double e = 5e-5;
  const gp_Pnt c[8] = {{0, 0, 0},        {40, 0, 0},     {40, 30, 0},
                       {0, 30, 0},       {0, 0, 20 - e}, {40, 0, 20 + e},
                       {40, 30, 20 - e}, {0, 30, 20 + e}};
  const auto quad = [&](int a, int b, int cc, int d) {
    BRepBuilderAPI_MakePolygon poly(c[a], c[b], c[cc], c[d], Standard_True);
    return poly.Wire();
  };
  BRepBuilderAPI_Sewing sew(1e-4);
  sew.Add(BRepBuilderAPI_MakeFace(quad(0, 3, 2, 1)).Face()); // bottom
  sew.Add(BRepBuilderAPI_MakeFace(quad(0, 1, 5, 4)).Face());
  sew.Add(BRepBuilderAPI_MakeFace(quad(1, 2, 6, 5)).Face());
  sew.Add(BRepBuilderAPI_MakeFace(quad(2, 3, 7, 6)).Face());
  sew.Add(BRepBuilderAPI_MakeFace(quad(3, 0, 4, 7)).Face());
  sew.Add(BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 20), gp_Dir(0, 0, 1)),
                                  quad(4, 5, 6, 7), Standard_True)
              .Face()); // the top: the plane, its edges off it
  sew.Perform();
  BRepBuilderAPI_MakeSolid solid(
      TopoDS::Shell(TopExp_Explorer(sew.SewedShape(), TopAbs_SHELL).Current()));
  TopoDS_Solid s = solid.Solid();
  BRepLib::OrientClosedSolid(s);
  const CADShape host(s);
  const CADShape pocketed = host.Difference(CADKernel::Instance().CreateBox(
      glm::dvec3(15, 10, 15), glm::dvec3(25, 20, 21)));
  const std::string path = WriteBrep(pocketed, "pocket.brep");
  Document doc;
  {
    auto imp = std::make_unique<ImportFeature>("Part", path);
    imp->keepOrigin = true; // where the file put it, not centred
    doc.AddFeature(std::move(imp));
  }
  const uint64_t imp = Last(doc)->GetId();
  REQUIRE(Last(doc)->GetError().empty());
  const Body& b = *BodyOf(doc, imp);
  const ElementRef top = FaceRef(b, {5, 5, 20});
  std::vector<ElementRef> pocket{
      FaceRef(b, {20, 15, 15}), FaceRef(b, {15, 15, 17}),
      FaceRef(b, {25, 15, 17}), FaceRef(b, {20, 10, 17}),
      FaceRef(b, {20, 20, 17})};
  doc.AddFeature(std::make_unique<DeleteFaceFeature>("Fill", imp, pocket));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  CHECK_THAT(VolumeOf(doc, imp), WithinRel(host.Volume(), 1e-6));
  // The host is still that same plane at z = 20, filled across.
  const Body& a = *BodyOf(doc, imp);
  const auto at = a.elementMap.Resolve(top.name + ";M" +
                                       std::to_string(Last(doc)->GetId()));
  REQUIRE(at.has_value());
  const auto info = Measure::DescribeFaceAt(a, *at);
  REQUIRE(info.has_value());
  CHECK(info->kind == "plane");
  CHECK_THAT(info->centroid.z, WithinAbs(20.0, 1e-4));
  CHECK_THAT(info->area, WithinRel(1200.0, 1e-5));
}

TEST_CASE("Direct edits round-trip through the file, fingerprints and all",
          "[direct_edit]") {
  Document doc;
  const uint64_t body = BlockWithBore(doc, 12.0);
  const Body& b = *BodyOf(doc, body);
  auto mv = std::make_unique<MoveFaceFeature>(
      "Shift", body, std::vector<ElementRef>{FaceRef(b, {17, 15, 10})},
      glm::dvec3(15, 0, 0));
  mv->translateExpr[0] = "15";
  doc.AddFeature(std::move(mv));
  REQUIRE(Last(doc)->GetError().empty());
  for (const Feature* f : {static_cast<const Feature*>(Last(doc))}) {
    const json j = DocumentIO::FeatureToJson(*f);
    auto back = DocumentIO::FeatureFromJson(j);
    REQUIRE(back != nullptr);
    CHECK(DocumentIO::FeatureToJson(*back) == j);
  }
  // Every type, built from the JSON an agent would send.
  for (
      const char* spec : {
          R"({"type":"DeleteFace","body_id":3,"faces":[{"feature_id":3,"element":"1/face5","fingerprint":{"surface":"plane","area_mm2":1200,"centroid_mm":[20,15,20]}}]})",
          R"({"type":"OffsetFace","body_id":3,"faces":[{"feature_id":3,"element":"1/face5"}],"distance_mm":2,"distance_mm_expr":"w/10"})",
          R"({"type":"MoveFace","body_id":3,"faces":[{"feature_id":3,"element":"1/face5"}],"translate_mm":[1,2,3],"translate_mm_expr":["","a",""],"rotate_deg":30,"rotate_deg_expr":"t","axis":{"kind":"z"}})",
          R"({"type":"ReplaceFace","body_id":3,"face":{"feature_id":3,"element":"1/face5"},"target":{"kind":"plane","datum_id":7}})",
          R"({"type":"ReplaceFace","body_id":3,"face":{"feature_id":3,"element":"1/face5"},"target":{"kind":"plane","base_plane":"yz"}})",
          R"({"type":"ReplaceFace","body_id":3,"face":{"feature_id":3,"element":"1/face5"},"target":{"kind":"sketch","sketch_id":4}})",
          R"({"type":"ReplaceFace","body_id":3,"face":{"feature_id":3,"element":"1/face5"},"target":{"kind":"face","face":{"feature_id":9,"element":"9/face0"}}})",
      }) {
    json j = json::parse(spec);
    j["name"] = "X";
    j["id"] = 11;
    auto f = DocumentIO::FeatureFromJson(j);
    REQUIRE(f != nullptr);
    const json out = DocumentIO::FeatureToJson(*f);
    INFO(out.dump());
    for (const auto& [k, v] : j.items())
      CHECK(out.at(k) == v);
  }
  // An unknown target kind is refused, naming the accepted ones.
  json bad = json::parse(
      R"({"type":"ReplaceFace","name":"X","body_id":3,"face":{"feature_id":3,"element":"1/face5"},"target":{"kind":"surface"}})");
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(bad),
                    Catch::Matchers::ContainsSubstring("plane, sketch, face"));
  // A face ref or a target holding what the writer never writes is
  // refused, not read as a default the next save would keep.
  for (
      const char* spec : {
          // a renamed key: the element read as "", the save dropped it
          R"({"type":"ReplaceFace","name":"X","body_id":3,"face":{"feature_id":3,"next_entity_id":"1/face5"},"target":{"kind":"plane","datum_id":7}})",
          R"({"type":"DeleteFace","name":"X","body_id":3,"faces":[{"feature_id":3}]})",
          R"({"type":"DeleteFace","name":"X","body_id":3,"faces":[{"feature_id":3,"element":5}]})",
          R"({"type":"DeleteFace","name":"X","body_id":3,"faces":[{"feature_id":2.5,"element":"1/face5"}]})",
          R"({"type":"DeleteFace","name":"X","body_id":3.5,"faces":[]})",
          R"({"type":"DeleteFace","name":"X","body_id":3,"faces":[{"feature_id":3,"element":"1/face5","fingerprint":{"surface":"plane","area_mm2":1,"centroid_mm":[0,0,0],"normal":[0,0,1]}}]})",
          R"({"type":"ReplaceFace","name":"X","body_id":3,"face":{"feature_id":3,"element":"1/face5"},"target":{"kind":"plane","datum_id":7,"base_plane":"yz"}})",
          R"({"type":"ReplaceFace","name":"X","body_id":3,"face":{"feature_id":3,"element":"1/face5"},"target":{"kind":"sketch","sketch_id":4,"datum_id":7}})",
      }) {
    INFO(spec);
    CHECK_THROWS(DocumentIO::FeatureFromJson(json::parse(spec)));
  }
}

namespace {

// tests/fixtures/step/ (LICENSES.txt there says where each came from),
// imported where the file put it.
uint64_t ImportStep(Document& doc, const std::string& name) {
  auto imp = std::make_unique<ImportFeature>(
      "Part", std::string(TestSourceDir()) + "/tests/fixtures/step/" + name);
  imp->keepOrigin = true;
  doc.AddFeature(std::move(imp));
  REQUIRE(Last(doc)->GetError().empty());
  return Last(doc)->GetId();
}

// The faces of `body` that `want` says yes to, as refs.
std::vector<ElementRef>
FacesWhere(const Body& body,
           const std::function<bool(const Measure::FaceInfo&)>& want) {
  std::vector<ElementRef> out;
  for (const auto& [i, n] : body.elementMap.names)
    if (const auto info = Measure::DescribeFaceAt(body, i); info && want(*info))
      out.push_back({body.createdBy, n});
  return out;
}

// The edited import against the file that has the edit made at its source:
// the same volume and the same number of faces, read by the same importer.
void SameAsFile(const Document& doc, uint64_t body, const std::string& file) {
  Document gt;
  const uint64_t g = ImportStep(gt, file);
  const Body& a = *BodyOf(doc, body);
  const Body& b = *BodyOf(gt, g);
  CHECK_THAT(a.shape.Volume(), WithinRel(b.shape.Volume(), 1e-6));
  CHECK(a.shape.FaceCount() == b.shape.FaceCount());
  glm::dvec3 lo0, hi0, lo1, hi1;
  a.shape.GetBoundingBox(lo0, hi0, true);
  b.shape.GetBoundingBox(lo1, hi1, true);
  CHECK(glm::length(lo0 - lo1) < 1e-4);
  CHECK(glm::length(hi0 - hi1) < 1e-4);
}

} // namespace

TEST_CASE("Delete Face takes a STEP knob's axial hole out", "[direct_edit]") {
  Document doc;
  const uint64_t imp =
      ImportStep(doc, "topup_ball_knob_axial_hole_rev_orig.step");
  const auto bore =
      FacesWhere(*BodyOf(doc, imp), [](const Measure::FaceInfo& f) {
        return f.kind == "cylinder" && f.hasRadius &&
               std::abs(f.radius - 3.0) < 1e-6;
      });
  REQUIRE_FALSE(bore.empty());
  doc.AddFeature(std::make_unique<DeleteFaceFeature>("Fill", imp, bore));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  SameAsFile(doc, imp, "topup_ball_knob_axial_hole_rev_gt.step");
}

TEST_CASE("Offset Face widens a STEP battery holder", "[direct_edit]") {
  // 30 wide to 39, about its centre: both Y walls 4.5 out.
  Document doc;
  const uint64_t imp = ImportStep(doc, "battery_holder_box_y_f130_orig.step");
  const auto walls =
      FacesWhere(*BodyOf(doc, imp), [](const Measure::FaceInfo& f) {
        return f.kind == "plane" && std::abs(std::abs(f.normal.y) - 1) < 1e-9 &&
               std::abs(std::abs(f.centroid.y) - 15.0) < 1e-6;
      });
  REQUIRE(walls.size() == 2);
  doc.AddFeature(std::make_unique<OffsetFaceFeature>("Wider", imp, walls, 4.5));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  SameAsFile(doc, imp, "battery_holder_box_y_f130_gt.step");
}

TEST_CASE("Offset Face thickens a STEP knob's stem", "[direct_edit]") {
  // The stem's radius 4.5 x 1.3: its round face 1.35 out.
  Document doc;
  const uint64_t imp =
      ImportStep(doc, "ball_knob_cylinder_radius_f130_orig.step");
  const auto stem =
      FacesWhere(*BodyOf(doc, imp), [](const Measure::FaceInfo& f) {
        return f.kind == "cylinder" && f.hasRadius &&
               std::abs(f.radius - 4.5) < 1e-6;
      });
  REQUIRE_FALSE(stem.empty());
  doc.AddFeature(
      std::make_unique<OffsetFaceFeature>("Thicker", imp, stem, 1.35));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  SameAsFile(doc, imp, "ball_knob_cylinder_radius_f130_gt.step");
}

TEST_CASE("Delete Face fills all 100 holes of a STEP plate in one feature",
          "[direct_edit][slow][asan_slow]") {
  Document doc;
  const uint64_t imp = ImportStep(doc, "plate_100_holes.step");
  const Body& b = *BodyOf(doc, imp);
  const auto bores = FacesWhere(
      b, [](const Measure::FaceInfo& f) { return f.kind == "cylinder"; });
  REQUIRE(bores.size() >= 100);
  glm::dvec3 lo, hi;
  b.shape.GetBoundingBox(lo, hi, true);
  const auto t0 = std::chrono::steady_clock::now();
  doc.AddFeature(std::make_unique<DeleteFaceFeature>("Fill", imp, bores));
  const double s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  const glm::dvec3 e = hi - lo;
  CHECK_THAT(VolumeOf(doc, imp), WithinRel(e.x * e.y * e.z, 1e-9));
  CHECK(BodyOf(doc, imp)->shape.FaceCount() == 6);
  // The budget docs/modeling/README.md#delete-face states: about 20 s here,
  // nearly all of it BRepAlgoAPI_Defeaturing; twice that on a loaded machine.
  INFO("100 holes deleted in " << s << " s");
  CHECK(s < 90.0);
}
