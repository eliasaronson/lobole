#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Measure.h"
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <glm/gtc/matrix_transform.hpp>
#include <gp_Ax2.hxx>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/ThreadStandards.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/io/Serialization.h"

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <filesystem>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
// The stable name of the body's (only) cylindrical face.
std::string CylFaceName(const Body& body) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
    if (s.GetType() == GeomAbs_Cylinder)
      return body.elementMap.names.at(i);
  }
  return {};
}
// A planar face's name (for the must-be-cylindrical error).
std::string PlanarFaceName(const Body& body) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
    if (s.GetType() == GeomAbs_Plane)
      return body.elementMap.names.at(i);
  }
  return {};
}
} // namespace

TEST_CASE("ISO thread table lookups", "[thread]") {
  const auto m8 = threads::Lookup("M8");
  REQUIRE(m8.has_value());
  CHECK_THAT(m8->pitch, WithinRel(1.25, 1e-12));
  CHECK_THAT(threads::MinorDiameter(*m8), WithinAbs(6.647, 1e-3));
  CHECK_THAT(threads::TapDrill(*m8), WithinAbs(6.75, 1e-9));

  const auto fine = threads::Lookup("M8x1");
  REQUIRE(fine.has_value());
  CHECK_THAT(fine->pitch, WithinRel(1.0, 1e-12));

  CHECK_FALSE(threads::Lookup("M7.3").has_value());
  CHECK(threads::All().size() >= 15);
  // The small coarse sizes are in the series, not just the big ones: M2 is
  // what a flight controller stack, a 19 mm camera and a 20x20 video
  // transmitter are all screwed together with.
  for (const char* small : {"M1.6", "M2", "M2.5"}) {
    const auto s = threads::Lookup(small);
    INFO(small);
    REQUIRE(s.has_value());
    CHECK(s->pitch > 0.3);
    CHECK(s->pitch < 0.5);
  }
}

TEST_CASE("Cosmetic thread leaves geometry untouched and fills the overlay",
          "[thread]") {
  Document doc;
  // An M8-sized boss: r = 4, h = 20, along +Z.
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Boss", glm::vec3(0), glm::vec3(0, 0, 1), 4.0, 20.0));
  const Body& body = doc.GetBodies()[0];
  const double v0 = body.shape.Volume();
  const std::string side = CylFaceName(body);
  REQUIRE(!side.empty());

  doc.AddFeature(std::make_unique<ThreadFeature>(
      "Thread", ElementRef{body.createdBy, side}, "M8", 1.25));
  auto* th = dynamic_cast<ThreadFeature*>(doc.GetFeature(1));
  REQUIRE(th != nullptr);
  CHECK(th->GetError().empty());
  // Geometry untouched — bit-exact volume, still one body.
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].shape.Volume() == v0);
  // Overlay: an external boss draws the minor-diameter circle
  // (r - 1.0825·P/2 = 4 - 0.6766 = 3.323), spanning the full face height.
  REQUIRE(th->overlay.valid);
  CHECK(th->overlay.external);
  CHECK_THAT(th->overlay.radius, WithinAbs(4.0 - 1.0825 * 1.25 * 0.5, 1e-4));
  CHECK_THAT(std::abs(th->overlay.v1 - th->overlay.v0), WithinAbs(20.0, 1e-6));

  // A partial thread length clips the span.
  th->length = 8.0;
  doc.Regenerate();
  CHECK_THAT(std::abs(th->overlay.v1 - th->overlay.v0), WithinAbs(8.0, 1e-6));
}

TEST_CASE("Thread designation must fit the face", "[thread]") {
  Document doc;
  // r = 4 face (M8-sized) with an M20 designation: rejected.
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Boss", glm::vec3(0), glm::vec3(0, 0, 1), 4.0, 10.0));
  const Body& body = doc.GetBodies()[0];
  doc.AddFeature(std::make_unique<ThreadFeature>(
      "Thread", ElementRef{body.createdBy, CylFaceName(body)}, "M20", 2.5));
  CHECK(!doc.GetFeature(1)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1); // document survives
}

