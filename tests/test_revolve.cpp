#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/features/DatumAxisFeature.h"
#include "core/Document.h"
#include "core/features/RevolveFeature.h"
#include "core/io/Serialization.h"
#include "TestIO.h"
#include "core/features/SketchFeature.h"

#include <cmath>
#include <filesystem>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

static const SketchPlane kXY(PlaneType::XY);

// A rectangle sketch on XY; returns its feature id. On XY the plane UV maps
// directly to world XY, so (1,0)->(3,3) sits at x in [1,3], y in [0,3], z=0 —
// entirely on the +x side of the global Y axis.
static uint64_t AddRectSketch(Document& doc, const glm::vec2& c1,
                              const glm::vec2& c2) {
  auto sk = std::make_unique<SketchFeature>("Profile", kXY);
  sk->AddRectangle(c1, c2);
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

static double BodyVolume(const Document& doc) {
  return doc.GetBodies()[0].shape.Volume();
}

// --- Pappus volume checks (rectangle w=2,h=3 at centroid R=2 from global Y) --

TEST_CASE("Full 360 revolve is a solid of revolution (Pappus)", "[revolve]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
  auto rev = std::make_unique<RevolveFeature>("Rev", sid, 360.0,
                                              ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Y;
  doc.AddFeature(std::move(rev));

  REQUIRE(doc.GetBodies().size() == 1);
  // Washer: outer r=3, inner r=1, height 3 -> pi(9-1)*3 = 24pi = 2pi*R*(w*h).
  CHECK_THAT(BodyVolume(doc), WithinRel(2.0 * M_PI * 2.0 * (2.0 * 3.0), 1e-3));
}

TEST_CASE("Partial-angle revolve scales volume by angle", "[revolve]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
  auto rev = std::make_unique<RevolveFeature>("Rev", sid, 90.0,
                                              ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Y;
  doc.AddFeature(std::move(rev));

  // V = R*w*h*angle = 2*2*3*(pi/2) = 6pi (a quarter of the full 24pi).
  CHECK_THAT(BodyVolume(doc), WithinRel(2.0 * 2.0 * 3.0 * (M_PI / 2.0), 1e-3));
}

TEST_CASE("Symmetric revolve matches one-directional volume but centers it",
          "[revolve]") {
  const auto build = [](bool symmetric) {
    Document doc;
    uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
    auto rev = std::make_unique<RevolveFeature>("Rev", sid, 90.0,
                                                ExtrudeMode::NewBody);
    rev->axis.kind = AxisRef::Kind::Y;
    rev->symmetric = symmetric;
    doc.AddFeature(std::move(rev));
    return doc;
  };

  Document one = build(false);
  Document sym = build(true);
  // Same swept volume either way.
  CHECK_THAT(BodyVolume(sym), WithinRel(BodyVolume(one), 1e-3));

  // Revolving about Y sweeps into z. Symmetric is z-centered; one-directional
  // is not.
  glm::vec3 omin, omax, smin, smax;
  one.GetBodies()[0].shape.GetBoundingBox(omin, omax);
  sym.GetBodies()[0].shape.GetBoundingBox(smin, smax);
  CHECK_THAT(smin.z + smax.z, WithinAbs(0.0f, 1e-3f));      // centered
  CHECK(std::abs(omin.z + omax.z) > 1.0f);                  // offset to one side
}

// --- Failure modes -----------------------------------------------------------

TEST_CASE("A profile that crosses the axis is rejected", "[revolve]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {-1, 0}, {3, 3}); // straddles x=0 (the Y axis)
  auto rev = std::make_unique<RevolveFeature>("Rev", sid, 360.0,
                                              ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Y;
  doc.AddFeature(std::move(rev));
  CHECK(doc.GetBodies().empty());
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("An axis out of the sketch plane fails", "[revolve]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
  auto rev = std::make_unique<RevolveFeature>("Rev", sid, 360.0,
                                              ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Z; // Z is the XY-plane normal, not in-plane
  doc.AddFeature(std::move(rev));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("A revolve referencing a missing sketch fails", "[revolve]") {
  Document doc;
  auto rev = std::make_unique<RevolveFeature>("Rev", 9999, 360.0,
                                              ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Y;
  doc.AddFeature(std::move(rev));
  CHECK_FALSE(doc.Regenerate());
}

// --- Modes + expressions + axis sources -------------------------------------

TEST_CASE("Revolve Cut subtracts from an existing body", "[revolve]") {
  Document doc;
  // A solid disc (full revolve of a rectangle touching the axis): r=2, h=3.
  uint64_t base = AddRectSketch(doc, {0, 0}, {2, 3});
  auto disc = std::make_unique<RevolveFeature>("Disc", base, 360.0,
                                               ExtrudeMode::NewBody);
  disc->axis.kind = AxisRef::Kind::Y;
  doc.AddFeature(std::move(disc));
  const double solid = BodyVolume(doc); // pi*2^2*3 = 12pi

  // Cut a full-revolve washer hole (inner r=... ) — reuse a thin ring cut.
  uint64_t cutSk = AddRectSketch(doc, {0.5, 0.5}, {1.5, 2.5});
  auto cut = std::make_unique<RevolveFeature>("Hole", cutSk, 360.0,
                                              ExtrudeMode::Cut);
  cut->axis.kind = AxisRef::Kind::Y;
  doc.AddFeature(std::move(cut));

  REQUIRE(doc.GetBodies().size() == 1);
  // Removed ring: 2pi*R*A, R=1, A=1*2=2 -> 4pi.
  CHECK_THAT(BodyVolume(doc), WithinRel(solid - 2.0 * M_PI * 1.0 * 2.0, 1e-3));
}

TEST_CASE("An expression-driven revolve angle follows a parameter edit",
          "[revolve]") {
  Document doc;
  doc.SetParameter("sweep", "180");
  uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
  auto rev = std::make_unique<RevolveFeature>("Rev", sid, 1.0,
                                              ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Y;
  rev->angleExpr = "sweep";
  doc.AddFeature(std::move(rev));

  // 180deg: half the full 24pi.
  CHECK_THAT(BodyVolume(doc), WithinRel(12.0 * M_PI, 1e-3));
  doc.SetParameter("sweep", "90");
  CHECK_THAT(BodyVolume(doc), WithinRel(6.0 * M_PI, 1e-3));
}

TEST_CASE("Revolve about a DatumAxisFeature", "[revolve]") {
  Document doc;
  // A datum axis coincident with global Y (two points along y).
  auto ax = std::make_unique<DatumAxisFeature>("Axis", DatumAxisKind::TwoPoints);
  ax->p0 = {0, 0, 0};
  ax->p1 = {0, 1, 0};
  doc.AddFeature(std::move(ax));
  const uint64_t axisId = doc.GetFeature(0)->GetId();

  uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
  auto rev = std::make_unique<RevolveFeature>("Rev", sid, 360.0,
                                              ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::DatumAxis;
  rev->axis.datumAxisId = axisId;
  doc.AddFeature(std::move(rev));

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(BodyVolume(doc), WithinRel(24.0 * M_PI, 1e-3));
}

// --- Serialization -----------------------------------------------------------

TEST_CASE("Revolve round-trips through JSON", "[revolve]") {
  const fs::path path = fs::temp_directory_path() / "designer_revolve_test.json";
  {
    Document doc;
    doc.SetParameter("sweep", "270");
    uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
    auto rev = std::make_unique<RevolveFeature>("Rev", sid, 1.0,
                                                ExtrudeMode::NewBody);
    rev->axis.kind = AxisRef::Kind::Y;
    rev->angleExpr = "sweep";
    rev->symmetric = true;
    doc.AddFeature(std::move(rev));
    REQUIRE(testio::Save(doc, path.string()));
  }

  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rev = dynamic_cast<RevolveFeature*>(loaded->GetFeature(1));
  REQUIRE(rev != nullptr);
  CHECK(rev->angleExpr == "sweep");
  CHECK(rev->symmetric);
  CHECK(rev->axis.kind == AxisRef::Kind::Y);
  CHECK_THAT(rev->angleDeg, WithinRel(270.0, 1e-9));
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(),
             WithinRel(0.75 * 24.0 * M_PI, 1e-3));

  fs::remove(path);
}
