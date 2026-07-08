#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <limits>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

static std::unique_ptr<BoxFeature> MakeBox(const std::string& name) {
  return std::make_unique<BoxFeature>(name, glm::vec3(0), 2.0, 2.0, 2.0);
}

TEST_CASE("MirrorEntities reflects geometry across an axis line", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t axis = sk.AddEntity(sketch::Line{{0, -5}, {0, 5}}); // the y-axis
  uint32_t seg = sk.AddEntity(sketch::Line{{2, 1}, {3, 4}});    // to the right

  SECTION("bake: unlinked copy, no constraints") {
    auto gen = sk.MirrorEntities({seg}, axis, /*link=*/false);
    REQUIRE(gen.size() == 1);
    const auto& copy = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(gen[0])]);
    CHECK_THAT(copy.a.x, WithinAbs(-2.0f, 1e-5f));
    CHECK_THAT(copy.a.y, WithinAbs(1.0f, 1e-5f));
    CHECK_THAT(copy.b.x, WithinAbs(-3.0f, 1e-5f));
    CHECK_THAT(copy.b.y, WithinAbs(4.0f, 1e-5f));
    CHECK(sk.constraints.empty()); // bake adds none
  }
  SECTION("linked: adds Symmetric constraints tying copy to source") {
    auto gen = sk.MirrorEntities({seg}, axis, /*link=*/true);
    REQUIRE(gen.size() == 1);
    int sym = 0;
    for (const auto& c : sk.constraints)
      if (c.kind == sketch::ConstraintKind::Symmetric) {
        sym++;
        CHECK(c.c.entity == axis); // symmetry about the axis line
      }
    CHECK(sym == 2); // both endpoints
  }
}

TEST_CASE("MirrorEntities reflects an arc keeping it CCW", "[sketch][arc]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t axis = sk.AddEntity(sketch::Line{{0, -5}, {0, 5}});
  // Arc on the right, CCW from 0 to pi/2.
  uint32_t a0 = sk.AddEntity(sketch::Arc{{2, 0}, 1.0, 0.0, M_PI / 2});
  auto gen = sk.MirrorEntities({a0}, axis, false);
  REQUIRE(gen.size() == 1);
  const auto& arc = std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(gen[0])]);
  CHECK_THAT(arc.center.x, WithinAbs(-2.0f, 1e-5f));
  CHECK(arc.endAngle > arc.startAngle); // still CCW-always
  // Endpoints are the reflections of the source's (winding swapped).
  glm::vec2 s = sketch::ArcPointUV(arc, 1), e = sketch::ArcPointUV(arc, 2);
  CHECK_THAT(glm::length(s - glm::vec2(-2, 1)), WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(glm::length(e - glm::vec2(-3, 0)), WithinAbs(0.0f, 1e-4f));
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
  CHECK_THAT(arc.center.x, WithinAbs(1.0f, 1e-4f));
  CHECK_THAT(arc.center.y, WithinAbs(1.0f, 1e-4f));
  // Lines trimmed to the tangent points (r along each from the corner).
  const auto& s1 = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l1)]);
  const auto& s2 = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l2)]);
  CHECK_THAT(glm::length(s1.b - glm::vec2(1, 0)), WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(glm::length(s2.a - glm::vec2(0, 1)), WithinAbs(0.0f, 1e-4f));
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
  const auto& c1 = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(gen[0])]);
  const auto& c2 = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(gen[1])]);
  CHECK_THAT(c1.a.x, WithinAbs(5.0f, 1e-5f));
  CHECK_THAT(c2.a.x, WithinAbs(10.0f, 1e-5f));
}