TEST_CASE("Thread errors on planar and dangling faces", "[thread]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Box", glm::vec3(0), 4, 4, 4));
  // AddFeature regenerates and move-assigns m_bodies, so a Body reference held
  // across it dangles. Capture what the later features need by value up front.
  const Body& body = doc.GetBodies()[0];
  const uint64_t createdBy = body.createdBy;
  const std::string planarFace = PlanarFaceName(body);
  doc.AddFeature(std::make_unique<ThreadFeature>(
      "Flat", ElementRef{createdBy, planarFace}, "M8", 1.25));
  CHECK(doc.GetFeature(1)->GetError().find("cylindrical") != std::string::npos);

  doc.AddFeature(std::make_unique<ThreadFeature>(
      "Gone", ElementRef{createdBy, "nope"}, "M8", 1.25));
  CHECK(!doc.GetFeature(2)->GetError().empty());
}

TEST_CASE("Thread round-trips through serialization",
          "[thread][serialization]") {
  namespace fs = std::filesystem;
  const fs::path path = fs::temp_directory_path() / "designer_thread_test.json";
  {
    Document doc;
    doc.AddFeature(std::make_unique<CylinderFeature>(
        "Boss", glm::vec3(0), glm::vec3(0, 0, 1), 4.0, 20.0));
    const Body& body = doc.GetBodies()[0];
    auto th = std::make_unique<ThreadFeature>(
        "Thread", ElementRef{body.createdBy, CylFaceName(body)}, "M8", 1.25);
    th->threadClass = "6g";
    th->allowEngagement = true;
    th->minimumEngagement = 3;
    th->length = 8.0;
    doc.AddFeature(std::move(th));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* th = dynamic_cast<ThreadFeature*>(loaded->GetFeature(1));
  REQUIRE(th != nullptr);
  CHECK(th->designation == "M8");
  CHECK(th->pitch == 1.25);
  CHECK(th->threadClass == "6g");
  CHECK(th->allowEngagement);
  CHECK(th->minimumEngagement == 3);
  CHECK(th->length == 8.0);
  CHECK_FALSE(th->modeled);
  CHECK(th->GetError().empty());
  CHECK(th->overlay.valid); // recomputed on load's regeneration
  fs::remove(path);
}

TEST_CASE("thread engagement relieves only matching bounded coaxial spans",
          "[thread][measure]") {
  Body screw, nut;
  screw.shape = CADShape(BRepPrimAPI_MakeCylinder(3, 12).Shape());
  nut.shape =
      CADShape(BRepAlgoAPI_Cut(BRepPrimAPI_MakeCylinder(6, 12).Shape(),
                               BRepPrimAPI_MakeCylinder(2.5, 12).Shape())
                   .Shape());
  const auto annotate = [](Body& b, double radius, bool external) {
    int i = 0;
    for (TopExp_Explorer ex(b.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), ++i) {
      BRepAdaptor_Surface surface(TopoDS::Face(ex.Current()));
      if (surface.GetType() == GeomAbs_Cylinder &&
          std::abs(surface.Cylinder().Radius() - radius) < 1e-6) {
        b.elementMap.names[i] = "thread";
        b.threadRegions.push_back({{1, "thread"}, "M6", 0, 3, external});
      }
    }
  };
  annotate(screw, 3, true);
  annotate(nut, 2.5, false);
  REQUIRE(screw.threadRegions.size() == 1);
  REQUIRE(nut.threadRegions.size() == 1);
  const auto check = [&](glm::mat4 xf = glm::mat4(1)) {
    auto c = Measure::ContactBetween({screw}, xf, {nut}, glm::mat4(1), false);
    REQUIRE(c.has_value());
    return c->volume;
  };
  CHECK(check() < 1e-6);
  const auto joined = Measure::ContactBetween({screw}, glm::mat4(1), {nut},
                                              glm::mat4(1), false);
  CHECK(joined->engagedThreadPairs == 1);
  CHECK(joined->threadEngagementVolume > 1);
  // The same two solids the other way round. The relief is measured in the
  // EXTERNAL thread's frame for exactly this reason: a whole-assembly sweep
  // picks the order, so an order-dependent answer means a screw that is
  // engaged when checked one way and clashing when checked the other.
  const auto reversed = Measure::ContactBetween({nut}, glm::mat4(1), {screw},
                                                glm::mat4(1), false);
  REQUIRE(reversed);
  CHECK(reversed->engagedThreadPairs == 1);
  CHECK(reversed->volume < 1e-6);
  const auto separated = Measure::ContactBetween(
      {screw}, glm::translate(glm::mat4(1), glm::vec3(0, 0, 20)), {nut},
      glm::mat4(1), false);
  REQUIRE(separated);
  CHECK(separated->engagedThreadPairs == 0);
  CHECK(check(glm::translate(glm::mat4(1), glm::vec3(0.1, 0, 0))) > 1);
  CHECK(check(glm::translate(glm::mat4(1), glm::vec3(0, 0, 10))) >
        1); // only 2 mm engaged
  nut.threadRegions[0].designation = "M6x0.75";
  CHECK(check() > 1); // a mismatched thread never becomes a contact exclusion
  nut.threadRegions[0].designation = "M6";
  screw.threadRegions[0].length = 4;
  CHECK(check() > 1); // the unthreaded remainder still collides
  screw.threadRegions.clear();
  CHECK(check() > 1); // no unilateral declaration of engagement
}

TEST_CASE("a hole drilled FOR a thread is threaded", "[thread][hole]") {
  // `fit: {thread: "M6", kind: "tapped"}` derives the 5 mm tap drill, which
  // is all the geometry can say. Until the hole recorded the thread as well,
  // nothing downstream knew what it was for: a screw in a correctly drilled,
  // correctly declared tapped hole came back as a CLASH with
  // engaged_thread_pairs 0, because the clearance check relieves an engaged
  // length only between threads it was told about.
  const auto block = [](const std::string& fit) {
    auto doc = std::make_unique<Document>();
    auto pad =
        std::make_unique<SketchFeature>("Block", SketchPlane(PlaneType::XY));
    pad->AddRectangle({-20, -20}, {20, 20});
    doc->AddFeature(std::move(pad));
    doc->AddFeature(std::make_unique<ExtrudeFeature>(
        "Pad", doc->GetFeature(0)->GetId(), 20.0, ExtrudeMode::NewBody));
    auto centre = std::make_unique<SketchFeature>(
        "Centre", SketchPlane(glm::vec3(0, 0, 20), glm::vec3(0, 0, 1),
                              glm::vec3(1, 0, 0)));
    centre->AddEntity(sketch::Point{{0, 0}, 1});
    doc->AddFeature(std::move(centre));
    const uint64_t at = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
    auto hole = std::make_unique<HoleFeature>("Tapping", at, HoleKind::Simple,
                                              5.0, 12.0);
    if (!fit.empty()) {
      hole->fitThread = "M6";
      hole->fitKind = fit;
    }
    doc->AddFeature(std::move(hole));
    doc->Regenerate();
    return doc;
  };
  // An M6 screw standing in the bore: the shank is the major diameter, so it
  // is 0.8 mm of steel wider than the hole all the way round — that overlap
  // IS the thread, and is exactly what must not read as interference.
  Body screw;
  screw.shape = CADShape(
      BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 9), gp_Dir(0, 0, 1)), 3, 10)
          .Shape());
  {
    int i = 0;
    for (TopExp_Explorer ex(screw.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), ++i) {
      BRepAdaptor_Surface surface(TopoDS::Face(ex.Current()));
      if (surface.GetType() == GeomAbs_Cylinder) {
        screw.elementMap.names[i] = "shank";
        screw.threadRegions.push_back({{1, "shank"}, "M6", 0, 0, true});
      }
    }
    REQUIRE(screw.threadRegions.size() == 1);
  }

  const std::unique_ptr<Document> tapped = block("tapped");
  INFO(tapped->GetFeature(tapped->FeatureCount() - 1)->GetError());
  REQUIRE(tapped->GetFeature(tapped->FeatureCount() - 1)->GetError().empty());
  const Body& drilled = tapped->GetBodies()[0];
  REQUIRE(drilled.threadRegions.size() == 1);
  CHECK(drilled.threadRegions[0].designation == "M6");
  CHECK_FALSE(drilled.threadRegions[0].external);
  const auto engaged = Measure::ContactBetween({drilled}, glm::mat4(1), {screw},
                                               glm::mat4(1), false);
  REQUIRE(engaged.has_value());
  CHECK(engaged->engagedThreadPairs == 1);
  CHECK(engaged->volume < 1e-6);
  CHECK(engaged->threadEngagementVolume > 1);

  // The same hole, drilled to the same 5 mm and declared as nothing: the
  // screw in it is interference, and should be.
  const std::unique_ptr<Document> plain = block("");
  const Body& raw = plain->GetBodies()[0];
  CHECK(raw.threadRegions.empty());
  const auto clash = Measure::ContactBetween({raw}, glm::mat4(1), {screw},
                                             glm::mat4(1), false);
  REQUIRE(clash.has_value());
  CHECK(clash->engagedThreadPairs == 0);
  CHECK(clash->volume > 1.0);
}
