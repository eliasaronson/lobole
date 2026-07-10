#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/DatumPlaneFeature.h"
#include "core/Document.h"
#include "core/ExtrudeFeature.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"

#include <filesystem>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

static const SketchPlane kXY(PlaneType::XY);

// Adds an offset datum (from a base plane by default) and returns its id.
static uint64_t AddOffsetDatum(Document& doc, double offset,
                               PlaneType base = PlaneType::XY,
                               uint64_t parentId = 0) {
  auto dp = std::make_unique<DatumPlaneFeature>("Plane", DatumPlaneKind::Offset);
  dp->offset = offset;
  dp->basePlane = base;
  dp->parentId = parentId;
  doc.AddFeature(std::move(dp));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// A rectangle sketch bound to a datum plane by id; returns the sketch id.
static uint64_t AddSketchOnDatum(Document& doc, uint64_t datumId,
                                 const glm::vec2& c1, const glm::vec2& c2) {
  // The baked plane is a placeholder; Execute overwrites it from the datum.
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  sk->planeRef = datumId;
  sk->AddRectangle(c1, c2);
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// --- Variant geometry --------------------------------------------------------

TEST_CASE("Offset datum shifts along the parent normal", "[datum]") {
  Document doc;
  AddOffsetDatum(doc, 5.0);
  auto* dp = dynamic_cast<DatumPlaneFeature*>(doc.GetFeature(0));
  REQUIRE(dp != nullptr);

  const SketchPlane& out = dp->OutputPlane();
  CHECK_THAT(out.GetOrigin().z, WithinRel(5.0f, 1e-5f));
  CHECK_THAT(out.GetNormal().z, WithinRel(1.0f, 1e-5f));
  // A datum contributes no body.
  CHECK(doc.GetBodies().empty());

  // flip negates the offset direction.
  dp->flip = true;
  doc.Regenerate();
  CHECK_THAT(dp->OutputPlane().GetOrigin().z, WithinRel(-5.0f, 1e-5f));
}

TEST_CASE("Angled datum rotates the parent normal about an axis", "[datum]") {
  Document doc;
  auto dp = std::make_unique<DatumPlaneFeature>("Plane", DatumPlaneKind::Angled);
  dp->basePlane = PlaneType::XY;           // normal +Z
  dp->axis.kind = AxisRef::Kind::X;        // hinge on world X
  dp->angleDeg = 90.0;
  doc.AddFeature(std::move(dp));
  auto* d = dynamic_cast<DatumPlaneFeature*>(doc.GetFeature(0));
  REQUIRE(d != nullptr);

  // +Z rotated 90 deg about +X -> -Y (right-hand rule).
  const glm::vec3 n = d->OutputPlane().GetNormal();
  CHECK_THAT(n.x, WithinAbs(0.0f, 1e-5f));
  CHECK_THAT(n.y, WithinAbs(-1.0f, 1e-5f));
  CHECK_THAT(n.z, WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("Angled datum with an axis parallel to the normal fails", "[datum]") {
  Document doc;
  auto dp = std::make_unique<DatumPlaneFeature>("Plane", DatumPlaneKind::Angled);
  dp->basePlane = PlaneType::XY;    // normal +Z
  dp->axis.kind = AxisRef::Kind::Z; // parallel -> degenerate
  doc.AddFeature(std::move(dp));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("ThroughPoints datum spans its three points", "[datum]") {
  Document doc;
  auto dp = std::make_unique<DatumPlaneFeature>("Plane",
                                                DatumPlaneKind::ThroughPoints);
  dp->p0 = {0, 0, 1};
  dp->p1 = {2, 0, 1};
  dp->p2 = {0, 3, 1};
  doc.AddFeature(std::move(dp));
  auto* d = dynamic_cast<DatumPlaneFeature*>(doc.GetFeature(0));
  REQUIRE(d != nullptr);

  // All three points lie at z=1, so the plane normal is +/-Z and origin at p0.
  CHECK_THAT(d->OutputPlane().GetOrigin().z, WithinRel(1.0f, 1e-5f));
  CHECK_THAT(std::abs(d->OutputPlane().GetNormal().z), WithinRel(1.0f, 1e-5f));
}

TEST_CASE("ThroughPoints with collinear points fails", "[datum]") {
  Document doc;
  auto dp = std::make_unique<DatumPlaneFeature>("Plane",
                                                DatumPlaneKind::ThroughPoints);
  dp->p0 = {0, 0, 0};
  dp->p1 = {1, 1, 1};
  dp->p2 = {2, 2, 2}; // collinear
  doc.AddFeature(std::move(dp));
  CHECK_FALSE(doc.Regenerate());
}

// --- Sketch tracking + extrude ----------------------------------------------

TEST_CASE("Sketch on an offset datum extrudes at the offset height",
          "[datum]") {
  Document doc;
  uint64_t datum = AddOffsetDatum(doc, 5.0);
  uint64_t sketch = AddSketchOnDatum(doc, datum, {0, 0}, {4, 2});
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketch, 2.0,
                                                  ExtrudeMode::NewBody));

  REQUIRE(doc.GetBodies().size() == 1);
  glm::vec3 min, max;
  doc.GetBodies()[0].shape.GetBoundingBox(min, max);
  // Base at z=5, extruded +2: bbox center z = 5 + 1.
  CHECK_THAT(((min + max) * 0.5f).z, WithinRel(6.0f, 1e-3f));
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(16.0, 1e-4));
}

TEST_CASE("Editing a datum offset moves the sketch and its extrusion",
          "[datum]") {
  Document doc;
  uint64_t datum = AddOffsetDatum(doc, 5.0);
  uint64_t sketch = AddSketchOnDatum(doc, datum, {0, 0}, {4, 2});
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketch, 2.0,
                                                  ExtrudeMode::NewBody));

  auto* dp = dynamic_cast<DatumPlaneFeature*>(doc.FindFeatureById(datum));
  REQUIRE(dp != nullptr);
  dp->offset = 10.0;
  doc.Regenerate();

  glm::vec3 min, max;
  doc.GetBodies()[0].shape.GetBoundingBox(min, max);
  CHECK_THAT(((min + max) * 0.5f).z, WithinRel(11.0f, 1e-3f));
}

