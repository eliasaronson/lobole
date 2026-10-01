#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/features/HoleFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <filesystem>
#include <map>
#include <set>

using Catch::Matchers::WithinRel;

namespace {
// A sketch of points on a plane at height z (normal +Z).
uint64_t PointSketch(Document& doc, double z,
                     const std::vector<glm::dvec2>& pts) {
  SketchPlane plane(glm::dvec3(0, 0, z), glm::dvec3(0, 0, 1),
                    glm::dvec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Holes", plane);
  for (const auto& p : pts)
    sk->AddEntity(sketch::Point{p});
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}
} // namespace

TEST_CASE("Simple hole drills a bore of the expected volume", "[hole]") {
  Document doc;
  // Box 10x10x10 spanning z[0,10].
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));
  // One hole at the box center on the top face (z=10), drilling down.
  const uint64_t sk = PointSketch(doc, 10.0, {{5, 5}});
  auto ho = std::make_unique<HoleFeature>("H", sk, HoleKind::Simple, 2.0, 5.0);
  doc.AddFeature(std::move(ho));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(1000.0 - M_PI * 1.0 * 1.0 * 5.0, 1e-3));
}

TEST_CASE("Four holes from one sketch (a bolt pattern)", "[hole]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 4));
  const uint64_t sk = PointSketch(doc, 4.0, {{2, 2}, {8, 2}, {8, 8}, {2, 8}});
  // Through-all: each hole d=1 pierces the 4-thick plate.
  auto ho = std::make_unique<HoleFeature>("H", sk, HoleKind::Simple, 1.0, 1.0);
  ho->through = true;
  doc.AddFeature(std::move(ho));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(400.0 - 4.0 * M_PI * 0.25 * 4.0, 1e-2));
}

TEST_CASE("Counterbore removes more than a simple hole", "[hole]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));
  const uint64_t sk = PointSketch(doc, 10.0, {{5, 5}});
  auto ho =
      std::make_unique<HoleFeature>("H", sk, HoleKind::Counterbore, 2.0, 6.0);
  ho->cbDiameter = 4.0;
  ho->cbDepth = 2.0;
  doc.AddFeature(std::move(ho));
  REQUIRE(doc.GetBodies().size() == 1);
  // bore pi*1^2*6 + counterbore pi*2^2*2 (the top 2 of the bore overlaps the
  // cbore, so total removed = pi*1^2*6 + pi*2^2*2 - pi*1^2*2).
  const double removed = M_PI * 1 * 1 * 6 + M_PI * 2 * 2 * 2 - M_PI * 1 * 1 * 2;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(1000.0 - removed, 1e-2));
}

TEST_CASE("A hole names its faces by role and by point", "[hole][toponaming]") {
  // Named by position, every hole's faces interleaved (7/face0..n), nothing
  // could ask for "the bores", and adding a point renamed the others.
  const auto build = [](const std::vector<glm::dvec2>& pts) {
    auto doc = std::make_unique<Document>();
    doc->AddFeature(
        std::make_unique<BoxFeature>("B", glm::vec3(0), 40, 10, 10));
    const uint64_t sk = PointSketch(*doc, 10.0, pts);
    auto ho =
        std::make_unique<HoleFeature>("H", sk, HoleKind::Counterbore, 3.0, 6.0);
    ho->cbDiameter = 6.0;
    ho->cbDepth = 2.0;
    doc->AddFeature(std::move(ho));
    return doc;
  };
  auto doc = build({{5, 5}, {20, 5}, {35, 5}});
  REQUIRE(doc->GetBodies().size() == 1);
  const uint64_t hole = doc->GetFeature(2)->GetId();
  std::map<std::string, int> roles;
  std::set<std::string> mine;
  for (const auto& [i, name] : doc->GetBodies()[0].elementMap.names) {
    const topo::ElementOrigin o = topo::DecodeElementName(name);
    REQUIRE(o.ok);
    if (o.madeBy != hole)
      continue;
    roles[topo::ElementRoleName(o.role)]++;
    mine.insert(name.substr(0, name.find(';')));
  }
  CHECK(roles["bore"] == 3);
  CHECK(roles["bottom"] == 3);
  CHECK(roles["counterbore"] == 3);
  CHECK(roles["counterbore_floor"] == 3);
  CHECK(roles.size() == 4); // nothing positional

  // Drop the middle point: the other two holes keep every name.
  auto* sk = dynamic_cast<SketchFeature*>(doc->GetFeature(1));
  REQUIRE(sk != nullptr);
  const uint32_t middle = sketch::EntityId(sk->entities[1]);
  sk->entities.erase(sk->entities.begin() + 1);
  REQUIRE(doc->Regenerate());
  std::set<std::string> kept;
  for (const std::string& n : mine)
    if (n.find("/p" + std::to_string(middle) + "/") == std::string::npos)
      kept.insert(n);
  CHECK(kept.size() == 8);
  std::set<std::string> after;
  for (const auto& [i, name] : doc->GetBodies()[0].elementMap.names)
    if (topo::DecodeElementName(name).madeBy == hole)
      after.insert(name.substr(0, name.find(';')));
  CHECK(after == kept);
}

