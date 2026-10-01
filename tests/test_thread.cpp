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
#include "core/parts/FastenerTable.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <cmath>
#include <filesystem>
#include <optional>

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

// --- unified tables (docs/modeling/holes-and-threads.md#thread-standards) ---

TEST_CASE("unified thread table lookups", "[thread]") {
  // A camera's 1/4-20 and a tripod's 3/8-16: the two threads the pan/tilt
  // head is built around.
  const auto quarter = threads::Lookup("1/4-20 UNC");
  REQUIRE(quarter.has_value());
  CHECK(quarter->series == threads::Series::UNC);
  CHECK_THAT(quarter->majorDia, WithinAbs(6.35, 1e-9));
  CHECK_THAT(quarter->pitch, WithinAbs(1.27, 1e-9));
  CHECK_THAT(threads::TapDrill(*quarter), WithinAbs(5.1054, 1e-4)); // #7
  const auto tripod = threads::Lookup("3/8-16 UNC");
  REQUIRE(tripod.has_value());
  CHECK_THAT(tripod->majorDia, WithinAbs(9.525, 1e-9));
  CHECK_THAT(tripod->pitch, WithinAbs(1.5875, 1e-9));
  CHECK_THAT(threads::TapDrill(*tripod), WithinAbs(7.9375, 1e-9)); // 5/16
  // Basic minor (the 2B minimum minor): 7.807 mm, the table the survey read
  // gives 7.798 for the 2B/3B minimum.
  CHECK_THAT(threads::MinorDiameter(*tripod), WithinAbs(7.807, 0.01));
  // The metric tap drill is still D - P.
  CHECK_THAT(threads::TapDrill(*threads::Lookup("M8")), WithinAbs(6.75, 1e-9));

  // The series reaches from #0 to 1" on both sides.
  for (const char* d : {"#4-40 UNC", "#10-24 UNC", "1-8 UNC", "#0-80 UNF",
                        "#10-32 UNF", "1/4-28 UNF", "1-12 UNF"}) {
    INFO(d);
    CHECK(threads::Lookup(d).has_value());
  }
  // Spellings of the same row: a number size without '#', a metric coarse
  // pitch written out, and a class suffix.
  CHECK(threads::Lookup("10-32 UNF")->designation == "#10-32 UNF");
  CHECK(threads::Lookup("M8x1.25")->designation == "M8");
  std::string why;
  const auto classed = threads::Resolve("3/8-16 UNC-2B", &why);
  REQUIRE(classed.has_value());
  CHECK(classed->standard.designation == "3/8-16 UNC");
  CHECK(classed->threadClass == "2B");
  CHECK(threads::Resolve("M8-6g", &why)->threadClass == "6g");
  // Free text is a custom thread: no row, and no error either.
  CHECK_FALSE(threads::Resolve("Tr16x4", &why).has_value());
  CHECK(why.empty());
}

TEST_CASE("a thread the tables do not carry is refused, naming what is",
          "[thread]") {
  std::string why;
  // Claims to be unified, is not a row: refused with the accepted list.
  CHECK_FALSE(threads::Resolve("3/8-17 UNC", &why).has_value());
  CHECK(why.find("3/8-16 UNC") != std::string::npos);
  // A size and TPI with no series: the row that exists is named, not picked.
  CHECK_FALSE(threads::Resolve("1/4-20", &why).has_value());
  CHECK(why.find("did you mean '1/4-20 UNC'") != std::string::npos);
  // An M size that is not in ISO 262's preferred list.
  CHECK_FALSE(threads::Resolve("M7", &why).has_value());
  CHECK(why.find("M8") != std::string::npos);
  // A metric class on a unified thread.
  CHECK_FALSE(threads::Resolve("3/8-16 UNC-6H", &why).has_value());
  CHECK(why.find("2B") != std::string::npos);
  // A class of the wrong side, and one the series has not got.
  const auto tripod = *threads::Lookup("3/8-16 UNC");
  CHECK(threads::ClassError(tripod, "2B", /*external=*/false).empty());
  CHECK(threads::ClassError(tripod, "2B", true).find("internal") !=
        std::string::npos);
  CHECK(threads::ClassError(tripod, "2C", false).find("1B") !=
        std::string::npos);
  CHECK(threads::ClassError(*threads::Lookup("M8"), "6H", true).find("6g") !=
        std::string::npos);
}

