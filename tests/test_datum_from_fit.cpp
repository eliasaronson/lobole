// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Datums on a scan (docs/modeling/README.md#datums-on-a-scan): a DatumPlane
// or DatumAxis `from_fit` refits its primitive on a body's triangles every
// regeneration, so it follows a re-scan, and a sketch, a mate and a
// measurement reach the scan through it. Each checked against the surface
// the scan was generated on, through a real regeneration.

#include "TestIO.h"
#include "TestPaths.h"
#include "mesh_organic.h"

#include "core/AssemblySolve.h"
#include "core/DatumFrame.h"
#include "core/Document.h"
#include "core/MeshFit.h"
#include "core/Occurrence.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = 3.14159265358979323846;

double LineAngleDeg(const glm::dvec3& a, const glm::dvec3& b) {
  const double c = std::abs(glm::dot(glm::normalize(a), glm::normalize(b)));
  return std::acos(std::min(1.0, c)) * 180.0 / kPi;
}

double FromLine(const glm::dvec3& p, const glm::dvec3& o, const glm::dvec3& d) {
  const glm::dvec3 w = p - o;
  return glm::length(w - glm::dot(w, d) * d);
}

std::unique_ptr<ImportFeature> ScanImport(const fs::path& stl) {
  auto imp = std::make_unique<ImportFeature>("scan", stl.string());
  imp->upAxis = interchange::UpAxis::None;
  imp->representation = mesh::Representation::Mesh;
  imp->keepOrigin = true; // the scan where it was generated
  return imp;
}

// A part holding the post's scan, an axis fitted to its cylinder and a
// plane to its top.
struct ScanPart {
  fixtures::Post post = fixtures::ScannedPost(40000);
  fs::path stl = fixtures::WriteStl(post.organic.mesh, "datum-from-fit");
  Document doc;
  uint64_t importId = 0, axisId = 0, planeId = 0;
  ScanPart() {
    doc.AddFeature(ScanImport(stl));
    importId = doc.GetFeature(0)->GetId();
    auto axis =
        std::make_unique<DatumAxisFeature>("Bore", DatumAxisKind::FromFit);
    axis->fit = {importId, post.onCylinder, mesh::Kind::Cylinder, {}};
    doc.AddFeature(std::move(axis));
    axisId = doc.GetFeature(1)->GetId();
    auto plane =
        std::make_unique<DatumPlaneFeature>("Top", DatumPlaneKind::FromFit);
    plane->fit = {importId, post.onTop, mesh::Kind::Plane, {}};
    doc.AddFeature(std::move(plane));
    planeId = doc.GetFeature(2)->GetId();
    REQUIRE(doc.Regenerate());
  }
  ~ScanPart() { fs::remove(stl); }
  const DatumAxisFeature& Axis() const {
    return *dynamic_cast<const DatumAxisFeature*>(doc.GetFeature(1));
  }
  const DatumPlaneFeature& Plane() const {
    return *dynamic_cast<const DatumPlaneFeature*>(doc.GetFeature(2));
  }
};

} // namespace

TEST_CASE("A datum from_fit is the scan's cylinder axis and top plane",
          "[datum][fit][mesh_body]") {
  ScanPart s;
  const DatumAxisFeature& axis = s.Axis();
  REQUIRE(axis.GetError().empty());
  std::printf("datum axis: %.4f deg off, %.4f mm off the true axis\n",
              LineAngleDeg(axis.Direction(), s.post.axis),
              FromLine(axis.Origin(), s.post.base, s.post.axis));
  CHECK(LineAngleDeg(axis.Direction(), s.post.axis) < 0.1);
  CHECK(FromLine(axis.Origin(), s.post.base, s.post.axis) < 0.05);
  // The axis point nearest the seed.
  CHECK(std::abs(glm::dot(axis.Origin() - s.post.onCylinder, s.post.axis)) <
        0.05);
  CHECK_THAT(axis.FitReport().at("radius_mm").get<double>(),
             WithinAbs(s.post.radius, 0.02));

  const DatumPlaneFeature& plane = s.Plane();
  REQUIRE(plane.GetError().empty());
  const SketchPlane& p = plane.OutputPlane();
  CHECK(glm::dot(p.GetNormal(), s.post.axis) > std::cos(0.2 * kPi / 180.0));
  CHECK_THAT(glm::dot(p.GetOrigin() - s.post.base, s.post.axis),
             WithinAbs(s.post.height + s.post.coneHeight, 0.02));
}