TEST_CASE("Hole errors and round-trips (v21)", "[hole]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<HoleFeature>("Bad", 999, HoleKind::Simple, 2.0, 5.0));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(0)->GetError().empty());

  Document doc2;
  doc2.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 8, 8, 8));
  const uint64_t sk = PointSketch(doc2, 8.0, {{4, 4}});
  auto ho =
      std::make_unique<HoleFeature>("H", sk, HoleKind::Countersink, 2.0, 4.0);
  ho->csDiameter = 4.0;
  ho->csDepth = 1.0;
  doc2.AddFeature(std::move(ho));
  const double v = doc2.GetBodies()[0].shape.Volume();

  const auto path = std::filesystem::temp_directory_path() / "hole_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc2, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<HoleFeature*>(loaded->GetFeature(2));
  REQUIRE(rt != nullptr);
  CHECK(rt->kind == HoleKind::Countersink);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(v, 1e-9));
  std::filesystem::remove(path);
}

// Fail-loud (dogfooding #3): a hole whose `diameterExpr` references a variable
// that resolves in no scope must stamp a feature error, NOT silently fall back
// to the literal twin and quietly drill the 1 mm default. Phase V's expression
// refresh surfaces the unresolved reference; this pins that the hole path is
// covered so a bad bore can never pass unnoticed again.
TEST_CASE("a hole centre over nothing is reported", "[hole]") {
  // The cut still removes material, so the body-mode "removed nothing"
  // warning stays quiet and the feature reads as clean — which is how a
  // pattern whose centres all landed 7.6 mm off the part survived two
  // rounds of "it built fine" (the SO-101 gripper's flange).
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0.0f), 20.0, 20.0, 5.0));
  SketchPlane plane(glm::vec3(0, 0, 5), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Centres", plane);
  sk->AddEntity(sketch::Point{{5.0f, 5.0f}});  // in the plate
  sk->AddEntity(sketch::Point{{60.0f, 5.0f}}); // in thin air
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(1)->GetId();
  doc.AddFeature(
      std::make_unique<HoleFeature>("Drill", skId, HoleKind::Simple, 3.0, 6.0));
  REQUIRE(doc.Regenerate());
  const Feature* hole = doc.GetFeature(2);
  CHECK(hole->GetError().empty()); // it did cut something
  CHECK_THAT(hole->GetWarning(),
             Catch::Matchers::ContainsSubstring("1 of 2 hole centres"));
  CHECK_THAT(hole->GetWarning(), Catch::Matchers::ContainsSubstring("60.00"));

  // Both over material: silent.
  auto* centres = dynamic_cast<SketchFeature*>(doc.GetFeature(1));
  REQUIRE(centres != nullptr);
  centres->entities.clear();
  centres->AddEntity(sketch::Point{{5.0f, 5.0f}});
  centres->AddEntity(sketch::Point{{12.0f, 12.0f}});
  doc.ClearRegenCache(); // the sketch was edited in place
  REQUIRE(doc.Regenerate());
  CHECK(doc.GetFeature(2)->GetWarning().empty());
}