TEST_CASE("a tapped hole takes a unified thread and its tabulated drill",
          "[thread][hole]") {
  std::string why;
  const auto drill =
      parts::HoleDiameter("3/8-16 UNC", parts::Fit::Tapped, 0.0, &why);
  INFO(why);
  REQUIRE(drill.has_value());
  CHECK_THAT(*drill, WithinAbs(7.9375, 1e-9));
  // The TPI is in the designation; a different pitch is a different thread.
  CHECK_FALSE(parts::HoleDiameter("3/8-16 UNC", parts::Fit::Tapped, 1.5, &why));
  CHECK(why.find("1.5875") != std::string::npos);
  // The metric path is untouched.
  CHECK_THAT(*parts::HoleDiameter("M6", parts::Fit::Tapped, 0.0, &why),
             WithinAbs(5.0, 1e-9));
}

TEST_CASE("a Thread refuses a designation or class the tables do not carry",
          "[thread]") {
  Document doc;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Boss", glm::vec3(0), glm::vec3(0, 0, 1), 4.7625, 10.0));
  const uint64_t by = doc.GetBodies()[0].createdBy;
  const std::string side = CylFaceName(doc.GetBodies()[0]);
  doc.AddFeature(std::make_unique<ThreadFeature>("Typo", ElementRef{by, side},
                                                 "3/8-17 UNC", 1.5));
  CHECK(doc.GetFeature(1)->GetError().find("3/8-16 UNC") != std::string::npos);
  // An internal class on a boss.
  doc.AddFeature(std::make_unique<ThreadFeature>("Side", ElementRef{by, side},
                                                 "3/8-16 UNC-2B", 1.5));
  CHECK(doc.GetFeature(2)->GetError().find("internal") != std::string::npos);
  // The designation and the class field disagreeing.
  auto both = std::make_unique<ThreadFeature>("Both", ElementRef{by, side},
                                              "3/8-16 UNC-2A", 1.5);
  both->threadClass = "3A";
  doc.AddFeature(std::move(both));
  CHECK(doc.GetFeature(3)->GetError().find("give one") != std::string::npos);
  // The right one resolves and takes the table's pitch.
  doc.AddFeature(std::make_unique<ThreadFeature>("Good", ElementRef{by, side},
                                                 "3/8-16 UNC-2A", 1.0));
  auto* good = dynamic_cast<ThreadFeature*>(doc.GetFeature(4));
  CHECK(good->GetError().empty());
  CHECK_THAT(good->pitch, WithinAbs(1.5875, 1e-9));
  REQUIRE(doc.GetBodies().size() == 1); // refusals never take the body

  // A hand that is neither is refused on load, not read as right-handed.
  nlohmann::json j = DocumentIO::FeatureToJson(*good);
  j["hand"] = "sideways";
  CHECK_THROWS(DocumentIO::FeatureFromJson(j));
  j["hand"] = "left";
  auto left = DocumentIO::FeatureFromJson(j);
  CHECK_FALSE(dynamic_cast<ThreadFeature*>(left.get())->rightHanded);
}

// --- modelled threads (docs/modeling/holes-and-threads.md#modelled-threads)

