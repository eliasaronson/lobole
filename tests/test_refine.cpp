// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/BooleanFeature.h"
#include "core/features/RefineFeature.h"
#include "core/io/Serialization.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shape.hxx>

#include <filesystem>

using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

static int FaceCount(const CADShape& s) {
  int n = 0;
  for (TopExp_Explorer ex(s.GetShape(), TopAbs_FACE); ex.More(); ex.Next())
    n++;
  return n;
}

// Two 2x2x2 boxes face-adjacent at x=2, unioned: the coplanar side faces are
// split by a seam edge, so the fused body has more than a clean box's 6 faces.
static void AddUnion(Document& doc, uint64_t& unionId) {
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 2.0, 2.0, 2.0));
  const uint64_t a = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(2, 0, 0), 2.0, 2.0, 2.0));
  const uint64_t b = doc.GetFeature(1)->GetId();
  doc.AddFeature(std::make_unique<BooleanFeature>("U", a, b, BooleanOp::Union));
  unionId = doc.GetFeature(2)->GetId();
}

TEST_CASE("Refine merges the coplanar seams from a union", "[refine]") {
  Document doc;
  uint64_t u;
  AddUnion(doc, u);
  REQUIRE(doc.GetBodies().size() == 1);
  const int before = FaceCount(doc.GetBodies()[0].shape);
  CHECK(before > 6); // seams split the long faces

  doc.AddFeature(std::make_unique<RefineFeature>("R", u));
  REQUIRE(doc.GetBodies().size() == 1);
  const auto& body = doc.GetBodies()[0];
  CHECK(FaceCount(body.shape) == 6); // back to a clean box (4x2x2)
  CHECK_THAT(body.shape.Volume(), WithinRel(16.0, 1e-6)); // volume preserved
  CHECK(body.createdBy == u); // modified in place; downstream refs survive
  CHECK(body.elementMap.names.size() == 6); // every face named
}

namespace {
// The centroid of face `index` (the ElementMap's 0-based explorer order).
glm::dvec3 FaceCentroid(const CADShape& s, int index) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(s.GetShape(), TopAbs_FACE, faces);
  GProp_GProps props;
  BRepGProp::SurfaceProperties(faces(index + 1), props);
  const gp_Pnt c = props.CentreOfMass();
  return {c.X(), c.Y(), c.Z()};
}
} // namespace

// A face Refine leaves alone keeps the name it had: a study's load, a
// bearing's seat or a mate made to it before the Refine still finds it.
// Refine used to rename every face "<id>/faceN", so the advice to refine a
// body that would not mesh broke every reference to it.
TEST_CASE("Refine keeps the names of the faces it leaves alone", "[refine]") {
  Document doc;
  uint64_t u;
  AddUnion(doc, u);
  REQUIRE(doc.GetBodies().size() == 1);
  // The end face at x = 0 is A's own, untouched by the seam at x = 2.
  std::string endName;
  {
    const Body& b = doc.GetBodies()[0];
    for (const auto& [idx, name] : b.elementMap.names)
      if (std::abs(FaceCentroid(b.shape, idx).x) < 1e-9)
        endName = name;
  }
  REQUIRE_FALSE(endName.empty());

  doc.AddFeature(std::make_unique<RefineFeature>("R", u));
  const Body& body = doc.GetBodies()[0];
  REQUIRE(FaceCount(body.shape) == 6);
  const auto idx = body.elementMap.Resolve(endName);
  INFO("before the Refine: " << endName);
  REQUIRE(idx.has_value());
  CHECK(std::abs(FaceCentroid(body.shape, *idx).x) < 1e-9);
  // Every face of the refined box is named, the merged long ones included.
  CHECK(body.elementMap.names.size() == 6);
}

TEST_CASE("Refine of a missing body fails", "[refine]") {
  Document doc;
  uint64_t u;
  AddUnion(doc, u);
  doc.AddFeature(std::make_unique<RefineFeature>("R", 9999));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("Refine round-trips through JSON", "[refine]") {
  const fs::path path = fs::temp_directory_path() / "designer_refine_test.json";
  {
    Document doc;
    uint64_t u;
    AddUnion(doc, u);
    doc.AddFeature(std::make_unique<RefineFeature>("R", u));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rf = dynamic_cast<RefineFeature*>(loaded->GetFeature(3));
  REQUIRE(rf != nullptr);
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK(FaceCount(loaded->GetBodies()[0].shape) == 6);
  fs::remove(path);
}