TEST_CASE("a through hole in a thin plate is not reported as air", "[hole]") {
  // A through bore is drilled a bounding-box DIAGONAL long, so sampling it
  // over its own length starts well past a thin plate and reports air for a
  // hole that plainly cut: a 108 x 88 x 3 carbon plate drilled twelve
  // correct holes and warned about all twelve. The samples belong in the
  // span where material can be, which is the bore clipped to the bodies.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>(
      "Plate", glm::vec3(-54.0f, -44.0f, 0.0f), 108.0, 88.0, 3.0));
  SketchPlane plane(glm::vec3(0, 0, 3), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Centres", plane);
  sk->AddEntity(sketch::Point{{36.77f, 36.77f}}); // on the plate
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(1)->GetId();
  auto ho =
      std::make_unique<HoleFeature>("Drill", skId, HoleKind::Simple, 3.2, 0.0);
  ho->through = true;
  doc.AddFeature(std::move(ho));
  REQUIRE(doc.Regenerate());
  CHECK(doc.GetFeature(2)->GetError().empty());
  CHECK(doc.GetFeature(2)->GetWarning().empty());

  // A centre genuinely off the plate is still reported — the clip narrows
  // where it looks, it does not stop it looking.
  auto* centres = dynamic_cast<SketchFeature*>(doc.GetFeature(1));
  REQUIRE(centres != nullptr);
  centres->AddEntity(sketch::Point{{80.0f, 0.0f}});
  doc.ClearRegenCache();
  REQUIRE(doc.Regenerate());
  CHECK_THAT(doc.GetFeature(2)->GetWarning(),
             Catch::Matchers::ContainsSubstring("1 of 2 hole centres"));
}

TEST_CASE("a through hole in a thin ledge far above the rest is not air",
          "[hole]") {
  // The span is clipped to every body's box, and a ledge 12.8 thick 160 mm
  // up a yoke sat between the first two of nine samples spread over that
  // span: three holes that cut the ledge were reported as drilling air. The
  // axis is now split where it crosses each body's faces, so a span inside
  // material is found however thin it is or wherever it lies.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>(
      "Base", glm::vec3(-50.0f, -50.0f, 0.0f), 100.0, 100.0, 10.0));
  doc.AddFeature(std::make_unique<BoxFeature>(
      "Ledge", glm::vec3(60.0f, 0.0f, 150.0f), 20.0, 20.0, 1.5));
  SketchPlane plane(glm::vec3(0, 0, 151.5f), glm::vec3(0, 0, 1),
                    glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Centres", plane);
  sk->AddEntity(sketch::Point{{70.0f, 10.0f}}); // on the ledge only
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(2)->GetId();
  auto ho =
      std::make_unique<HoleFeature>("Drill", skId, HoleKind::Simple, 3.0, 0.0);
  ho->through = true;
  doc.AddFeature(std::move(ho));
  REQUIRE(doc.Regenerate());
  CHECK(doc.GetFeature(3)->GetError().empty());
  CHECK(doc.GetFeature(3)->GetWarning().empty());

  // A centre over neither body is still reported.
  auto* centres = dynamic_cast<SketchFeature*>(doc.GetFeature(2));
  REQUIRE(centres != nullptr);
  centres->AddEntity(sketch::Point{{90.0f, 40.0f}});
  doc.ClearRegenCache();
  REQUIRE(doc.Regenerate());
  CHECK_THAT(doc.GetFeature(3)->GetWarning(),
             Catch::Matchers::ContainsSubstring("1 of 2 hole centres"));
}

