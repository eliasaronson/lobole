#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/ElementMap.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchProfile.h"

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <filesystem>

using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

static const SketchPlane kXY(PlaneType::XY);

// Rectangles are four lines now; build them at exact corners so BuildLoops
// chains them without needing a solve.
static std::vector<sketch::Entity> RectLines(glm::dvec2 c1, glm::dvec2 c2) {
  glm::dvec2 a{c1.x, c1.y}, b{c2.x, c1.y}, c{c2.x, c2.y}, d{c1.x, c2.y};
  return {sketch::Line{a, b}, sketch::Line{b, c}, sketch::Line{c, d},
          sketch::Line{d, a}};
}

// --- Geometry ---------------------------------------------------------------

TEST_CASE("SegmentIntersect finds crossings and rejects misses", "[profile]") {
  using Catch::Matchers::WithinAbs;
  // Crosses at the midpoint of segment 1.
  auto hit = sketch::SegmentIntersect({0, 0}, {2, 0}, {1, -1}, {1, 1});
  REQUIRE(hit.has_value());
  CHECK_THAT(*hit, WithinAbs(0.5, 1e-5));
  // Parallel: no intersection.
  CHECK_FALSE(sketch::SegmentIntersect({0, 0}, {2, 0}, {0, 1}, {2, 1}));
  // Lines would cross beyond segment 1's extent (x=3 > 2).
  CHECK_FALSE(sketch::SegmentIntersect({0, 0}, {2, 0}, {3, -1}, {3, 1}));
}

// --- BuildLoops -------------------------------------------------------------

TEST_CASE("Rectangle (as four lines) yields one loop", "[profile]") {
  auto loops = sketch::BuildLoops(RectLines({0, 0}, {4, 2}));
  REQUIRE(loops.size() == 1);
  CHECK_THAT(std::abs(loops[0].SignedArea()), WithinRel(8.0, 1e-9));
}

TEST_CASE("Construction geometry is excluded from the profile", "[profile]") {
  std::vector<sketch::Entity> e = RectLines({0, 0}, {4, 2});
  // A construction diagonal + a construction reference circle: ignored by
  // loops.
  sketch::Line diag{{0, 0}, {4, 2}};
  diag.construction = true;
  e.push_back(diag);
  sketch::Circle ref{{2, 1}, 0.5};
  ref.construction = true;
  e.push_back(ref);

  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 1); // just the rectangle; construction contributes 0
  CHECK_THAT(std::abs(loops[0].SignedArea()), WithinRel(8.0, 1e-9));

  // Marking a rectangle side construction opens the profile.
  std::get<sketch::Line>(e[0]).construction = true;
  CHECK(sketch::BuildLoops(e).empty());
}

TEST_CASE("Out-of-order lines with a flipped segment chain into one loop",
          "[profile]") {
  // A triangle given in scrambled order, one segment reversed
  std::vector<sketch::Entity> e = {
      sketch::Line{{0, 0}, {4, 0}},
      sketch::Line{{2, 3}, {0, 0}}, // reversed direction, closes back to start
      sketch::Line{{4, 0}, {2, 3}},
  };
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 1);
  CHECK_THAT(std::abs(loops[0].SignedArea()), WithinRel(6.0, 1e-4));
}

TEST_CASE("Open polyline yields no loops", "[profile]") {
  std::vector<sketch::Entity> e = {sketch::Line{{0, 0}, {4, 0}},
                                   sketch::Line{{4, 0}, {4, 2}}};
  CHECK(sketch::BuildLoops(e).empty());
}

// --- BuildPrism volumes -----------------------------------------------------

TEST_CASE("Rectangle extrudes to a box of the expected volume", "[extrude]") {
  auto loops = sketch::BuildLoops(RectLines({0, 0}, {4, 2}));
  CADShape prism = sketch::BuildPrism(kXY, loops, 3.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(24.0, 1e-6));
}

TEST_CASE("Circle extrudes to a cylinder volume", "[extrude]") {
  auto loops = sketch::BuildLoops({sketch::Circle{{0, 0}, 1.5}});
  CADShape prism = sketch::BuildPrism(kXY, loops, 2.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(M_PI * 1.5 * 1.5 * 2.0, 1e-3));
}

TEST_CASE("Ellipse extrudes to an elliptic-prism volume",
          "[extrude][ellipse]") {
  const double a = 2.0, b = 1.0;
  auto loops = sketch::BuildLoops({sketch::Ellipse{{0, 0}, a, b, 0.0}});
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(kXY, loops, 3.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(M_PI * a * b * 3.0, 1e-3));
}

TEST_CASE("A rotated ellipse extrudes to the same volume",
          "[extrude][ellipse]") {
  // Rotation leaves the enclosed area (π·a·b) unchanged.
  const double a = 2.0, b = 1.0;
  auto loops = sketch::BuildLoops({sketch::Ellipse{{0, 0}, a, b, 0.7}});
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(kXY, loops, 3.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(M_PI * a * b * 3.0, 1e-3));
}