namespace {

// The volume a screw-invariant cutter removes from a blank it passes all the
// way through: Pappus-Guldin holds for the screw motion, so each turn takes
// 2*pi*rbar*A and a length L takes L/P turns' worth. A and rbar are the
// groove's section INSIDE the blank, a trapezoid between radii r0 < r1 with
// widths w0 (at r0) and w1 (at r1).
double ScrewCutVolume(double r0, double w0, double r1, double w1, double L,
                      double P) {
  const double h = r1 - r0;
  const double area = 0.5 * (w0 + w1) * h;
  const double fromR0 = h * (w0 + 2.0 * w1) / (3.0 * (w0 + w1));
  return 2.0 * M_PI * (r0 + fromR0) * area * L / P;
}

// A point at `radius` and `angleRad` round +Z at height z.
glm::vec3 Polar(double radius, double angleRad, double z) {
  return glm::vec3(radius * std::cos(angleRad), radius * std::sin(angleRad), z);
}

// Follows a helix of either hand at `radius` from a point the groove removed:
// along the groove's own hand every sample is removed; along the other hand
// the track crosses the teeth. Returns how many of 16 samples over a turn
// are still material.
int SolidAlongHelix(const Body& body, double radius, double z, double pitch,
                    bool rightHanded) {
  int solid = 0;
  for (int k = 1; k <= 16; k++) {
    const double t = k / 16.0;
    const double a = (rightHanded ? 1.0 : -1.0) * 2.0 * M_PI * t;
    if (body.shape.ContainsPoint(Polar(radius, a, z + pitch * t)))
      solid++;
  }
  return solid;
}

// A height near mid-boss where the point at angle 0 and `radius` is removed.
std::optional<double> GrooveHeight(const Body& body, double radius, double z0,
                                   double pitch) {
  for (int i = 0; i < 40; i++) {
    const double z = z0 + pitch * i / 40.0;
    if (!body.shape.ContainsPoint(Polar(radius, 0.0, z)))
      return z;
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("a modelled M8x1.25 cuts the ISO groove in either hand",
          "[thread][slow]") {
  const double P = 1.25, H = P * std::sqrt(3.0) / 2.0, R = 4.0;
  const auto build = [&](bool right) {
    auto doc = std::make_unique<Document>();
    doc->AddFeature(std::make_unique<CylinderFeature>(
        "Boss", glm::vec3(0), glm::vec3(0, 0, 1), R, 20.0));
    const Body& body = doc->GetBodies()[0];
    auto th = std::make_unique<ThreadFeature>(
        "Thread", ElementRef{body.createdBy, CylFaceName(body)}, "M8", P);
    th->modeled = true;
    th->rightHanded = right;
    doc->AddFeature(std::move(th));
    return doc;
  };
  const double blank = M_PI * R * R * 20.0;
  // Both ends open, so the groove runs out through both: the removed volume
  // is exact. The section inside the blank runs from the basic minor (root
  // flat P/4) to the major (7P/8 wide), 5H/8 deep.
  const double groove =
      ScrewCutVolume(R - 5.0 * H / 8.0, P / 4.0, R, 7.0 * P / 8.0, 20.0, P);

  auto right = build(true);
  const auto* th = right->GetFeature(1);
  INFO(th->GetError());
  REQUIRE(th->GetError().empty());
  REQUIRE(right->GetBodies().size() == 1);
  const Body& rb = right->GetBodies()[0];
  const double vr = rb.shape.Volume();
  CHECK(vr < blank);
  CHECK_THAT(blank - vr, WithinRel(groove, 5e-3));
  // Regenerated twice, the same solid.
  right->Regenerate();
  right->Regenerate();
  CHECK_THAT(right->GetBodies()[0].shape.Volume(), WithinRel(vr, 1e-9));
  CHECK(dynamic_cast<const ThreadFeature*>(right->GetFeature(1))->overlay.cut);

  auto left = build(false);
  REQUIRE(left->GetFeature(1)->GetError().empty());
  const Body& lb = left->GetBodies()[0];
  CHECK_THAT(lb.shape.Volume(), WithinRel(vr, 1e-4));

  // The hand, read off the solid: from a removed point just above the root,
  // a helix of the thread's own hand stays in the groove for a whole turn,
  // and one of the other hand crosses the teeth.
  const double probe = R - 5.0 * H / 8.0 + 0.1;
  const auto zr = GrooveHeight(rb, probe, 8.0, P);
  REQUIRE(zr.has_value());
  CHECK(SolidAlongHelix(rb, probe, *zr, P, true) == 0);
  CHECK(SolidAlongHelix(rb, probe, *zr, P, false) > 4);
  const auto zl = GrooveHeight(lb, probe, 8.0, P);
  REQUIRE(zl.has_value());
  CHECK(SolidAlongHelix(lb, probe, *zl, P, false) == 0);
  CHECK(SolidAlongHelix(lb, probe, *zl, P, true) > 4);
}

TEST_CASE("a modelled thread stops square at a shoulder", "[thread]") {
  // A bolt-like part: a boss standing on a plate. The groove runs out
  // through the free end and stops square at the plate, cutting nothing of
  // the plate.
  Document doc;
  auto plate =
      std::make_unique<SketchFeature>("Plate", SketchPlane(PlaneType::XY));
  plate->AddRectangle({-10, -10}, {10, 10});
  doc.AddFeature(std::move(plate));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "PlatePad", doc.GetFeature(0)->GetId(), 5.0, ExtrudeMode::NewBody));
  auto boss = std::make_unique<SketchFeature>(
      "BossSketch",
      SketchPlane(glm::vec3(0, 0, 5), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  boss->AddEntity(sketch::Circle{{0, 0}, 4.0});
  doc.AddFeature(std::move(boss));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "BossPad", doc.GetFeature(2)->GetId(), 12.0, ExtrudeMode::Add));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  const double before = body.shape.Volume();
  auto th = std::make_unique<ThreadFeature>(
      "Thread", ElementRef{body.createdBy, CylFaceName(body)}, "M8-6g", 1.0);
  th->modeled = true;
  doc.AddFeature(std::move(th));
  INFO(doc.GetFeature(4)->GetError());
  REQUIRE(doc.GetFeature(4)->GetError().empty());
  const Body& cut = doc.GetBodies()[0];
  const double P = 1.25, H = P * std::sqrt(3.0) / 2.0;
  const double groove =
      ScrewCutVolume(4.0 - 5.0 * H / 8.0, P / 4.0, 4.0, 7.0 * P / 8.0, 12.0, P);
  CHECK_THAT(before - cut.shape.Volume(), WithinRel(groove, 5e-3));
  // The plate is whole: a point just under its top face, inside the groove's
  // radial band, is still material all the way round.
  for (int k = 0; k < 12; k++)
    CHECK(cut.shape.ContainsPoint(Polar(3.6, k * M_PI / 6.0, 4.95)));
}

namespace {

// The planar faces of `body` lying in z = `z` (normal along Z).
std::vector<int> FacesInPlane(const Body& body, double z) {
  std::vector<int> out;
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
    if (s.GetType() != GeomAbs_Plane)
      continue;
    const gp_Pln pl = s.Plane();
    if (std::abs(std::abs(pl.Axis().Direction().Z()) - 1.0) < 1e-9 &&
        std::abs(pl.Location().Z() - z) < 1e-6)
      out.push_back(i);
  }
  return out;
}

TopoDS_Face FaceAt(const Body& body, int index) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_FACE, faces);
  return TopoDS::Face(faces(index + 1));
}