TEST_CASE("a counterbore drilled from a face keeps no skin over it", "[hole]") {
  // A counterbore drilled from the plane of the face it opens, at heights
  // that are not binary fractions. While a primitive's position was float, a
  // box at z = 24.7 sat at 24.70000076 and the counterbore kept a sealed
  // skin over itself; placed in double, the face is where the plane is
  // (docs/conventions.md#the-model-is-double). The box's BOTTOM face is the
  // one at the typed height, and the hole is drilled up into it.
  for (const double t : {0.3, 13.4, 24.7, 1e3 + 0.1, -0.7, -13.4}) {
    CAPTURE(t);
    Document doc;
    doc.AddFeature(std::make_unique<BoxFeature>("B", glm::dvec3(0, 0, t), 20.0,
                                                20.0, 13.4));
    // Facing down, V is -Y.
    auto pts = std::make_unique<SketchFeature>(
        "Holes", SketchPlane(glm::dvec3(0, 0, t), glm::dvec3(0, 0, -1),
                             glm::dvec3(1, 0, 0)));
    pts->AddEntity(sketch::Point{{10, -10}});
    doc.AddFeature(std::move(pts));
    const uint64_t sk = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
    auto ho =
        std::make_unique<HoleFeature>("H", sk, HoleKind::Counterbore, 4.5, 0.0);
    ho->through = true;
    ho->cbDiameter = 8.0;
    ho->cbDepth = 4.5;
    doc.AddFeature(std::move(ho));
    REQUIRE(doc.Regenerate());
    REQUIRE(doc.GetBodies().size() == 1);
    // Nothing looks up into the part at the bottom: no skin over the
    // counterbore.
    int lids = 0;
    for (TopExp_Explorer ex(doc.GetBodies()[0].shape.GetShape(), TopAbs_FACE);
         ex.More(); ex.Next()) {
      const TopoDS_Face f = TopoDS::Face(ex.Current());
      BRepAdaptor_Surface s(f);
      if (s.GetType() != GeomAbs_Plane)
        continue;
      gp_Dir n = s.Plane().Axis().Direction();
      if (f.Orientation() == TopAbs_REVERSED)
        n.Reverse();
      if (n.Z() > 0.5 && std::abs(s.Plane().Location().Z() - t) < 1e-3)
        lids++;
    }
    CHECK(lids == 0);
    const double removed =
        M_PI * 2.25 * 2.25 * (13.4 - 4.5) + M_PI * 16.0 * 4.5;
    CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
               WithinRel(20.0 * 20.0 * 13.4 - removed, 1e-6));
  }
}

TEST_CASE("Hole with an unresolved diameter_expr errors", "[hole]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));
  const uint64_t sk = PointSketch(doc, 10.0, {{5, 5}});
  // No `bore` variable is defined anywhere -> the expr can't resolve.
  auto ho = std::make_unique<HoleFeature>("H", sk, HoleKind::Simple, 1.0, 5.0);
  ho->diameterExpr = "bore";
  doc.AddFeature(std::move(ho));

  CHECK_FALSE(doc.Regenerate());
  const Feature* hole = doc.GetFeature(2);
  REQUIRE(hole->TypeName() == "Hole");
  CHECK_FALSE(hole->GetError().empty());
  CHECK(hole->GetError().find("bore") != std::string::npos);
}

// A blind hole drilled for a thread is modelled as drilled: its floor is a
// jobber drill's 118-degree point, and it is threaded short of that floor by
// a tap's lead (docs/modeling/holes-and-threads.md#hole).
TEST_CASE("a blind tapped hole has a drill point and a thread short of it",
          "[hole]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 20, 20, 20));
  const uint64_t sk = PointSketch(doc, 20.0, {{10, 10}});
  auto ho =
      std::make_unique<HoleFeature>("M6", sk, HoleKind::Simple, 1.0, 12.0);
  ho->fitThread = "M6";
  ho->fitKind = "tapped";
  doc.AddFeature(std::move(ho));
  REQUIRE(doc.Regenerate());
  const auto* hole = dynamic_cast<const HoleFeature*>(doc.GetFeature(2));
  REQUIRE(hole != nullptr);
  CHECK(hole->PointAngleUsed() == 118.0);
  // M6 coarse is 1 mm a pitch: 12 less three pitches of the tap's lead.
  CHECK_THAT(hole->ThreadDepthUsed(), WithinRel(9.0, 1e-9));
  const Body& body = doc.GetBodies()[0];
  // The cone: 5 mm across (M6's tap drill), 118 degrees, under 12 mm of bore.
  const double r = 2.5, cone = r / std::tan(59.0 * M_PI / 180.0);
  CHECK_THAT(body.shape.Volume(),
             WithinRel(8000.0 - M_PI * r * r * 12.0 - M_PI * r * r * cone / 3.0,
                       1e-6));
  bool point = false;
  for (const auto& [index, name] : body.elementMap.names) {
    const topo::ElementOrigin o = topo::DecodeElementName(name);
    point = point || (o.ok && o.role == topo::ElementRole::DrillPoint);
  }
  CHECK(point);
  REQUIRE(body.holes.size() == 1);
  CHECK(body.holes[0].pointAngleDeg == 118.0);
  CHECK(body.holes[0].depth == 12.0);
  CHECK(body.holes[0].thread == "M6");
  CHECK(body.holes[0].feature == "M6");
  REQUIRE(body.threads.size() == 1);
  CHECK_THAT(body.threads[0].length, WithinRel(9.0, 1e-9));
  REQUIRE(body.threadRegions.size() == 1);
  CHECK_THAT(body.threadRegions[0].length, WithinRel(9.0, 1e-9));
}