TEST_CASE("A datum from_fit follows a re-scan", "[datum][fit][mesh_body]") {
  ScanPart s;
  const glm::dvec3 before = s.Axis().Origin();
  // The same object scanned again: other triangles, other noise, standing
  // half a millimetre over.
  const glm::dvec3 moved(0.5, -0.3, 0.0);
  const fixtures::Post again = fixtures::ScannedPost(52000, 0.04);
  const fs::path stl2 = fixtures::WriteStl(
      fixtures::Translated(again.organic.mesh, moved), "datum-rescan");
  dynamic_cast<ImportFeature*>(s.doc.GetFeature(0))->path = stl2.string();
  REQUIRE(s.doc.Regenerate());
  const DatumAxisFeature& axis = s.Axis();
  REQUIRE(axis.GetError().empty());
  CHECK(FromLine(axis.Origin(), again.base + moved, again.axis) < 0.05);
  CHECK(FromLine(before, again.base + moved, again.axis) > 0.4);
  const DatumPlaneFeature& plane = s.Plane();
  REQUIRE(plane.GetError().empty());
  CHECK_THAT(glm::dot(plane.OutputPlane().GetOrigin() - (again.base + moved),
                      again.axis),
             WithinAbs(again.height + again.coneHeight, 0.02));
  fs::remove(stl2);
}

TEST_CASE("A datum from_fit refits to the same datum every regeneration",
          "[datum][fit][mesh_body]") {
  ScanPart s;
  const glm::dvec3 o = s.Axis().Origin(), d = s.Axis().Direction();
  const glm::dvec3 n = s.Plane().OutputPlane().GetNormal();
  s.doc.ClearRegenCache();
  REQUIRE(s.doc.Regenerate());
  CHECK(s.Axis().Origin() == o); // to the bit
  CHECK(s.Axis().Direction() == d);
  CHECK(s.Plane().OutputPlane().GetNormal() == n);
}

TEST_CASE("A datum from_fit reads back as written, and refuses what it is "
          "not",
          "[datum][fit][serialize]") {
  ScanPart s;
  const nlohmann::json a = DocumentIO::FeatureToJson(s.Axis());
  CHECK(a.at("kind") == "from_fit");
  CHECK(a.at("fit").at("kind") == "cylinder");
  CHECK(a.at("fit").at("body_id") == s.importId);
  const auto back = DocumentIO::FeatureFromJson(a);
  CHECK(DocumentIO::FeatureToJson(*back) == a);

  nlohmann::json p = DocumentIO::FeatureToJson(s.Plane());
  p["fit"]["tolerance_mm"] = 0.2;
  const auto pback = DocumentIO::FeatureFromJson(p);
  const auto* dp = dynamic_cast<const DatumPlaneFeature*>(pback.get());
  REQUIRE(dp != nullptr);
  REQUIRE(dp->fit.toleranceMm);
  CHECK(*dp->fit.toleranceMm == 0.2);

  const auto refused = [](nlohmann::json j, const char* needle) {
    INFO(j.dump());
    try {
      (void)DocumentIO::FeatureFromJson(j);
      FAIL("accepted");
    } catch (const std::exception& e) {
      CHECK_THAT(std::string(e.what()), ContainsSubstring(needle));
    }
  };
  nlohmann::json j = a;
  j["fit"]["kind"] = "auto"; // a datum keeps the kind it was fitted as
  refused(j, "accepted: cylinder, cone");
  j = a;
  j["fit"]["kind"] = "plane"; // an axis has no plane
  refused(j, "accepted: cylinder, cone");
  j = p;
  j["fit"]["kind"] = "cylinder";
  refused(j, "accepted: plane");
  j = a;
  j["fit"]["tolerance_mm"] = 0.0;
  refused(j, "tolerance_mm");
  j = a;
  j["fit"]["body_id"] = 0;
  refused(j, "body_id");
  j = a;
  j["fit"]["seed"] = 1;
  refused(j, "seed");
  j = a;
  j["kind"] = "from_fitt";
  refused(j, "from_fit");
}

