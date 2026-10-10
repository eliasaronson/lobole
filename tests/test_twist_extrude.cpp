// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Twisted and herringbone extrudes (docs/modeling/README.md#twisted-extrudes):
// the profile carried along the sketch normal by one screw motion.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <filesystem>
#include <optional>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const SketchPlane kXY(PlaneType::XY);

struct Twisted {
  Document doc;
  ExtrudeFeature* ext = nullptr;
};

// A sketch with `build` applied, and a twisted extrude of it.
template <class Fn>
void Make(Twisted& t, Fn build, double distance, double twistDeg,
          bool herringbone = false, ExtrudeEnd end = ExtrudeEnd::Blind) {
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  build(*sk);
  t.doc.AddFeature(std::move(sk));
  const uint64_t skId = t.doc.GetFeature(0)->GetId();
  auto ext = std::make_unique<ExtrudeFeature>("Twist", skId, distance,
                                              ExtrudeMode::NewBody);
  ext->twistDeg = twistDeg;
  ext->herringbone = herringbone;
  ext->endMode = end;
  t.doc.AddFeature(std::move(ext));
  t.ext = dynamic_cast<ExtrudeFeature*>(t.doc.GetFeature(1));
}

const Body& OnlyBody(const Document& doc) {
  REQUIRE(doc.GetBodies().size() == 1);
  return doc.GetBodies()[0];
}

// The face a name resolves to on `body`.
std::optional<TopoDS_Face> FaceNamed(const Body& body, const std::string& nm) {
  const auto idx = body.elementMap.Resolve(nm);
  if (!idx)
    return std::nullopt;
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++)
    if (i == *idx)
      return TopoDS::Face(ex.Current());
  return std::nullopt;
}

std::vector<gp_Pnt> VerticesOf(const TopoDS_Shape& s) {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(s, TopAbs_VERTEX, map);
  std::vector<gp_Pnt> out;
  for (int i = 1; i <= map.Extent(); i++)
    out.push_back(BRep_Tool::Pnt(TopoDS::Vertex(map(i))));
  return out;
}

// Does `pts` hold a point within 1e-4 of (x, y, z)?
bool HasPoint(const std::vector<gp_Pnt>& pts, double x, double y, double z) {
  for (const gp_Pnt& p : pts)
    if (p.Distance(gp_Pnt(x, y, z)) < 1e-4)
      return true;
  return false;
}

// The rectangle's corners (+-5, +-2) turned `deg` about Z, all at height z.
bool HasRectAt(const std::vector<gp_Pnt>& pts, double deg, double z) {
  const double a = deg * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
  for (const double x : {-5.0, 5.0})
    for (const double y : {-2.0, 2.0})
      if (!HasPoint(pts, c * x - s * y, s * x + c * y, z))
        return false;
  return true;
}

int CountSolids(const CADShape& s) {
  int n = 0;
  for (TopExp_Explorer ex(s.GetShape(), TopAbs_SOLID); ex.More(); ex.Next())
    n++;
  return n;
}

void Rect(SketchFeature& sk) { sk.AddRectangle({-5, -2}, {5, 2}); }

} // namespace

TEST_CASE("A rectangle twisted 90 degrees keeps its volume and turns its top",
          "[extrude][twist]") {
  Twisted t;
  Make(t, Rect, 20.0, 90.0);
  REQUIRE(t.ext->GetError().empty());
  const Body& body = OnlyBody(t.doc);
  CHECK(Measure::CheckGeometry(body.shape).valid);
  CHECK(CountSolids(body.shape) == 1);
  // Cavalieri: every section is the profile, so the volume is the prism's.
  CHECK_THAT(body.shape.Volume(), WithinRel(10.0 * 4.0 * 20.0, 1e-5));

  // The bottom is the sketch; the top is the sketch turned 90 degrees
  // counter-clockwise about +Z (right-handed about the direction of travel).
  const auto cap0 = FaceNamed(body, std::to_string(t.ext->GetId()) + "/cap0");
  const auto cap1 = FaceNamed(body, std::to_string(t.ext->GetId()) + "/cap1");
  REQUIRE(cap0);
  REQUIRE(cap1);
  CHECK(HasRectAt(VerticesOf(*cap0), 0.0, 0.0));
  CHECK(HasRectAt(VerticesOf(*cap1), 90.0, 20.0));

  // One helicoidal side face per sketch line, named as a prism's are, and
  // none of them planar.
  int sides = 0;
  for (const auto& [idx, nm] : body.elementMap.names) {
    if (nm.find("/g") == std::string::npos)
      continue;
    sides++;
    const auto f = FaceNamed(body, nm);
    REQUIRE(f);
    CHECK(BRepAdaptor_Surface(*f).GetType() != GeomAbs_Plane);
  }
  CHECK(sides == 4);
  CHECK(body.shape.FaceCount() == 6);
}