TEST_CASE("Datum-on-datum offsets compose", "[datum]") {
  Document doc;
  uint64_t first = AddOffsetDatum(doc, 3.0);
  uint64_t second = AddOffsetDatum(doc, 4.0, PlaneType::XY, first);
  auto* d2 = dynamic_cast<DatumPlaneFeature*>(doc.FindFeatureById(second));
  REQUIRE(d2 != nullptr);
  CHECK_THAT(d2->OutputPlane().GetOrigin().z, WithinRel(7.0f, 1e-5f));
}

TEST_CASE("A datum offset driven by a variable follows a parameter edit",
          "[datum]") {
  Document doc;
  doc.SetParameter("gap", "4");
  auto dp = std::make_unique<DatumPlaneFeature>("Plane", DatumPlaneKind::Offset);
  dp->offsetExpr = "gap";
  doc.AddFeature(std::move(dp));
  auto* d = dynamic_cast<DatumPlaneFeature*>(doc.GetFeature(0));
  REQUIRE(d != nullptr);
  CHECK_THAT(d->OutputPlane().GetOrigin().z, WithinRel(4.0f, 1e-5f));

  doc.SetParameter("gap", "9");
  CHECK_THAT(d->OutputPlane().GetOrigin().z, WithinRel(9.0f, 1e-5f));
}

TEST_CASE("Angled datum can hinge on a sketch line", "[datum]") {
  Document doc;
  // A sketch on XY with a line along the U (world X) axis.
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  const uint32_t lineId = sk->AddEntity(sketch::Line{{0, 0}, {1, 0}});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();

  auto dp = std::make_unique<DatumPlaneFeature>("Plane", DatumPlaneKind::Angled);
  dp->axis.kind = AxisRef::Kind::SketchLine;
  dp->axis.sketchId = sketchId;
  dp->axis.entityId = lineId;
  dp->angleDeg = 90.0;
  doc.AddFeature(std::move(dp));
  auto* d = dynamic_cast<DatumPlaneFeature*>(doc.GetFeature(1));
  REQUIRE(d != nullptr);

  // Hinge on X, +Z rotated 90 deg -> -Y, same as the world-X-axis case.
  CHECK_THAT(d->OutputPlane().GetNormal().y, WithinAbs(-1.0f, 1e-4f));
}