TEST_CASE("A datum from_fit says why it cannot fit", "[datum][fit]") {
  ScanPart s;
  SECTION("a point off the scan") {
    dynamic_cast<DatumAxisFeature*>(s.doc.GetFeature(1))->fit.pointMm =
        s.post.base + glm::dvec3(200.0, 0.0, 0.0);
    s.doc.Regenerate();
    CHECK_THAT(s.Axis().GetError(), ContainsSubstring("from the nearest"));
    CHECK(s.Axis().FitReport().is_null());
  }
  SECTION("a cylinder asked of the flat top") {
    dynamic_cast<DatumAxisFeature*>(s.doc.GetFeature(1))->fit.pointMm =
        s.post.onTop;
    s.doc.Regenerate();
    CHECK_THAT(s.Axis().GetError(), ContainsSubstring("flat"));
    // A failed datum leaves the default, never the last good one.
    CHECK(s.Axis().Direction() == glm::dvec3(1.0, 0.0, 0.0));
  }
}

TEST_CASE("A datum from_fit over two surfaces fails naming both",
          "[datum][fit]") {
  // A bore stepping from R4.68 to R5.18 at 12.4 mm, fitted at a tolerance
  // that takes both (docs/modeling/README.md#two-surfaces): the datum fails
  // with fit_primitive's sentence, and its output is the default.
  const fixtures::Bore b = fixtures::ScannedBore(
      [](double h) { return h < 12.4 ? 4.68 : 5.18; }, 25.0, 0.25, 0.05);
  const fs::path stl = fixtures::WriteStl(b.organic.mesh, "two-surfaces");
  Document doc;
  doc.AddFeature(ScanImport(stl));
  const uint64_t importId = doc.GetFeature(0)->GetId();
  auto axis =
      std::make_unique<DatumAxisFeature>("Bore", DatumAxisKind::FromFit);
  axis->fit = {importId, b.At(6.0, 30.0), mesh::Kind::Cylinder, 0.6};
  doc.AddFeature(std::move(axis));
  doc.Regenerate();
  const auto& a = *dynamic_cast<const DatumAxisFeature*>(doc.GetFeature(1));
  CHECK_THAT(a.GetError(),
             ContainsSubstring("two cylinders here: R4.68 mm and R5.18 mm, "
                               "stepping at 12.4 mm along the axis"));
  CHECK(a.FitReport().is_null());
  CHECK(a.Direction() == glm::dvec3(1.0, 0.0, 0.0));
  fs::remove(stl);
}

TEST_CASE("A sketch on a fitted plane stands on the scan's top",
          "[datum][fit][sketch]") {
  ScanPart s;
  auto sk =
      std::make_unique<SketchFeature>("On top", SketchPlane(PlaneType::XY));
  sk->planeRef = s.planeId;
  sk->AddEntity(sketch::Circle{{0, 0}, 3.0});
  s.doc.AddFeature(std::move(sk));
  REQUIRE(s.doc.Regenerate());
  const auto* placed = dynamic_cast<const SketchFeature*>(s.doc.GetFeature(3));
  REQUIRE(placed->GetError().empty());
  CHECK_THAT(glm::dot(placed->plane.GetOrigin() - s.post.base, s.post.axis),
             WithinAbs(s.post.height + s.post.coneHeight, 0.02));
  CHECK(LineAngleDeg(placed->plane.GetNormal(), s.post.axis) < 0.2);
}

