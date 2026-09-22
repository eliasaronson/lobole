#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchProfile.h"

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
  auto rev =
      std::make_unique<RevolveFeature>("Rev", sid, 360.0, ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Y;
  doc.AddFeature(std::move(rev));

  REQUIRE(doc.GetBodies().size() == 1);
  // Washer: outer r=3, inner r=1, height 3 -> pi(9-1)*3 = 24pi = 2pi*R*(w*h).
  CHECK_THAT(BodyVolume(doc), WithinRel(2.0 * M_PI * 2.0 * (2.0 * 3.0), 1e-3));
}

TEST_CASE("Partial-angle revolve scales volume by angle", "[revolve]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
  auto rev =
      std::make_unique<RevolveFeature>("Rev", sid, 90.0, ExtrudeMode::NewBody);
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
  CHECK_THAT(smin.z + smax.z, WithinAbs(0.0f, 1e-3f)); // centered
  CHECK(std::abs(omin.z + omax.z) > 1.0f);             // offset to one side
}

// --- Failure modes -----------------------------------------------------------

TEST_CASE("A profile that crosses the axis is rejected", "[revolve]") {
  Document doc;
  uint64_t sid =
      AddRectSketch(doc, {-1, 0}, {3, 3}); // straddles x=0 (the Y axis)
  auto rev =
      std::make_unique<RevolveFeature>("Rev", sid, 360.0, ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Y;
  doc.AddFeature(std::move(rev));
  CHECK(doc.GetBodies().empty());
  CHECK_FALSE(doc.Regenerate());
  // The rejection carries the specific reason, not the generic fallback.
  CHECK(doc.GetFeature(1)->GetError().find("cross the axis") !=
        std::string::npos);
}

TEST_CASE("An axis out of the sketch plane fails", "[revolve]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
  auto rev =
      std::make_unique<RevolveFeature>("Rev", sid, 360.0, ExtrudeMode::NewBody);
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
  auto cut =
      std::make_unique<RevolveFeature>("Hole", cutSk, 360.0, ExtrudeMode::Cut);
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
  auto rev =
      std::make_unique<RevolveFeature>("Rev", sid, 1.0, ExtrudeMode::NewBody);
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
  auto ax =
      std::make_unique<DatumAxisFeature>("Axis", DatumAxisKind::TwoPoints);
  ax->p0 = {0, 0, 0};
  ax->p1 = {0, 1, 0};
  doc.AddFeature(std::move(ax));
  const uint64_t axisId = doc.GetFeature(0)->GetId();

  uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
  auto rev =
      std::make_unique<RevolveFeature>("Rev", sid, 360.0, ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::DatumAxis;
  rev->axis.datumAxisId = axisId;
  doc.AddFeature(std::move(rev));

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(BodyVolume(doc), WithinRel(24.0 * M_PI, 1e-3));
}

// --- Serialization -----------------------------------------------------------

TEST_CASE("Revolve round-trips through JSON", "[revolve]") {
  const fs::path path =
      fs::temp_directory_path() / "designer_revolve_test.json";
  {
    Document doc;
    doc.SetParameter("sweep", "270");
    uint64_t sid = AddRectSketch(doc, {1, 0}, {3, 3});
    auto rev =
        std::make_unique<RevolveFeature>("Rev", sid, 1.0, ExtrudeMode::NewBody);
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

// --- Exactly-authored arc profiles (RPC path: no solver noise) ------------

// A revolve half-section in (r, z) sketch UV: a driver-like outline whose
// dust-cap dome is an arc meeting a line at exact coordinates. `apexZ` sets
// the dome apex; the arc is the circle through (10,-13) and (0,apexZ) with a
// horizontal tangent at the apex, exactly as an RPC client authors it.
static std::vector<sketch::Entity> ArcCapProfile(double apexZ) {
  const double sag = 13.0 + apexZ; // cone depth 13
  const double r = (10.0 * 10.0 + sag * sag) / (2.0 * sag);
  const double zc = apexZ - r;
  const double a0 = std::atan2(-13.0 - zc, 10.0);
  const glm::vec2 A{0.0f, -62.1f}, B{38.0f, -62.1f}, C{38.0f, -25.7f},
      D{49.5f, -8.0f}, E{52.5f, -7.4f}, F{52.5f, -5.4f}, G{61.0f, -3.4f},
      H{61.0f, -2.4f}, I{46.0f, 0.0f}, J{10.0f, -13.0f},
      K{0.0f, static_cast<float>(apexZ)};
  std::vector<sketch::Entity> e;
  for (auto [p, q] : {std::pair{A, B},
                      {B, C},
                      {C, D},
                      {D, E},
                      {E, F},
                      {F, G},
                      {G, H},
                      {H, I},
                      {I, J},
                      {K, A}})
    e.push_back(sketch::Line{p, q});
  e.push_back(sketch::Arc{{0.0f, static_cast<float>(zc)}, r, a0, M_PI / 2.0});
  return e;
}

// The plane the RPC repro used: XZ with normal -Y, so sketch (u,v) = (x,z)
// and the revolve axis is world Z.
static const SketchPlane kXZNegY({0, 0, 0}, {0, -1, 0}, {1, 0, 0});

TEST_CASE("Exact-coordinate arc profile revolves (float32 joint tolerance)",
          "[revolve][arc]") {
  // Regression: with apex -9.5 the dome arc's joint vertices sat ~1e-6 off
  // the exact-double trimming circle (all UV math is float32) while their
  // tolerance was measured only against the float endpoint — MakeEdge's
  // projection failed and the whole revolve threw "BRep_API: command not
  // done". Solver-noised GUI arcs accidentally cleared the bar; exactly-
  // authored RPC arcs are the ones that collapsed it.
  auto loops = sketch::BuildLoops(ArcCapProfile(-9.5));
  REQUIRE(loops.size() == 1);
  CADShape solid = sketch::BuildRevolve(kXZNegY, loops, {0, 0, 0}, {0, 0, 1},
                                        2.0 * M_PI, false, false);
  REQUIRE(solid.IsValid());
  // Straight-capped variant of this profile measures ~3.13e5; the dome only
  // trades sub-1% volume. Loose bounds keep the check shape-level.
  CHECK(solid.Volume() > 3.0e5);
  CHECK(solid.Volume() < 3.25e5);
}

TEST_CASE("Arc profile builds across a dome-radius sweep", "[revolve][arc]") {
  // The failure was bit-pattern dependent (R=16.0357 failed, R=14.5 built).
  // Sweep the apex so the arc radius walks through many float32 patterns;
  // every single one must build.
  for (int k = 0; k < 40; k++) {
    const double apex = -9.5 + 0.01 * k;
    auto loops = sketch::BuildLoops(ArcCapProfile(apex));
    REQUIRE(loops.size() == 1);
    CADShape solid = sketch::BuildRevolve(kXZNegY, loops, {0, 0, 0}, {0, 0, 1},
                                          2.0 * M_PI, false, false);
    INFO("apex " << apex);
    REQUIRE(solid.IsValid());
  }
}

TEST_CASE("BuildLoops arc endpoints match the wire builder's rounding",
          "[revolve][arc]") {
  // BuildLoops and MakeLoopWire must derive arc endpoints with the same
  // float32 rounding (center + float(r*cos a)), or the joint midpoints drift
  // from the trimming circle by an extra ulp.
  auto loops = sketch::BuildLoops(ArcCapProfile(-9.5));
  REQUIRE(loops.size() == 1);
  const sketch::LoopEdge* arc = nullptr;
  for (const auto& e : loops[0].edges)
    if (e.arc)
      arc = &e;
  REQUIRE(arc != nullptr);
  const auto onCircle = [&](double ang) {
    return glm::vec2{
        arc->center.x + static_cast<float>(arc->radius * std::cos(ang)),
        arc->center.y + static_cast<float>(arc->radius * std::sin(ang))};
  };
  const glm::vec2 s = onCircle(arc->angStart), en = onCircle(arc->angEnd);
  // BuildLoops may swap a/b to fit the walk; accept either pairing, bitwise.
  const bool fwd = arc->a == s && arc->b == en;
  const bool rev = arc->a == en && arc->b == s;
  CHECK((fwd || rev));
}

// --- Custom axis (v127)
// -------------------------------------------------------

TEST_CASE("Revolve about a custom axis off the origin", "[revolve]") {
  // The same 2 x 3 rectangle, 10 mm further out, turned about a Y-parallel
  // axis through (10, 0, 0): Pappus gives the same 24 pi, and the solid sits
  // round x = 10, not round the world Y axis.
  Document doc;
  uint64_t sid = AddRectSketch(doc, {11, 0}, {13, 3});
  auto rev =
      std::make_unique<RevolveFeature>("Rev", sid, 360.0, ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Custom;
  rev->axis.origin = {10.0, 0.0, 0.0};
  rev->axis.direction = {0.0, 2.0, 0.0}; // need not be unit
  doc.AddFeature(std::move(rev));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(BodyVolume(doc), WithinRel(24.0 * M_PI, 1e-3));
  glm::vec3 lo, hi;
  doc.GetBodies()[0].shape.GetBoundingBox(lo, hi, /*tight=*/true);
  CHECK_THAT(lo.x, WithinAbs(7.0, 1e-3));
  CHECK_THAT(hi.x, WithinAbs(13.0, 1e-3));
}

TEST_CASE("A custom axis out of the sketch plane fails", "[revolve]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {11, 0}, {13, 3});
  auto rev =
      std::make_unique<RevolveFeature>("Rev", sid, 360.0, ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Custom;
  rev->axis.origin = {10.0, 0.0, 5.0}; // 5 mm above the XY sketch
  rev->axis.direction = {0.0, 1.0, 0.0};
  doc.AddFeature(std::move(rev));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("A custom axis round-trips, and a zero direction is refused",
          "[revolve]") {
  AxisRef a;
  a.kind = AxisRef::Kind::Custom;
  a.origin = {1.5, -2.0, 3.25};
  a.direction = {0.0, 0.0, -1.0};
  RevolveFeature rev("Rev", 1, 90.0, ExtrudeMode::NewBody);
  rev.axis = a;
  const nlohmann::json j = DocumentIO::FeatureToJson(rev);
  CHECK(j.at("axis").at("kind") == "custom");
  auto back = DocumentIO::FeatureFromJson(j);
  auto* r = dynamic_cast<RevolveFeature*>(back.get());
  REQUIRE(r);
  CHECK(r->axis.kind == AxisRef::Kind::Custom);
  CHECK(r->axis.origin == glm::dvec3(1.5, -2.0, 3.25));
  CHECK(r->axis.direction == glm::dvec3(0.0, 0.0, -1.0));

  nlohmann::json bad = j;
  bad["axis"]["direction"] = {0, 0, 0};
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(bad),
                    Catch::Matchers::ContainsSubstring("direction is zero"));
  bad["axis"] = {{"kind", "diagonal"}};
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(bad),
                    Catch::Matchers::ContainsSubstring("custom {origin"));
}

// The v127 part golden: a custom-axis revolve and a helix that starts at
// 90 degrees. Written by this recipe (DESIGNER_WRITE_GOLDEN_V127=<path>) and
// loaded here every time, so the file and the code cannot drift.
TEST_CASE("The v127 golden: custom axis and helix start", "[revolve][golden]") {
  Document doc;
  uint64_t sid = AddRectSketch(doc, {11, 0}, {13, 3});
  auto rev =
      std::make_unique<RevolveFeature>("Rev", sid, 360.0, ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Custom;
  rev->axis.origin = {10.0, 0.0, 0.0};
  rev->axis.direction = {0.0, 1.0, 0.0};
  doc.AddFeature(std::move(rev));
  auto cv = std::make_unique<CurveFeature>("Coil", Curve3DKind::Helix);
  cv->axisOrigin = {0, 0, 0};
  cv->axisDir = {0, 0, 1};
  cv->radius = 2.0;
  cv->pitch = 1.0;
  cv->turns = 1.0;
  cv->startDeg = 90.0;
  doc.AddFeature(std::move(cv));
  REQUIRE(doc.Regenerate());
  if (const char* out = std::getenv("DESIGNER_WRITE_GOLDEN_V127"))
    REQUIRE(testio::Save(doc, out));

  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) /
                          "tests/golden/v127-custom-axis-helix-start.json";
  auto g = testio::Load(golden.string());
  REQUIRE(g != nullptr);
  REQUIRE(g->GetBodies().size() == 1);
  CHECK_THAT(g->GetBodies()[0].shape.Volume(), WithinRel(24.0 * M_PI, 1e-3));
  auto* coil = dynamic_cast<CurveFeature*>(g->GetFeature(2));
  REQUIRE(coil);
  CHECK(glm::length(coil->Polyline().front() - glm::vec3(0, 2, 0)) < 1e-3f);
}