TEST_CASE("a hole's point and thread depth are stated, refused and saved",
          "[hole]") {
  const auto build = [](auto&& edit) {
    auto doc = std::make_unique<Document>();
    doc->AddFeature(
        std::make_unique<BoxFeature>("B", glm::vec3(0), 20, 20, 20));
    const uint64_t sk = PointSketch(*doc, 20.0, {{10, 10}});
    auto ho =
        std::make_unique<HoleFeature>("H", sk, HoleKind::Simple, 5.0, 12.0);
    edit(*ho);
    doc->AddFeature(std::move(ho));
    doc->Regenerate();
    return doc;
  };
  // A plain diameter says nothing about how it is made: a flat floor.
  {
    auto doc = build([](HoleFeature&) {});
    CHECK(doc->GetBodies()[0].holes.at(0).pointAngleDeg == 180.0);
    CHECK_THAT(doc->GetBodies()[0].shape.Volume(),
               WithinRel(8000.0 - M_PI * 6.25 * 12.0, 1e-6));
  }
  // Stated: a 140 degree point, and an M6 threaded 10 deep.
  auto stated = build([](HoleFeature& h) {
    h.pointAngleDeg = 140.0;
    h.fitThread = "M6";
    h.fitKind = "tapped";
    h.threadDepth = 10.0;
  });
  REQUIRE(stated->GetFeature(2)->GetError().empty());
  CHECK(stated->GetBodies()[0].holes.at(0).pointAngleDeg == 140.0);
  CHECK(stated->GetBodies()[0].threads.at(0).length == 10.0);
  const std::string path =
      (std::filesystem::temp_directory_path() / "hole-point-depth.dsgn")
          .string();
  REQUIRE(testio::Save(*stated, path));
  const auto loaded = testio::Load(path);
  REQUIRE(loaded != nullptr);
  const auto* back = dynamic_cast<const HoleFeature*>(loaded->GetFeature(2));
  REQUIRE(back != nullptr);
  CHECK(back->pointAngleDeg == 140.0);
  CHECK(back->threadDepth == 10.0);
  // Refused, never read as something else.
  CHECK_THAT(build([](HoleFeature& h) { h.threadDepth = 5.0; })
                 ->GetFeature(2)
                 ->GetError(),
             Catch::Matchers::ContainsSubstring("TAPPED"));
  CHECK_THAT(build([](HoleFeature& h) { h.pointAngleDeg = 10.0; })
                 ->GetFeature(2)
                 ->GetError(),
             Catch::Matchers::ContainsSubstring("30 to 180"));
  CHECK_THAT(build([](HoleFeature& h) {
               h.fitThread = "M6";
               h.fitKind = "tapped";
               h.threadDepth = 13.0;
             })
                 ->GetFeature(2)
                 ->GetError(),
             Catch::Matchers::ContainsSubstring("no deeper"));
  // A through hole is threaded through, whatever its point.
  auto through = build([](HoleFeature& h) {
    h.fitThread = "M6";
    h.fitKind = "tapped";
    h.through = true;
  });
  CHECK(through->GetBodies()[0].threads.at(0).length == 0.0);
  CHECK(through->GetBodies()[0].holes.at(0).through);
}
