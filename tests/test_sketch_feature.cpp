// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Units.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchFillet.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSolver.h"
#include "core/sketch/SketchSplit.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

static std::unique_ptr<BoxFeature> MakeBox(const std::string& name) {
  return std::make_unique<BoxFeature>(name, glm::dvec3(0), 2.0, 2.0, 2.0);
}

TEST_CASE("MirrorEntities reflects geometry across an axis line", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t axis = sk.AddEntity(sketch::Line{{0, -5}, {0, 5}}); // the y-axis
  uint32_t seg = sk.AddEntity(sketch::Line{{2, 1}, {3, 4}});   // to the right

  SECTION("bake: unlinked copy, no constraints") {
    auto gen = sk.MirrorEntities({seg}, axis, /*link=*/false);
    REQUIRE(gen.size() == 1);
    const auto& copy =
        std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(gen[0])]);
    CHECK_THAT(copy.a.x, WithinAbs(-2.0, 1e-5));
    CHECK_THAT(copy.a.y, WithinAbs(1.0, 1e-5));
    CHECK_THAT(copy.b.x, WithinAbs(-3.0, 1e-5));
    CHECK_THAT(copy.b.y, WithinAbs(4.0, 1e-5));
    CHECK(sk.constraints.empty()); // bake adds none
  }
  SECTION("linked: a Mirror op that tracks source AND axis (v51)") {
    auto gen = sk.MirrorEntities({seg}, axis, /*link=*/true);
    REQUIRE(gen.size() == 1);
    REQUIRE(sk.derived.size() == 1);
    CHECK(sk.derived[0].kind == sketch::DerivedKind::Mirror);
    CHECK(sk.derived[0].axisEntity == axis);
    CHECK(sk.constraints.empty()); // no symmetry-constraint fan-out anymore

    // Move the SOURCE: the copy follows on regeneration, id-stable.
    std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(seg)]).a = {2, 2};
    sk.RegenerateDerived();
    const auto& c1 =
        std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(gen[0])]);
    CHECK_THAT(c1.a.x, WithinAbs(-2.0, 1e-5));
    CHECK_THAT(c1.a.y, WithinAbs(2.0, 1e-5));

    // Move the AXIS (shift to x = 1): the copy re-reflects.
    auto& ax = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(axis)]);
    ax.a = {1, -5};
    ax.b = {1, 5};
    sk.RegenerateDerived();
    const auto& c2 =
        std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(gen[0])]);
    CHECK_THAT(c2.a.x, WithinAbs(0.0, 1e-5)); // 2 reflected about x=1
  }
}

TEST_CASE("Mirror across a datum axis reflects and links to it",
          "[sketch][mirror][datum]") {
  // No axis entity needed — the Y axis datum reflects across x = 0; the
  // linked Mirror op (v51) records kAxisY as its axis.
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t seg = sk.AddEntity(sketch::Line{{1, 0}, {2, 1}});
  auto gen = sk.MirrorEntities({seg}, sketch::kAxisY, /*link=*/true);
  REQUIRE(gen.size() == 1);

  const auto& copy =
      std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(gen[0])]);
  CHECK_THAT(copy.a.x, WithinAbs(-1.0, 1e-4)); // reflected across x = 0
  CHECK_THAT(copy.b.x, WithinAbs(-2.0, 1e-4));
  CHECK_THAT(copy.b.y, WithinAbs(1.0, 1e-4));

  REQUIRE(sk.derived.size() == 1);
  CHECK(sk.derived[0].kind == sketch::DerivedKind::Mirror);
  CHECK(sk.derived[0].axisEntity == sketch::kAxisY);
  CHECK(sk.constraints.empty());

  sketch::Solver solver; // the datum-referencing constraints solve
  solver.Load(sk);
  CHECK(solver.Solve().ok);
}

TEST_CASE("MirrorEntities reflects an arc keeping it CCW", "[sketch][arc]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t axis = sk.AddEntity(sketch::Line{{0, -5}, {0, 5}});
  // Arc on the right, CCW from 0 to pi/2.
  uint32_t a0 = sk.AddEntity(sketch::Arc{{2, 0}, 1.0, 0.0, M_PI / 2});
  auto gen = sk.MirrorEntities({a0}, axis, false);
  REQUIRE(gen.size() == 1);
  const auto& arc =
      std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(gen[0])]);
  CHECK_THAT(arc.center.x, WithinAbs(-2.0, 1e-5));
  CHECK(arc.endAngle > arc.startAngle); // still CCW-always
  // Endpoints are the reflections of the source's (winding swapped).
  glm::dvec2 s = sketch::ArcPointUV(arc, 1), e = sketch::ArcPointUV(arc, 2);
  CHECK_THAT(glm::length(s - glm::dvec2(-2, 1)), WithinAbs(0.0, 1e-4));
  CHECK_THAT(glm::length(e - glm::dvec2(-3, 0)), WithinAbs(0.0, 1e-4));
}

TEST_CASE("FilletCorner rounds a right-angle corner with a tangent arc",
          "[sketch][arc]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  // Two lines meeting at the origin at 90 degrees.
  uint32_t l1 = sk.AddEntity(sketch::Line{{4, 0}, {0, 0}}); // corner at b=(0,0)
  uint32_t l2 = sk.AddEntity(sketch::Line{{0, 0}, {0, 4}}); // corner at a=(0,0)
  sk.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {l1, 1}, {l2, 0}, 0.0});

  const double r = 1.0;
  uint32_t arcId = sk.FilletCorner(l1, l2, r);
  REQUIRE(arcId != 0);
  const auto& arc = std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(arcId)]);
  CHECK_THAT(arc.radius, WithinAbs(r, 1e-5));
  // For a right angle at the origin, the fillet centre is at (r, r).
  CHECK_THAT(arc.center.x, WithinAbs(1.0, 1e-4));
  CHECK_THAT(arc.center.y, WithinAbs(1.0, 1e-4));
  // Lines trimmed to the tangent points (r along each from the corner).
  const auto& s1 = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l1)]);
  const auto& s2 = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l2)]);
  CHECK_THAT(glm::length(s1.b - glm::dvec2(1, 0)), WithinAbs(0.0, 1e-4));
  CHECK_THAT(glm::length(s2.a - glm::dvec2(0, 1)), WithinAbs(0.0, 1e-4));
  // Old corner coincident removed; two tangents added.
  int tang = 0, coincAtOrigin = 0;
  for (const auto& c : sk.constraints) {
    if (c.kind == sketch::ConstraintKind::Tangent)
      tang++;
    if (c.kind == sketch::ConstraintKind::Coincident && c.a.entity == l1 &&
        c.a.which == 1 && c.b.entity == l2)
      coincAtOrigin++;
  }
  CHECK(tang == 2);
  CHECK(coincAtOrigin == 0);
}

TEST_CASE("Linear pattern bakes stepped copies", "[sketch][pattern]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {1, 0}});
  auto gen = sk.AddLinearPattern({l}, {5, 0}, /*count=*/3, /*link=*/false);
  REQUIRE(gen.size() == 2); // count-1 copies
  CHECK(sk.entities.size() == 3);
  CHECK(sk.derived.empty()); // bake stores no record
  const auto& c1 =
      std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(gen[0])]);
  const auto& c2 =
      std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(gen[1])]);
  CHECK_THAT(c1.a.x, WithinAbs(5.0, 1e-5));
  CHECK_THAT(c2.a.x, WithinAbs(10.0, 1e-5));
}

TEST_CASE("2D linear pattern fills a grid (v50)", "[sketch][pattern]") {
  // Unlinked bake: 2 x 3 grid = 5 copies beyond the seed.
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t p = sk.AddEntity(sketch::Point{{0, 0}});
  auto gen = sk.AddLinearPattern({p}, {2, 0}, /*count=*/2, /*link=*/false,
                                 /*step2=*/{0, 3}, /*count2=*/3);
  REQUIRE(gen.size() == 5);
  CHECK(sk.entities.size() == 6);

  // Linked: same grid as a derived op; the k index unfolds row-major.
  SketchFeature sk2("S2", SketchPlane(PlaneType::XY));
  uint32_t q = sk2.AddEntity(sketch::Point{{0, 0}});
  auto gen2 = sk2.AddLinearPattern({q}, {2, 0}, 2, /*link=*/true, {0, 3}, 3);
  REQUIRE(gen2.size() == 5);
  REQUIRE(sk2.derived.size() == 1);
  CHECK(sk2.derived[0].count2 == 3);
  // k=1 → (2,0); k=2 → row 1 col 0 → (0,3); k=3 → (2,3); k=5 → (2,6).
  const auto at = [&](uint32_t id) {
    return std::get<sketch::Point>(sk2.entities[sk2.IndexOfEntity(id)]).p;
  };
  CHECK_THAT(at(gen2[0]).x, WithinAbs(2.0, 1e-5));
  CHECK_THAT(at(gen2[0]).y, WithinAbs(0.0, 1e-5));
  CHECK_THAT(at(gen2[1]).x, WithinAbs(0.0, 1e-5));
  CHECK_THAT(at(gen2[1]).y, WithinAbs(3.0, 1e-5));
  CHECK_THAT(at(gen2[4]).x, WithinAbs(2.0, 1e-5));
  CHECK_THAT(at(gen2[4]).y, WithinAbs(6.0, 1e-5));

  // Shrinking the second direction prunes id-stably.
  sk2.derived[0].count2 = 1;
  sk2.RegenerateDerived();
  CHECK(sk2.entities.size() == 2); // seed + the k=1 instance
  CHECK(sk2.IndexOfEntity(gen2[0]) != SIZE_MAX);
}

TEST_CASE("Circular pattern centre reference tracks its point (v51)",
          "[sketch][pattern]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t ctr = sk.AddEntity(sketch::Point{{0, 0}});
  uint32_t c = sk.AddEntity(sketch::Circle{{2, 0}, 0.5});
  auto gen = sk.AddCircularPattern({c}, {9, 9} /*ignored*/, M_PI, 2,
                                   /*link=*/true, ctr, 0);
  REQUIRE(gen.size() == 1);
  // Centre resolved from the point (0,0): the copy lands at (-2, 0).
  const auto& g0 =
      std::get<sketch::Circle>(sk.entities[sk.IndexOfEntity(gen[0])]);
  CHECK_THAT(g0.center.x, WithinAbs(-2.0, 1e-4));

  // Move the centre point: the pattern follows on regeneration.
  std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(ctr)]).p = {2, 2};
  sk.RegenerateDerived();
  const auto& g1 =
      std::get<sketch::Circle>(sk.entities[sk.IndexOfEntity(gen[0])]);
  CHECK_THAT(g1.center.x, WithinAbs(2.0, 1e-4)); // (2,0) rotated pi about
  CHECK_THAT(g1.center.y, WithinAbs(4.0, 1e-4)); // (2,2) → (2,4)
}

TEST_CASE("Circular pattern bakes rotated copies", "[sketch][pattern]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t p = sk.AddEntity(sketch::Point{{1, 0}});
  auto gen = sk.AddCircularPattern({p}, {0, 0}, M_PI / 2, /*count=*/4, false);
  REQUIRE(gen.size() == 3); // 90, 180, 270 degrees
  const auto& q1 =
      std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(gen[0])]);
  CHECK_THAT(q1.p.x, WithinAbs(0.0, 1e-5));
  CHECK_THAT(q1.p.y, WithinAbs(1.0, 1e-5)); // (1,0) rotated 90 → (0,1)
  const auto& q2 =
      std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(gen[1])]);
  CHECK_THAT(q2.p.x, WithinAbs(-1.0, 1e-5)); // 180 → (-1,0)
}

TEST_CASE("A linked pattern regenerates when its source moves",
          "[sketch][pattern]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t p = sk.AddEntity(sketch::Point{{0, 0}});
  auto gen = sk.AddLinearPattern({p}, {2, 0}, 3, /*link=*/true);
  REQUIRE(gen.size() == 2);
  REQUIRE(sk.derived.size() == 1);

  // Move the source, regenerate: instances keep ids and follow.
  std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(p)]).p = {0, 5};
  sk.RegenerateDerived();
  CHECK(sk.entities.size() == 3); // still one source + two instances
  const auto& g0 =
      std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(gen[0])]);
  const auto& g1 =
      std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(gen[1])]);
  CHECK_THAT(g0.p.y, WithinAbs(5.0, 1e-5)); // followed the source in y
  CHECK_THAT(g0.p.x, WithinAbs(2.0, 1e-5));
  CHECK_THAT(g1.p.x, WithinAbs(4.0, 1e-5));
}