TEST_CASE("A filleted-corner profile (lines + arc) extrudes",
          "[extrude][arc]") {
  // 4x2 rectangle with the top-right corner rounded by a quarter arc of r=0.5:
  // the arc runs CCW from (4,1.5) to (3.5,2), centred at (3.5,1.5).
  const double r = 0.5;
  std::vector<sketch::Entity> e;
  e.push_back(sketch::Line{{0.0, 0.0}, {4.0, 0.0}});
  e.push_back(sketch::Line{{4.0, 0.0}, {4.0, 1.5}});
  e.push_back(sketch::Arc{{3.5, 1.5}, r, 0.0, M_PI / 2});
  e.push_back(sketch::Line{{3.5, 2.0}, {0.0, 2.0}});
  e.push_back(sketch::Line{{0.0, 2.0}, {0.0, 0.0}});

  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(kXY, loops, 2.0, false);
  REQUIRE(prism.IsValid());
  // Area = rect - (corner square - quarter disk); volume = area * height.
  const double area = 8.0 - (r * r - M_PI * r * r / 4.0);
  CHECK_THAT(prism.Volume(), WithinRel(area * 2.0, 1e-3));
}

TEST_CASE("A half-disk (diameter line + semicircle arc) extrudes",
          "[extrude][arc]") {
  // Semicircle radius 1 centred at origin, closed by its diameter.
  std::vector<sketch::Entity> e;
  e.push_back(sketch::Line{{-1.0, 0.0}, {1.0, 0.0}});   // diameter
  e.push_back(sketch::Arc{{0.0, 0.0}, 1.0, 0.0, M_PI}); // upper semicircle
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(kXY, loops, 3.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(0.5 * M_PI * 1.0 * 1.0 * 3.0, 1e-3));
}

TEST_CASE("Rounded flanges extrude on translated oblique planes",
          "[extrude][arc]") {
  std::vector<sketch::Entity> edges{
      sketch::Line{{-24, -34}, {24, -34}},
      sketch::Arc{{24, -22}, 12, -M_PI / 2, 0},
      sketch::Line{{36, -22}, {36, 22}},
      sketch::Arc{{24, 22}, 12, 0, M_PI / 2},
      sketch::Line{{24, 34}, {-24, 34}},
      sketch::Arc{{-24, 22}, 12, M_PI / 2, M_PI},
      sketch::Line{{-36, 22}, {-36, -22}},
      sketch::Arc{{-24, -22}, 12, M_PI, 3 * M_PI / 2}};
  const auto loops = sketch::BuildLoops(edges);
  REQUIRE(loops.size() == 1);
  for (const glm::dvec3 origin :
       {glm::dvec3(202, 0, 212.4), glm::dvec3(10202, 500, 1212.4)}) {
    const SketchPlane plane(origin, {0.8, 0, -0.6}, {0, 1, 0});
    const CADShape prism = sketch::BuildPrism(plane, loops, 10, false);
    REQUIRE(prism.IsValid());
    const double area = 72 * 68 - (4 - M_PI) * 12 * 12;
    CHECK_THAT(prism.Volume(), WithinRel(area * 10, 1e-6));
  }
}

TEST_CASE("Offset expands a rectangle loop with rounded corners",
          "[extrude][offset]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  auto rect = sk.AddRectangle({0, 0}, {4, 2});
  std::vector<uint32_t> chain(rect.begin(), rect.end());
  const double d = 0.5;
  auto gen = sk.OffsetChain(chain, d, /*joinArc=*/true);
  REQUIRE(!gen.empty());
  // Extrude just the offset boundary (not the original rect).
  std::vector<sketch::Entity> offsetEnts;
  for (uint32_t id : gen)
    offsetEnts.push_back(sk.entities[sk.IndexOfEntity(id)]);
  auto loops = sketch::BuildLoops(offsetEnts);
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(kXY, loops, 1.0, false);
  REQUIRE(prism.IsValid());
  // Outward offset by d: area = rect + perimeter*d + a full disc of radius d
  // (the four quarter-arc corners), height 1.
  const double area = 8.0 + 2.0 * d * (4.0 + 2.0) + M_PI * d * d;
  CHECK_THAT(prism.Volume(), WithinRel(area, 1e-2));
}

TEST_CASE("Rectangle with an inner circle extrudes with a hole", "[extrude]") {
  std::vector<sketch::Entity> e = RectLines({0, 0}, {4, 2});
  e.push_back(sketch::Circle{{2, 1}, 0.5});
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 2);
  CADShape prism = sketch::BuildPrism(kXY, loops, 3.0, false);
  REQUIRE(prism.IsValid());
  const double expected = (8.0 - M_PI * 0.25) * 3.0;
  CHECK_THAT(prism.Volume(), WithinRel(expected, 1e-3));
}

TEST_CASE("Reversed extrude sits on the opposite side of the plane",
          "[extrude]") {
  auto loops = sketch::BuildLoops(RectLines({0, 0}, {2, 2}));
  CADShape prism = sketch::BuildPrism(kXY, loops, 2.0, true);
  REQUIRE(prism.IsValid());
  glm::dvec3 min, max;
  prism.GetBoundingBox(min, max);
  CHECK(max.z <= 1e-6); // extruded toward -Z
  CHECK(min.z <= -2.0 + 1e-6);
}

TEST_CASE("Offset plane places the prism where expected", "[extrude]") {
  SketchPlane plane(PlaneType::XY, glm::dvec3(10, 0, 5));
  auto loops = sketch::BuildLoops(RectLines({-1, -1}, {1, 1}));
  CADShape prism = sketch::BuildPrism(plane, loops, 2.0, false);
  REQUIRE(prism.IsValid());
  glm::dvec3 min, max;
  prism.GetBoundingBox(min, max);
  glm::dvec3 center = (min + max) * 0.5;
  CHECK_THAT(center.x, WithinRel(10.0, 1e-3));
  CHECK_THAT(center.z, WithinRel(6.0, 1e-3)); // 5 + half of 2
}

