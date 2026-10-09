// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A sketch plane and the geometry on it are double precision, end to end
// (docs/sketching/README.md#plane): a plane typed at a face's height lies ON
// that face, a solve writes back what it solved, and a save loads back the
// same bits.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {

// The plane at height z whose normal points along `up` (+1 or -1) on Z. U is
// +X either way, so V is +Y facing up and -Y facing down.
SketchPlane PlaneAt(double z, double up) {
  return SketchPlane(glm::dvec3(0, 0, z), glm::dvec3(0, 0, up),
                     glm::dvec3(1, 0, 0));
}

uint64_t RectSketch(Document& doc, const SketchPlane& plane, glm::dvec2 lo,
                    glm::dvec2 hi) {
  auto sk = std::make_unique<SketchFeature>("Sketch", plane);
  sk->AddRectangle(lo, hi);
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// The middle 10 x 10 of the slab, in the UV of PlaneAt(z, up).
uint64_t MiddleOf(Document& doc, double z, double up) {
  return RectSketch(doc, PlaneAt(z, up), {5, 5 * up}, {15, 15 * up});
}

// A 20 x 20 slab one of whose faces lies at z = t, made the way a person
// makes it: a rectangle on the XY plane extruded |t| -- up for t > 0, down
// for t < 0 -- so the face is at 0 + t.
void SlabTo(Document& doc, double t) {
  const uint64_t base =
      RectSketch(doc, SketchPlane(PlaneType::XY), {0, 0}, {20, 20});
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Slab", base, std::abs(t), ExtrudeMode::NewBody, t < 0.0));
}

// Planar faces near height t (within 1 um) whose outward normal points
// along `dir` on Z: a skin left over a pocket shows up as one looking INTO
// the part.
int FacesAt(const Body& body, double t, double dir) {
  int n = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next()) {
    const TopoDS_Face f = TopoDS::Face(ex.Current());
    const BRepAdaptor_Surface surf(f);
    if (surf.GetType() != GeomAbs_Plane)
      continue;
    gp_Dir nd = surf.Plane().Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED)
      nd.Reverse();
    if (nd.Z() * dir > 0.5 && std::abs(surf.Plane().Location().Z() - t) < 1e-3)
      n++;
  }
  return n;
}

std::vector<double> Heights() {
  std::vector<double> h;
  for (int k = 1; k <= 100; k++) {
    h.push_back(0.1 * k);
    h.push_back(-0.1 * k);
  }
  for (double t : {13.4, 24.7, 1e3 + 0.1, -(1e3 + 0.1), 99.9, -13.4})
    h.push_back(t);
  return h;
}

} // namespace

TEST_CASE("A plane typed at a face's height lies on it",
          "[sketch][precision]") {
  // The plane stores what it was given: 13.4 is 13.4, not 13.39999962.
  CHECK(PlaneAt(13.4, 1).GetOrigin().z == 13.4);
  CHECK(PlaneAt(1e3 + 0.1, 1).GetOrigin().z == 1e3 + 0.1);
  CHECK(PlaneAt(-0.7, -1).GetOrigin().z == -0.7);
  // And the face a slab extruded to 13.4 makes is at 13.4 too.
  Document doc;
  SlabTo(doc, 13.4);
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(FacesAt(doc.GetBodies()[0], 13.4, +1) == 1);
}