TEST_CASE("A part mated to a scan's fitted axis stands on that axis",
          "[datum][fit][assembly]") {
  ScanPart s;
  Document pin; // a part with nothing but its origin
  const assembly::PartResolver resolve = [&](uint64_t partId, uint64_t) {
    return partId == 1 ? &s.doc : &pin;
  };
  AssemblyDocument asmDoc;
  Occurrence scan;
  scan.partId = 1;
  scan.fixed = true;
  asmDoc.AddOccurrence(scan);
  Occurrence p;
  p.partId = 2;
  p.position = {40, -20, 15};
  asmDoc.AddOccurrence(p);
  Mate m;
  m.type = MateType::Fastened;
  m.a.occurrenceId = 1;
  m.a.anchor.frameId = s.axisId;
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  asmDoc.AddMate(m);
  REQUIRE(assembly::Solve(asmDoc, resolve).erroredMates == 0);
  const Occurrence& placed = *asmDoc.FindById(2);
  const glm::dvec3 z =
      glm::dmat3(glm::mat4_cast(placed.orientation)) * glm::dvec3(0, 0, 1);
  std::printf("mated pin: %.4f deg off the axis, %.4f mm off it\n",
              LineAngleDeg(z, s.post.axis),
              FromLine(placed.position, s.post.base, s.post.axis));
  CHECK(LineAngleDeg(z, s.post.axis) < 0.1);
  CHECK(FromLine(placed.position, s.post.base, s.post.axis) < 0.05);
  // And the anchor is the frame the `frame` query reports.
  const glm::dmat4 f = datumframe::Matrix(s.Axis());
  CHECK(glm::length(glm::dvec3(f[3]) - s.Axis().Origin()) < 1e-12);
  CHECK(glm::length(glm::dvec3(f[2]) - s.Axis().Direction()) < 1e-12);
}

namespace {

// A part holding a cup's scan alone.
struct CupPart {
  fixtures::Cup cup = fixtures::ScannedCup(60000);
  fs::path stl = fixtures::WriteStl(cup.organic.mesh, "fit-hints-cup");
  Document doc;
  uint64_t importId = 0;
  CupPart() {
    doc.AddFeature(ScanImport(stl));
    importId = doc.GetFeature(0)->GetId();
    REQUIRE(doc.Regenerate());
  }
  ~CupPart() { fs::remove(stl); }
  bool Fit(const meshfit::Request& r, meshfit::Result& out,
           std::string& error) const {
    return meshfit::Fit(meshfit::BodiesOf(doc.GetBodies(), importId), r, out,
                        error);
  }
};

} // namespace