// --- ExtrudeFeature modes (through a Document) ------------------------------

static uint64_t AddRectSketch(Document& doc, const glm::dvec2& c1,
                              const glm::dvec2& c2,
                              const SketchPlane& plane = kXY) {
  auto sk = std::make_unique<SketchFeature>("Sketch", plane);
  sk->AddRectangle(c1, c2);
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

TEST_CASE("ExtrudeFeature NewBody adds a body", "[extrude][feature]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {0, 0}, {4, 2});
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", sid, 3.0, ExtrudeMode::NewBody));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(24.0, 1e-6));
}

TEST_CASE("ExtrudeFeature Add fuses into an existing body",
          "[extrude][feature]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Base", glm::dvec3(0, 0, 0), 4, 4,
                                              1));   // z 0..1
  uint64_t sid = AddRectSketch(doc, {1, 1}, {3, 3}); // on XY, overlaps base top
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Boss", sid, 2.0, ExtrudeMode::Add));

  REQUIRE(doc.GetBodies().size() == 1); // fused, not appended
  // Base (16) + boss above z=1 (2x2x1 that isn't already inside the base)
  CHECK(doc.GetBodies()[0].shape.Volume() > 16.0);
  CHECK(doc.GetBodies()[0].shape.Volume() < 16.0 + 8.0);
}

TEST_CASE("ExtrudeFeature Cut removes material", "[extrude][feature]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Base", glm::dvec3(0, 0, 0), 4, 4,
                                              2));   // z 0..2, vol 32
  uint64_t sid = AddRectSketch(doc, {1, 1}, {3, 3}); // 2x2 hole
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pocket", sid, 5.0, ExtrudeMode::Cut));

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(32.0 - 2 * 2 * 2, 1e-4));
}

TEST_CASE("A cut opens a face a float-placed primitive left above its plane",
          "[extrude][feature]") {
  // A sketch plane is exact, but a primitive's position is single
  // precision: a box placed at z = 24.7 sits at 24.70000076, so its top is
  // 0.76 um above a plane typed at 29.7. A pocket cut down from that plane
  // kept a skin over itself -- a sealed void that passes BRepCheck (the
  // pan/tilt head's bearing seat had a "shoulder" at both ends). The tool
  // starts a hair behind the plane instead (CoplanarStartSlack).
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::vec3(0, 0, 24.7), 20, 20, 5.0));
  const SketchPlane top(glm::dvec3(0, 0, 29.7), glm::dvec3(0, 0, 1),
                        glm::dvec3(1, 0, 0));
  uint64_t sid = AddRectSketch(doc, {5, 5}, {15, 15}, top);
  auto cut = std::make_unique<ExtrudeFeature>("Pocket", sid, 3.0,
                                              ExtrudeMode::Cut, true);
  doc.AddFeature(std::move(cut));
  REQUIRE(doc.GetBodies().size() == 1);
  // Open: no face looks down from the top of the part.
  int lids = 0;
  for (TopExp_Explorer ex(doc.GetBodies()[0].shape.GetShape(), TopAbs_FACE);
       ex.More(); ex.Next()) {
    const TopoDS_Face f = TopoDS::Face(ex.Current());
    BRepAdaptor_Surface surf(f);
    if (surf.GetType() != GeomAbs_Plane)
      continue;
    gp_Dir n = surf.Plane().Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED)
      n.Reverse();
    if (n.Z() < -0.5 && surf.Plane().Location().Z() > 29.0)
      lids++;
  }
  CHECK(lids == 0);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(20.0 * 20.0 * 5.0 - 10.0 * 10.0 * 3.0, 1e-5));
}

TEST_CASE(
    "An add fuses to a face a float-placed primitive left below its plane",
    "[extrude][feature]") {
  // The other way round: a box placed at z = 13.4 sits at 13.39999962, so a
  // boss grown up from a plane typed at its top, 18.4, started 0.38 um above
  // the face, and the part was two solids.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::vec3(0, 0, 13.4), 20, 20, 5.0));
  const SketchPlane top(glm::dvec3(0, 0, 18.4), glm::dvec3(0, 0, 1),
                        glm::dvec3(1, 0, 0));
  uint64_t sid = AddRectSketch(doc, {5, 5}, {15, 15}, top);
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Boss", sid, 5.0, ExtrudeMode::Add));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].shape.SolidCount() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(20.0 * 20.0 * 5.0 + 10.0 * 10.0 * 5.0, 1e-5));
}

TEST_CASE("ExtrudeFeature Cut that misses every body warns but is not an error",
          "[extrude][feature][warning]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Base", glm::vec3(0, 0, 0), 4, 4,
                                              2)); // footprint x,y 0..4
  // Profile sits far outside the base footprint: the cut prism never
  // intersects it (the "silent no-op cut" that bit the eye-bracket build).
  uint64_t sid = AddRectSketch(doc, {10, 10}, {12, 12});
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pocket", sid, 5.0, ExtrudeMode::Cut));

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(32.0, 1e-9)); // untouched
  auto* cut = doc.GetFeature(doc.FeatureCount() - 1);
  CHECK(cut->GetError().empty());         // built fine — not broken
  CHECK_FALSE(cut->GetWarning().empty()); // ...but flagged as a no-op
}