TEST_CASE("Changing a linked pattern's count is id-stable",
          "[sketch][pattern]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t p = sk.AddEntity(sketch::Point{{0, 0}});
  auto gen = sk.AddLinearPattern({p}, {2, 0}, 3, /*link=*/true);
  REQUIRE(gen.size() == 2);
  const uint32_t inst1 = gen[0]; // the k=1 instance

  // Shrink to count 2: the k=2 instance is removed, k=1 keeps its id.
  sk.derived[0].count = 2;
  sk.RegenerateDerived();
  CHECK(sk.entities.size() == 2);              // source + one instance
  CHECK(sk.IndexOfEntity(inst1) != SIZE_MAX);  // k=1 survived with its id
  CHECK(sk.IndexOfEntity(gen[1]) == SIZE_MAX); // k=2 deleted

  // Grow to count 4: k=1 keeps its id; k=2,3 are (re)created.
  sk.derived[0].count = 4;
  sk.RegenerateDerived();
  CHECK(sk.entities.size() == 4);             // source + three instances
  CHECK(sk.IndexOfEntity(inst1) != SIZE_MAX); // k=1 id preserved throughout
  CHECK(sk.derived[0].instances.size() == 3);
}

TEST_CASE("RemoveDerivedOp drops the op and its generated entities",
          "[sketch][pattern]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t p = sk.AddEntity(sketch::Point{{0, 0}});
  auto gen = sk.AddLinearPattern({p}, {2, 0}, 3, /*link=*/true);
  REQUIRE(gen.size() == 2);
  REQUIRE(sk.entities.size() == 3); // source + two instances

  sk.RemoveDerivedOp(0);
  CHECK(sk.derived.empty());
  CHECK(sk.entities.size() == 1); // only the source remains
  for (uint32_t g : gen)
    CHECK(sk.IndexOfEntity(g) == SIZE_MAX); // instances deleted
}

TEST_CASE("Editing a linked pattern's step regenerates it id-stably",
          "[sketch][pattern]") {
  // The core of EditPatternParams (N4): changing the transform moves the
  // generated instances but keeps their ids.
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t p = sk.AddEntity(sketch::Point{{0, 0}});
  auto gen = sk.AddLinearPattern({p}, {2, 0}, 3, /*link=*/true);
  REQUIRE(gen.size() == 2);
  const uint32_t inst1 = gen[0]; // k=1 at (2,0)

  sk.derived[0].vec = glm::dvec2{0, 5}; // re-aim the step to +Y, spacing 5
  sk.RegenerateDerived();

  CHECK(sk.entities.size() == 3);             // count unchanged
  const size_t idx = sk.IndexOfEntity(inst1); // same id...
  REQUIRE(idx != SIZE_MAX);
  const auto& moved = std::get<sketch::Point>(sk.entities[idx]);
  CHECK_THAT(moved.p.x, WithinAbs(0.0, 1e-5)); // ...moved to (0,5)
  CHECK_THAT(moved.p.y, WithinAbs(5.0, 1e-5));
}

TEST_CASE("A linked pattern round-trips through JSON", "[sketch][pattern]") {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  uint32_t p = sk->AddEntity(sketch::Point{{0, 0}});
  sk->AddCircularPattern({p}, {0, 0}, M_PI / 2, 4, /*link=*/true);
  sk->SetId(3);
  REQUIRE(sk->derived.size() == 1);
  const size_t entCount = sk->entities.size(); // source + 3 instances

  auto j = DocumentIO::FeatureToJson(*sk);
  auto restored = DocumentIO::FeatureFromJson(j);
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);
  REQUIRE(rsk->derived.size() == 1);
  CHECK(rsk->derived[0].kind == sketch::DerivedKind::CircularPattern);
  CHECK(rsk->derived[0].count == 4);
  CHECK(rsk->derived[0].instances.size() == 3);
  CHECK(rsk->entities.size() == entCount);
  CHECK(DocumentIO::FeatureToJson(*rsk) == j); // stable round-trip
}

TEST_CASE("A driven dimension round-trips, written as its measure",
          "[sketch][io][driven]") {
  using K = sketch::ConstraintKind;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk->AddEntity(sketch::Line{{0, 0}, {30, 40}});
  sketch::Constraint d{K::Distance, {l, 0}, {l, 1}, 7.0}; // 7: not its truth
  d.driven = true;
  sk->constraints.push_back(d);
  sk->constraints.push_back({K::Horizontal, {l, 0}, {}}); // never "driven"
  sk->SetId(4);

  const nlohmann::json j = DocumentIO::FeatureToJson(*sk);
  const nlohmann::json& cj = j.at("constraints");
  CHECK(cj.at(0).at("driven") == true);
  CHECK_THAT(cj.at(0).at("value").get<double>(), WithinAbs(50.0, 1e-12));
  CHECK_FALSE(cj.at(1).contains("driven")); // omitted when false

  auto restored = DocumentIO::FeatureFromJson(j);
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);
  CHECK(rsk->constraints.at(0).driven);
  CHECK_FALSE(rsk->constraints.at(1).driven);
  CHECK(DocumentIO::FeatureToJson(*rsk) == j);
}

TEST_CASE("The loader refuses a driven flag where it means nothing",
          "[sketch][io][driven]") {
  using K = sketch::ConstraintKind;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk->AddEntity(sketch::Line{{0, 0}, {30, 40}});
  sk->constraints.push_back({K::Horizontal, {l, 0}, {}});
  sk->constraints.push_back({K::Distance, {l, 0}, {l, 1}, 50.0});
  sk->SetId(5);
  const nlohmann::json good = DocumentIO::FeatureToJson(*sk);

  SECTION("on a horizontal, naming the dimension kinds") {
    nlohmann::json j = good;
    j["constraints"][0]["driven"] = true;
    CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                      Catch::Matchers::ContainsSubstring(
                          "a horizontal constraint cannot be driven") &&
                          Catch::Matchers::ContainsSubstring("distance_pl"));
  }
  SECTION("with an expression") {
    nlohmann::json j = good;
    j["constraints"][1]["driven"] = true;
    j["constraints"][1]["expr"] = "w";
    CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                      Catch::Matchers::ContainsSubstring("cannot carry an "
                                                         "expression"));
  }
  SECTION("that is not a boolean") {
    nlohmann::json j = good;
    j["constraints"][1]["driven"] = "yes";
    CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                      Catch::Matchers::ContainsSubstring("true or false"));
  }
}

TEST_CASE("FilletCorner rejects bad inputs", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t l1 = sk.AddEntity(sketch::Line{{4, 0}, {0, 0}});
  uint32_t l2 = sk.AddEntity(sketch::Line{{0, 0}, {0, 4}});
  uint32_t l3 =
      sk.AddEntity(sketch::Line{{10, 0}, {14, 0}}); // no shared corner
  CHECK(sk.FilletCorner(l1, l3, 1.0) == 0);         // no shared corner
  CHECK(sk.FilletCorner(l1, l2, 100.0) == 0);       // radius too large
  CHECK(sk.FilletCorner(l1, l2, 0.0) == 0);         // non-positive radius
}

TEST_CASE("Features get unique, monotonic ids", "[ids]") {
  Document doc;
  doc.AddFeature(MakeBox("A"));
  doc.AddFeature(MakeBox("B"));

  const uint64_t idA = doc.GetFeature(0)->GetId();
  const uint64_t idB = doc.GetFeature(1)->GetId();
  CHECK(idA != 0);
  CHECK(idB > idA);

  size_t index = 99;
  CHECK(doc.FindFeatureById(idB, &index) == doc.GetFeature(1));
  CHECK(index == 1);
  CHECK(doc.FindFeatureById(9999) == nullptr);
}

TEST_CASE("Ids survive remove-undo and modify round-trips", "[ids][undo]") {
  Document doc;
  CommandStack stack;
  stack.Execute(std::make_unique<AddFeatureCommand>(MakeBox("A")), doc);
  stack.Execute(std::make_unique<AddFeatureCommand>(MakeBox("B")), doc);
  const uint64_t idA = doc.GetFeature(0)->GetId();

  // Remove + undo re-inserts the same object: id kept, counter not confused
  stack.Execute(std::make_unique<RemoveFeatureCommand>(0), doc);
  stack.Undo(doc);
  CHECK(doc.GetFeature(0)->GetId() == idA);

  // Modify round-trips the id through JSON
  auto before = DocumentIO::FeatureToJson(*doc.GetFeature(0));
  auto after = before;
  after["width"] = 5.0;
  stack.Execute(std::make_unique<ModifyFeatureCommand>(0, before, after), doc);
  CHECK(doc.GetFeature(0)->GetId() == idA);

  // New features never collide with existing ids
  stack.Execute(std::make_unique<AddFeatureCommand>(MakeBox("C")), doc);
  CHECK(doc.GetFeature(2)->GetId() > doc.GetFeature(1)->GetId());
}

TEST_CASE("Explicit ids from files/RPC advance the counter", "[ids]") {
  Document doc;
  auto boxWithId = MakeBox("X");
  boxWithId->SetId(50);
  doc.AddFeature(std::move(boxWithId));
  doc.AddFeature(MakeBox("Y"));
  CHECK(doc.GetFeature(1)->GetId() == 51);
}

TEST_CASE("v1 documents without ids load and get ids assigned", "[ids][io]") {
  fs::path path = fs::temp_directory_path() / "designer_v1_doc.json";
  {
    std::ofstream out(path);
    out << R"({"format": "designer-document", "version": 1, "features": [
      {"type": "Box", "name": "Old", "position": [0,0,0],
       "width": 2, "height": 2, "depth": 2}]})";
  }
  std::string error;
  auto doc = testio::Load(path.string());
  REQUIRE(doc != nullptr);
  CHECK(doc->GetFeature(0)->GetId() != 0);
  fs::remove(path);
}

TEST_CASE("Entity coordinate expressions round-trip through JSON (v38)",
          "[sketch][exprs]") {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t p = sk->AddEntity(sketch::Point{{1.0, 2.0}});
  const uint32_t l = sk->AddEntity(sketch::Line{{0.0, 0.0}, {4.0, 0.0}});
  const uint32_t c = sk->AddEntity(sketch::Circle{{2.0, 1.0}, 0.5});
  const uint32_t a = sk->AddEntity(sketch::Arc{{1.0, 1.0}, 2.0, 0.25, 1.75});
  sk->entityExprs[p] = {{"x", "w / 2"}, {"y", "h"}};
  sk->entityExprs[l] = {{"bx", "w"}};
  sk->entityExprs[c] = {{"r", "bore / 2"}};
  sk->entityExprs[a] = {{"cx", "w"}, {"cy", "h"}, {"r", "fil"}};
  sk->SetId(9);

  const auto j = DocumentIO::FeatureToJson(*sk);
  auto restored = DocumentIO::FeatureFromJson(j);
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);
  CHECK(rsk->entityExprs == sk->entityExprs);
  // Idempotent: writing the restored feature reproduces the same JSON.
  CHECK(DocumentIO::FeatureToJson(*rsk) == j);
  // The expr keys ride on the entity objects ("<field>_expr").
  CHECK(j.at("entities")[2].at("r_expr") == "bore / 2");
  // An entity without exprs carries no *_expr keys (compact JSON).
  auto sk2 = std::make_unique<SketchFeature>("S2", SketchPlane(PlaneType::XY));
  sk2->AddEntity(sketch::Circle{{0.0, 0.0}, 1.0});
  const auto j2 = DocumentIO::FeatureToJson(*sk2);
  CHECK_FALSE(j2.at("entities")[0].contains("r_expr"));
}

