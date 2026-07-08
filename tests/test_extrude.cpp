#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/ExtrudeFeature.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"

#include <cmath>
#include <filesystem>

using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

static const SketchPlane kXY(PlaneType::XY);

// Rectangles are four lines now; build them at exact corners so BuildLoops
// chains them without needing a solve.
static std::vector<sketch::Entity> RectLines(glm::vec2 c1, glm::vec2 c2) {
  glm::vec2 a{c1.x, c1.y}, b{c2.x, c1.y}, c{c2.x, c2.y}, d{c1.x, c2.y};
  return {sketch::Line{a, b}, sketch::Line{b, c}, sketch::Line{c, d},
          sketch::Line{d, a}};
}

// --- Geometry ---------------------------------------------------------------

TEST_CASE("SegmentIntersect finds crossings and rejects misses", "[profile]") {
  using Catch::Matchers::WithinAbs;
  // Crosses at the midpoint of segment 1.
  auto hit = sketch::SegmentIntersect({0, 0}, {2, 0}, {1, -1}, {1, 1});
  REQUIRE(hit.has_value());
  CHECK_THAT(*hit, WithinAbs(0.5f, 1e-5f));
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
  // A construction diagonal + a construction reference circle: ignored by loops.
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

TEST_CASE("Ellipse extrudes to an elliptic-prism volume", "[extrude][ellipse]") {
  const double a = 2.0, b = 1.0;
  auto loops = sketch::BuildLoops({sketch::Ellipse{{0, 0}, a, b, 0.0}});
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(kXY, loops, 3.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(M_PI * a * b * 3.0, 1e-3));
}

TEST_CASE("A rotated ellipse extrudes to the same volume", "[extrude][ellipse]") {
  // Rotation leaves the enclosed area (π·a·b) unchanged.
  const double a = 2.0, b = 1.0;
  auto loops = sketch::BuildLoops({sketch::Ellipse{{0, 0}, a, b, 0.7}});
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(kXY, loops, 3.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(M_PI * a * b * 3.0, 1e-3));
}

TEST_CASE("A filleted-corner profile (lines + arc) extrudes", "[extrude][arc]") {
  // 4x2 rectangle with the top-right corner rounded by a quarter arc of r=0.5:
  // the arc runs CCW from (4,1.5) to (3.5,2), centred at (3.5,1.5).
  const double r = 0.5;
  std::vector<sketch::Entity> e;
  e.push_back(sketch::Line{{0.0f, 0.0f}, {4.0f, 0.0f}});
  e.push_back(sketch::Line{{4.0f, 0.0f}, {4.0f, 1.5f}});
  e.push_back(sketch::Arc{{3.5f, 1.5f}, r, 0.0, M_PI / 2});
  e.push_back(sketch::Line{{3.5f, 2.0f}, {0.0f, 2.0f}});
  e.push_back(sketch::Line{{0.0f, 2.0f}, {0.0f, 0.0f}});

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
  e.push_back(sketch::Line{{-1.0f, 0.0f}, {1.0f, 0.0f}}); // diameter
  e.push_back(sketch::Arc{{0.0f, 0.0f}, 1.0, 0.0, M_PI}); // upper semicircle
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(kXY, loops, 3.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(0.5 * M_PI * 1.0 * 1.0 * 3.0, 1e-3));
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
  glm::vec3 min, max;
  prism.GetBoundingBox(min, max);
  CHECK(max.z <= 1e-6);       // extruded toward -Z
  CHECK(min.z <= -2.0 + 1e-6);
}

TEST_CASE("Offset plane places the prism where expected", "[extrude]") {
  SketchPlane plane(PlaneType::XY, glm::vec3(10, 0, 5));
  auto loops = sketch::BuildLoops(RectLines({-1, -1}, {1, 1}));
  CADShape prism = sketch::BuildPrism(plane, loops, 2.0, false);
  REQUIRE(prism.IsValid());
  glm::vec3 min, max;
  prism.GetBoundingBox(min, max);
  glm::vec3 center = (min + max) * 0.5f;
  CHECK_THAT(center.x, WithinRel(10.0f, 1e-3f));
  CHECK_THAT(center.z, WithinRel(6.0f, 1e-3f)); // 5 + half of 2
}

// --- ExtrudeFeature modes (through a Document) ------------------------------

static uint64_t AddRectSketch(Document& doc, const glm::vec2& c1,
                              const glm::vec2& c2,
                              const SketchPlane& plane = kXY) {
  auto sk = std::make_unique<SketchFeature>("Sketch", plane);
  sk->AddRectangle(c1, c2);
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

TEST_CASE("ExtrudeFeature NewBody adds a body", "[extrude][feature]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {0, 0}, {4, 2});
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sid, 3.0,
                                                  ExtrudeMode::NewBody));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(24.0, 1e-6));
}

TEST_CASE("ExtrudeFeature Add fuses into an existing body", "[extrude][feature]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Base", glm::vec3(0, 0, 0), 4, 4,
                                              1)); // z 0..1
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
  doc.AddFeature(std::make_unique<BoxFeature>("Base", glm::vec3(0, 0, 0), 4, 4,
                                              2)); // z 0..2, vol 32
  uint64_t sid = AddRectSketch(doc, {1, 1}, {3, 3}); // 2x2 hole
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pocket", sid, 5.0, ExtrudeMode::Cut));

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(32.0 - 2 * 2 * 2, 1e-4));
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
  stack.Execute(std::make_unique<AddFeatureCommand>(
                    std::make_unique<ExtrudeFeature>("Pad", sid, 2.0,
                                                     ExtrudeMode::NewBody)),
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
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sid, 3.0,
                                                  ExtrudeMode::NewBody));
  const double vol = doc.GetBodies()[0].shape.Volume();

  fs::path path = fs::temp_directory_path() / "designer_extrude_doc.json";
  std::string error;
  REQUIRE(DocumentIO::Save(doc, path.string(), &error));
  auto loaded = DocumentIO::Load(path.string(), &error);
  REQUIRE(loaded != nullptr);
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(vol, 1e-9));

  // The extrude's sketch reference resolved after load
  auto* ext = dynamic_cast<ExtrudeFeature*>(loaded->GetFeature(1));
  REQUIRE(ext != nullptr);
  CHECK(loaded->FindFeatureById(ext->sketchId) != nullptr);

  fs::remove(path);
}