TEST_CASE("A cut that only touches the body leaves it exactly as it was",
          "[extrude][feature][warning][toponaming]") {
  // Drawn on the top face and extruded away from the part, the tool shares
  // only a face with it. The boolean imprinted its outline anyway, splitting
  // the top in two and renaming it (cap1;M4.0 / cap1;M4.1), for a cut that
  // removed nothing.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::dvec3(0, 0, 0), 4, 4, 2));
  const auto before = doc.GetBodies()[0].elementMap.names;
  const SketchPlane top(glm::dvec3(0, 0, 2), glm::dvec3(0, 0, 1),
                        glm::dvec3(1, 0, 0));
  uint64_t sid = AddRectSketch(doc, {1, 1}, {3, 3}, top);
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Away", sid, 5.0, ExtrudeMode::Cut));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_FALSE(doc.GetFeature(doc.FeatureCount() - 1)->GetWarning().empty());
  CHECK(doc.GetBodies()[0].elementMap.names == before);
}

TEST_CASE("ExtrudeFeature Cut that removes material carries no warning",
          "[extrude][feature][warning]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::dvec3(0, 0, 0), 4, 4, 2));
  uint64_t sid = AddRectSketch(doc, {1, 1}, {3, 3}); // real pocket
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pocket", sid, 5.0, ExtrudeMode::Cut));
  CHECK(doc.GetFeature(doc.FeatureCount() - 1)->GetWarning().empty());
}

TEST_CASE("Fixing a no-op cut clears its warning on the next regen",
          "[extrude][feature][warning]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::dvec3(0, 0, 0), 4, 4, 2));
  uint64_t sid = AddRectSketch(doc, {10, 10}, {12, 12}); // misses
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pocket", sid, 5.0, ExtrudeMode::Cut));
  REQUIRE_FALSE(doc.GetFeature(doc.FeatureCount() - 1)->GetWarning().empty());

  // Move the profile onto the base; the warning must not linger.
  auto* sk = dynamic_cast<SketchFeature*>(doc.GetFeature(1));
  REQUIRE(sk != nullptr);
  sk->entities.clear();
  sk->AddRectangle({1, 1}, {3, 3});
  doc.Regenerate();
  CHECK(doc.GetFeature(doc.FeatureCount() - 1)->GetWarning().empty());
}

TEST_CASE("Extrude referencing a missing sketch fails gracefully",
          "[extrude][feature]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", 999, 1.0, ExtrudeMode::NewBody));
  CHECK_FALSE(doc.Regenerate());
  CHECK(doc.GetBodies().empty());
}

TEST_CASE("Deleting a sketch a later extrude uses stops regeneration",
          "[extrude][feature][undo]") {
  Document doc;
  CommandStack stack;
  uint64_t sid = AddRectSketch(doc, {0, 0}, {2, 2});
  stack.Execute(
      std::make_unique<AddFeatureCommand>(std::make_unique<ExtrudeFeature>(
          "Pad", sid, 2.0, ExtrudeMode::NewBody)),
      doc);
  REQUIRE(doc.GetBodies().size() == 1);

  // Remove the sketch (index 0): regeneration now stops at the extrude
  stack.Execute(std::make_unique<RemoveFeatureCommand>(0), doc);
  CHECK(doc.GetBodies().empty());

  // Undo restores the sketch and the model
  stack.Undo(doc);
  CHECK(doc.GetBodies().size() == 1);
}

TEST_CASE("Extrude survives a document JSON round-trip", "[extrude][io]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {0, 0}, {4, 2});
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", sid, 3.0, ExtrudeMode::NewBody));
  const double vol = doc.GetBodies()[0].shape.Volume();

  fs::path path = fs::temp_directory_path() / "designer_extrude_doc.json";
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(vol, 1e-9));

  // The extrude's sketch reference resolved after load
  auto* ext = dynamic_cast<ExtrudeFeature*>(loaded->GetFeature(1));
  REQUIRE(ext != nullptr);
  CHECK(loaded->FindFeatureById(ext->sketchId) != nullptr);

  fs::remove(path);
}

TEST_CASE("Solver float noise at a line/arc joint does not distort the prism",
          "[extrude][arc]") {
  // Regression: coordinates exactly as PlaneGCS wrote them back after
  // solving a filleted 40x20 rectangle (float32 noise ~1e-7 at the joint
  // between the trimmed top line and the fillet arc). The noisy joint made
  // the profile lose ~5% of its area while still extruding "successfully".
  std::vector<sketch::Entity> e;
  e.push_back(sketch::Line{{0.0, 0.0}, {40.0, 0.0}});
  e.push_back(sketch::Line{{40.0, 0.0}, {40.0, 20.0}});
  e.push_back(sketch::Line{{40.0, 20.0}, {3.999999761581421, 20.0}});
  e.push_back(sketch::Line{{0.0, 16.0}, {0.0, 0.0}});
  e.push_back(sketch::Arc{
      {4.0, 16.0}, 3.9999999543595246, 1.5707963501871345, 3.1415926649999117});

  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(kXY, loops, 5.0, false);
  REQUIRE(prism.IsValid());
  const double area = 40.0 * 20.0 - (16.0 - M_PI * 4.0);
  CHECK_THAT(prism.Volume(), WithinRel(area * 5.0, 1e-4));
}