// --- Failure modes -----------------------------------------------------------

TEST_CASE("A datum with a missing parent fails regeneration", "[datum]") {
  Document doc;
  auto dp = std::make_unique<DatumPlaneFeature>("Plane", DatumPlaneKind::Offset);
  dp->parentId = 9999; // no such feature
  doc.AddFeature(std::move(dp));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("Deleting a datum a sketch references stops regeneration",
          "[datum]") {
  Document doc;
  uint64_t datum = AddOffsetDatum(doc, 5.0);
  uint64_t sketch = AddSketchOnDatum(doc, datum, {0, 0}, {4, 2});
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketch, 2.0,
                                                  ExtrudeMode::NewBody));
  REQUIRE(doc.GetBodies().size() == 1);

  // Remove the datum (index 0); the sketch's planeRef now dangles.
  size_t idx = 0;
  doc.FindFeatureById(datum, &idx);
  doc.RemoveFeature(idx);
  CHECK_FALSE(doc.Regenerate());
}

// --- Serialization -----------------------------------------------------------

TEST_CASE("Datum + tracked sketch + extrude round-trip through JSON",
          "[datum]") {
  const fs::path path = fs::temp_directory_path() / "designer_datum_test.json";
  {
    Document doc;
    doc.SetParameter("gap", "5");
    auto dp = std::make_unique<DatumPlaneFeature>("Plane",
                                                  DatumPlaneKind::Offset);
    dp->offsetExpr = "gap";
    doc.AddFeature(std::move(dp));
    const uint64_t datum = doc.GetFeature(0)->GetId();
    uint64_t sketch = AddSketchOnDatum(doc, datum, {0, 0}, {4, 2});
    doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketch, 2.0,
                                                    ExtrudeMode::NewBody));
    REQUIRE(DocumentIO::Save(doc, path.string()));
  }

  std::string error;
  const auto loaded = DocumentIO::Load(path.string(), &error);
  REQUIRE(loaded != nullptr);

  auto* dp = dynamic_cast<DatumPlaneFeature*>(loaded->GetFeature(0));
  REQUIRE(dp != nullptr);
  CHECK(dp->offsetExpr == "gap");
  CHECK_THAT(dp->OutputPlane().GetOrigin().z, WithinRel(5.0f, 1e-5f));

  auto* sk = dynamic_cast<SketchFeature*>(loaded->GetFeature(1));
  REQUIRE(sk != nullptr);
  CHECK(sk->planeRef == dp->GetId());

  REQUIRE(loaded->GetBodies().size() == 1);
  glm::vec3 min, max;
  loaded->GetBodies()[0].shape.GetBoundingBox(min, max);
  CHECK_THAT(((min + max) * 0.5f).z, WithinRel(6.0f, 1e-3f));

  fs::remove(path);
}

TEST_CASE("Datum regeneration is deterministic", "[datum]") {
  Document doc;
  uint64_t datum = AddOffsetDatum(doc, 5.0);
  uint64_t sketch = AddSketchOnDatum(doc, datum, {0, 0}, {4, 2});
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketch, 2.0,
                                                  ExtrudeMode::NewBody));

  glm::vec3 min1, max1;
  doc.GetBodies()[0].shape.GetBoundingBox(min1, max1);
  doc.Regenerate();
  glm::vec3 min2, max2;
  doc.GetBodies()[0].shape.GetBoundingBox(min2, max2);

  CHECK_THAT(min1.z, WithinAbs(min2.z, 1e-6f));
  CHECK_THAT(max1.z, WithinAbs(max2.z, 1e-6f));
}