double FaceArea(const TopoDS_Face& f) {
  GProp_GProps props;
  BRepGProp::SurfaceProperties(f, props);
  return props.Mass();
}

} // namespace

TEST_CASE("a thread squared on a shoulder leaves no stray edges on it",
          "[thread]") {
  // An M8 shank on a plate, as a bolt's shank stands on its head. The
  // thread's tool ends ON the shoulder, so the cut drew its footprint there:
  // arcs on the shoulder's face that bound no volume. They are merged away
  // -- only on the faces the thread touched, so every other face keeps its
  // name, and the shoulder keeps its own.
  Document doc;
  auto plate =
      std::make_unique<SketchFeature>("Plate", SketchPlane(PlaneType::XY));
  plate->AddRectangle({-10, -10}, {10, 10});
  doc.AddFeature(std::move(plate));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "PlatePad", doc.GetFeature(0)->GetId(), 5.0, ExtrudeMode::NewBody));
  auto boss = std::make_unique<SketchFeature>(
      "BossSketch",
      SketchPlane(glm::vec3(0, 0, 5), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  boss->AddEntity(sketch::Circle{{0, 0}, 4.0});
  doc.AddFeature(std::move(boss));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "BossPad", doc.GetFeature(2)->GetId(), 12.0, ExtrudeMode::Add));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body before = doc.GetBodies()[0];
  const std::vector<int> shoulderBefore = FacesInPlane(before, 5.0);
  REQUIRE(shoulderBefore.size() == 1);
  const std::string shoulderName =
      before.elementMap.names.at(shoulderBefore[0]);
  const double shoulderArea = FaceArea(FaceAt(before, shoulderBefore[0]));
  const std::string shankName = CylFaceName(before);

  auto th = std::make_unique<ThreadFeature>(
      "Thread", ElementRef{before.createdBy, shankName}, "M8-6g", 1.0);
  th->modeled = true;
  doc.AddFeature(std::move(th));
  const uint64_t threadId = doc.GetFeature(4)->GetId();
  INFO(doc.GetFeature(4)->GetError());
  REQUIRE(doc.GetFeature(4)->GetError().empty());
  const Body& cut = doc.GetBodies()[0];

  // The shoulder is ONE face, named for the shoulder it was.
  const std::vector<int> shoulder = FacesInPlane(cut, 5.0);
  for (int f : shoulder)
    UNSCOPED_INFO("face " << f << " in the shoulder's plane: "
                          << cut.elementMap.names.at(f) << ", area "
                          << FaceArea(FaceAt(cut, f)));
  REQUIRE(shoulder.size() == 1);
  CHECK(cut.elementMap.names.at(shoulder[0]) ==
        shoulderName + ";M" + std::to_string(threadId));
  // ...and has no stray edge: every edge of it borders exactly one other
  // face, and that face turns away from the shoulder's plane (a plate side,
  // the shank, a groove flank).
  const TopoDS_Face face = FaceAt(cut, shoulder[0]);
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndUniqueAncestors(cut.shape.GetShape(), TopAbs_EDGE,
                                      TopAbs_FACE, edgeFaces);
  int edges = 0;
  for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next(), edges++) {
    const TopTools_ListOfShape& adj = edgeFaces.FindFromKey(ex.Current());
    CHECK(adj.Extent() == 2);
    for (const TopoDS_Shape& other : adj) {
      if (other.IsSame(face))
        continue;
      const BRepAdaptor_Surface s(TopoDS::Face(other));
      CHECK((s.GetType() != GeomAbs_Plane ||
             std::abs(s.Plane().Axis().Direction().Z()) < 0.5));
    }
  }
  CHECK(edges > 4);
  // The groove's end opens onto it: the shoulder grew by the groove's
  // footprint inside the shank, and lost nothing.
  CHECK(FaceArea(face) > shoulderArea);

  // The faces of the body the thread did not touch -- the plate's sides and
  // bottom -- keep their names and their faces.
  int kept = 0;
  for (const auto& [index, name] : before.elementMap.names) {
    if (index == shoulderBefore[0] || name == shankName)
      continue;
    const std::optional<int> now = cut.elementMap.Resolve(name);
    if (!now)
      continue; // the shank's top: notched by the thread, so ";M"
    CHECK_THAT(FaceArea(FaceAt(cut, *now)),
               WithinRel(FaceArea(FaceAt(before, index)), 1e-9));
    kept++;
  }
  CHECK(kept == 5);
  // Nothing was named afresh (a Refine renames every face): each face is a
  // face of the body before, or of the thread's tool, carried through.
  for (const auto& [index, name] : cut.elementMap.names) {
    INFO(name);
    bool carried = name.rfind(std::to_string(threadId) + "/face", 0) == 0;
    for (const auto& [i, was] : before.elementMap.names)
      carried = carried || name.rfind(was, 0) == 0;
    CHECK(carried);
  }
}