TEST_CASE("Disjoint profiles extrude as independent solids (multi-profile)",
          "[extrude][profile]") {
  // Two separate circles (a bolt-hole sketch): one prism per profile group,
  // assembled into a compound whose volume is the sum. (Before M3.1 this was
  // first a silent no-op, then a loud error.)
  std::vector<sketch::Entity> e = {sketch::Circle{{10, 0}, 3},
                                   sketch::Circle{{-10, 0}, 3}};
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 2);
  CADShape prism = sketch::BuildPrism(kXY, loops, 5.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(2.0 * M_PI * 9.0 * 5.0, 1e-3));

  // A circle properly inside another still extrudes as a ring.
  std::vector<sketch::Entity> ring = {sketch::Circle{{0, 0}, 5},
                                      sketch::Circle{{0, 0}, 2}};
  CADShape tube = sketch::BuildPrism(kXY, sketch::BuildLoops(ring), 2.0, false);
  REQUIRE(tube.IsValid());
  CHECK_THAT(tube.Volume(), WithinRel(M_PI * (25.0 - 4.0) * 2.0, 1e-3));
}

TEST_CASE("Four-bolt-hole cut works from ONE sketch", "[extrude][profile]") {
  // The flange from the S1 live run that motivated multi-profile: a plate,
  // then a single sketch holding four disjoint circles cut through it.
  auto plate = sketch::BuildLoops(RectLines({-30, -20}, {30, 20}));
  CADShape body = sketch::BuildPrism(kXY, plate, 6.0, false);
  REQUIRE(body.IsValid());

  std::vector<sketch::Entity> holes = {
      sketch::Circle{{22, 12}, 3}, sketch::Circle{{-22, 12}, 3},
      sketch::Circle{{-22, -12}, 3}, sketch::Circle{{22, -12}, 3}};
  CADShape tool =
      sketch::BuildPrism(kXY, sketch::BuildLoops(holes), 6.0, false);
  REQUIRE(tool.IsValid());
  CADShape cut = body.Difference(tool);
  REQUIRE(cut.IsValid());
  CHECK_THAT(cut.Volume(), WithinRel((2400.0 - 4.0 * M_PI * 9.0) * 6.0, 1e-3));
}

TEST_CASE("Islands inside holes become solids again (3-deep nesting)",
          "[extrude][profile]") {
  // Outer rect, circular hole, island circle inside the hole:
  // area = rect - hole + island.
  std::vector<sketch::Entity> e = RectLines({0, 0}, {10, 10});
  e.push_back(sketch::Circle{{5, 5}, 3});
  e.push_back(sketch::Circle{{5, 5}, 1});
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 3);
  CADShape prism = sketch::BuildPrism(kXY, loops, 2.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(),
             WithinRel((100.0 - M_PI * 9.0 + M_PI) * 2.0, 1e-3));
}

TEST_CASE("Overlapping loops fail loudly", "[extrude][profile]") {
  std::vector<sketch::Entity> e = {sketch::Circle{{0, 0}, 3},
                                   sketch::Circle{{2, 0}, 3}};
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 2);
  CHECK(!sketch::BuildPrism(kXY, loops, 5.0, false).IsValid());
}

TEST_CASE("Multi-profile revolve sums the groups", "[extrude][profile]") {
  // Two disjoint rectangles revolved 360 deg about Y: two coaxial rings.
  // Pappus: V = 2*pi*R*A per ring.
  std::vector<sketch::Entity> e = RectLines({2, 0}, {3, 1});
  for (auto& le : RectLines({5, 0}, {6, 1}))
    e.push_back(le);
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 2);
  CADShape rings = sketch::BuildRevolve(
      kXY, loops, glm::dvec3(0), glm::dvec3(0, 1, 0), 2.0 * M_PI, false, false);
  REQUIRE(rings.IsValid());
  const double v = 2.0 * M_PI * 2.5 * 1.0 + 2.0 * M_PI * 5.5 * 1.0;
  CHECK_THAT(rings.Volume(), WithinRel(v, 1e-3));
}

TEST_CASE("Multi-profile prisms keep element names per group",
          "[extrude][profile][toponaming]") {
  sketch::Circle a{{10, 0}, 3};
  a.id = 7;
  sketch::Circle b{{-10, 0}, 3};
  b.id = 9;
  std::vector<sketch::Entity> e = {a, b};
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 2);
  ElementMap map;
  CADShape prism = sketch::BuildPrismNamed(kXY, loops, 5.0, false, 42, map);
  REQUIRE(prism.IsValid());
  // Each cylinder: side face named from its circle entity, caps keyed by the
  // group (the outer loop's entity id) so the two pairs never collide.
  std::vector<std::string> names;
  for (const auto& [idx, name] : map.names)
    names.push_back(name);
  auto has = [&](const std::string& n) {
    return std::find(names.begin(), names.end(), n) != names.end();
  };
  CHECK(has("42/g7"));
  CHECK(has("42/g9"));
  CHECK(has("42/cap0@g7"));
  CHECK(has("42/cap1@g7"));
  CHECK(has("42/cap0@g9"));
  CHECK(has("42/cap1@g9"));
  // Every face of both cylinders is named (3 faces each).
  CHECK(map.names.size() == 6);
}

TEST_CASE("Symmetric extrude straddles the sketch plane", "[extrude][end]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto ext =
      std::make_unique<ExtrudeFeature>("Pad", skId, 6.0, ExtrudeMode::NewBody);
  ext->endMode = ExtrudeEnd::Symmetric;
  doc.AddFeature(std::move(ext));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(48.0, 1e-6));
  glm::dvec3 min, max;
  doc.GetBodies()[0].shape.GetBoundingBox(min, max);
  CHECK_THAT(min.z, Catch::Matchers::WithinAbs(-3.0, 1e-4));
  CHECK_THAT(max.z, Catch::Matchers::WithinAbs(3.0, 1e-4));
}