TEST_CASE("SketchFeature JSON round-trip with all entity types", "[sketch]") {
  SketchPlane plane(glm::dvec3(1, 2, 3), glm::dvec3(0, 1, 1),
                    glm::dvec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Sketch 1", plane);
  sk->AddEntity(sketch::Point{{1.0, 2.0}});
  sk->AddEntity(sketch::Line{{0.0, 0.0}, {4.0, 0.0}});
  sk->AddEntity(sketch::Circle{{2.0, 1.0}, 0.5});
  sk->AddEntity(sketch::Arc{{1.0, 1.0}, 2.0, 0.25, 1.75});
  sk->AddEntity(sketch::Ellipse{{3.0, 2.0}, 1.5, 0.75, 0.6});
  sk->SetId(7);

  auto j = DocumentIO::FeatureToJson(*sk);
  auto restored = DocumentIO::FeatureFromJson(j);
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);

  CHECK(rsk->GetId() == 7);
  CHECK(rsk->entities.size() == 5);
  CHECK(DocumentIO::FeatureToJson(*rsk) == j);

  // The arc survives with its angles intact.
  REQUIRE(std::holds_alternative<sketch::Arc>(rsk->entities[3]));
  const auto& arc = std::get<sketch::Arc>(rsk->entities[3]);
  CHECK_THAT(arc.radius, WithinAbs(2.0, 1e-6));
  CHECK_THAT(arc.startAngle, WithinAbs(0.25, 1e-6));
  CHECK_THAT(arc.endAngle, WithinAbs(1.75, 1e-6));

  // The ellipse survives with its radii and rotation intact.
  REQUIRE(std::holds_alternative<sketch::Ellipse>(rsk->entities[4]));
  const auto& ell = std::get<sketch::Ellipse>(rsk->entities[4]);
  CHECK_THAT(ell.center.x, WithinAbs(3.0, 1e-6));
  CHECK_THAT(ell.rMajor, WithinAbs(1.5, 1e-6));
  CHECK_THAT(ell.rMinor, WithinAbs(0.75, 1e-6));
  CHECK_THAT(ell.rotation, WithinAbs(0.6, 1e-6));

  // Plane axes reconstructed orthonormally
  CHECK_THAT(glm::length(rsk->plane.GetNormal()), WithinAbs(1.0, 1e-4));
  CHECK_THAT(glm::dot(rsk->plane.GetNormal(), rsk->plane.GetUAxis()),
             WithinAbs(0.0, 1e-4));
  glm::dvec3 world = rsk->plane.PlaneToWorld({1.0, 1.0});
  glm::dvec3 expected = sk->plane.PlaneToWorld({1.0, 1.0});
  CHECK_THAT(glm::length(world - expected), WithinAbs(0.0, 1e-4));

  // A sketch contributes no bodies
  Document doc;
  doc.AddFeature(std::move(restored));
  doc.AddFeature(std::make_unique<BoxFeature>("Box", glm::dvec3(0), 1, 1, 1));
  REQUIRE(doc.Regenerate());
  CHECK(doc.GetBodies().size() == 1);
}

TEST_CASE("ArcFromThreePoints builds a CCW arc through the middle point",
          "[sketch][arc]") {
  // Unit circle: start (1,0), through top (0,1), end (-1,0) → upper semicircle.
  auto arc = sketch::ArcFromThreePoints({1, 0}, {0, 1}, {-1, 0});
  REQUIRE(arc.has_value());
  CHECK_THAT(arc->center.x, WithinAbs(0.0, 1e-4));
  CHECK_THAT(arc->center.y, WithinAbs(0.0, 1e-4));
  CHECK_THAT(arc->radius, WithinAbs(1.0, 1e-4));
  // The span must include the through-point's angle (π/2).
  CHECK(arc->startAngle <= M_PI / 2 + 1e-6);
  CHECK(arc->endAngle >= M_PI / 2 - 1e-6);
  CHECK(arc->endAngle > arc->startAngle); // CCW-always

  // Same endpoints but bulging through the bottom → the other (lower) arc.
  auto lower = sketch::ArcFromThreePoints({1, 0}, {0, -1}, {-1, 0});
  REQUIRE(lower.has_value());
  double m = 0.5 * (lower->startAngle + lower->endAngle);
  CHECK(std::sin(m) < 0.0); // midpoint of the swept arc is below the x-axis

  // Collinear points have no circumcircle.
  CHECK_FALSE(sketch::ArcFromThreePoints({0, 0}, {1, 0}, {2, 0}).has_value());
}

TEST_CASE("EllipseFromClicks derives radii and rotation", "[sketch][ellipse]") {
  // Centre at origin, major-axis end at (2,0) → rMajor 2, rotation 0. A third
  // point at (0.5, 0.75) is 0.75 off the major (x) axis → rMinor 0.75.
  sketch::Ellipse e = sketch::EllipseFromClicks({0, 0}, {2, 0}, {0.5, 0.75});
  CHECK_THAT(e.rMajor, WithinAbs(2.0, 1e-5));
  CHECK_THAT(e.rMinor, WithinAbs(0.75, 1e-5));
  CHECK_THAT(e.rotation, WithinAbs(0.0, 1e-5));

  // Rotated major axis: end at (0,3) → rMajor 3, rotation π/2. The minor point
  // (−1.2, 0) is 1.2 off the (now vertical) major axis.
  sketch::Ellipse r = sketch::EllipseFromClicks({0, 0}, {0, 3}, {-1.2, 0});
  CHECK_THAT(r.rMajor, WithinAbs(3.0, 1e-5));
  CHECK_THAT(r.rMinor, WithinAbs(1.2, 1e-5));
  CHECK_THAT(r.rotation, WithinAbs(M_PI / 2, 1e-5));
}

TEST_CASE("Sketch constraints round-trip through JSON",
          "[sketch][serialization]") {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  uint32_t line0 = sk->AddEntity(sketch::Line{{0, 0}, {4, 0}});
  uint32_t circ0 = sk->AddEntity(sketch::Circle{{2, 1}, 0.5});
  sk->constraints.push_back(
      {sketch::ConstraintKind::Horizontal, {line0, 0}, {0, 0}, 0.0});
  sk->constraints.push_back(
      {sketch::ConstraintKind::Distance, {line0, 0}, {line0, 1}, 4.0});
  sk->constraints.push_back(
      {sketch::ConstraintKind::Radius, {circ0, 0}, {0, 0}, 0.5});
  uint32_t line1 = sk->AddEntity(sketch::Line{{0, 0}, {0, 3}});
  uint32_t pt0 = sk->AddEntity(sketch::Point{{1, 1}});
  sk->constraints.push_back({sketch::ConstraintKind::Angle,
                             {line0, 0},
                             {line1, 0},
                             glm::radians(90.0)});
  sk->constraints.push_back(
      {sketch::ConstraintKind::Tangent, {line0, 0}, {circ0, 0}, 0.0});
  sk->constraints.push_back(
      {sketch::ConstraintKind::PointOnLine, {pt0, 0}, {line0, 0}, 0.0});

  auto j = DocumentIO::FeatureToJson(*sk);
  auto restored = DocumentIO::FeatureFromJson(j);
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);
  REQUIRE(rsk->constraints.size() == 6);
  CHECK(rsk->constraints[0].kind == sketch::ConstraintKind::Horizontal);
  CHECK(rsk->constraints[1].kind == sketch::ConstraintKind::Distance);
  CHECK(rsk->constraints[1].value == 4.0);
  CHECK(rsk->constraints[2].a.entity == circ0);
  CHECK(rsk->constraints[3].kind == sketch::ConstraintKind::Angle);
  CHECK(rsk->constraints[4].kind == sketch::ConstraintKind::Tangent);
  CHECK(rsk->constraints[5].kind == sketch::ConstraintKind::PointOnLine);
  // Kind strings survive as expected.
  CHECK(j["constraints"][3]["kind"] == "angle");
  CHECK(j["constraints"][4]["kind"] == "tangent");
  CHECK(j["constraints"][5]["kind"] == "point_on_line");
  CHECK(DocumentIO::FeatureToJson(*rsk) == j);
}

TEST_CASE("Pre-v4 sketch migrates entity indices to stable ids",
          "[sketch][serialization]") {
  // A v3 sketch: entities carry no "id", constraints reference entity indices.
  nlohmann::json j = {
      {"type", "Sketch"},
      {"name", "Legacy"},
      {"id", 5},
      {"plane",
       {{"origin", {0.0, 0.0, 0.0}},
        {"normal", {0.0, 0.0, 1.0}},
        {"uAxis", {1.0, 0.0, 0.0}}}},
      {"entities",
       {{{"type", "line"}, {"a", {0.0, 0.0}}, {"b", {4.0, 0.0}}},
        {{"type", "circle"}, {"center", {2.0, 1.0}}, {"radius", 0.5}}}},
      {"constraints",
       {{{"kind", "horizontal"}, {"a", {0, 0}}, {"b", {0, 0}}, {"value", 0.0}},
        {{"kind", "radius"}, {"a", {1, 0}}, {"b", {0, 0}}, {"value", 0.5}}}}};

  auto restored = DocumentIO::FeatureFromJson(j);
  auto* sk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(sk != nullptr);
  REQUIRE(sk->entities.size() == 2);
  // Entities get ids 1..N in list order.
  CHECK(sketch::EntityId(sk->entities[0]) == 1u);
  CHECK(sketch::EntityId(sk->entities[1]) == 2u);
  // Index refs shifted to the matching ids (index 0 -> id 1, index 1 -> id 2).
  CHECK(sk->constraints[0].a.entity == 1u); // Horizontal on the line
  CHECK(sk->constraints[1].a.entity == 2u); // Radius on the circle
  // Re-saving now carries per-entity ids (v4 shape). There is no
  // `next_entity_id` any more: ids are minted collision-resistantly rather
  // than handed out by a per-sketch counter (F9), because two people editing
  // one sketch offline both got the same next id — and a face's stable name
  // is "<feature>/g<entityId>", so that made face names ambiguous.
  auto j2 = DocumentIO::FeatureToJson(*sk);
  CHECK(j2["entities"][0]["id"] == 1u);
  CHECK_FALSE(j2.contains("next_entity_id"));
  // Anything added later cannot collide with the migrated 1..N.
  const uint32_t fresh = sk->AddEntity(sketch::Circle{{5, 5}, 1.0});
  CHECK(fresh != 1u);
  CHECK(fresh != 2u);
}

TEST_CASE("Dimension label offset round-trips", "[sketch][serialization]") {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  uint32_t l0 = sk->AddEntity(sketch::Line{{0, 0}, {4, 0}});
  sketch::Constraint dim{
      sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 4.0};
  dim.labelOffset = {0.5, 1.2};
  sk->constraints.push_back(dim);

  auto restored = DocumentIO::FeatureFromJson(DocumentIO::FeatureToJson(*sk));
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);
  CHECK(rsk->constraints[0].labelOffset.x == 0.5);
  CHECK(rsk->constraints[0].labelOffset.y == 1.2);
}

TEST_CASE("Construction flag round-trips through JSON",
          "[sketch][serialization]") {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  sketch::Line ref{{0, 0}, {4, 0}};
  ref.construction = true;
  sk->AddEntity(ref);
  sk->AddEntity(sketch::Line{{0, 0}, {0, 3}}); // solid

  auto j = DocumentIO::FeatureToJson(*sk);
  auto restored = DocumentIO::FeatureFromJson(j);
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);
  CHECK(sketch::IsConstruction(rsk->entities[0]));
  CHECK_FALSE(sketch::IsConstruction(rsk->entities[1]));
  // Solid entities omit the key; construction ones carry it.
  CHECK(j["entities"][0].contains("construction"));
  CHECK_FALSE(j["entities"][1].contains("construction"));
}

TEST_CASE("Legacy rigid rect migrates to four lines with H/V",
          "[sketch][serialization]") {
  // A v4 sketch with a rigid "rect" entity (and a stray constraint on it).
  nlohmann::json j = {
      {"type", "Sketch"},
      {"name", "Legacy"},
      {"id", 9},
      {"next_entity_id", 3},
      {"plane",
       {{"origin", {0.0, 0.0, 0.0}},
        {"normal", {0.0, 0.0, 1.0}},
        {"uAxis", {1.0, 0.0, 0.0}}}},
      {"entities",
       {{{"type", "line"}, {"a", {0.0, 5.0}}, {"b", {4.0, 5.0}}, {"id", 1}},
        {{"type", "rect"}, {"c1", {0.0, 0.0}}, {"c2", {4.0, 2.0}}, {"id", 2}}}},
      {"constraints",
       // A constraint referencing the rigid rect (id 2) — should be dropped.
       {{{"kind", "coincident"},
         {"a", {1, 1}},
         {"b", {2, 0}},
         {"value", 0.0}}}}};

  auto restored = DocumentIO::FeatureFromJson(j);
  auto* sk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(sk != nullptr);
  // 1 original line + 4 rectangle lines.
  REQUIRE(sk->entities.size() == 5);
  for (const auto& e : sk->entities)
    CHECK(std::holds_alternative<sketch::Line>(e));
  // The rect's own constraints (referencing id 2) are gone; the rectangle
  // brings its 8 (4 coincident + 2 H + 2 V).
  size_t hv = 0;
  for (const auto& c : sk->constraints)
    if (c.kind == sketch::ConstraintKind::Horizontal ||
        c.kind == sketch::ConstraintKind::Vertical)
      hv++;
  CHECK(hv == 4);
  for (const auto& c : sk->constraints)
    CHECK((c.a.entity != 2u && c.b.entity != 2u)); // no dangling rect ref
  // The four rect lines still close into an extrudable loop.
  CHECK(sketch::BuildLoops(sk->entities).size() == 1);
}