TEST_CASE("A negative twist turns the other way", "[extrude][twist]") {
  Twisted t;
  Make(t, Rect, 10.0, -30.0);
  const Body& body = OnlyBody(t.doc);
  const auto cap1 = FaceNamed(body, std::to_string(t.ext->GetId()) + "/cap1");
  REQUIRE(cap1);
  CHECK(HasRectAt(VerticesOf(*cap1), -30.0, 10.0));
}

TEST_CASE("A twisted circle is a cylinder", "[extrude][twist]") {
  Twisted t;
  Make(
      t, [](SketchFeature& sk) { sk.AddEntity(sketch::Circle{{0, 0}, 5.0}); },
      10.0, 60.0);
  const Body& body = OnlyBody(t.doc);
  CHECK(Measure::CheckGeometry(body.shape).valid);
  CHECK_THAT(body.shape.Volume(), WithinRel(M_PI * 25.0 * 10.0, 1e-5));
  // Its side face lies on the cylinder of the circle's radius.
  bool found = false;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next()) {
    BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
    if (s.GetType() == GeomAbs_Plane)
      continue;
    found = true;
    for (int i = 0; i <= 4; i++)
      for (int j = 0; j <= 4; j++) {
        const double u = s.FirstUParameter() +
                         (s.LastUParameter() - s.FirstUParameter()) * i / 4.0;
        const double v = s.FirstVParameter() +
                         (s.LastVParameter() - s.FirstVParameter()) * j / 4.0;
        const gp_Pnt p = s.Value(u, v);
        CHECK_THAT(std::hypot(p.X(), p.Y()), WithinAbs(5.0, 1e-5));
      }
  }
  CHECK(found);
}

TEST_CASE("A twisted profile with a hole keeps the hole", "[extrude][twist]") {
  Twisted t;
  Make(
      t,
      [](SketchFeature& sk) {
        sk.AddRectangle({-5, -5}, {5, 5});
        sk.AddEntity(sketch::Circle{{1, 0}, 2.0});
      },
      8.0, 45.0);
  const Body& body = OnlyBody(t.doc);
  CHECK(Measure::CheckGeometry(body.shape).valid);
  CHECK_THAT(body.shape.Volume(), WithinRel((100.0 - M_PI * 4.0) * 8.0, 1e-5));
}

TEST_CASE("A herringbone turns to the apex and back", "[extrude][twist]") {
  Twisted t;
  Make(t, Rect, 20.0, 90.0, /*herringbone=*/true);
  REQUIRE(t.ext->GetError().empty());
  const Body& body = OnlyBody(t.doc);
  CHECK(Measure::CheckGeometry(body.shape).valid);
  CHECK(CountSolids(body.shape) == 1);
  CHECK_THAT(body.shape.Volume(), WithinRel(800.0, 1e-5));
  const std::vector<gp_Pnt> all = VerticesOf(body.shape.GetShape());
  // The same helix angle as the plain twist: half of it at the mid-plane.
  CHECK(HasRectAt(all, 45.0, 10.0));
  // Both ends are the profile itself.
  const std::string tag = std::to_string(t.ext->GetId());
  const auto cap0 = FaceNamed(body, tag + "/cap0");
  const auto cap1 = FaceNamed(body, tag + "/cap1");
  REQUIRE(cap0);
  REQUIRE(cap1);
  CHECK(HasRectAt(VerticesOf(*cap0), 0.0, 0.0));
  CHECK(HasRectAt(VerticesOf(*cap1), 0.0, 20.0));
  // Four sides each half, the upper ones told apart; no face left at the
  // apex.
  int lower = 0, upper = 0;
  for (const auto& [idx, nm] : body.elementMap.names) {
    if (nm.find("/g") == std::string::npos)
      continue;
    (nm.find("@h2") != std::string::npos ? upper : lower)++;
  }
  CHECK(lower == 4);
  CHECK(upper == 4);
  CHECK(body.shape.FaceCount() == 10);
}

TEST_CASE("A symmetric twist is the sketch at the sketch plane",
          "[extrude][twist]") {
  Twisted t;
  Make(t, Rect, 20.0, 90.0, false, ExtrudeEnd::Symmetric);
  const Body& body = OnlyBody(t.doc);
  glm::dvec3 lo, hi;
  body.shape.GetBoundingBox(lo, hi);
  CHECK_THAT(lo.z, WithinAbs(-10.0, 1e-4));
  CHECK_THAT(hi.z, WithinAbs(10.0, 1e-4));
  const std::string tag = std::to_string(t.ext->GetId());
  const auto cap0 = FaceNamed(body, tag + "/cap0");
  const auto cap1 = FaceNamed(body, tag + "/cap1");
  REQUIRE(cap0);
  REQUIRE(cap1);
  CHECK(HasRectAt(VerticesOf(*cap0), -45.0, -10.0));
  CHECK(HasRectAt(VerticesOf(*cap1), 45.0, 10.0));
}