TEST_CASE("Two-sided extrude sweeps both ways with separate distances",
          "[extrude][end]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto ext =
      std::make_unique<ExtrudeFeature>("Pad", skId, 5.0, ExtrudeMode::NewBody);
  ext->endMode = ExtrudeEnd::TwoSided;
  ext->distance2 = 2.0;
  doc.AddFeature(std::move(ext));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0 * 7.0, 1e-6));
  glm::dvec3 min, max;
  doc.GetBodies()[0].shape.GetBoundingBox(min, max);
  CHECK_THAT(min.z, Catch::Matchers::WithinAbs(-2.0, 1e-4));
  CHECK_THAT(max.z, Catch::Matchers::WithinAbs(5.0, 1e-4));
}

TEST_CASE("End conditions round-trip through serialization (v18)",
          "[extrude][end][serialization]") {
  Document doc;
  doc.SetParameter("back", "2", "");
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto ext =
      std::make_unique<ExtrudeFeature>("Pad", skId, 5.0, ExtrudeMode::NewBody);
  ext->endMode = ExtrudeEnd::TwoSided;
  ext->distance2 = 1.0;
  ext->distance2Expr = "back";
  doc.AddFeature(std::move(ext));

  const auto path = std::filesystem::temp_directory_path() / "end_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<ExtrudeFeature*>(loaded->GetFeature(1));
  REQUIRE(rt != nullptr);
  CHECK(rt->endMode == ExtrudeEnd::TwoSided);
  CHECK(rt->distance2Expr == "back");
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(),
             WithinRel(8.0 * 7.0, 1e-6)); // 5 forward + expr-driven 2 back
  std::filesystem::remove(path);
}

TEST_CASE("Through-all cut pierces the whole body without a distance",
          "[extrude][end]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Base", glm::dvec3(0, 0, 0), 4, 4,
                                              2)); // z 0..2, vol 32
  // Sketch ON the top face, cutting back down through the box — the case
  // that used to need a plane parked behind the material plus an overshoot
  // distance. The 0.123 literal must be ignored.
  SketchPlane top(PlaneType::XY, glm::dvec3(0, 0, 2));
  uint64_t sid = AddRectSketch(doc, {1, 1}, {3, 3}, top);
  auto ext = std::make_unique<ExtrudeFeature>("Slot", sid, 0.123,
                                              ExtrudeMode::Cut, true);
  ext->endMode = ExtrudeEnd::ThroughAll;
  doc.AddFeature(std::move(ext));

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(32.0 - 2 * 2 * 2, 1e-4));
  CHECK(doc.GetFeature(doc.FeatureCount() - 1)->GetWarning().empty());
}

TEST_CASE("Through-all cut sweeps forward without reversed", "[extrude][end]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::dvec3(0, 0, 0), 4, 4, 2));
  uint64_t sid = AddRectSketch(doc, {1, 1}, {3, 3}); // on XY at z=0, +normal up
  auto ext =
      std::make_unique<ExtrudeFeature>("Slot", sid, 1.0, ExtrudeMode::Cut);
  ext->endMode = ExtrudeEnd::ThroughAll;
  doc.AddFeature(std::move(ext));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(32.0 - 2 * 2 * 2, 1e-4));
}

TEST_CASE("Through-all requires cut mode", "[extrude][end]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::dvec3(0, 0, 0), 4, 4, 2));
  uint64_t sid = AddRectSketch(doc, {1, 1}, {3, 3});
  auto ext =
      std::make_unique<ExtrudeFeature>("Pad", sid, 1.0, ExtrudeMode::NewBody);
  ext->endMode = ExtrudeEnd::ThroughAll;
  doc.AddFeature(std::move(ext));
  auto* f = doc.GetFeature(doc.FeatureCount() - 1);
  CHECK_FALSE(f->GetError().empty());
  CHECK(f->GetError().find("cut") != std::string::npos);
  CHECK(doc.GetBodies().size() == 1); // just the base — nothing was padded
}

TEST_CASE("Through-all cut that misses warns like a blind cut",
          "[extrude][end][warning]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::dvec3(0, 0, 0), 4, 4, 2));
  SketchPlane top(PlaneType::XY, glm::dvec3(0, 0, 2));
  uint64_t sid = AddRectSketch(doc, {10, 10}, {12, 12}, top); // off to the side
  auto ext = std::make_unique<ExtrudeFeature>("Slot", sid, 1.0,
                                              ExtrudeMode::Cut, true);
  ext->endMode = ExtrudeEnd::ThroughAll;
  doc.AddFeature(std::move(ext));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(32.0, 1e-9));
  auto* cut = doc.GetFeature(doc.FeatureCount() - 1);
  CHECK(cut->GetError().empty());
  CHECK_FALSE(cut->GetWarning().empty());
}