TEST_CASE("Circular pattern bakes rotated copies", "[sketch][pattern]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t p = sk.AddEntity(sketch::Point{{1, 0}});
  auto gen = sk.AddCircularPattern({p}, {0, 0}, M_PI / 2, /*count=*/4, false);
  REQUIRE(gen.size() == 3); // 90, 180, 270 degrees
  const auto& q1 = std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(gen[0])]);
  CHECK_THAT(q1.p.x, WithinAbs(0.0f, 1e-5f));
  CHECK_THAT(q1.p.y, WithinAbs(1.0f, 1e-5f)); // (1,0) rotated 90 → (0,1)
  const auto& q2 = std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(gen[1])]);
  CHECK_THAT(q2.p.x, WithinAbs(-1.0f, 1e-5f)); // 180 → (-1,0)
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
  const auto& g0 = std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(gen[0])]);
  const auto& g1 = std::get<sketch::Point>(sk.entities[sk.IndexOfEntity(gen[1])]);
  CHECK_THAT(g0.p.y, WithinAbs(5.0f, 1e-5f)); // followed the source in y
  CHECK_THAT(g0.p.x, WithinAbs(2.0f, 1e-5f));
  CHECK_THAT(g1.p.x, WithinAbs(4.0f, 1e-5f));
}

