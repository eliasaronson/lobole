// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <filesystem>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

// Total volume across all bodies.
static double TotalVolume(const Document& doc) {
  double v = 0.0;
  for (const auto& b : doc.GetBodies())
    v += b.shape.Volume();
  return v;
}

static uint64_t AddBox(Document& doc, const char* name, glm::vec3 pos,
                       double s) {
  doc.AddFeature(std::make_unique<BoxFeature>(name, pos, s, s, s));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// --- Linear ------------------------------------------------------------------

TEST_CASE("Linear pattern of disjoint instances yields separate bodies",
          "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0); // [0,2]^3, vol 8
  auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 3;
  pat->spacing = 5.0; // 5 apart, 2 wide -> disjoint
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 3); // seed consumed, 3 instances
  CHECK_THAT(TotalVolume(doc), WithinRel(24.0, 1e-4));
}

TEST_CASE("Linear pattern of touching instances fuses into one body",
          "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0);
  auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 3;
  pat->spacing = 1.0; // overlap: [0,2],[1,3],[2,4] -> contiguous [0,4]
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(16.0, 1e-4)); // 4x2x2
}

// --- Circular ----------------------------------------------------------------

TEST_CASE("Circular pattern arrays around an axis", "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {2, -0.5, -0.5}, 1.0); // offset from Z, vol 1
  auto pat =
      std::make_unique<PatternFeature>("Arr", PatternKind::Circular, box);
  pat->axis.kind = AxisRef::Kind::Z; // rotate about global Z through origin
  pat->count = 4;
  pat->angleDeg = 90.0; // 0/90/180/270 -> four disjoint instances
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 4);
  CHECK_THAT(TotalVolume(doc), WithinRel(4.0, 1e-4));
}

// --- Mirror ------------------------------------------------------------------

TEST_CASE("Mirror across a plane the body is clear of yields two bodies",
          "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {1, 0, 0}, 2.0); // [1,3]^3, clear of x=0
  auto pat = std::make_unique<PatternFeature>("Mir", PatternKind::Mirror, box);
  pat->basePlane = PlaneType::YZ; // the x=0 plane
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 2); // original + reflection, disjoint
  CHECK_THAT(TotalVolume(doc), WithinRel(16.0, 1e-4));
}

TEST_CASE("Mirror of a body touching the plane merges into one body",
          "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0); // face on x=0
  auto pat = std::make_unique<PatternFeature>("Mir", PatternKind::Mirror, box);
  pat->basePlane = PlaneType::YZ;
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 1); // [-2,2]x[0,2]x[0,2]
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(16.0, 1e-4));
}

// --- Expressions -------------------------------------------------------------

TEST_CASE("Expression-driven count and spacing follow parameter edits",
          "[pattern]") {
  Document doc;
  doc.SetParameter("n", "3");
  doc.SetParameter("gap", "5");
  uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0);
  auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
  pat->axis.kind = AxisRef::Kind::X;
  pat->countExpr = "n";
  pat->spacingExpr = "gap";
  doc.AddFeature(std::move(pat));
  REQUIRE(doc.GetBodies().size() == 3);

  doc.SetParameter("n", "5");
  CHECK(doc.GetBodies().size() == 5); // count follows the variable
  CHECK_THAT(TotalVolume(doc), WithinRel(40.0, 1e-4));
}

// --- Failure + serialization -------------------------------------------------

TEST_CASE("A pattern with a missing source fails", "[pattern]") {
  Document doc;
  AddBox(doc, "A", {0, 0, 0}, 2.0);
  auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, 9999);
  pat->axis.kind = AxisRef::Kind::X;
  doc.AddFeature(std::move(pat));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("Pattern round-trips through JSON", "[pattern]") {
  const fs::path path =
      fs::temp_directory_path() / "designer_pattern_test.json";
  {
    Document doc;
    doc.SetParameter("n", "4");
    uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0);
    auto pat =
        std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
    pat->axis.kind = AxisRef::Kind::X;
    pat->countExpr = "n";
    pat->spacing = 5.0;
    doc.AddFeature(std::move(pat));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* pat = dynamic_cast<PatternFeature*>(loaded->GetFeature(1));
  REQUIRE(pat != nullptr);
  CHECK(pat->kind == PatternKind::Linear);
  CHECK(pat->countExpr == "n");
  CHECK(pat->axis.kind == AxisRef::Kind::X);
  CHECK(loaded->GetBodies().size() == 4);
  fs::remove(path);
}