TEST_CASE("A cut from a face's height opens it at any height",
          "[sketch][precision][extrude]") {
  // A pocket cut into a part from a plane at the height of its face. A plane
  // rounded through float stopped 0.4 um short of a face at 13.4 and kept a
  // sealed skin over the pocket, which passed BRepCheck.
  for (const double t : Heights()) {
    CAPTURE(t);
    const double out = t > 0.0 ? 1.0 : -1.0; // the face's outward normal
    Document doc;
    SlabTo(doc, t);
    const uint64_t sk = MiddleOf(doc, t, out);
    const double depth = std::abs(t) / 2.0;
    doc.AddFeature(std::make_unique<ExtrudeFeature>(
        "Pocket", sk, depth, ExtrudeMode::Cut, /*reversed=*/true));
    REQUIRE(doc.GetBodies().size() == 1);
    const Body& body = doc.GetBodies()[0];
    CHECK(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
    CHECK(body.shape.SolidCount() == 1);
    CHECK(FacesAt(body, t, -out) == 0); // nothing looks into the part there
    CHECK_THAT(body.shape.Volume(),
               WithinRel(400.0 * std::abs(t) - 100.0 * depth, 1e-9));
  }
}

TEST_CASE("An add from a face's height fuses at any height",
          "[sketch][precision][extrude]") {
  // A boss grown out of a face from a plane at its height. Rounded the other
  // way (24.7 -> 24.70000076) it started above the face: two solids.
  for (const double t : Heights()) {
    CAPTURE(t);
    const double out = t > 0.0 ? 1.0 : -1.0;
    Document doc;
    SlabTo(doc, t);
    const uint64_t sk = MiddleOf(doc, t, out);
    doc.AddFeature(
        std::make_unique<ExtrudeFeature>("Boss", sk, 2.0, ExtrudeMode::Add));
    REQUIRE(doc.GetBodies().size() == 1);
    const Body& body = doc.GetBodies()[0];
    CHECK(body.shape.SolidCount() == 1);
    CHECK_THAT(body.shape.Volume(),
               WithinRel(400.0 * std::abs(t) + 100.0 * 2.0, 1e-9));
  }
}

TEST_CASE("A feature drawn on a face's plane keeps its face on that plane",
          "[sketch][precision][extrude]") {
  // A cap: a skirt up to z = 40, then a top added from the plane at 40. The
  // face the top grows from is ON the skirt's, so the tool is not moved
  // behind it -- its underside is at 40 exactly, where the tube end it sits
  // on is (the f1-cap-tube benchmark found it 38 nm low and in no contact).
  Document doc;
  auto skirt = std::make_unique<SketchFeature>("Skirt", PlaneAt(28.0, 1));
  skirt->AddEntity(sketch::Circle{{0, 0}, 18.0});
  skirt->AddEntity(sketch::Circle{{0, 0}, 15.2});
  doc.AddFeature(std::move(skirt));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Skirt", doc.GetFeature(0)->GetId(), 12.0, ExtrudeMode::NewBody));
  auto top = std::make_unique<SketchFeature>("Top", PlaneAt(40.0, 1));
  top->AddEntity(sketch::Circle{{0, 0}, 18.0});
  doc.AddFeature(std::move(top));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Top", doc.GetFeature(2)->GetId(), 3.0, ExtrudeMode::Add));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& cap = doc.GetBodies()[0];
  CHECK(cap.shape.SolidCount() == 1);
  // The underside inside the bore looks down from exactly z = 40.
  bool underside = false;
  for (TopExp_Explorer ex(cap.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next()) {
    const TopoDS_Face f = TopoDS::Face(ex.Current());
    const BRepAdaptor_Surface surf(f);
    if (surf.GetType() != GeomAbs_Plane)
      continue;
    gp_Dir nd = surf.Plane().Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED)
      nd.Reverse();
    if (nd.Z() < -0.5 && surf.Plane().Location().Z() == 40.0)
      underside = true;
  }
  CHECK(underside);
}

TEST_CASE("An expression-driven sketch does not drift over regenerations",
          "[sketch][precision][solver]") {
  // A solve writes back exactly what it solved, and the next regeneration
  // starts from it: no round trip through float that the next solve then
  // "corrects". Ten regenerations leave every coordinate bit-identical.
  Document doc;
  REQUIRE(doc.SetParameter("w", "13.4"));
  REQUIRE(doc.SetParameter("h", "0.1 * 7"));
  auto owned = std::make_unique<SketchFeature>("S", PlaneAt(24.7, 1));
  SketchFeature* sk = owned.get();
  using K = sketch::ConstraintKind;
  const auto ids = sk->AddRectangle({0.3, 0.1}, {13.4, 7.7});
  sk->constraints.push_back({K::Fixed, {ids[0], 0}, {}});
  sketch::Constraint width{K::Distance, {ids[0], 0}, {ids[0], 1}, 13.4};
  width.valueExpr = "w";
  sk->constraints.push_back(width);
  sketch::Constraint height{K::Distance, {ids[1], 0}, {ids[1], 1}, 0.7};
  height.valueExpr = "h * 11";
  sk->constraints.push_back(height);
  REQUIRE(sk->FilletCorner(ids[1], ids[2], 1.1) != 0);
  const uint32_t pt = sk->AddEntity(sketch::Point{{0.0, 0.0}});
  sk->entityExprs[pt] = {{"x", "w / 3"}, {"y", "h"}};
  doc.AddFeature(std::move(owned));
  REQUIRE(doc.Regenerate());
  sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  REQUIRE(sk != nullptr);
  REQUIRE(sk->GetError().empty());

  const nlohmann::json first = DocumentIO::FeatureToJson(*sk);
  // The expression's value, not its float neighbour.
  const auto& p = std::get<sketch::Point>(sk->entities[sk->IndexOfEntity(pt)]);
  CHECK(p.p.x == 13.4 / 3.0);
  CHECK(p.p.y == 0.1 * 7);
  for (int i = 0; i < 10; i++) {
    REQUIRE(doc.Regenerate());
    sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
    REQUIRE(sk != nullptr);
    CHECK(DocumentIO::FeatureToJson(*sk) == first);
  }
}

TEST_CASE("A sketch saves and loads back bit for bit",
          "[sketch][precision][serialization]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>(
      "S", SketchPlane(glm::dvec3(0.1, -1e3 - 0.1, 13.4),
                       glm::dvec3(0.3, 0.1, 1.7), glm::dvec3(1, 0, 0)));
  sk->AddEntity(sketch::Point{{0.1, 0.2}});
  sk->AddEntity(sketch::Line{{0.1 * 3, 0.7}, {13.4, 24.7}});
  sk->AddEntity(sketch::Circle{{-0.3, 1e3 + 0.1}, 0.1 * 3});
  sk->AddEntity(sketch::Arc{{0.6, 0.9}, 1.1, 0.1, 2.3});
  sketch::BSpline spline;
  spline.poles = {{0.1, 0.2}, {0.3, 0.7}, {1.9, 0.1 * 7}, {2.2, 3.3}};
  sk->AddEntity(spline);
  doc.AddFeature(std::move(sk));
  const auto* before = dynamic_cast<const SketchFeature*>(doc.GetFeature(0));
  REQUIRE(before != nullptr);

  const auto path = std::filesystem::temp_directory_path() /
                    "designer_sketch_precision_roundtrip.json";
  REQUIRE(testio::Save(doc, path.string()));
  auto loaded = testio::Load(path.string());
  std::filesystem::remove(path);
  REQUIRE(loaded != nullptr);
  const auto* after = dynamic_cast<const SketchFeature*>(loaded->GetFeature(0));
  REQUIRE(after != nullptr);

  CHECK(after->plane.GetOrigin() == before->plane.GetOrigin());
  CHECK(after->plane.GetNormal() == before->plane.GetNormal());
  CHECK(after->plane.GetUAxis() == before->plane.GetUAxis());
  CHECK(after->plane.GetOrigin().z == 13.4);
  REQUIRE(after->entities.size() == before->entities.size());
  CHECK(DocumentIO::FeatureToJson(*after) ==
        DocumentIO::FeatureToJson(*before));
  const auto& line = std::get<sketch::Line>(after->entities[1]);
  CHECK(line.a.x == 0.1 * 3);
  CHECK(line.b == glm::dvec2(13.4, 24.7));
  const auto& c = std::get<sketch::Circle>(after->entities[2]);
  CHECK(c.center.y == 1e3 + 0.1);
  CHECK(std::get<sketch::BSpline>(after->entities[4]).poles[2].y == 0.1 * 7);
}