TEST_CASE("SplitEntity splits a line and transfers its constraints",
          "[sketch]") {
  using K = sketch::ConstraintKind;
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t l0 = sk.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  uint32_t l1 =
      sk.AddEntity(sketch::Line{{4, 0}, {4, 3}}); // meets l0's far end
  sk.constraints.push_back({K::Coincident, {l0, 1}, {l1, 0}, 0.0});
  sk.constraints.push_back({K::Horizontal, {l0, 0}, {0, 0}, 0.0});
  sk.constraints.push_back(
      {K::Distance, {l0, 0}, {l0, 1}, 4.0}); // whole length

  const std::vector<uint32_t> ids = sk.SplitEntity(l0, {{{2, 0}}});
  REQUIRE(ids.size() == 2);
  CHECK(ids[0] == l0); // piece 1 keeps the id
  const uint32_t l2 = ids[1];
  REQUIRE(sk.entities.size() == 3);

  auto& p1 = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l0)]);
  auto& p2 = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l2)]);
  CHECK(p1.a == glm::dvec2(0, 0));
  CHECK(p1.b == glm::dvec2(2, 0)); // piece 1: a -> P
  CHECK(p2.a == glm::dvec2(2, 0));
  CHECK(p2.b == glm::dvec2(4, 0)); // piece 2: P -> original end

  int coincident = 0, horizontal = 0, distance = 0;
  bool spanDistance = false, farJoinMoved = false, cutAdded = false;
  for (const auto& c : sk.constraints) {
    if (c.kind == K::Coincident) {
      coincident++;
      // far-end join moved to piece 2: {l2,1} ~ {l1,0}
      if ((c.a.entity == l2 && c.a.which == 1 && c.b.entity == l1) ||
          (c.b.entity == l2 && c.b.which == 1 && c.a.entity == l1))
        farJoinMoved = true;
      // cut join: {l0,1} ~ {l2,0}
      if ((c.a.entity == l0 && c.a.which == 1 && c.b.entity == l2 &&
           c.b.which == 0) ||
          (c.b.entity == l0 && c.b.which == 1 && c.a.entity == l2 &&
           c.a.which == 0))
        cutAdded = true;
    } else if (c.kind == K::Horizontal) {
      horizontal++;
    } else if (c.kind == K::Distance) {
      distance++;
      // re-anchored to span both pieces: {l0,0}-{l2,1}
      if (c.a.entity == l0 && c.b.entity == l2 && c.b.which == 1)
        spanDistance = true;
    }
  }
  CHECK(coincident == 2); // transferred far join + new cut join
  CHECK(horizontal == 2); // original on l0 + copy on l2 (no parallel then)
  for (const auto& c : sk.constraints)
    CHECK(c.kind != K::Parallel);
  CHECK(distance == 1);
  CHECK(spanDistance);
  CHECK(farJoinMoved);
  CHECK(cutAdded);
}

TEST_CASE("TrimLineAt removes the segment between two crossings", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t l0 = sk.AddEntity(sketch::Line{{0, 0}, {6, 0}}); // horizontal
  sk.AddEntity(sketch::Line{{2, -1}, {2, 1}});              // crosses at x=2
  sk.AddEntity(sketch::Line{{4, -1}, {4, 1}});              // crosses at x=4

  // Click on the middle span [2,4]; it should be removed, leaving [0,2] +
  // [4,6].
  REQUIRE(sk.TrimLineAt(l0, {3, 0}));
  REQUIRE(sk.entities.size() == 4); // 2 crossers + 2 remaining pieces

  bool left = false, right = false;
  for (const auto& e : sk.entities) {
    if (auto* l = std::get_if<sketch::Line>(&e)) {
      auto near = [](glm::dvec2 p, glm::dvec2 q) {
        return glm::length(p - q) < 1e-4;
      };
      if ((near(l->a, {0, 0}) && near(l->b, {2, 0})) ||
          (near(l->a, {2, 0}) && near(l->b, {0, 0})))
        left = true;
      if ((near(l->a, {4, 0}) && near(l->b, {6, 0})) ||
          (near(l->a, {6, 0}) && near(l->b, {4, 0})))
        right = true;
    }
  }
  CHECK(left);
  CHECK(right);
}

TEST_CASE("TrimSpanAt reports the span a trim would remove", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t l0 = sk.AddEntity(sketch::Line{{0, 0}, {6, 0}});
  sk.AddEntity(sketch::Line{{2, -1}, {2, 1}}); // crosses at x=2
  sk.AddEntity(sketch::Line{{4, -1}, {4, 1}}); // crosses at x=4

  auto span = sk.TrimSpanAt(l0, {3, 0}); // cursor in the middle span
  REQUIRE(span.has_value());
  auto near = [](glm::dvec2 p, glm::dvec2 q) {
    return glm::length(p - q) < 1e-4;
  };
  CHECK(((near(span->first, {2, 0}) && near(span->second, {4, 0})) ||
         (near(span->first, {4, 0}) && near(span->second, {2, 0}))));
  // Preview is read-only: nothing changed.
  CHECK(sk.entities.size() == 3);
  // Not a line → nullopt.
  uint32_t c0 = sk.AddEntity(sketch::Circle{{0, 0}, 1.0});
  CHECK_FALSE(sk.TrimSpanAt(c0, {1, 0}).has_value());
}

TEST_CASE("TrimCurveAt turns a crossed circle into the complementary arc",
          "[sketch][arc]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t c0 = sk.AddEntity(sketch::Circle{{0, 0}, 1.0}); // unit circle
  sk.AddEntity(sketch::Line{{-2, 0}, {2, 0}}); // x-axis: crosses at (±1,0)

  // Click the top span → it's removed, leaving the lower semicircle arc.
  REQUIRE(sk.TrimCurveAt(c0, {0, 1}));
  REQUIRE(sk.entities.size() == 2); // circle→arc (same id) + the crossing line

  size_t idx = sk.IndexOfEntity(c0);
  REQUIRE(idx != SIZE_MAX);
  REQUIRE(std::holds_alternative<sketch::Arc>(sk.entities[idx]));
  const auto& arc = std::get<sketch::Arc>(sk.entities[idx]);
  CHECK_THAT(arc.radius, WithinAbs(1.0, 1e-4));
  CHECK(arc.endAngle > arc.startAngle); // CCW-always
  double mid = 0.5 * (arc.startAngle + arc.endAngle);
  CHECK(std::sin(mid) < 0.0); // surviving span is the bottom
}

TEST_CASE("TrimCurveAt splits a circle where another CIRCLE crosses it",
          "[sketch][arc]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t c0 = sk.AddEntity(sketch::Circle{{0, 0}, 2.0});
  // A small circle straddling the big one's rightmost point (2, 0): the two
  // crossings bracket a short span around angle 0.
  sk.AddEntity(sketch::Circle{{2, 0}, 0.5});

  // Click at (2,0) — inside the small circle's bite → that span is removed
  // and the big circle becomes the complementary arc.
  REQUIRE(sk.TrimCurveAt(c0, {2, 0}));
  size_t idx = sk.IndexOfEntity(c0);
  REQUIRE(idx != SIZE_MAX);
  REQUIRE(std::holds_alternative<sketch::Arc>(sk.entities[idx]));
  const auto& arc = std::get<sketch::Arc>(sk.entities[idx]);
  CHECK_THAT(arc.radius, WithinAbs(2.0, 1e-4));
  // The surviving sweep is almost the whole circle (the bite is small).
  CHECK(arc.endAngle - arc.startAngle > 5.5);
  // Its midpoint is on the LEFT side, away from the removed bite at (2,0).
  const double mid = 0.5 * (arc.startAngle + arc.endAngle);
  CHECK(std::cos(mid) < 0.0);
}

TEST_CASE("TrimCurveAt splits a crossed arc, dropping the clicked sub-span",
          "[sketch][arc]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  // Upper semicircle arc (0 → π), crossed by the y-axis at the top (0,1).
  uint32_t a0 = sk.AddEntity(sketch::Arc{{0, 0}, 1.0, 0.0, M_PI});
  sk.AddEntity(sketch::Line{{0, -2}, {0, 2}}); // crosses the arc at angle π/2

  // Click the right sub-span (angle ~π/4) → removed; left sub-span survives.
  glm::dvec2 clickRight(std::cos(M_PI / 4), std::sin(M_PI / 4));
  REQUIRE(sk.TrimCurveAt(a0, clickRight));

  size_t idx = sk.IndexOfEntity(a0);
  REQUIRE(idx != SIZE_MAX);
  REQUIRE(std::holds_alternative<sketch::Arc>(sk.entities[idx]));
  const auto& arc = std::get<sketch::Arc>(sk.entities[idx]);
  // Survivor is [π/2, π]: its midpoint is up-left (x<0, y>0).
  double mid = 0.5 * (arc.startAngle + arc.endAngle);
  CHECK(std::cos(mid) < 0.0);
  CHECK(std::sin(mid) > 0.0);
}

TEST_CASE("A circle crossing a line splits it for trimming", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {6, 0}});
  sk.AddEntity(sketch::Circle{{3, 0}, 1.0}); // crosses the line at x=2 and x=4
  auto near = [](glm::dvec2 p, glm::dvec2 q) {
    return glm::length(p - q) < 1e-3;
  };

  // Clicking the middle brackets the [2,4] span between the circle crossings.
  auto span = sk.TrimSpanAt(l, {3, 0});
  REQUIRE(span.has_value());
  CHECK(((near(span->first, {2, 0}) && near(span->second, {4, 0})) ||
         (near(span->first, {4, 0}) && near(span->second, {2, 0}))));

  // Trim removes that span → two line pieces remain (plus the circle).
  REQUIRE(sk.TrimLineAt(l, {3, 0}));
  int lines = 0, circles = 0;
  for (const auto& e : sk.entities) {
    if (std::holds_alternative<sketch::Line>(e))
      lines++;
    if (std::holds_alternative<sketch::Circle>(e))
      circles++;
  }
  CHECK(lines == 2);
  CHECK(circles == 1);
}

TEST_CASE("TrimLineAt with no crossings deletes the whole line", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t l0 = sk.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  sk.AddEntity(sketch::Line{{0, 2}, {4, 2}}); // parallel, never crosses
  REQUIRE(sk.TrimLineAt(l0, {2, 0}));
  CHECK(sk.entities.size() == 1); // only the parallel line remains
}

TEST_CASE("SplitEntity refuses, naming why, and changes nothing",
          "[sketch][split]") {
  using K = sketch::ConstraintKind;
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t c0 = sk.AddEntity(sketch::Circle{{0, 0}, 1.0});
  const uint32_t l0 = sk.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  const uint32_t l1 = sk.AddEntity(sketch::Line{{0, 3}, {4, 3}});
  const uint32_t a0 = sk.AddEntity(sketch::Arc{{10, 0}, 2.0, 0.0, M_PI});
  const uint32_t sp =
      sk.AddEntity(sketch::BSpline{{{10, 5}, {12, 8}, {14, 3}, {16, 5}}});
  const uint32_t el = sk.AddEntity(sketch::Ellipse{{20, 0}, 3.0, 2.0, 0.0});
  const uint32_t ea =
      sk.AddEntity(sketch::EllipseArc{{30, 0}, 3.0, 2.0, 0.0, 0.0, M_PI});
  const uint32_t pt = sk.AddEntity(sketch::Point{{1, 1}});
  const uint32_t eqA = sk.AddEntity(sketch::Line{{0, 6}, {4, 6}});
  const uint32_t eqB = sk.AddEntity(sketch::Line{{0, 7}, {4, 7}});
  sk.constraints.push_back({K::Equal, {eqA, 0}, {eqB, 0}, 0.0});
  sketch::Constraint len{K::ArcLength, {a0, 0}, {0, 0}, M_PI * 2.0};
  sk.constraints.push_back(len);
  const nlohmann::json before = DocumentIO::FeatureToJson(sk);
  const auto why = [&](uint32_t id,
                       const std::vector<SketchFeature::SplitCut>& cuts) {
    std::string w;
    CHECK(sk.SplitEntity(id, cuts, &w).empty());
    CHECK(DocumentIO::FeatureToJson(sk) == before);
    return w;
  };
  const auto has = [](const std::string& w, const std::string& needle) {
    INFO(w);
    return w.find(needle) != std::string::npos;
  };
  CHECK(has(why(999, {{{2, 0}}}), "no sketch entity with id 999"));
  CHECK(has(why(l0, {{{0, 0}}}), "at an end of line"));
  CHECK(has(why(l0, {{{4, 0}}}), "at an end of line"));
  CHECK(has(why(l0, {{{2, 1}}}), "more than a hundredth of its length"));
  CHECK(has(why(l0, {{{2, 0}, l1}}), "does not cross line"));
  CHECK(has(why(l0, {{{2, 0}, l0}}), "cannot cross itself"));
  CHECK(has(why(l0, {{{1, 0}}, {{3, 0}}}), "takes one cut"));
  CHECK(has(why(c0, {{{1, 0}}}), "takes two cuts"));
  CHECK(has(why(c0, {{{1, 0}}, {{1, 0}}}), "are at one point"));
  CHECK(has(why(sp, {{{12, 6}}}), "a spline is not split yet"));
  CHECK(has(why(el, {{{23, 0}}, {{17, 0}}}), "an ellipse is not split yet"));
  CHECK(has(why(ea, {{{33, 0}}}), "an elliptical arc is not split yet"));
  CHECK(has(why(sp, {{{12, 6}}}), "Split takes lines, arcs and circles"));
  CHECK(has(why(pt, {{{1, 1}}}), "a point, which has no curve to split"));
  CHECK(has(why(eqA, {{{2, 6}}}), "carries an equal length (constraint 0)"));
  CHECK(has(why(a0, {{{10, 2}}}), "carries an arc length (constraint 1)"));
}