TEST_CASE("Pattern regeneration is deterministic", "[pattern]") {
  Document doc;
  uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0);
  auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 3;
  pat->spacing = 5.0;
  doc.AddFeature(std::move(pat));
  const double v1 = TotalVolume(doc);
  doc.Regenerate();
  CHECK_THAT(TotalVolume(doc), WithinAbs(v1, 1e-9));
}

TEST_CASE("A pattern with a missing source reports why", "[pattern][error]") {
  Document doc;
  auto pat = std::make_unique<PatternFeature>("Pat", PatternKind::Linear, 9999);
  pat->count = 3;
  doc.AddFeature(std::move(pat));
  CHECK_FALSE(doc.Regenerate());
  const std::string err = doc.GetFeature(0)->GetError();
  CHECK(err.find("source body") != std::string::npos);
}

// --- Pattern of a CUT (R14) --------------------------------------------------

TEST_CASE("Pattern of a cut replicates the cutter", "[pattern]") {
  // A plate with one drilled hole; the pattern repeats the HOLE, not a body.
  // This used to refuse "source body not found", pushing repeated holes into
  // one multi-loop sketch and throwing the pattern's design intent away.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 20, 5));
  auto sk =
      std::make_unique<SketchFeature>("HoleSk", SketchPlane(PlaneType::XY));
  sk->AddEntity(sketch::Circle{{10, 10}, 3.0});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(1)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Hole", skId, 5.0,
                                                  ExtrudeMode::Cut, false));
  const uint64_t cutId = doc.GetFeature(2)->GetId();

  auto pat =
      std::make_unique<PatternFeature>("Holes x4", PatternKind::Linear, cutId);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 4;
  pat->spacing = 12.0;
  doc.AddFeature(std::move(pat));

  INFO(doc.GetFeature(3)->GetError());
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  const double hole = 3.1415926535897932 * 9.0 * 5.0;
  CHECK_THAT(TotalVolume(doc), WithinRel(6000.0 - 4.0 * hole, 1e-3));

  // Each instance's faces are name-resolvable, suffixed per instance — a
  // fillet or a CAM pick on the third hole has something to hold on to.
  const ElementMap& em = doc.GetBodies()[0].elementMap;
  int suffixed = 0;
  for (const auto& [index, name] : em.names)
    if (name.find('#') != std::string::npos)
      suffixed++;
  CHECK(suffixed >= 3); // the barrel of instances 1..3 (0 is the source cut)
}

TEST_CASE("Pattern of a Hole feature repeats the hole", "[pattern]") {
  // The test above patterns an Extrude CUT; a Hole feature exposed no tool,
  // so patterning one -- a bolt circle -- refused "source body not found".
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 20, 5));
  auto sk = std::make_unique<SketchFeature>(
      "HoleSk",
      SketchPlane(glm::vec3(0, 0, 5), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Point{{10, 10}});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(1)->GetId();
  auto hole =
      std::make_unique<HoleFeature>("Hole", skId, HoleKind::Simple, 6.0, 5.0);
  hole->through = true;
  doc.AddFeature(std::move(hole));
  const uint64_t holeId = doc.GetFeature(2)->GetId();
  INFO(doc.GetFeature(2)->GetError());
  REQUIRE(doc.GetFeature(2)->GetError().empty());
  const double one = 6000.0 - TotalVolume(doc);
  REQUIRE(one > 100.0);

  auto pat =
      std::make_unique<PatternFeature>("Holes x4", PatternKind::Linear, holeId);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 4;
  pat->spacing = 12.0;
  doc.AddFeature(std::move(pat));
  INFO(doc.GetFeature(3)->GetError());
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  CHECK_THAT(TotalVolume(doc), WithinRel(6000.0 - 4.0 * one, 1e-3));
}