TEST_CASE("A side hint fits a cup's inside or its outside from its rim",
          "[datum][fit][mesh_body]") {
  // Clicked on the rim, between the two walls: the nearest surface is the
  // rim's flat, and the walls are each 0.6 mm off.
  CupPart c;
  for (const bool outside : {false, true}) {
    meshfit::Request r;
    r.pointMm = c.cup.onRim;
    r.kind = mesh::Kind::Cylinder;
    r.hints.outside = outside;
    meshfit::Result res;
    std::string error;
    INFO((outside ? "outside" : "inside"));
    REQUIRE(c.Fit(r, res, error));
    INFO(error);
    const mesh::FitResult& f = res.region.fit;
    const double want = outside ? c.cup.outerRadius : c.cup.innerRadius;
    std::printf("cup rim, side %s: radius %.4f (%.1f), axis %.4f deg %.4f mm "
                "off, %zu seeds tried\n",
                outside ? "outside" : "inside", f.radius, want,
                LineAngleDeg(f.axis, c.cup.axis),
                FromLine(c.cup.base, f.origin, f.axis), res.seedsTried);
    CHECK_THAT(f.radius, WithinAbs(want, 0.03));
    CHECK(LineAngleDeg(f.axis, c.cup.axis) < 0.1);
    CHECK(FromLine(c.cup.base, f.origin, f.axis) < 0.05);
    CHECK(res.seedsTried > 1); // the rim's flat was tried and passed by
  }
  SECTION("a hint the surface cannot meet is refused, naming it") {
    meshfit::Request r;
    r.pointMm = c.cup.onRim;
    r.kind = mesh::Kind::Cylinder;
    r.hints.outside = false;
    r.hints.radiusMin = 20.0;
    r.hints.radiusMax = 30.0;
    meshfit::Result res;
    std::string error;
    CHECK_FALSE(c.Fit(r, res, error));
    CHECK_THAT(error, ContainsSubstring("radius_mm [20, 30]"));
  }
  SECTION("an axis hint off the cup's axis is refused, naming it") {
    meshfit::Request r;
    r.pointMm = c.cup.onRim;
    r.kind = mesh::Kind::Cylinder;
    r.hints.outside = true;
    // 20 degrees off the cup's axis, held to 5.
    const glm::dvec3 across =
        glm::normalize(glm::cross(c.cup.axis, glm::dvec3(1, 0, 0)));
    r.hints.axis = std::cos(20.0 * kPi / 180.0) * c.cup.axis +
                   std::sin(20.0 * kPi / 180.0) * across;
    r.hints.axisToleranceRad = 5.0 * kPi / 180.0;
    meshfit::Result res;
    std::string error;
    const bool fitted = c.Fit(r, res, error);
    INFO(meshfit::ResultJson(res).dump());
    CHECK_FALSE(fitted);
    CHECK_THAT(error, ContainsSubstring("axis"));
  }
  SECTION("a hint the kind cannot take is refused before anything is read") {
    meshfit::Request r;
    r.pointMm = c.cup.onRim;
    r.kind = mesh::Kind::Plane;
    r.hints.outside = true;
    meshfit::Result res;
    std::string error;
    CHECK_THROWS_WITH(c.Fit(r, res, error),
                      ContainsSubstring("side is for a curved kind"));
  }
}

TEST_CASE("A datum from_fit keeps its hints and its clip, and refits by them",
          "[datum][fit][serialize]") {
  CupPart c;
  auto axis =
      std::make_unique<DatumAxisFeature>("Bore", DatumAxisKind::FromFit);
  axis->fit = {c.importId, c.cup.onRim, mesh::Kind::Cylinder, {}};
  axis->fit.hints.outside = false;
  axis->fit.hints.radiusMin = 12.0;
  axis->fit.hints.radiusMax = 15.0;
  axis->fit.clipSigma = 2;
  c.doc.AddFeature(std::move(axis));
  REQUIRE(c.doc.Regenerate());
  const auto& a = *dynamic_cast<const DatumAxisFeature*>(c.doc.GetFeature(1));
  INFO(a.GetError());
  REQUIRE(a.GetError().empty());
  CHECK_THAT(a.FitReport().at("radius_mm").get<double>(),
             WithinAbs(c.cup.innerRadius, 0.03));
  CHECK(a.FitReport().at("clip_sigma") == 2);
  const nlohmann::json j = DocumentIO::FeatureToJson(a);
  CHECK(j.at("fit").at("side") == "inside");
  CHECK(j.at("fit").at("clip_sigma") == 2);
  CHECK(j.at("fit").at("radius_mm") == nlohmann::json{12.0, 15.0});
  CHECK_FALSE(j.at("fit").contains("axis"));
  const auto back = DocumentIO::FeatureFromJson(j);
  CHECK(DocumentIO::FeatureToJson(*back) == j);
  // Written only when given: a datum fitted without them writes neither.
  ScanPart s;
  const nlohmann::json plain = DocumentIO::FeatureToJson(s.Axis());
  CHECK_FALSE(plain.at("fit").contains("clip_sigma"));
  CHECK_FALSE(plain.at("fit").contains("side"));

  const auto refused = [](nlohmann::json bad, const char* needle) {
    INFO(bad.dump());
    try {
      (void)DocumentIO::FeatureFromJson(bad);
      FAIL("accepted");
    } catch (const std::exception& e) {
      CHECK_THAT(std::string(e.what()), ContainsSubstring(needle));
    }
  };
  nlohmann::json b = j;
  b["fit"]["clip_sigma"] = 4;
  refused(b, "1, 2, 3 or \"all\"");
  b = j;
  b["fit"]["clip_sigma"] = "most";
  refused(b, "1, 2, 3 or \"all\"");
  b = j;
  b["fit"]["side"] = "within";
  refused(b, "inside");
  b = j;
  b["fit"]["radius_mm"] = {15.0, 12.0};
  refused(b, "radius_mm");
  b = j;
  b["fit"]["axis"] = {0, 0, 1};
  refused(b, "axis_tolerance_deg");
  b = j;
  b["fit"]["within"] = {{"min_mm", {0, 0, 0}}, {"max", {1, 1, 1}}};
  refused(b, "within");
  nlohmann::json p = DocumentIO::FeatureToJson(s.Plane());
  p["fit"]["side"] = "inside";
  refused(p, "side does not apply to a plane");
  p = DocumentIO::FeatureToJson(s.Plane());
  p["fit"]["clip_sigma"] = "all";
  const auto pb = DocumentIO::FeatureFromJson(p);
  CHECK(dynamic_cast<const DatumPlaneFeature&>(*pb).fit.clipSigma == 0);
}