TEST_CASE("Entity ids stay stable across insert and delete", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t a = sk.AddEntity(sketch::Line{{0, 0}, {1, 0}});
  uint32_t b = sk.AddEntity(sketch::Line{{1, 0}, {1, 1}});
  sk.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {a, 1}, {b, 0}});

  // Inserting a new entity shifts indices but not ids.
  uint32_t c = sk.AddEntity(sketch::Circle{{5, 5}, 1.0});
  // Distinct and non-zero — the property. Which NUMBERS they are is not:
  // they used to be 1, 2, 3 from a per-sketch counter, and that counter is
  // exactly what made two people editing one sketch offline mint the same
  // id (F9). 0 stays reserved for "none".
  CHECK(a != 0u);
  CHECK(b != 0u);
  CHECK(c != 0u);
  CHECK(a != b);
  CHECK(b != c);
  CHECK(a != c);
  CHECK(sk.IndexOfEntity(a) == 0u);
  CHECK(sk.IndexOfEntity(b) == 1u);
  // The A–B constraint still resolves after the insert.
  CHECK(sk.constraints[0].a.entity == a);

  // Deleting A shifts B down an index, but B's id is unchanged and resolvable.
  sk.DeleteEntity(a);
  CHECK(sk.IndexOfEntity(a) == std::numeric_limits<size_t>::max());
  CHECK(sk.IndexOfEntity(b) == 0u);
  CHECK(sketch::EntityId(sk.entities[0]) == b);
}

TEST_CASE("Cross-sketch linked copies sync until the link breaks",
          "[sketch][linked_copy]") {
  Document doc;
  auto a = std::make_unique<SketchFeature>("A", SketchPlane(PlaneType::XY));
  const uint32_t c1 = a->AddEntity(sketch::Circle{{0, 0}, 1.0});
  doc.AddFeature(std::move(a));
  const uint64_t aId = doc.GetFeature(0)->GetId();
  auto b = std::make_unique<SketchFeature>("B", SketchPlane(PlaneType::XY));
  doc.AddFeature(std::move(b));
  auto* skB = dynamic_cast<SketchFeature*>(doc.GetFeature(1));
  REQUIRE(skB != nullptr);

  sketch::DerivedOp op;
  op.kind = sketch::DerivedKind::LinkedCopy;
  op.sourceIds = {c1};
  op.vec = {3.0, 0.0};
  op.count = 2;
  op.sourceSketchId = aId;
  skB->derived.push_back(op);
  REQUIRE(doc.Regenerate());
  REQUIRE(skB->entities.size() == 1);
  {
    const auto* copy = std::get_if<sketch::Circle>(&skB->entities[0]);
    REQUIRE(copy != nullptr);
    CHECK_THAT(copy->center.x, WithinAbs(3.0, 1e-6));
    CHECK_THAT(copy->radius, WithinAbs(1.0, 1e-9));
  }

  // Editing the source syncs the copy on the next regeneration.
  auto* skA = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  std::get<sketch::Circle>(skA->entities[0]).radius = 2.0;
  REQUIRE(doc.Regenerate());
  CHECK_THAT(std::get<sketch::Circle>(skB->entities[0]).radius,
             WithinAbs(2.0, 1e-9));

  // Break the link: the copy keeps its id + geometry and stops syncing.
  const uint32_t copyId = sketch::EntityId(skB->entities[0]);
  skB->BreakLink(0);
  std::get<sketch::Circle>(skA->entities[0]).radius = 0.5;
  REQUIRE(doc.Regenerate());
  CHECK(sketch::EntityId(skB->entities[0]) == copyId);
  CHECK_THAT(std::get<sketch::Circle>(skB->entities[0]).radius,
             WithinAbs(2.0, 1e-9)); // no longer tracks

  // A vanished source entity is a first-class regen failure.
  sketch::DerivedOp op2 = op;
  skB->derived.push_back(op2);
  REQUIRE(doc.Regenerate());
  skA->DeleteEntity(c1);
  CHECK_FALSE(doc.Regenerate());
  CHECK_FALSE(skB->GetError().empty());
}

TEST_CASE("ProjectFaceBoundary maps face outlines to construction geometry",
          "[sketch][project]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Box", glm::dvec3(0.0), 2.0, 2.0, 2.0));
  doc.Regenerate();
  REQUIRE(doc.GetBodies().size() == 1);
  const SketchPlane xy(PlaneType::XY);
  // A face parallel to XY (a top/bottom cap) projects to 4 construction lines;
  // a perpendicular side face projects to a line segment (its edges normal to
  // the plane collapse to points and are dropped). Find the parallel one.
  int fourLineFaces = 0;
  for (int f = 0; f < 6; f++) {
    const auto ents =
        sketch::ProjectFaceBoundary(doc.GetBodies()[0].shape, f, xy);
    for (const auto& e : ents) {
      REQUIRE(std::holds_alternative<sketch::Line>(e));
      CHECK(std::get<sketch::Line>(e).construction);
    }
    if (ents.size() == 4)
      fourLineFaces++;
  }
  CHECK(fourLineFaces == 2); // the two XY-parallel caps

  Document doc2;
  doc2.AddFeature(std::make_unique<CylinderFeature>(
      "Cyl", glm::dvec3(0.0), glm::dvec3(0, 0, 1), 1.0, 2.0));
  doc2.Regenerate();
  REQUIRE(doc2.GetBodies().size() == 1);
  // Some face (a cap) projects to a single full construction circle.
  bool sawCircle = false;
  for (int f = 0; f < 4; f++) {
    const auto ce =
        sketch::ProjectFaceBoundary(doc2.GetBodies()[0].shape, f, xy);
    if (ce.size() == 1 && std::holds_alternative<sketch::Circle>(ce[0]))
      sawCircle = true;
  }
  CHECK(sawCircle);
}

TEST_CASE("HasUserGeometry ignores generated external geometry", "[sketch]") {
  // A face-attached sketch arrives holding the host face's projected outline;
  // that is not the user's work, so the Hole tool may discard such a sketch.
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  CHECK_FALSE(HasUserGeometry(sk)); // empty

  sketch::ExternalGeom ex;
  ex.ref = ElementRef{7, "2/cap0"};
  ex.kind = sketch::ExternalKind::Face;
  ex.genIds = {sk.AddEntity(sketch::Line{{0, 0}, {4, 0}}),
               sk.AddEntity(sketch::Line{{4, 0}, {4, 2}})};
  sk.external.push_back(ex);
  CHECK_FALSE(HasUserGeometry(sk)); // only projected outline

  sk.AddEntity(sketch::Point{{2.0, 1.0}}); // the user places a hole centre
  CHECK(HasUserGeometry(sk));
}

// ---------------------------------------------------------------------------
// Sketch entity ids and merges (F9)
//
// A face's stable name is literally "<feature>/g<entityId>"
// (SketchProfile.cpp). So two entities sharing an id make a face NAME
// ambiguous, and every reference keyed on it — a fillet, a mate, a drawing
// dimension — can bind to the wrong face while the document rebuilds
// perfectly and says nothing. That was reachable: `nextEntityId++` is a
// per-sketch counter, so two people editing one sketch offline both got the
// same next id.
// ---------------------------------------------------------------------------

TEST_CASE("Two sketches edited separately do not mint the same entity ids",
          "[sketch]") {
  // The offline-collaborator shape: one sketch, copied, both copies extended.
  SketchFeature base("S", SketchPlane(PlaneType::XY));
  for (int i = 0; i < 4; i++)
    base.AddEntity(sketch::Line{{double(i), 0}, {double(i + 1), 0}});

  SketchFeature alice = base;
  SketchFeature bob = base;
  std::set<uint32_t> aliceAdded, bobAdded;
  for (int i = 0; i < 8; i++) {
    aliceAdded.insert(alice.AddEntity(sketch::Line{{0, double(i)}, {1, 0}}));
    bobAdded.insert(bob.AddEntity(sketch::Circle{{double(i), 5}, 1.0}));
  }

  // Under the old counter every one of these collided: 5,6,7... on both
  // sides. Now nothing either of them added is a name the other also minted.
  std::vector<uint32_t> shared;
  std::set_intersection(aliceAdded.begin(), aliceAdded.end(), bobAdded.begin(),
                        bobAdded.end(), std::back_inserter(shared));
  CHECK(shared.empty());
}

// The tripwire. Ids are collision-resistant now, so this should be
// unreachable — but a project can arrive by rsync, a zip or a hand-edit, and
// "the face names built from it are ambiguous" must be a refusal rather than
// a document that quietly points a fillet at the wrong face.
TEST_CASE("A sketch with two entities sharing an id is refused",
          "[sketch][serialization]") {
  // Built explicitly rather than as one brace-init literal: nlohmann reads a
  // nested initialiser list as an OBJECT, so an array of entity objects has
  // to be pushed.
  nlohmann::json entity = {
      {"type", "line"}, {"a", {0.0, 0.0}}, {"b", {1.0, 0.0}}, {"id", 9}};
  nlohmann::json j;
  j["type"] = "Sketch";
  j["name"] = "Profile";
  j["plane"] = {{"origin", {0.0, 0.0, 0.0}},
                {"normal", {0.0, 0.0, 1.0}},
                {"uAxis", {1.0, 0.0, 0.0}}};
  j["entities"] = nlohmann::json::array();
  j["entities"].push_back(entity);
  entity["b"] = {1.0, 1.0};
  j["entities"].push_back(entity); // same id 9
  j["constraints"] = nlohmann::json::array();

  REQUIRE_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                      Catch::Matchers::ContainsSubstring("two entities") &&
                          Catch::Matchers::ContainsSubstring("id 9"));
}

TEST_CASE("SiblingRegions expands a pattern member to the whole family",
          "[sketch][pattern]") {
  // The five-hole flange case: one circle, circular-patterned x5. Picking
  // any one hole as an extrude profile must mean all five — the user who
  // filed this had to click each hole of their bolt circle by hand.
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t c = sk.AddEntity(sketch::Circle{{10, 0}, 2.0});
  auto gen = sk.AddCircularPattern({c}, {0, 0}, 2.0 * M_PI / 5.0, /*count=*/5,
                                   /*link=*/true);
  REQUIRE(gen.size() == 4);

  // The source expands to all five loops; any instance expands to the same.
  const auto fam = sk.SiblingRegions({c});
  REQUIRE(fam.size() == 5);
  CHECK(sk.SiblingRegions({gen[2]}) == fam);

  // A loop with no pattern behind it stays itself.
  const uint32_t lone = sk.AddEntity(sketch::Circle{{40, 40}, 1.0});
  const auto solo = sk.SiblingRegions({lone});
  REQUIRE(solo.size() == 1);
  CHECK(solo[0] == std::vector<uint32_t>{lone});

  // Every family row is a real `regions` entry: filtering the sketch's loops
  // to the family keeps exactly the five pattern circles.
  auto loops = sketch::BuildLoops(sk.entities);
  REQUIRE(loops.size() == 6);
  REQUIRE(sketch::FilterLoopsToRegions(loops, fam));
  CHECK(loops.size() == 5);
}