TEST_CASE("Pattern of an adding extrude replicates the boss", "[pattern]") {
  // A boss extruded ADD into a plate is fused into the plate's body, so it
  // leaves no body of its own — and patterning it refused "source body not
  // found". A watch's ring of contrate teeth went in as one extrude per
  // tooth because of it. The pattern repeats the boss's TOOL now, fused like
  // the original.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 20, 5));
  auto sk =
      std::make_unique<SketchFeature>("BossSk", SketchPlane(PlaneType::XY));
  sk->AddEntity(sketch::Circle{{10, 10}, 2.0});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(1)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Boss", skId, 10.0,
                                                  ExtrudeMode::Add, false));
  const uint64_t bossId = doc.GetFeature(2)->GetId();
  REQUIRE(doc.GetFeature(2)->GetError().empty());

  auto pat = std::make_unique<PatternFeature>("Bosses x4", PatternKind::Linear,
                                              bossId);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 4;
  pat->spacing = 12.0;
  doc.AddFeature(std::move(pat));

  INFO(doc.GetFeature(3)->GetError());
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  // Each boss stands 5 mm proud of the 5 mm plate it starts inside.
  const double boss = 3.1415926535897932 * 4.0 * 5.0;
  CHECK_THAT(TotalVolume(doc), WithinRel(6000.0 + 4.0 * boss, 1e-3));
  const ElementMap& em = doc.GetBodies()[0].elementMap;
  int suffixed = 0;
  for (const auto& [index, name] : em.names)
    if (name.find('#') != std::string::npos)
      suffixed++;
  CHECK(suffixed >= 3); // instances 1..3 carry suffixed names
}

TEST_CASE("Pattern of a cut through a MISSING source still says so",
          "[pattern]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 20, 20, 5));
  auto pat = std::make_unique<PatternFeature>("Ghost", PatternKind::Linear,
                                              uint64_t{999});
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 2;
  pat->spacing = 5.0;
  doc.AddFeature(std::move(pat));
  CHECK_FALSE(doc.GetFeature(1)->GetError().empty());
}

TEST_CASE("Mirroring a cut mirrors the hole", "[pattern]") {
  // The mirror branch of the cut path — a bracket's two symmetric holes.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(-30, 0, 0), 60, 20, 5));
  auto sk =
      std::make_unique<SketchFeature>("HoleSk", SketchPlane(PlaneType::XY));
  sk->AddEntity(sketch::Circle{{20, 10}, 3.0});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(1)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Hole", skId, 5.0,
                                                  ExtrudeMode::Cut, false));
  const uint64_t cutId = doc.GetFeature(2)->GetId();
  auto pat =
      std::make_unique<PatternFeature>("Mirror", PatternKind::Mirror, cutId);
  pat->basePlane = PlaneType::YZ; // x = 0
  doc.AddFeature(std::move(pat));

  INFO(doc.GetFeature(3)->GetError());
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  const double hole = 3.1415926535897932 * 9.0 * 5.0;
  CHECK_THAT(TotalVolume(doc), WithinRel(6000.0 - 2.0 * hole, 1e-3));
}

TEST_CASE("Circular pattern about a custom axis off the origin", "[pattern]") {
  // A unit box beside the axis x = 10, y = 0 (parallel to Z): four copies at
  // 90 degrees land round (10, 0), disjoint, all within 3 mm of it.
  Document doc;
  uint64_t box = AddBox(doc, "A", {12, -0.5, -0.5}, 1.0);
  auto pat =
      std::make_unique<PatternFeature>("Arr", PatternKind::Circular, box);
  pat->axis.kind = AxisRef::Kind::Custom;
  pat->axis.origin = {10.0, 0.0, 0.0};
  pat->axis.direction = {0.0, 0.0, 1.0};
  pat->count = 4;
  pat->angleDeg = 90.0;
  doc.AddFeature(std::move(pat));

  REQUIRE(doc.GetBodies().size() == 4);
  CHECK_THAT(TotalVolume(doc), WithinRel(4.0, 1e-4));
  for (const Body& b : doc.GetBodies()) {
    glm::dvec3 lo, hi;
    b.shape.GetBoundingBox(lo, hi, /*tight=*/true);
    CHECK(std::abs((lo.x + hi.x) / 2 - 10.0) < 3.0);
    CHECK(std::abs((lo.y + hi.y) / 2) < 3.0);
  }
}