TEST_CASE("A datum axis from_fit is a scanned ring's axis, through its "
          "centre",
          "[datum][fit][mesh_body]") {
  const fixtures::Ring ring = fixtures::ScannedRing(20.0, 5.0, 0.5, 0.05);
  const fs::path stl = fixtures::WriteStl(ring.organic.mesh, "datum-torus");
  Document doc;
  doc.AddFeature(ScanImport(stl));
  const uint64_t importId = doc.GetFeature(0)->GetId();
  auto axis =
      std::make_unique<DatumAxisFeature>("Ring axis", DatumAxisKind::FromFit);
  axis->fit = {importId, ring.At(120.0, 30.0), mesh::Kind::Torus, {}};
  doc.AddFeature(std::move(axis));
  REQUIRE(doc.Regenerate());
  const auto& a = *dynamic_cast<const DatumAxisFeature*>(doc.GetFeature(1));
  INFO(a.GetError());
  REQUIRE(a.GetError().empty());
  std::printf("ring datum: axis %.4f deg off, origin %.4f mm off the centre\n",
              LineAngleDeg(a.Direction(), ring.axis),
              glm::length(a.Origin() - ring.centre));
  CHECK(LineAngleDeg(a.Direction(), ring.axis) < 0.1);
  CHECK(glm::length(a.Origin() - ring.centre) < 0.05);
  CHECK(glm::dot(a.Direction(), ring.axis) > 0.0); // largest component +
  CHECK_THAT(a.FitReport().at("major_radius_mm").get<double>(),
             WithinAbs(20.0, 0.05));
  // It reads back as written; a radius_mm on it is refused.
  nlohmann::json j = DocumentIO::FeatureToJson(a);
  CHECK(j.at("fit").at("kind") == "torus");
  CHECK(DocumentIO::FeatureToJson(*DocumentIO::FeatureFromJson(j)) == j);
  j["fit"]["radius_mm"] = {4.0, 6.0};
  try {
    (void)DocumentIO::FeatureFromJson(j);
    FAIL("accepted");
  } catch (const std::exception& e) {
    CHECK_THAT(std::string(e.what()),
               ContainsSubstring("radius_mm does not apply to a torus"));
  }
  fs::remove(stl);
}