#include <iterator>

#include <set>

// --- Fillet of lines and arcs (docs/sketching/README.md#fillet) ------------

namespace {

using FK = sketch::ConstraintKind;

double DistanceToCurve(const sketch::Entity& curve, glm::dvec2 p) {
  if (const auto* l = std::get_if<sketch::Line>(&curve)) {
    const glm::dvec2 d = glm::normalize(l->b - l->a);
    const glm::dvec2 q = p - l->a;
    return std::abs(q.x * d.y - q.y * d.x);
  }
  const auto& a = std::get<sketch::Arc>(curve);
  return std::abs(glm::length(p - a.center) - a.radius);
}

// The fillet arc's end (1 or 2) that lies on `curve`, as it was.
glm::dvec2 EndOn(const sketch::Arc& arc, const sketch::Entity& curve) {
  const glm::dvec2 e1 = sketch::ArcPointUV(arc, 1),
                   e2 = sketch::ArcPointUV(arc, 2);
  return DistanceToCurve(curve, e1) < DistanceToCurve(curve, e2) ? e1 : e2;
}

// How far the fillet arc is from tangent to `curve` at `at`: the sine of the
// angle between their tangents there.
double OffTangent(const sketch::Arc& arc, const sketch::Entity& curve,
                  glm::dvec2 at) {
  const glm::dvec2 radial = glm::normalize(at - arc.center);
  if (const auto* l = std::get_if<sketch::Line>(&curve))
    return std::abs(glm::dot(radial, glm::normalize(l->b - l->a)));
  const glm::dvec2 other =
      glm::normalize(at - std::get<sketch::Arc>(curve).center);
  return std::abs(radial.x * other.y - radial.y * other.x);
}

// The whole check of one corner: the tangent points on the curves as they
// were (1e-9 mm), the arc tangent to both, the sketch solving clean one DOF
// up (the radius), still in place after the solve, and the profile
// extruding.
void CheckFilletedCorner(std::unique_ptr<SketchFeature> sk, uint32_t c1,
                         uint32_t c2, double r, glm::dvec2 corner) {
  const sketch::Entity o1 = sk->entities[sk->IndexOfEntity(c1)];
  const sketch::Entity o2 = sk->entities[sk->IndexOfEntity(c2)];
  sketch::Solver s0;
  s0.Load(*sk);
  const sketch::SolveResult r0 = s0.Solve();
  REQUIRE(r0.ok);
  std::string why;
  const uint32_t arcId = sk->FilletCorner(c1, c2, r, corner, &why);
  INFO(why);
  REQUIRE(arcId != 0);
  const auto check = [&](double tol) {
    const auto& arc =
        std::get<sketch::Arc>(sk->entities[sk->IndexOfEntity(arcId)]);
    CHECK_THAT(arc.radius, WithinAbs(r, tol));
    for (const sketch::Entity* o : {&o1, &o2}) {
      const glm::dvec2 t = EndOn(arc, *o);
      CHECK(DistanceToCurve(*o, t) < tol);
      CHECK(OffTangent(arc, *o, t) < 1e-9);
    }
    // Each curve now ends at its tangent point.
    const auto endsAt = [&](uint32_t id, const sketch::Entity& o) {
      const sketch::Entity& e = sk->entities[sk->IndexOfEntity(id)];
      const glm::dvec2 t = EndOn(arc, o);
      double best = 1e300;
      for (uint8_t w : {0, 1, 2})
        if (!(std::holds_alternative<sketch::Line>(e) && w == 2) &&
            !(std::holds_alternative<sketch::Arc>(e) && w == 0))
          best = std::min(best, glm::length(sketch::EntityPointUV(e, w) - t));
      return best;
    };
    CHECK(endsAt(c1, o1) < tol);
    CHECK(endsAt(c2, o2) < tol);
  };
  check(1e-9);
  sketch::Solver s1;
  s1.Load(*sk);
  const sketch::SolveResult r1 = s1.Solve();
  CHECK(r1.ok);
  CHECK(r1.conflictingConstraints.empty());
  CHECK(r1.redundantConstraints.empty());
  CHECK(r1.dof == r0.dof + 1); // the radius
  s1.WriteBack(sk->entities);
  check(1e-9);

  Document doc;
  doc.AddFeature(std::move(sk));
  const uint64_t sid = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", sid, 2.0, ExtrudeMode::NewBody));
  CHECK(doc.GetFeature(0)->GetError().empty());
  CHECK(doc.GetFeature(1)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].shape.Volume() > 0.0);
}

void Join(SketchFeature& sk, uint32_t a, uint8_t wa, uint32_t b, uint8_t wb) {
  sk.constraints.push_back({FK::Coincident, {a, wa}, {b, wb}, 0.0});
}

double Deg(double d) { return d * M_PI / 180.0; }

} // namespace

TEST_CASE("Fillet a line and an arc whose centre is inside the corner",
          "[sketch][arc][fillet]") {
  // A half disc: the corner at the origin is the line and the arc's end.
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk->AddEntity(sketch::Line{{0, 0}, {10, 0}});
  const uint32_t a = sk->AddEntity(sketch::Arc{{5, 0}, 5.0, 0.0, M_PI});
  Join(*sk, l, 1, a, 1);
  Join(*sk, a, 2, l, 0);
  CheckFilletedCorner(std::move(sk), l, a, 1.0, {0, 0});
}

TEST_CASE("Fillet a line and an arc that bulges into the corner",
          "[sketch][arc][fillet]") {
  // A square whose left side is an arc bowed into it.
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t b = sk->AddEntity(sketch::Line{{0, 0}, {10, 0}});
  const uint32_t rgt = sk->AddEntity(sketch::Line{{10, 0}, {10, 10}});
  const uint32_t t = sk->AddEntity(sketch::Line{{10, 10}, {0, 10}});
  const uint32_t a = sk->AddEntity(
      sketch::Arc{{-5, 5}, std::sqrt(50.0), Deg(-45.0), Deg(45.0)});
  Join(*sk, b, 1, rgt, 0);
  Join(*sk, rgt, 1, t, 0);
  Join(*sk, t, 1, a, 2);
  Join(*sk, a, 1, b, 0);
  CheckFilletedCorner(std::move(sk), b, a, 1.0, {0, 0});
}

TEST_CASE("Fillet two arcs meeting convex", "[sketch][arc][fillet]") {
  // A lens: inside both circles, corners where they cross.
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const double up = std::atan2(3.0, 4.0); // (4, 0) from (0, -3)
  const uint32_t top = sk->AddEntity(sketch::Arc{{0, -3}, 5.0, up, M_PI - up});
  const uint32_t bot =
      sk->AddEntity(sketch::Arc{{0, 3}, 5.0, M_PI + up, 2.0 * M_PI - up});
  Join(*sk, top, 1, bot, 2);
  Join(*sk, top, 2, bot, 1);
  CheckFilletedCorner(std::move(sk), top, bot, 0.5, {4, 0});
}

TEST_CASE("Fillet two arcs meeting concave", "[sketch][arc][fillet]") {
  // A crescent: inside the big circle, outside the small one.
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const double y = 25.0 / 8.0, x = std::sqrt(25.0 - y * y);
  const double aBig = std::atan2(y, x);
  const double aSmall = std::atan2(y - 4.0, x);
  const uint32_t big =
      sk->AddEntity(sketch::Arc{{0, 0}, 5.0, M_PI - aBig, 2.0 * M_PI + aBig});
  const uint32_t small = sk->AddEntity(
      sketch::Arc{{0, 4}, 4.0, M_PI - aSmall, 2.0 * M_PI + aSmall});
  Join(*sk, big, 2, small, 2);
  Join(*sk, small, 1, big, 1);
  CheckFilletedCorner(std::move(sk), big, small, 0.3, {x, y});
}

TEST_CASE("A fillet too large names the largest that fits, and that fits",
          "[sketch][arc][fillet]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {10, 0}});
  const uint32_t a = sk.AddEntity(sketch::Arc{{5, 0}, 5.0, 0.0, M_PI});
  Join(sk, l, 1, a, 1);
  Join(sk, a, 2, l, 0);
  const nlohmann::json before = DocumentIO::FeatureToJson(sk);
  std::string why;
  double largest = -1.0;
  CHECK(sk.FilletCorner(l, a, 4.0, glm::dvec2(0, 0), &why, &largest) == 0);
  CHECK(DocumentIO::FeatureToJson(sk) == before); // a refusal changes nothing
  INFO(why);
  CHECK_THAT(why, Catch::Matchers::StartsWith("4 mm does not fit this "
                                              "corner; "));
  CHECK_THAT(why, Catch::Matchers::EndsWith(" mm is the largest that does"));
  REQUIRE(largest > 0.5);
  REQUIRE(largest < 4.0);
  CHECK(why.find(units::FormatNumber(largest)) != std::string::npos);
  CHECK(sk.FilletCorner(l, a, largest, glm::dvec2(0, 0)) != 0);
}

TEST_CASE("FilletCorner refuses, naming why, and changes nothing",
          "[sketch][fillet]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {10, 0}});
  const uint32_t a = sk.AddEntity(sketch::Arc{{5, 0}, 5.0, 0.0, M_PI});
  const uint32_t sp =
      sk.AddEntity(sketch::BSpline{{{10, 0}, {12, 3}, {14, -2}, {16, 0}}});
  const uint32_t ea = sk.AddEntity(
      sketch::EllipseArc{{-3, 0}, 3.0, 2.0, 0.0, Deg(180.0), Deg(360.0)});
  const uint32_t c = sk.AddEntity(sketch::Circle{{20, 20}, 2.0});
  Join(sk, l, 1, a, 1);
  Join(sk, a, 2, l, 0);
  const nlohmann::json before = DocumentIO::FeatureToJson(sk);
  const auto why = [&](uint32_t x, uint32_t y,
                       std::optional<glm::dvec2> near = std::nullopt) {
    std::string w;
    CHECK(sk.FilletCorner(x, y, 1.0, near, &w) == 0);
    CHECK(DocumentIO::FeatureToJson(sk) == before);
    return w;
  };
  // Two shared ends and no `near`: both named.
  CHECK(why(l, a).find("share two ends, at (0, 0) and (10, 0)") !=
        std::string::npos);
  CHECK(why(l, sp).find("spline " + std::to_string(sp) +
                        ": a spline end is not filleted yet") == 0);
  CHECK(why(ea, l).find("elliptical arc " + std::to_string(ea) +
                        ": an elliptical arc end is not filleted yet") == 0);
  CHECK(why(c, l).find("is a circle, which has no end") != std::string::npos);
  CHECK(why(l, l).find("two different curves") != std::string::npos);
  CHECK(why(c, 999).find("no sketch entity with id 999") == 0);
}

TEST_CASE("FilletCorner refuses a curve a linked op generates",
          "[sketch][fillet]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t l1 = sk.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  const uint32_t l2 = sk.AddEntity(sketch::Line{{0, 0}, {0, 4}});
  const auto copies = sk.AddLinearPattern({l1, l2}, {10, 0}, 2, true);
  REQUIRE(copies.size() == 2);
  std::string why;
  CHECK(sk.FilletCorner(copies[0], copies[1], 1.0, std::nullopt, &why) == 0);
  CHECK(why.find("is generated by a linked pattern") != std::string::npos);
  CHECK(sk.GeneratedBy(l1).empty());
}

