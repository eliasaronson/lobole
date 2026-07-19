#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/ThreadStandards.h"
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
  const Body& body = doc.GetBodies()[0];
  doc.AddFeature(std::make_unique<ThreadFeature>(
      "Flat", ElementRef{body.createdBy, PlanarFaceName(body)}, "M8", 1.25));
  CHECK(doc.GetFeature(1)->GetError().find("cylindrical") !=
        std::string::npos);

  doc.AddFeature(std::make_unique<ThreadFeature>(
      "Gone", ElementRef{body.createdBy, "nope"}, "M8", 1.25));
  CHECK(!doc.GetFeature(2)->GetError().empty());
}

TEST_CASE("Thread round-trips through serialization", "[thread][serialization]") {
  namespace fs = std::filesystem;
  const fs::path path =
      fs::temp_directory_path() / "designer_thread_test.json";
  {
    Document doc;
    doc.AddFeature(std::make_unique<CylinderFeature>(
        "Boss", glm::vec3(0), glm::vec3(0, 0, 1), 4.0, 20.0));
    const Body& body = doc.GetBodies()[0];
    auto th = std::make_unique<ThreadFeature>(
        "Thread", ElementRef{body.createdBy, CylFaceName(body)}, "M8", 1.25);
    th->threadClass = "6g";
    th->length = 8.0;
    doc.AddFeature(std::move(th));
    REQUIRE(DocumentIO::Save(doc, path.string()));
  }
  const auto loaded = DocumentIO::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* th = dynamic_cast<ThreadFeature*>(loaded->GetFeature(1));
  REQUIRE(th != nullptr);
  CHECK(th->designation == "M8");
  CHECK(th->pitch == 1.25);
  CHECK(th->threadClass == "6g");
  CHECK(th->length == 8.0);
  CHECK_FALSE(th->modeled);
  CHECK(th->GetError().empty());
  CHECK(th->overlay.valid); // recomputed on load's regeneration
  fs::remove(path);
}