TEST_CASE("The vNEXT golden's ring axis is fitted to its torus",
          "[datum][fit][golden]") {
  // tests/golden/vNEXT-datum-fit-torus.json: a circle of radius 5 at 20
  // from Z revolved about it, its axis fitted to its own tessellation as a
  // torus, outside.
  auto doc = testio::Load(std::string(TestSourceDir()) +
                          "/tests/golden/vNEXT-datum-fit-torus.json");
  REQUIRE(doc != nullptr);
  const auto& axis = *dynamic_cast<const DatumAxisFeature*>(doc->GetFeature(2));
  INFO(axis.GetError());
  REQUIRE(axis.GetError().empty());
  CHECK(axis.fit.kind == mesh::Kind::Torus);
  CHECK(LineAngleDeg(axis.Direction(), {0, 0, 1}) < 1e-3);
  CHECK(axis.Direction().z > 0.0);
  CHECK(glm::length(axis.Origin()) < 1e-3);
  CHECK_THAT(axis.FitReport().at("major_radius_mm").get<double>(),
             WithinAbs(20.0, 0.01));
  CHECK_THAT(axis.FitReport().at("minor_radius_mm").get<double>(),
             WithinAbs(5.0, 0.01));
  CHECK(axis.FitReport().at("concave") == false);
}

TEST_CASE("The v215 golden's datums are fitted on load",
          "[datum][fit][golden]") {
  // tests/golden/v215-datum-from-fit.json: a 10 mm shaft 30 long on the
  // origin, its axis and its top fitted to its own tessellation -- a datum
  // from_fit takes a B-Rep body as it takes a scan.
  auto doc = testio::Load(std::string(TestSourceDir()) +
                          "/tests/golden/v215-datum-from-fit.json");
  REQUIRE(doc != nullptr);
  const auto& axis = *dynamic_cast<const DatumAxisFeature*>(doc->GetFeature(2));
  INFO(axis.GetError());
  REQUIRE(axis.GetError().empty());
  CHECK(LineAngleDeg(axis.Direction(), {0, 0, 1}) < 1e-3);
  CHECK(axis.Direction().z > 0.0); // the largest component positive
  CHECK(FromLine({0, 0, 0}, axis.Origin(), axis.Direction()) < 1e-3);
  CHECK_THAT(axis.FitReport().at("radius_mm").get<double>(),
             WithinAbs(10.0, 0.01));
  const auto& top = *dynamic_cast<const DatumPlaneFeature*>(doc->GetFeature(3));
  INFO(top.GetError());
  REQUIRE(top.GetError().empty());
  CHECK(glm::length(top.OutputPlane().GetNormal() - glm::dvec3(0, 0, 1)) <
        1e-9);
  CHECK_THAT(top.OutputPlane().GetOrigin().z, WithinAbs(30.0, 1e-6));
  CHECK(top.FitReport().at("tolerance_mm") == 0.05);
}

TEST_CASE("The v230 golden's datums are fitted by their hints and clip",
          "[datum][fit][golden]") {
  // tests/golden/v230-datum-fit-hints.json: v215's shaft, its axis fitted
  // outside, 9-11 mm, along Z inside a box and clipped at 2 sigma, its top
  // along Z with every point used.
  auto doc = testio::Load(std::string(TestSourceDir()) +
                          "/tests/golden/v230-datum-fit-hints.json");
  REQUIRE(doc != nullptr);
  const auto& axis = *dynamic_cast<const DatumAxisFeature*>(doc->GetFeature(2));
  INFO(axis.GetError());
  REQUIRE(axis.GetError().empty());
  CHECK(axis.fit.hints.outside == std::optional<bool>(true));
  CHECK(axis.fit.clipSigma == std::optional<int>(2));
  CHECK(axis.FitReport().at("clip_sigma") == 2);
  CHECK(LineAngleDeg(axis.Direction(), {0, 0, 1}) < 1e-3);
  CHECK_THAT(axis.FitReport().at("radius_mm").get<double>(),
             WithinAbs(10.0, 0.01));
  const auto& top = *dynamic_cast<const DatumPlaneFeature*>(doc->GetFeature(3));
  INFO(top.GetError());
  REQUIRE(top.GetError().empty());
  CHECK(top.FitReport().at("clip_sigma") == "all");
  CHECK_THAT(top.OutputPlane().GetOrigin().z, WithinAbs(30.0, 1e-6));
}