TEST_CASE("CornerAt takes the sector the cursor is in", "[sketch][fillet]") {
  // The owner's corner: a spline, a vertical line and two collinear
  // horizontal lines, all ending at the origin.
  struct Ids {
    uint32_t h1, h2, v, s;
  };
  Ids id{};
  auto make = [&id] {
    auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
    id.h1 = sk->AddEntity(sketch::Line{{0, 0}, {10, 0}});
    id.h2 = sk->AddEntity(sketch::Line{{0, 0}, {-10, 0}});
    id.v = sk->AddEntity(sketch::Line{{0, 0}, {0, 10}});
    id.s = sk->AddEntity(sketch::BSpline{{{0, 0}, {3, -5}, {6, -6}, {10, -8}}});
    Join(*sk, id.h1, 0, id.h2, 0);
    Join(*sk, id.h2, 0, id.v, 0);
    Join(*sk, id.v, 0, id.s, 0);
    sk->constraints.push_back({FK::Horizontal, {id.h1, 0}, {}, 0.0});
    sk->constraints.push_back({FK::Horizontal, {id.h2, 0}, {}, 0.0});
    sk->constraints.push_back({FK::Vertical, {id.v, 0}, {}, 0.0});
    return sk;
  };
  auto sk = make();
  const auto& es = sk->entities;
  using Pair = std::pair<uint32_t, uint32_t>;
  const auto sorted = [](uint32_t a, uint32_t b) {
    return a < b ? Pair{a, b} : Pair{b, a};
  };
  const auto pair = [&](glm::dvec2 cursor) {
    const auto c = sketch::CornerAt(es, {0, 0}, cursor, 0.1);
    REQUIRE(c.has_value());
    return sorted(c->a.entity, c->b.entity);
  };
  CHECK(pair({1, 1}) == sorted(id.h1, id.v));
  CHECK(pair({-1, 1}) == sorted(id.v, id.h2));
  CHECK(pair({-1, -1}) == sorted(id.h2, id.s));
  CHECK(pair({1, -0.2}) == sorted(id.s, id.h1));
  // On the vertex, four curves end: refused, saying what to do.
  const auto onVertex = sketch::CornerAt(es, {0, 0}, {0.01, 0.01}, 0.1);
  REQUIRE(!onVertex.has_value());
  CHECK(onVertex.error().find("4 curves end at this point; move into the "
                              "corner") == 0);
  // A sector bounded by the spline is refused by name in stage A.
  std::string why;
  CHECK(sk->FilletCorner(id.h1, id.s, 1.0, glm::dvec2(0, 0), &why) == 0);
  CHECK(why.find("spline " + std::to_string(id.s)) == 0);

  // Each line sector fillets; the other two curves stay at the corner and
  // nothing reads redundant.
  for (int sector = 0; sector < 2; sector++) {
    auto fresh = make();
    const uint32_t x = sector == 0 ? id.h1 : id.v;
    const uint32_t y = sector == 0 ? id.v : id.h2;
    std::string w;
    const uint32_t made = fresh->FilletCorner(x, y, 1.0, glm::dvec2(0, 0), &w);
    INFO(w);
    REQUIRE(made != 0);
    sketch::Solver solver;
    solver.Load(*fresh);
    const sketch::SolveResult r = solver.Solve();
    CHECK(r.ok);
    CHECK(r.redundantConstraints.empty());
    CHECK(r.conflictingConstraints.empty());
    solver.WriteBack(fresh->entities);
    for (uint32_t other : {id.h1, id.h2, id.v})
      if (other != x && other != y) {
        const auto& ln = std::get<sketch::Line>(
            fresh->entities[fresh->IndexOfEntity(other)]);
        CHECK(glm::length(ln.a) < 1e-9);
      }
    const auto& spl =
        std::get<sketch::BSpline>(fresh->entities[fresh->IndexOfEntity(id.s)]);
    CHECK(glm::length(spl.poles[0]) < 1e-9);
    // No coincident is repeated.
    std::set<std::pair<std::pair<uint32_t, int>, std::pair<uint32_t, int>>>
        seen;
    for (const auto& con : fresh->constraints)
      if (con.kind == FK::Coincident) {
        const std::pair<uint32_t, int> a{con.a.entity, con.a.which},
            b{con.b.entity, con.b.which};
        CHECK(seen.insert(a < b ? std::pair{a, b} : std::pair{b, a}).second);
      }
  }
}

TEST_CASE("CornerAt refuses one curve, a wide side and a straight run",
          "[sketch][fillet]") {
  std::vector<sketch::Entity> es = {sketch::Line{{0, 0}, {4, 0}, 1},
                                    sketch::Line{{0, 0}, {0, 4}, 2}};
  // Outside the right angle: three quarters of a turn.
  const auto wide = sketch::CornerAt(es, {0, 0}, {-1, -1}, 0.1);
  REQUIRE(!wide);
  CHECK(wide.error().find("wider than half a turn") != std::string::npos);
  // On the vertex of two curves: the side under half a turn.
  const auto on = sketch::CornerAt(es, {0, 0}, {0, 0}, 0.1);
  REQUIRE(on);
  CHECK(on->a.entity == 1);
  CHECK(on->b.entity == 2);
  const auto one = sketch::CornerAt(es, {4, 0}, {3, 1}, 0.1);
  REQUIRE(!one);
  CHECK(one.error().find("only line 1 ends here") == 0);
  std::vector<sketch::Entity> run = {sketch::Line{{0, 0}, {4, 0}, 1},
                                     sketch::Line{{0, 0}, {-4, 0}, 2}};
  const auto straight = sketch::CornerAt(run, {0, 0}, {1, 1}, 0.1);
  REQUIRE(!straight);
  CHECK(straight.error().find("continue each other") != std::string::npos);
}

// The Trim tool's click refused only a linked pattern's or mirror's
// instances: a projected or section line went through, and the next
// regeneration re-stamped it from the model. The core refuses every
// generated entity now (SketchFeature::GeneratedBy), so the RPC path and
// the click cannot differ.
TEST_CASE("Trim refuses a projected or section entity, changing nothing",
          "[sketch][trim]") {
  for (const auto kind :
       {sketch::ExternalKind::Face, sketch::ExternalKind::Section}) {
    SketchFeature sk("S", SketchPlane(PlaneType::XY));
    const uint32_t line = sk.AddEntity(sketch::Line{{0, 0}, {6, 0}});
    const uint32_t circle = sk.AddEntity(sketch::Circle{{3, 0}, 2.0});
    sk.AddEntity(sketch::Line{{2, -1}, {2, 1}});
    sk.AddEntity(sketch::Line{{4, -1}, {4, 1}});
    sketch::ExternalGeom ex;
    ex.ref = ElementRef{7, "2/cap0"};
    ex.kind = kind;
    ex.genIds = {line, circle};
    sk.external.push_back(ex);
    const nlohmann::json before = DocumentIO::FeatureToJson(sk);
    CHECK_FALSE(sk.TrimLineAt(line, {3, 0}));
    CHECK_FALSE(sk.TrimCurveAt(circle, {3, 2}));
    CHECK(DocumentIO::FeatureToJson(sk) == before);
  }
}

// --- Split (docs/sketching/README.md#split) ---------------------------------

namespace {

int SolvedDof(SketchFeature& sk) {
  sketch::Solver s;
  s.Load(sk);
  const sketch::SolveResult r = s.Solve();
  REQUIRE(r.ok);
  CHECK(r.conflictingConstraints.empty());
  CHECK(r.redundantConstraints.empty());
  s.WriteBack(sk.entities);
  return r.dof;
}

// Every point of `piece` lies on `curve` (a line's line or an arc's circle).
double OffCurve(const sketch::Entity& piece, const sketch::Entity& curve) {
  double worst = 0.0;
  for (const glm::dvec2& p : sketch::TessellateEntity(piece)) {
    if (const auto* l = std::get_if<sketch::Line>(&curve)) {
      const glm::dvec2 d = glm::normalize(l->b - l->a), q = p - l->a;
      worst = std::max(worst, std::abs(q.x * d.y - q.y * d.x));
    } else {
      const glm::dvec2 c = std::holds_alternative<sketch::Circle>(curve)
                               ? std::get<sketch::Circle>(curve).center
                               : std::get<sketch::Arc>(curve).center;
      const double r = std::holds_alternative<sketch::Circle>(curve)
                           ? std::get<sketch::Circle>(curve).radius
                           : std::get<sketch::Arc>(curve).radius;
      worst = std::max(worst, std::abs(glm::length(p - c) - r));
    }
  }
  return worst;
}

bool SameGeometry(const std::vector<sketch::Entity>& a,
                  const std::vector<sketch::Entity>& b, double tol) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++) {
    const auto pa = sketch::TessellateEntity(a[i]);
    const auto pb = sketch::TessellateEntity(b[i]);
    if (pa.size() != pb.size())
      return false;
    for (size_t k = 0; k < pa.size(); k++)
      if (glm::length(pa[k] - pb[k]) > tol)
        return false;
  }
  return true;
}

// The whole check of one split: two pieces, the first keeping the id, each
// on the original curve and together as long as it; the sketch solving
// clean at the DOF it had plus `dofUp`, and nothing moving in the solve;
// either piece made construction leaving the other where it was; and the
// ids the same after a round trip through the file.
void CheckSplit(SketchFeature& sk, uint32_t id,
                const std::vector<SketchFeature::SplitCut>& cuts, int dofUp) {
  const int dof0 = SolvedDof(sk);
  const sketch::Entity original = sk.entities[sk.IndexOfEntity(id)];
  std::string why;
  const std::vector<uint32_t> ids = sk.SplitEntity(id, cuts, &why);
  INFO(why);
  REQUIRE(ids.size() == 2);
  CHECK(ids[0] == id);
  double length = 0.0;
  for (uint32_t p : ids) {
    const sketch::Entity& e = sk.entities[sk.IndexOfEntity(p)];
    CHECK(OffCurve(e, original) < 1e-9);
    length += sketch::CurveLength(e);
  }
  CHECK_THAT(length, WithinAbs(sketch::CurveLength(original), 1e-9));
  const std::vector<sketch::Entity> cut = sk.entities;
  CHECK(SolvedDof(sk) == dof0 + dofUp);
  CHECK(SameGeometry(sk.entities, cut, 1e-9));
  for (uint32_t p : ids) {
    SketchFeature c = sk;
    sketch::SetConstruction(c.entities[c.IndexOfEntity(p)], true);
    SolvedDof(c);
    CHECK(SameGeometry(c.entities, cut, 1e-9));
  }
  const nlohmann::json j = DocumentIO::FeatureToJson(sk);
  const auto back = DocumentIO::FeatureFromJson(j);
  const auto* sk2 = dynamic_cast<const SketchFeature*>(back.get());
  REQUIRE(sk2 != nullptr);
  for (uint32_t p : ids)
    CHECK(sk2->IndexOfEntity(p) != SIZE_MAX);
}

} // namespace

TEST_CASE("Split a line at a crossing and at a free point", "[sketch][split]") {
  for (const bool crossing : {true, false}) {
    INFO((crossing ? "at the crossing" : "free"));
    SketchFeature sk("S", SketchPlane(PlaneType::XY));
    const uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {10, 4}});
    const uint32_t c = sk.AddEntity(sketch::Line{{5, -3}, {6, 9}});
    sk.constraints.push_back({FK::Distance, {l, 0}, {l, 1}, std::hypot(10, 4)});
    if (crossing)
      CheckSplit(sk, l, {{{5.3, 2.0}, c}}, 0);
    else
      CheckSplit(sk, l, {{{3.0, 1.2}}}, 1);
    // Collinear: piece 2 parallel to piece 1, the whole-edge distance
    // spanning both.
    bool parallel = false, spans = false;
    for (const auto& k : sk.constraints) {
      parallel |= k.kind == FK::Parallel;
      spans |= k.kind == FK::Distance && k.a.entity == l && k.b.entity != l;
    }
    CHECK(parallel);
    CHECK(spans);
  }
}

TEST_CASE("Split an arc at a crossing and at a free point",
          "[sketch][split][arc]") {
  for (const bool crossing : {true, false}) {
    INFO((crossing ? "at the crossing" : "free"));
    SketchFeature sk("S", SketchPlane(PlaneType::XY));
    const uint32_t a = sk.AddEntity(sketch::Arc{{0, 0}, 5.0, 0.0, M_PI});
    const uint32_t c = sk.AddEntity(sketch::Line{{1, -1}, {-1, 8}});
    const uint32_t far = sk.AddEntity(sketch::Line{{-5, 0}, {-5, -4}});
    Join(sk, a, 2, far, 0); // the far end's coincident goes with piece 2
    sketch::Constraint r{FK::Radius, {a, 0}, {0, 0}, 5.0};
    sk.constraints.push_back(r);
    if (crossing)
      CheckSplit(sk, a, {{{0.2, 4.9}, c}}, 0);
    else
      CheckSplit(sk, a, {{{3.5, 3.5}}}, 1);
    bool moved = false;
    for (const auto& k : sk.constraints)
      moved |= k.kind == FK::Coincident && k.b.entity == far &&
               k.a.entity != a && k.a.which == 2;
    CHECK(moved);
  }
}