TEST_CASE("Through-all round-trips through serialization (v52)",
          "[extrude][end][serialization]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Base", glm::dvec3(0, 0, 0), 4, 4, 2));
  SketchPlane top(PlaneType::XY, glm::dvec3(0, 0, 2));
  uint64_t sid = AddRectSketch(doc, {1, 1}, {3, 3}, top);
  auto ext = std::make_unique<ExtrudeFeature>("Slot", sid, 1.0,
                                              ExtrudeMode::Cut, true);
  ext->endMode = ExtrudeEnd::ThroughAll;
  doc.AddFeature(std::move(ext));

  const auto path = std::filesystem::temp_directory_path() / "thru_rt.json";
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<ExtrudeFeature*>(loaded->GetFeature(2));
  REQUIRE(rt != nullptr);
  CHECK(rt->endMode == ExtrudeEnd::ThroughAll);
  CHECK(rt->reversed);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(),
             WithinRel(32.0 - 2 * 2 * 2, 1e-4));
  std::filesystem::remove(path);
}

TEST_CASE("Up-to-face extrude reaches the target and follows upstream edits",
          "[extrude][end][toponaming]") {
  Document doc;
  // Pad A: 4x2 rect at origin, height 6 -> top cap is "<padId>/cap1" at z=6.
  auto skA = std::make_unique<SketchFeature>("A", kXY);
  skA->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(skA));
  const uint64_t skAId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto padA = std::make_unique<ExtrudeFeature>("PadA", skAId, 6.0,
                                               ExtrudeMode::NewBody);
  doc.AddFeature(std::move(padA));
  const uint64_t padAId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();

  // Pad B: a 2x1 rect beside A, extruded up to A's top face.
  auto skB = std::make_unique<SketchFeature>("B", kXY);
  skB->AddRectangle({10, 0}, {12, 1});
  doc.AddFeature(std::move(skB));
  const uint64_t skBId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto padB = std::make_unique<ExtrudeFeature>("PadB", skBId, 1.0,
                                               ExtrudeMode::NewBody);
  padB->endMode = ExtrudeEnd::UpToFace;
  padB->endTarget = ElementRef{padAId, std::to_string(padAId) + "/cap1"};
  doc.AddFeature(std::move(padB));

  REQUIRE(doc.GetBodies().size() == 2);
  CHECK_THAT(doc.GetBodies()[1].shape.Volume(), WithinRel(2.0 * 6.0, 1e-6));

  // The toponaming payoff: raise pad A -> pad B follows its face.
  auto* a = dynamic_cast<ExtrudeFeature*>(doc.FindFeatureById(padAId));
  REQUIRE(a != nullptr);
  a->distance = 9.0;
  REQUIRE(doc.Regenerate());
  CHECK_THAT(doc.GetBodies()[1].shape.Volume(), WithinRel(2.0 * 9.0, 1e-6));

  // Losing the target is a loud feature error, not a stale guess.
  a->distance = 6.0;
  auto* b =
      dynamic_cast<ExtrudeFeature*>(doc.GetFeature(doc.FeatureCount() - 1));
  b->endTarget.name = "nonsense";
  CHECK_FALSE(doc.Regenerate());
  CHECK(!b->GetError().empty());
}

TEST_CASE("Up-to-face round-trips through serialization",
          "[extrude][end][serialization]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto ext =
      std::make_unique<ExtrudeFeature>("Pad", skId, 5.0, ExtrudeMode::NewBody);
  ext->endMode = ExtrudeEnd::UpToFace;
  ext->endTarget = ElementRef{7, "7/cap1"};
  doc.AddFeature(std::move(ext));

  const auto path = std::filesystem::temp_directory_path() / "utf_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<ExtrudeFeature*>(loaded->GetFeature(1));
  REQUIRE(rt != nullptr);
  CHECK(rt->endMode == ExtrudeEnd::UpToFace);
  CHECK(rt->endTarget.featureId == 7);
  CHECK(rt->endTarget.name == "7/cap1");
  std::filesystem::remove(path);
}

TEST_CASE("Thin extrude walls a circle exactly", "[extrude][thin]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  sketch::Circle c{{0, 0}, 3.0};
  sk->entities.push_back(c);
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto ext =
      std::make_unique<ExtrudeFeature>("Tube", skId, 4.0, ExtrudeMode::NewBody);
  ext->thin = true;
  ext->thinThickness = 0.5;
  ext->thinSide = sketch::ThinSide::Out;
  doc.AddFeature(std::move(ext));
  REQUIRE(doc.GetBodies().size() == 1);
  // Ring r=3..3.5, height 4.
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(M_PI * (3.5 * 3.5 - 9.0) * 4.0, 1e-3));
}

TEST_CASE("Thin extrude wall sides (rect, inward vs outward)",
          "[extrude][thin]") {
  auto build = [](sketch::ThinSide side, double thickness) {
    Document doc;
    auto sk = std::make_unique<SketchFeature>("S", kXY);
    sk->AddRectangle({0, 0}, {4, 2});
    doc.AddFeature(std::move(sk));
    const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
    auto ext = std::make_unique<ExtrudeFeature>("Wall", skId, 1.0,
                                                ExtrudeMode::NewBody);
    ext->thin = true;
    ext->thinThickness = thickness;
    ext->thinSide = side;
    doc.AddFeature(std::move(ext));
    REQUIRE(doc.GetBodies().size() == 1);
    return doc.GetBodies()[0].shape.Volume();
  };
  // Inward: sharp corners, ring area = 8 - (4-1)(2-1) = 5.
  CHECK_THAT(build(sketch::ThinSide::In, 0.5), WithinRel(5.0, 1e-3));
  // Outward: arc-joined convex corners, area = P*t + pi*t^2 (P = 12).
  CHECK_THAT(build(sketch::ThinSide::Out, 0.5),
             WithinRel(12.0 * 0.5 + M_PI * 0.25, 1e-3));
  // Too thick inward: the offset degenerates -> loud failure, not garbage.
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto ext =
      std::make_unique<ExtrudeFeature>("Wall", skId, 1.0, ExtrudeMode::NewBody);
  ext->thin = true;
  ext->thinThickness = 3.0; // > half the short side
  ext->thinSide = sketch::ThinSide::In;
  doc.AddFeature(std::move(ext));
  CHECK(doc.GetBodies().empty());
  CHECK(!doc.GetFeature(1)->GetError().empty());
}