TEST_CASE("a modelled 3/8-16 UNC-2B cuts the groove in a tapped hole wall",
          "[thread][hole]") {
  // A 10 mm plate, the tripod thread tapped through it.
  Document doc;
  auto pad =
      std::make_unique<SketchFeature>("Block", SketchPlane(PlaneType::XY));
  pad->AddRectangle({-15, -15}, {15, 15});
  doc.AddFeature(std::move(pad));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Pad", doc.GetFeature(0)->GetId(), 10.0, ExtrudeMode::NewBody));
  auto centre = std::make_unique<SketchFeature>(
      "Centre",
      SketchPlane(glm::vec3(0, 0, 10), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  centre->AddEntity(sketch::Point{{0, 0}, 1});
  doc.AddFeature(std::move(centre));
  auto hole = std::make_unique<HoleFeature>(
      "Tapping", doc.GetFeature(2)->GetId(), HoleKind::Simple, 1.0, 10.0);
  hole->through = true;
  hole->fitThread = "3/8-16 UNC";
  hole->fitKind = "tapped";
  doc.AddFeature(std::move(hole));
  INFO(doc.GetFeature(3)->GetError());
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  const Body& drilled = doc.GetBodies()[0];
  const double before = drilled.shape.Volume();
  CHECK_THAT(before,
             WithinRel(30.0 * 30.0 * 10.0 - M_PI * 3.96875 * 3.96875 * 10.0,
                       1e-6)); // the 5/16" drill

  auto th = std::make_unique<ThreadFeature>(
      "Tripod", ElementRef{drilled.createdBy, CylFaceName(drilled)},
      "3/8-16 UNC-2B", 1.0);
  th->modeled = true;
  doc.AddFeature(std::move(th));
  const auto* feature = doc.GetFeature(4);
  INFO(feature->GetError());
  REQUIRE(feature->GetError().empty());
  CHECK_FALSE(dynamic_cast<const ThreadFeature*>(feature)->overlay.external);
  // The section inside the wall: from the drilled radius (widest) out to
  // the major, where the internal root flat is P/8.
  const double P = 1.5875, R = 4.7625, r = 3.96875;
  const double tan30 = std::tan(M_PI / 6.0);
  const double groove =
      ScrewCutVolume(r, P / 8.0 + 2.0 * (R - r) * tan30, R, P / 8.0, 10.0, P);
  const double after = doc.GetBodies()[0].shape.Volume();
  CHECK_THAT(before - after, WithinRel(groove, 5e-3));
}

TEST_CASE("a modelled thread that cannot be cut keeps the body", "[thread]") {
  // A custom 3 mm pitch on a 2 mm boss: the root of that groove would sit
  // below the axis. The feature errors, the body and the overlay stay.
  Document doc;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Pin", glm::vec3(0), glm::vec3(0, 0, 1), 1.0, 10.0));
  const Body& body = doc.GetBodies()[0];
  const double before = body.shape.Volume();
  auto th = std::make_unique<ThreadFeature>(
      "Coarse", ElementRef{body.createdBy, CylFaceName(body)}, "custom", 3.0);
  th->modeled = true;
  doc.AddFeature(std::move(th));
  const auto* feature = dynamic_cast<const ThreadFeature*>(doc.GetFeature(1));
  CHECK_FALSE(feature->GetError().empty());
  CHECK(feature->overlay.valid);
  CHECK_FALSE(feature->overlay.cut);
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].shape.Volume() == before);
}