TEST_CASE("Split a circle at two crossings and at two free points",
          "[sketch][split][arc]") {
  for (const bool crossing : {true, false}) {
    INFO((crossing ? "at the crossings" : "free"));
    SketchFeature sk("S", SketchPlane(PlaneType::XY));
    const uint32_t c = sk.AddEntity(sketch::Circle{{0, 0}, 5.0});
    const uint32_t l = sk.AddEntity(sketch::Line{{-8, 1}, {8, 1}});
    sk.constraints.push_back({FK::Diameter, {c, 0}, {0, 0}, 10.0});
    if (crossing)
      CheckSplit(sk, c, {{{4.9, 1.0}, l}, {{-4.9, 1.0}, l}}, 0);
    else
      CheckSplit(sk, c, {{{4, -3}}, {{-3, 4}}}, 2);
    // Both pieces are arcs now, the first from the first cut CCW.
    const auto& p1 = std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(c)]);
    const glm::dvec2 start = sketch::ArcPointUV(p1, 1);
    CHECK(glm::length(start - (crossing ? glm::dvec2(std::sqrt(24.0), 1.0)
                                        : glm::dvec2(4, -3))) < 1e-9);
  }
}

TEST_CASE("The owner's line over a circle: split at both crossings",
          "[sketch][split]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk.AddEntity(sketch::Line{{-10, 0}, {10, 0}});
  const uint32_t c = sk.AddEntity(sketch::Circle{{0, 0}, 4.0});
  sk.constraints.push_back({FK::Horizontal, {l, 0}, {0, 0}, 0.0});
  const int dof0 = SolvedDof(sk);
  const auto first = sk.SplitEntity(l, {{{-4, 0}, c}});
  REQUIRE(first.size() == 2);
  const auto second = sk.SplitEntity(first[1], {{{4, 0}, c}});
  REQUIRE(second.size() == 2);
  const uint32_t inner = second[0];
  const std::vector<sketch::Entity> cut = sk.entities;
  CHECK(SolvedDof(sk) == dof0);
  CHECK(SameGeometry(sk.entities, cut, 1e-9));
  // Every piece horizontal (each carries the H the line did).
  int h = 0;
  for (const auto& k : sk.constraints)
    h += k.kind == FK::Horizontal && k.b.entity == 0;
  CHECK(h == 3);
  sketch::SetConstruction(sk.entities[sk.IndexOfEntity(inner)], true);
  CHECK(SolvedDof(sk) == dof0);
  CHECK(SameGeometry(sk.entities, cut, 1e-9));
}

TEST_CASE("Split carries a midpoint as a symmetric of the outer ends",
          "[sketch][split]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {8, 0}});
  const uint32_t m = sk.AddEntity(sketch::Point{{4, 0}});
  sk.constraints.push_back({FK::Midpoint, {m, 0}, {l, 0}, 0.0});
  CheckSplit(sk, l, {{{2, 0}}}, 1);
  bool symmetric = false;
  for (const auto& k : sk.constraints) {
    CHECK(k.kind != FK::Midpoint);
    if (k.kind == FK::Symmetric && k.c.entity == m) {
      symmetric = true;
      CHECK(k.a.entity == l);
      CHECK(k.a.which == 0);
      CHECK(k.b.which == 1);
      CHECK(k.b.entity != l);
    }
  }
  CHECK(symmetric);
  // Still the middle: drag the far end out and the point follows.
  const size_t far = sk.IndexOfEntity(sk.constraints.back().b.entity);
  std::get<sketch::Line>(sk.entities[far]).b = {12, 0};
  SolvedDof(sk);
  const auto& p = std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(m)]);
  const auto& a = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l)]);
  const auto& b = std::get<sketch::Line>(sk.entities[far]);
  CHECK(glm::length(p.p - 0.5 * (a.a + b.b)) < 1e-9);
}

TEST_CASE("Split at another curve's end joins it there, in place of the "
          "point-on-line",
          "[sketch][split]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t bar = sk.AddEntity(sketch::Line{{0, 0}, {10, 0}});
  const uint32_t stem = sk.AddEntity(sketch::Line{{4, 0}, {4, 5}});
  sk.constraints.push_back({FK::PointOnLine, {stem, 0}, {bar, 0}, 0.0});
  // Offered as a crossing too: the end wins.
  CheckSplit(sk, bar, {{{4, 0}, stem}}, 0);
  int onLine = 0;
  bool joined = false;
  for (const auto& k : sk.constraints) {
    onLine += k.kind == FK::PointOnLine;
    joined |= k.kind == FK::Coincident && k.b.entity == stem &&
              k.b.which == 0 && k.a.entity == bar && k.a.which == 1;
  }
  CHECK(onLine == 0);
  CHECK(joined);
}

TEST_CASE("Split moves a tangent joined at the far end to piece 2",
          "[sketch][split]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {10, 0}});
  const uint32_t a = sk.AddEntity(sketch::Arc{{10, 3}, 3.0, -M_PI / 2, 0.0});
  Join(sk, l, 1, a, 1);
  sk.constraints.push_back({FK::Tangent, {l, 0}, {a, 0}, 0.0});
  CheckSplit(sk, l, {{{4, 0}}}, 1);
  for (const auto& k : sk.constraints)
    if (k.kind == FK::Tangent)
      CHECK((k.a.entity != l && k.b.entity == a));
}

TEST_CASE("Split refuses a generated curve", "[sketch][split]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  const auto copies = sk.AddLinearPattern({l}, {0, 5}, 2, true);
  REQUIRE(copies.size() == 1);
  const uint32_t proj = sk.AddEntity(sketch::Line{{0, 10}, {4, 10}});
  sketch::ExternalGeom ex;
  ex.ref = ElementRef{7, "2/cap0"};
  ex.genIds = {proj};
  sk.external.push_back(ex);
  const nlohmann::json before = DocumentIO::FeatureToJson(sk);
  std::string why;
  CHECK(sk.SplitEntity(copies[0], {{{2, 5}}}, &why).empty());
  CHECK(why.find("is generated by a linked pattern") != std::string::npos);
  CHECK(sk.SplitEntity(proj, {{{2, 10}}}, &why).empty());
  CHECK(why.find("is generated by a projection") != std::string::npos);
  CHECK(DocumentIO::FeatureToJson(sk) == before);
  // The source splits, and its pattern repeats both pieces.
  const auto ids = sk.SplitEntity(l, {{{2, 0}}});
  REQUIRE(ids.size() == 2);
  CHECK(sk.derived[0].instances.size() == 2);
}

TEST_CASE("Split is one undo step, and undo restores the ids",
          "[sketch][split]") {
  Document doc;
  auto own = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t l = own->AddEntity(sketch::Line{{0, 0}, {6, 0}});
  doc.AddFeature(std::move(own));
  auto* sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  const nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  REQUIRE(sk->SplitEntity(l, {{{2, 0}}}).size() == 2);
  const nlohmann::json after = DocumentIO::FeatureToJson(*sk);
  CommandStack stack;
  stack.Execute(std::make_unique<ModifyFeatureCommand>(0, before, after), doc);
  sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  CHECK(sk->entities.size() == 2);
  stack.Undo(doc);
  sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  CHECK(DocumentIO::FeatureToJson(*sk) == before);
  CHECK(sk->IndexOfEntity(l) != SIZE_MAX);
  stack.Redo(doc);
  sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  CHECK(DocumentIO::FeatureToJson(*sk) == after);
}

TEST_CASE("A trimmed arc's far end keeps its coincident",
          "[sketch][split][trim][arc]") {
  // An arc from (5, 0) over the top to (-5, 0), its far end joined to a
  // line; two lines cross its middle, which is trimmed out.
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t a = sk.AddEntity(sketch::Arc{{0, 0}, 5.0, 0.0, M_PI});
  const uint32_t tail = sk.AddEntity(sketch::Line{{-5, 0}, {-5, -4}});
  const uint32_t head = sk.AddEntity(sketch::Line{{5, 0}, {5, -4}});
  Join(sk, a, 2, tail, 0);
  Join(sk, a, 1, head, 0);
  sk.AddEntity(sketch::Line{{-2, -1}, {-2, 8}});
  sk.AddEntity(sketch::Line{{2, -1}, {2, 8}});
  SolvedDof(sk);
  REQUIRE(sk.TrimCurveAt(a, {0, 5}));
  // Two arcs: the first keeps the id and the start's coincident, the second
  // took the far end's.
  uint32_t second = 0;
  for (const auto& k : sk.constraints)
    if (k.kind == FK::Coincident && k.b.entity == tail)
      second = k.a.entity;
  REQUIRE(second != 0);
  CHECK(second != a);
  REQUIRE(std::holds_alternative<sketch::Arc>(
      sk.entities[sk.IndexOfEntity(second)]));
  const std::vector<sketch::Entity> trimmed = sk.entities;
  SolvedDof(sk);
  CHECK(SameGeometry(sk.entities, trimmed, 1e-9));
  const auto& far =
      std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(second)]);
  CHECK(glm::length(sketch::ArcPointUV(far, 2) - glm::dvec2(-5, 0)) < 1e-9);
  const auto& near = std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(a)]);
  CHECK(glm::length(sketch::ArcPointUV(near, 1) - glm::dvec2(5, 0)) < 1e-9);
  // The survivors are not tied to each other.
  for (const auto& k : sk.constraints)
    CHECK(!(k.kind == FK::Concentric));
}

TEST_CASE("Trimming an arc's end drops that end's constraints",
          "[sketch][trim][arc]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t a = sk.AddEntity(sketch::Arc{{0, 0}, 5.0, 0.0, M_PI});
  const uint32_t tail = sk.AddEntity(sketch::Line{{-5, 0}, {-5, -4}});
  Join(sk, a, 2, tail, 0);
  sk.AddEntity(sketch::Line{{-2, -1}, {-2, 8}});
  SolvedDof(sk);
  REQUIRE(sk.TrimCurveAt(a, {-4, 3})); // the span from the crossing to the end
  for (const auto& k : sk.constraints)
    CHECK(!(k.kind == FK::Coincident &&
            (k.a.entity == tail || k.b.entity == tail)));
  const std::vector<sketch::Entity> trimmed = sk.entities;
  SolvedDof(sk);
  CHECK(SameGeometry(sk.entities, trimmed, 1e-9)); // nothing pulls it back
}

TEST_CASE("Crossings finds where any curve crosses a line, arc or circle",
          "[sketch][split]") {
  const sketch::Entity line = sketch::Line{{-10, 0}, {10, 0}};
  const auto near = [](const std::vector<glm::dvec2>& xs, glm::dvec2 p) {
    for (const glm::dvec2& x : xs)
      if (glm::length(x - p) < 1e-12)
        return true;
    return false;
  };
  const auto circle = sketch::Crossings(line, sketch::Circle{{0, 0}, 4.0});
  CHECK(circle.size() == 2);
  CHECK(near(circle, {4, 0}));
  CHECK(near(circle, {-4, 0}));
  CHECK(
      sketch::Crossings(line, sketch::Ellipse{{0, 0}, 5.0, 2.0, 0.0}).size() ==
      2);
  const auto spline = sketch::Crossings(
      line, sketch::BSpline{{{-6, -3}, {-2, 6}, {2, -6}, {6, 3}}});
  CHECK(spline.size() == 3);
  for (const glm::dvec2& x : spline)
    CHECK(std::abs(x.y) < 1e-12);
  // Off the arc's sweep, and past the line's end, no crossing.
  CHECK(sketch::Crossings(sketch::Arc{{0, 0}, 4.0, 0.0, M_PI},
                          sketch::Line{{-10, -1}, {10, -1}})
            .empty());
  CHECK(sketch::Crossings(line, sketch::Line{{20, -1}, {20, 1}}).empty());
}
TEST_CASE("A circle split at opposite points stays one circle",
          "[sketch][split][arc]") {
  // Cut where a line through the centre crosses it: the two arcs' joints
  // are a diameter apart, where an equal radius would let the second arc
  // flip over to the mirror circle.
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t c = sk.AddEntity(sketch::Circle{{0, 0}, 5.0});
  const uint32_t l = sk.AddEntity(sketch::Line{{-8, 0}, {8, 0}});
  CheckSplit(sk, c, {{{5, 0.1}, l}, {{-5, 0.1}, l}}, 0);
  // Pull the second arc's centre away and solve: the joints bring it back
  // onto the first arc's circle.
  uint32_t other = 0;
  for (const sketch::Entity& e : sk.entities)
    if (std::holds_alternative<sketch::Arc>(e) && sketch::EntityId(e) != c)
      other = sketch::EntityId(e);
  REQUIRE(other != 0);
  const auto ids = std::vector<uint32_t>{c, other};
  auto& second = std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(ids[1])]);
  second.center += glm::dvec2(0.3, 0.4);
  SolvedDof(sk);
  const auto& a1 = std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(c)]);
  const auto& a2 = std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(ids[1])]);
  CHECK(glm::length(a1.center - a2.center) < 1e-9);
  CHECK(std::abs(a1.radius - a2.radius) < 1e-9);
}