TEST_CASE("Thin round-trips through serialization", "[extrude][thin]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  sk->AddRectangle({0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto ext =
      std::make_unique<ExtrudeFeature>("Wall", skId, 1.0, ExtrudeMode::NewBody);
  ext->thin = true;
  ext->thinThickness = 0.25;
  ext->thinSide = sketch::ThinSide::Both;
  doc.AddFeature(std::move(ext));
  const auto path = std::filesystem::temp_directory_path() / "thin_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<ExtrudeFeature*>(loaded->GetFeature(1));
  REQUIRE(rt != nullptr);
  CHECK(rt->thin);
  CHECK(rt->thinSide == sketch::ThinSide::Both);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(),
             WithinRel(doc.GetBodies()[0].shape.Volume(), 1e-9));
  std::filesystem::remove(path);
}

TEST_CASE("Extrude regions select one profile of a multi-region sketch",
          "[extrude][regions]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("Profiles", kXY);
  const uint32_t c1 = sk->AddEntity(sketch::Circle{{0, 0}, 1.0});
  const uint32_t c2 = sk->AddEntity(sketch::Circle{{5, 0}, 1.0});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(0)->GetId();

  auto ext =
      std::make_unique<ExtrudeFeature>("One", skId, 2.0, ExtrudeMode::NewBody);
  ext->regions = {{c1}};
  doc.AddFeature(std::move(ext));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(2.0 * M_PI, 1e-3)); // one r=1 h=2 cylinder, not two

  // Selecting both regions is the whole sketch again.
  auto* e = dynamic_cast<ExtrudeFeature*>(doc.GetFeature(1));
  REQUIRE(e != nullptr);
  e->regions = {{c1}, {c2}};
  REQUIRE(doc.Regenerate());
  double total = 0.0;
  for (const auto& b : doc.GetBodies())
    total += b.shape.Volume();
  CHECK_THAT(total, WithinRel(4.0 * M_PI, 1e-3));

  // A hole rides along with its selected outer region.
  auto* skf = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  REQUIRE(skf != nullptr);
  skf->AddEntity(sketch::Circle{{0, 0}, 0.5}); // hole inside circle 1
  e->regions = {{c1}};
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(2.0 * M_PI * (1.0 - 0.25), 1e-3)); // annulus

  // A vanished region is a first-class regen failure, not a silent skip.
  e->regions = {{9999}};
  CHECK_FALSE(doc.Regenerate());
  CHECK_FALSE(e->GetError().empty());
}

TEST_CASE("A cut into an empty document reports why", "[extrude][error]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  for (auto& e : RectLines({0, 0}, {2, 2}))
    sk->entities.push_back(e);
  doc.AddFeature(std::move(sk));
  const uint64_t sid = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Cut", sid, 2.0, ExtrudeMode::Cut));
  CHECK_FALSE(doc.Regenerate());
  const std::string err = doc.GetFeature(1)->GetError();
  CHECK(err.find("no bodies") != std::string::npos);
}

// A rectangle whose corners are visited in the given direction: the sketch
// rectangle tool follows the drag, so a hole is as likely to come out
// clockwise as counter-clockwise.
static std::vector<sketch::Entity> RectLinesDir(glm::dvec2 c1, glm::dvec2 c2,
                                                bool ccw) {
  glm::dvec2 a{c1.x, c1.y}, b{c2.x, c1.y}, c{c2.x, c2.y}, d{c1.x, c2.y};
  if (ccw)
    return {sketch::Line{a, b}, sketch::Line{b, c}, sketch::Line{c, d},
            sketch::Line{d, a}};
  return {sketch::Line{a, d}, sketch::Line{d, c}, sketch::Line{c, b},
          sketch::Line{b, a}};
}

// A hole used to be ADDED, not subtracted, whenever its loop wound the same
// way as its outer boundary — the face builder reversed every inner wire on
// the assumption that loops came out counter-clockwise. Circles are CCW by
// construction, so only rectangles (and other chained loops) hit it, and the
// result was a plate whose volume exceeded its own outline.
TEST_CASE("Holes subtract at every winding of outer and hole", "[profile]") {
  for (bool outerCcw : {false, true}) {
    for (bool holeCcw : {false, true}) {
      std::vector<sketch::Entity> e =
          RectLinesDir({0, 0}, {100, 100}, outerCcw);
      for (const auto& x : RectLinesDir({20, 20}, {40, 40}, holeCcw))
        e.push_back(x);
      const auto loops = sketch::BuildLoops(e);
      REQUIRE(loops.size() == 2);
      CADShape s = sketch::BuildPrism(kXY, loops, 1.0, false);
      REQUIRE(s.IsValid());
      INFO("outer ccw=" << outerCcw << " hole ccw=" << holeCcw);
      CHECK_THAT(s.Volume(), WithinRel(100.0 * 100.0 - 20.0 * 20.0, 1e-9));
    }
  }
}