TEST_CASE("Changing a linked pattern's count is id-stable", "[sketch][pattern]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t p = sk.AddEntity(sketch::Point{{0, 0}});
  auto gen = sk.AddLinearPattern({p}, {2, 0}, 3, /*link=*/true);
  REQUIRE(gen.size() == 2);
  const uint32_t inst1 = gen[0]; // the k=1 instance

  // Shrink to count 2: the k=2 instance is removed, k=1 keeps its id.
  sk.derived[0].count = 2;
  sk.RegenerateDerived();
  CHECK(sk.entities.size() == 2); // source + one instance
  CHECK(sk.IndexOfEntity(inst1) != SIZE_MAX);       // k=1 survived with its id
  CHECK(sk.IndexOfEntity(gen[1]) == SIZE_MAX);       // k=2 deleted

  // Grow to count 4: k=1 keeps its id; k=2,3 are (re)created.
  sk.derived[0].count = 4;
  sk.RegenerateDerived();
  CHECK(sk.entities.size() == 4); // source + three instances
  CHECK(sk.IndexOfEntity(inst1) != SIZE_MAX);       // k=1 id preserved throughout
  CHECK(sk.derived[0].instances.size() == 3);
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

TEST_CASE("FilletCorner rejects bad inputs", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t l1 = sk.AddEntity(sketch::Line{{4, 0}, {0, 0}});
  uint32_t l2 = sk.AddEntity(sketch::Line{{0, 0}, {0, 4}});
  uint32_t l3 = sk.AddEntity(sketch::Line{{10, 0}, {14, 0}}); // no shared corner
  CHECK(sk.FilletCorner(l1, l3, 1.0) == 0);  // no shared corner
  CHECK(sk.FilletCorner(l1, l2, 100.0) == 0); // radius too large
  CHECK(sk.FilletCorner(l1, l2, 0.0) == 0);   // non-positive radius
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
  auto doc = DocumentIO::Load(path.string(), &error);
  REQUIRE(doc != nullptr);
  CHECK(doc->GetFeature(0)->GetId() != 0);
  fs::remove(path);
}

TEST_CASE("SketchFeature JSON round-trip with all entity types", "[sketch]") {
  SketchPlane plane(glm::vec3(1, 2, 3), glm::vec3(0, 1, 1),
                    glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Sketch 1", plane);
  sk->AddEntity(sketch::Point{{1.0f, 2.0f}});
  sk->AddEntity(sketch::Line{{0.0f, 0.0f}, {4.0f, 0.0f}});
  sk->AddEntity(sketch::Circle{{2.0f, 1.0f}, 0.5});
  sk->AddEntity(sketch::Arc{{1.0f, 1.0f}, 2.0, 0.25, 1.75});
  sk->SetId(7);

  auto j = DocumentIO::FeatureToJson(*sk);
  auto restored = DocumentIO::FeatureFromJson(j);
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);

  CHECK(rsk->GetId() == 7);
  CHECK(rsk->entities.size() == 4);
  CHECK(DocumentIO::FeatureToJson(*rsk) == j);

  // The arc survives with its angles intact.
  REQUIRE(std::holds_alternative<sketch::Arc>(rsk->entities[3]));
  const auto& arc = std::get<sketch::Arc>(rsk->entities[3]);
  CHECK_THAT(arc.radius, WithinAbs(2.0, 1e-6));
  CHECK_THAT(arc.startAngle, WithinAbs(0.25, 1e-6));
  CHECK_THAT(arc.endAngle, WithinAbs(1.75, 1e-6));

  // Plane axes reconstructed orthonormally
  CHECK_THAT(glm::length(rsk->plane.GetNormal()), WithinAbs(1.0f, 1e-4f));
  CHECK_THAT(glm::dot(rsk->plane.GetNormal(), rsk->plane.GetUAxis()),
             WithinAbs(0.0f, 1e-4f));
  glm::vec3 world = rsk->plane.PlaneToWorld({1.0f, 1.0f});
  glm::vec3 expected = sk->plane.PlaneToWorld({1.0f, 1.0f});
  CHECK_THAT(glm::length(world - expected), WithinAbs(0.0f, 1e-4f));

  // A sketch contributes no bodies
  Document doc;
  doc.AddFeature(std::move(restored));
  doc.AddFeature(std::make_unique<BoxFeature>("Box", glm::vec3(0), 1, 1, 1));
  REQUIRE(doc.Regenerate());
  CHECK(doc.GetBodies().size() == 1);
}

TEST_CASE("ArcFromThreePoints builds a CCW arc through the middle point",
          "[sketch][arc]") {
  // Unit circle: start (1,0), through top (0,1), end (-1,0) → upper semicircle.
  auto arc = sketch::ArcFromThreePoints({1, 0}, {0, 1}, {-1, 0});
  REQUIRE(arc.has_value());
  CHECK_THAT(arc->center.x, WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(arc->center.y, WithinAbs(0.0f, 1e-4f));
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

TEST_CASE("Sketch constraints round-trip through JSON", "[sketch][serialization]") {
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
  sk->constraints.push_back(
      {sketch::ConstraintKind::Angle, {line0, 0}, {line1, 0}, glm::radians(90.0)});
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
  CHECK(sk->nextEntityId == 3u);
  // Re-saving now carries per-entity ids (v4 shape).
  auto j2 = DocumentIO::FeatureToJson(*sk);
  CHECK(j2["entities"][0]["id"] == 1u);
  CHECK(j2["next_entity_id"] == 3u);
}

TEST_CASE("Dimension label offset round-trips", "[sketch][serialization]") {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  uint32_t l0 = sk->AddEntity(sketch::Line{{0, 0}, {4, 0}});
  sketch::Constraint dim{sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 4.0};
  dim.labelOffset = {0.5f, 1.2f};
  sk->constraints.push_back(dim);

  auto restored = DocumentIO::FeatureFromJson(DocumentIO::FeatureToJson(*sk));
  auto* rsk = dynamic_cast<SketchFeature*>(restored.get());
  REQUIRE(rsk != nullptr);
  CHECK(rsk->constraints[0].labelOffset.x == 0.5f);
  CHECK(rsk->constraints[0].labelOffset.y == 1.2f);
}

TEST_CASE("Construction flag round-trips through JSON", "[sketch][serialization]") {
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
       {{{"kind", "coincident"}, {"a", {1, 1}}, {"b", {2, 0}}, {"value", 0.0}}}}};

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
  uint32_t l1 = sk.AddEntity(sketch::Line{{4, 0}, {4, 3}}); // meets l0's far end
  sk.constraints.push_back({K::Coincident, {l0, 1}, {l1, 0}, 0.0});
  sk.constraints.push_back({K::Horizontal, {l0, 0}, {0, 0}, 0.0});
  sk.constraints.push_back({K::Distance, {l0, 0}, {l0, 1}, 4.0}); // whole length

  uint32_t l2 = sk.SplitEntity(l0, {2, 0});
  REQUIRE(l2 != 0);
  REQUIRE(sk.entities.size() == 3);

  auto& p1 = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l0)]);
  auto& p2 = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l2)]);
  CHECK(p1.a == glm::vec2(0, 0));
  CHECK(p1.b == glm::vec2(2, 0)); // piece 1: a -> P
  CHECK(p2.a == glm::vec2(2, 0));
  CHECK(p2.b == glm::vec2(4, 0)); // piece 2: P -> original end

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
  CHECK(horizontal == 2); // original on l0 + copy on l2
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

  // Click on the middle span [2,4]; it should be removed, leaving [0,2] + [4,6].
  REQUIRE(sk.TrimLineAt(l0, {3, 0}));
  REQUIRE(sk.entities.size() == 4); // 2 crossers + 2 remaining pieces

  bool left = false, right = false;
  for (const auto& e : sk.entities) {
    if (auto* l = std::get_if<sketch::Line>(&e)) {
      auto near = [](glm::vec2 p, glm::vec2 q) {
        return glm::length(p - q) < 1e-4f;
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
  auto near = [](glm::vec2 p, glm::vec2 q) { return glm::length(p - q) < 1e-4f; };
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
  CHECK(arc.endAngle > arc.startAngle);              // CCW-always
  double mid = 0.5 * (arc.startAngle + arc.endAngle);
  CHECK(std::sin(mid) < 0.0);                        // surviving span is the bottom
}

TEST_CASE("TrimCurveAt splits a crossed arc, dropping the clicked sub-span",
          "[sketch][arc]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  // Upper semicircle arc (0 → π), crossed by the y-axis at the top (0,1).
  uint32_t a0 = sk.AddEntity(sketch::Arc{{0, 0}, 1.0, 0.0, M_PI});
  sk.AddEntity(sketch::Line{{0, -2}, {0, 2}}); // crosses the arc at angle π/2

  // Click the right sub-span (angle ~π/4) → removed; left sub-span survives.
  glm::vec2 clickRight(std::cos(M_PI / 4), std::sin(M_PI / 4));
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

TEST_CASE("TrimLineAt with no crossings deletes the whole line", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t l0 = sk.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  sk.AddEntity(sketch::Line{{0, 2}, {4, 2}}); // parallel, never crosses
  REQUIRE(sk.TrimLineAt(l0, {2, 0}));
  CHECK(sk.entities.size() == 1); // only the parallel line remains
}

TEST_CASE("SplitEntity rejects a non-line or a degenerate cut", "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t c0 = sk.AddEntity(sketch::Circle{{0, 0}, 1.0});
  uint32_t l0 = sk.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  CHECK(sk.SplitEntity(c0, {1, 0}) == 0);       // not a line
  CHECK(sk.SplitEntity(l0, {0, 0}) == 0);       // at endpoint a
  CHECK(sk.SplitEntity(l0, {4, 0}) == 0);       // at endpoint b
  CHECK(sk.SplitEntity(999, {2, 0}) == 0);      // unknown id
  CHECK(sk.entities.size() == 2);               // nothing added
}

TEST_CASE("Entity ids stay stable across insert and delete",
          "[sketch]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  uint32_t a = sk.AddEntity(sketch::Line{{0, 0}, {1, 0}});
  uint32_t b = sk.AddEntity(sketch::Line{{1, 0}, {1, 1}});
  sk.constraints.push_back({sketch::ConstraintKind::Coincident, {a, 1}, {b, 0}});

  // Inserting a new entity shifts indices but not ids.
  uint32_t c = sk.AddEntity(sketch::Circle{{5, 5}, 1.0});
  CHECK(a == 1u);
  CHECK(b == 2u);
  CHECK(c == 3u);
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