TEST_CASE("A symmetric herringbone has its apex on the sketch plane",
          "[extrude][twist]") {
  Twisted t;
  Make(t, Rect, 20.0, 90.0, true, ExtrudeEnd::Symmetric);
  const Body& body = OnlyBody(t.doc);
  const std::vector<gp_Pnt> all = VerticesOf(body.shape.GetShape());
  CHECK(HasRectAt(all, 0.0, 0.0));
  CHECK(HasRectAt(all, -45.0, -10.0));
  CHECK(HasRectAt(all, -45.0, 10.0));
}

TEST_CASE("A reversed twist runs down and stays right-handed",
          "[extrude][twist]") {
  Twisted t;
  Make(t, Rect, 10.0, 90.0);
  t.ext->reversed = true;
  t.doc.Regenerate();
  const Body& body = OnlyBody(t.doc);
  const auto cap1 = FaceNamed(body, std::to_string(t.ext->GetId()) + "/cap1");
  REQUIRE(cap1);
  // Right-handed about -Z: clockwise seen from above.
  CHECK(HasRectAt(VerticesOf(*cap1), -90.0, -10.0));
}

TEST_CASE("A twisted cut removes a twisted pocket", "[extrude][twist]") {
  Document doc;
  auto base = std::make_unique<SketchFeature>("Base", kXY);
  base->AddRectangle({-10, -10}, {10, 10});
  doc.AddFeature(std::move(base));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Block", doc.GetFeature(0)->GetId(), 10.0, ExtrudeMode::NewBody));
  auto pocket = std::make_unique<SketchFeature>("Pocket", kXY);
  pocket->AddRectangle({-5, -2}, {5, 2});
  doc.AddFeature(std::move(pocket));
  auto cut = std::make_unique<ExtrudeFeature>("Cut", doc.GetFeature(2)->GetId(),
                                              6.0, ExtrudeMode::Cut);
  cut->twistDeg = 120.0;
  doc.AddFeature(std::move(cut));
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  const Body& body = OnlyBody(doc);
  CHECK(Measure::CheckGeometry(body.shape).valid);
  CHECK_THAT(body.shape.Volume(), WithinRel(4000.0 - 40.0 * 6.0, 1e-5));
}

TEST_CASE("A twist is refused with an end mode it does not take, and thin",
          "[extrude][twist]") {
  for (const ExtrudeEnd end :
       {ExtrudeEnd::TwoSided, ExtrudeEnd::UpToFace, ExtrudeEnd::ThroughAll}) {
    ExtrudeFeature ext("E", 1, 5.0, ExtrudeMode::Cut);
    ext.endMode = end;
    CHECK(ext.WhyIncomplete().empty()); // no twist: nothing to refuse
    ext.twistDeg = 30.0;
    const std::string why = ext.WhyIncomplete();
    CHECK(why.find("blind or symmetric") != std::string::npos);
  }
  ExtrudeFeature thin("E", 1, 5.0, ExtrudeMode::NewBody);
  thin.thin = true;
  thin.twistDeg = 30.0;
  CHECK(thin.WhyIncomplete().find("thin") != std::string::npos);

  // A file that carries one fails the feature with the same words.
  Twisted t;
  Make(t, Rect, 10.0, 30.0, false, ExtrudeEnd::TwoSided);
  CHECK(t.ext->GetError().find("blind or symmetric") != std::string::npos);
  CHECK(t.doc.GetBodies().empty());
}

TEST_CASE("Twist and herringbone round-trip, the twist live as an expression",
          "[extrude][twist][serialization]") {
  Twisted t;
  t.doc.SetParameter("turn", "40", "");
  Make(t, Rect, 12.0, 1.0, true);
  t.ext->twistExpr = "turn * 2";
  t.doc.Regenerate();
  REQUIRE(t.ext->GetError().empty());
  CHECK_THAT(t.ext->twistDeg, WithinAbs(80.0, 1e-9));

  const auto json = DocumentIO::FeatureToJson(*t.ext);
  CHECK(json.at("twist_deg").get<double>() == 80.0);
  CHECK(json.at("twist_expr").get<std::string>() == "turn * 2");
  CHECK(json.at("herringbone").get<bool>());
  // An untwisted extrude writes none of it.
  ExtrudeFeature plain("P", 1, 5.0, ExtrudeMode::NewBody);
  const auto pj = DocumentIO::FeatureToJson(plain);
  CHECK_FALSE(pj.contains("twist_deg"));
  CHECK_FALSE(pj.contains("herringbone"));

  const auto path =
      std::filesystem::temp_directory_path() / "twist_extrude_rt.json";
  REQUIRE(testio::Save(t.doc, path.string()));
  auto loaded = testio::Load(path.string());
  std::filesystem::remove(path);
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<ExtrudeFeature*>(loaded->GetFeature(1));
  REQUIRE(rt != nullptr);
  CHECK(rt->twistExpr == "turn * 2");
  CHECK(rt->herringbone);
  REQUIRE(loaded->GetBodies().size() == 1);
  // A steep twist: the pipe's surfaces are within its 1e-4 mm tolerance.
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(),
             WithinRel(40.0 * 12.0, 1e-4));
}
