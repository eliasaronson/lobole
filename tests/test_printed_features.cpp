// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// What printed parts asked of the modeller: a teardrop hole
// (docs/modeling/holes-and-threads.md#printed-holes), a tapered extrude
// (docs/modeling/README.md#tapered-extrudes) and the TPU table
// (docs/tolerances/README.md#tpu). Each is checked by its outcome: the
// section a hole cuts, what check_print says of it, a taper's volume against
// its frustum, and the names a taper keeps.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/PrintAllowance.h"
#include "core/PrintCheck.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <cmath>
#include <map>
#include <set>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kBox = 20.0; // a 20 mm cube, the hole along X at its middle
constexpr double kR = 3.0;    // the hole's radius

// A cube on the bed with one hole drilled along X through its middle, and
// the part printed in `material` (none: unprinted).
struct Drilled {
  Document doc;
  HoleFeature* hole = nullptr;
};

void Drill(Drilled& d, HolePrinted printed, const char* material = "pla",
           HoleKind kind = HoleKind::Simple, glm::dvec3 normal = {1, 0, 0}) {
  if (material != nullptr)
    d.doc.SetPrintProcess(printfit::Process{"fdm", material, std::nullopt});
  d.doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::dvec3(0), kBox, kBox, kBox));
  // A sketch on the +X face (normal +X drills -X); or the top (+Z).
  const bool alongX = normal.x != 0.0;
  SketchPlane plane(alongX ? glm::dvec3(kBox, 0, 0) : glm::dvec3(0, 0, kBox),
                    normal, alongX ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Centre", plane);
  sk->AddEntity(sketch::Point{glm::dvec2(kBox / 2, kBox / 2)});
  d.doc.AddFeature(std::move(sk));
  auto ho = std::make_unique<HoleFeature>("H", d.doc.GetFeature(1)->GetId(),
                                          kind, 2 * kR, kBox);
  ho->through = true;
  ho->printed = printed;
  if (kind == HoleKind::Counterbore) {
    ho->cbDiameter = 4 * kR;
    ho->cbDepth = 4.0;
  }
  d.doc.AddFeature(std::move(ho));
  d.hole = dynamic_cast<HoleFeature*>(d.doc.GetFeature(2));
}

printcheck::Report Check(const Document& doc) {
  std::vector<const Body*> bodies;
  for (const Body& b : doc.GetBodies())
    bodies.push_back(&b);
  printcheck::Options o;
  o.maxOverhangDeg = 45.0;
  o.maxBridgeMm = 10.0;
  return printcheck::Check(bodies, o);
}

std::map<std::string, int> HoleRoles(const Document& doc, uint64_t hole) {
  std::map<std::string, int> roles;
  for (const auto& [i, name] : doc.GetBodies()[0].elementMap.names) {
    const topo::ElementOrigin o = topo::DecodeElementName(name);
    if (o.ok && o.madeBy == hole)
      roles[topo::ElementRoleName(o.role)]++;
  }
  return roles;
}

} // namespace

TEST_CASE("a teardrop hole cuts a round with a 45-degree roof",
          "[hole][print][teardrop]") {
  Drilled d;
  Drill(d, HolePrinted::Teardrop);
  REQUIRE(d.hole->GetError().empty());
  REQUIRE(d.doc.GetBodies().size() == 1);
  const Body& b = d.doc.GetBodies()[0];
  // The section: three quarters of the round under the 45-degree tangents,
  // and the kite from the centre to the point, r^2.
  const double section = 0.75 * M_PI * kR * kR + kR * kR;
  CHECK_THAT(b.shape.Volume(),
             WithinRel(kBox * kBox * kBox - section * kBox, 1e-6));
  // The point is up +Z, sqrt(2) r over the axis: the material's lowest
  // point over the hole there.
  CHECK(b.shape.ContainsPoint(
      glm::dvec3(kBox / 2, kBox / 2, kBox / 2 + kR * std::sqrt(2.0) + 0.01)));
  CHECK_FALSE(b.shape.ContainsPoint(
      glm::dvec3(kBox / 2, kBox / 2, kBox / 2 + kR * std::sqrt(2.0) - 0.01)));
  // Its faces by role: the round is the bore, the two flanks its roof.
  const auto roles = HoleRoles(d.doc, d.hole->GetId());
  CHECK(roles.at("bore") == 1);
  CHECK(roles.at("roof") == 2);
}

TEST_CASE("a flat teardrop's roof is no wider than the process bridges",
          "[hole][print][teardrop]") {
  Drilled d;
  Drill(d, HolePrinted::TeardropFlat);
  REQUIRE(d.hole->GetError().empty());
  // On a r = 3 hole the flat sits on the round's top: 2 (sqrt2 - 1) r wide,
  // well under 10 mm. The tip over it, h^2 with h = (sqrt2 - 1) r, is
  // material again.
  const double h = (std::sqrt(2.0) - 1.0) * kR;
  const double section = 0.75 * M_PI * kR * kR + kR * kR - h * h;
  CHECK_THAT(d.doc.GetBodies()[0].shape.Volume(),
             WithinRel(kBox * kBox * kBox - section * kBox, 1e-6));
  CHECK(HoleRoles(d.doc, d.hole->GetId()).at("roof") == 3);
  const auto r = Check(d.doc);
  CHECK(r.bridges.status == printcheck::Status::Pass);
  CHECK_THAT(r.bridges.worstSpanMm, WithinAbs(2.0 * h, 0.05));
}

TEST_CASE("check_print passes a teardrop hole and fails the round one",
          "[hole][print][teardrop]") {
  Drilled round, tear;
  Drill(round, HolePrinted::Round);
  Drill(tear, HolePrinted::Teardrop);
  const auto a = Check(round.doc);
  CHECK(a.overhang.status == printcheck::Status::Fail);
  const auto b = Check(tear.doc);
  INFO("teardrop worst " << b.overhang.worstDeg);
  CHECK(b.overhang.status == printcheck::Status::Pass);
  CHECK_THAT(b.overhang.worstDeg, WithinAbs(45.0, 1e-6));
  CHECK(b.Ok());
}

TEST_CASE("a pin in a teardrop hole clears its round and its point",
          "[hole][print][teardrop]") {
  Drilled d;
  Drill(d, HolePrinted::Teardrop);
  // A pin 0.1 mm a side under the round, along the bore.
  const double gap = 0.1;
  const TopoDS_Shape pin =
      BRepPrimAPI_MakeCylinder(
          gp_Ax2(gp_Pnt(-5, kBox / 2, kBox / 2), gp_Dir(1, 0, 0)), kR - gap,
          kBox + 10)
          .Shape();
  BRepExtrema_DistShapeShape dist(d.doc.GetBodies()[0].shape.GetShape(), pin);
  REQUIRE(dist.IsDone());
  // The round holds it at the gap; nothing at the point comes nearer.
  CHECK_THAT(dist.Value(), WithinAbs(gap, 1e-6));
  for (int i = 1; i <= dist.NbSolution(); ++i)
    CHECK(dist.PointOnShape1(i).Z() < kBox / 2 + kR / std::sqrt(2.0) + 1e-6);
}

TEST_CASE("a teardrop is refused where it has no meaning, naming why",
          "[hole][print][teardrop]") {
  SECTION("an unprinted part") {
    Drilled d;
    Drill(d, HolePrinted::Teardrop, nullptr);
    CHECK_THAT(d.hole->GetError(), ContainsSubstring("set_print_process"));
  }
  SECTION("a hole along the print direction") {
    Drilled d;
    Drill(d, HolePrinted::Teardrop, "pla", HoleKind::Simple, {0, 0, 1});
    CHECK_THAT(d.hole->GetError(),
               ContainsSubstring("across the print direction") &&
                   ContainsSubstring("0.0 degrees"));
  }
  SECTION("a countersink") {
    Drilled d;
    Drill(d, HolePrinted::Teardrop, "pla", HoleKind::Countersink);
    CHECK_THAT(d.hole->GetError(), ContainsSubstring("countersink"));
  }
  SECTION("an unknown spelling on load") {
    nlohmann::json j = {{"type", "Hole"},
                        {"name", "H"},
                        {"id", 9},
                        {"sketch_id", 1},
                        {"printed", "tear"}};
    CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                      ContainsSubstring("teardrop_flat"));
  }
}

TEST_CASE("a teardrop's point follows the part's print direction",
          "[hole][print][teardrop]") {
  Drilled d;
  Drill(d, HolePrinted::Teardrop);
  const Body before = d.doc.GetBodies()[0];
  // Printed on its side, up +Y: the point turns to +Y.
  printset::PartSettings s;
  s.part.direction = glm::dvec3(0, 2, 0);
  d.doc.SetPrintSettings(s);
  const Body& after = d.doc.GetBodies()[0];
  const double tip = kR * std::sqrt(2.0) - 0.01;
  CHECK(before.shape.ContainsPoint(
      glm::dvec3(kBox / 2, kBox / 2 + tip, kBox / 2)));
  CHECK_FALSE(after.shape.ContainsPoint(
      glm::dvec3(kBox / 2, kBox / 2 + tip, kBox / 2)));
  CHECK(after.shape.ContainsPoint(
      glm::dvec3(kBox / 2, kBox / 2, kBox / 2 + tip)));
  // And the round round-trips, `printed` with it.
  const nlohmann::json j = DocumentIO::FeatureToJson(*d.hole);
  CHECK(j.at("printed") == "teardrop");
  auto back = DocumentIO::FeatureFromJson(j);
  CHECK(dynamic_cast<HoleFeature&>(*back).printed == HolePrinted::Teardrop);
}

TEST_CASE("a counterbored teardrop points both its rounds",
          "[hole][print][teardrop]") {
  Drilled d;
  Drill(d, HolePrinted::Teardrop, "pla", HoleKind::Counterbore);
  REQUIRE(d.hole->GetError().empty());
  const auto r = Check(d.doc);
  CHECK(r.overhang.status == printcheck::Status::Pass);
  CHECK(HoleRoles(d.doc, d.hole->GetId()).at("counterbore") == 1);
}

// --- tapered extrudes -------------------------------------------------------

namespace {

struct Tapered {
  Document doc;
  ExtrudeFeature* ext = nullptr;
};

template <class Fn>
void Taper(Tapered& t, Fn build, double distance, double taperDeg,
           bool reversed = false) {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  build(*sk);
  t.doc.AddFeature(std::move(sk));
  auto ext = std::make_unique<ExtrudeFeature>("T", t.doc.GetFeature(0)->GetId(),
                                              distance, ExtrudeMode::NewBody,
                                              reversed);
  ext->taperDeg = taperDeg;
  t.doc.AddFeature(std::move(ext));
  t.ext = dynamic_cast<ExtrudeFeature*>(t.doc.GetFeature(1));
}

void Square(SketchFeature& sk, double a) {
  const glm::dvec2 p[4] = {
      {-a / 2, -a / 2}, {a / 2, -a / 2}, {a / 2, a / 2}, {-a / 2, a / 2}};
  for (int i = 0; i < 4; ++i)
    sk.AddEntity(sketch::Line{p[i], p[(i + 1) % 4]});
}

std::set<std::string> FaceNames(const Document& doc) {
  std::set<std::string> out;
  for (const auto& [i, n] : doc.GetBodies()[0].elementMap.names)
    out.insert(n);
  return out;
}

} // namespace

TEST_CASE("a tapered extrude is the frustum of its profile",
          "[extrude][taper]") {
  const double a = 20.0, L = 10.0;
  for (const double deg : {10.0, -10.0, 30.0}) {
    INFO("taper " << deg);
    Tapered t;
    Taper(t, [&](SketchFeature& sk) { Square(sk, a); }, L, deg);
    REQUIRE(t.ext->GetError().empty());
    const double k = std::tan(glm::radians(deg));
    // The side shrinks by 2 z tan(taper): the integral of its square.
    const double v = (std::pow(a, 3) - std::pow(a - 2 * k * L, 3)) / (6 * k);
    CHECK_THAT(t.doc.GetBodies()[0].shape.Volume(), WithinRel(v, 1e-6));
  }
  // A round is a cone's frustum.
  Tapered c;
  Taper(
      c, [](SketchFeature& sk) { sk.AddEntity(sketch::Circle{{0, 0}, 8.0}); },
      L, 20.0);
  REQUIRE(c.ext->GetError().empty());
  const double r1 = 8.0 - L * std::tan(glm::radians(20.0));
  CHECK_THAT(c.doc.GetBodies()[0].shape.Volume(),
             WithinRel(M_PI * L / 3 * (64.0 + 8.0 * r1 + r1 * r1), 1e-6));
  // A plate with a hole: the hole opens as the outside closes.
  Tapered h;
  Taper(
      h,
      [&](SketchFeature& sk) {
        Square(sk, a);
        sk.AddEntity(sketch::Circle{{0, 0}, 4.0});
      },
      L, 10.0);
  REQUIRE(h.ext->GetError().empty());
  const double k = std::tan(glm::radians(10.0));
  const double outer = (std::pow(a, 3) - std::pow(a - 2 * k * L, 3)) / (6 * k);
  const double r2 = 4.0 + L * k;
  CHECK_THAT(
      h.doc.GetBodies()[0].shape.Volume(),
      WithinRel(outer - M_PI * L / 3 * (16.0 + 4.0 * r2 + r2 * r2), 1e-6));
}

TEST_CASE("a taper keeps every wall's name across an edit",
          "[extrude][taper][toponaming]") {
  Tapered t;
  Taper(t, [](SketchFeature& sk) { Square(sk, 20.0); }, 10.0, 0.0);
  REQUIRE(t.ext->GetError().empty());
  const std::set<std::string> straight = FaceNames(t.doc);
  REQUIRE(straight.size() == 6);
  t.ext->taperDeg = 15.0;
  REQUIRE(t.doc.Regenerate());
  CHECK(FaceNames(t.doc) == straight);
  // The wall a name points at is the one that leans: its normal tilts up.
  const Body& b = t.doc.GetBodies()[0];
  for (const auto& [idx, name] : b.elementMap.names) {
    if (name.find("/g") == std::string::npos)
      continue;
    int i = 0;
    for (TopExp_Explorer ex(b.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), ++i) {
      if (i != idx)
        continue;
      BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
      REQUIRE(s.GetType() == GeomAbs_Plane);
      gp_Dir n = s.Plane().Axis().Direction();
      if (ex.Current().Orientation() == TopAbs_REVERSED)
        n.Reverse();
      CHECK_THAT(n.Z(), WithinAbs(std::sin(glm::radians(15.0)), 1e-9));
    }
  }
  // And a length edit keeps them too.
  t.ext->distance = 12.0;
  REQUIRE(t.doc.Regenerate());
  CHECK(FaceNames(t.doc) == straight);
}

TEST_CASE("a taper past the profile's closing angle is refused, naming it",
          "[extrude][taper]") {
  // A 20 mm square over 10 mm closes at atan(10 / 10) = 45 degrees.
  Tapered t;
  Taper(t, [](SketchFeature& sk) { Square(sk, 20.0); }, 10.0, 46.0);
  CHECK_THAT(t.ext->GetError(), ContainsSubstring("under 45 degrees"));
  t.ext->taperDeg = 44.0;
  REQUIRE(t.doc.Regenerate());
  CHECK(t.ext->GetError().empty());
  // Leaning out, the hole closes: a Ø8 hole over 10 mm at atan(4/10).
  Tapered h;
  Taper(
      h,
      [](SketchFeature& sk) {
        Square(sk, 20.0);
        sk.AddEntity(sketch::Circle{{0, 0}, 4.0});
      },
      10.0, -25.0);
  CHECK_THAT(h.ext->GetError(), ContainsSubstring("under 21.8 degrees"));
  CHECK_THAT(h.ext->GetError(), ContainsSubstring("8 mm apart"));
  // Twisted and thin are refused, and so is another end condition.
  t.ext->twistDeg = 10.0;
  CHECK_THAT(t.ext->WhyIncomplete(), ContainsSubstring("twisted or tapered"));
  t.ext->twistDeg = 0.0;
  t.ext->endMode = ExtrudeEnd::Symmetric;
  CHECK_THAT(t.ext->WhyIncomplete(), ContainsSubstring("end_mode blind"));
}

TEST_CASE("a tapered lobe prints its underside at 45 degrees",
          "[extrude][taper][print]") {
  // A disc extruded DOWN from z = 10, leaning in 45: an underside a printer
  // stands, which the straight disc's ceiling is not.
  Tapered t;
  Taper(
      t, [](SketchFeature& sk) { sk.AddEntity(sketch::Circle{{0, 0}, 8.0}); },
      6.0, 45.0, /*reversed=*/true);
  REQUIRE(t.ext->GetError().empty());
  std::vector<const Body*> bodies{&t.doc.GetBodies()[0]};
  printcheck::Options o; // up +Z: the small end, at z = -6, on the bed
  const auto r = printcheck::Check(bodies, o);
  CHECK(r.overhang.status == printcheck::Status::Pass);
  CHECK_THAT(r.overhang.worstDeg, WithinAbs(45.0, 1e-6));
}

TEST_CASE("a taper round-trips", "[extrude][taper]") {
  Tapered t;
  Taper(t, [](SketchFeature& sk) { Square(sk, 20.0); }, 10.0, 12.5);
  const nlohmann::json j = DocumentIO::FeatureToJson(*t.ext);
  CHECK(j.at("taper_deg") == 12.5);
  auto back = DocumentIO::FeatureFromJson(j);
  CHECK(dynamic_cast<ExtrudeFeature&>(*back).taperDeg == 12.5);
  t.ext->taperDeg = 0.0;
  CHECK_FALSE(DocumentIO::FeatureToJson(*t.ext).contains("taper_deg"));
}

// --- TPU
// ------------------------------------------------------------------------

TEST_CASE("the TPU table is FDM's, sourced, and to be confirmed",
          "[print][allowance][tpu]") {
  const printfit::Process tpu{"fdm", "tpu", std::nullopt};
  CHECK(printfit::Validate(tpu).empty());
  const printfit::Table* t = printfit::Find(tpu);
  REQUIRE(t != nullptr);
  for (const printfit::Row& r : t->rows) {
    INFO(printfit::PrintedName(r.kind));
    CHECK_THAT(r.source, ContainsSubstring("TO CONFIRM WITH A COUPON"));
  }
  // check_print's limits: the same 45 degrees, a shorter bridge than PLA's,
  // its gap a side, and its own chords.
  const printfit::Process pla{"fdm", "pla", std::nullopt};
  CHECK(printfit::MaxOverhangDeg(tpu) == printfit::MaxOverhangDeg(pla));
  CHECK(printfit::MaxBridgeMm(tpu) < printfit::MaxBridgeMm(pla));
  CHECK_THAT(printfit::SmallestGapSideMm(*t), WithinAbs(0.5, 1e-12));
  CHECK_THAT(printfit::StlDeflectionMm(tpu), WithinAbs(0.02, 1e-12));
  CHECK(printfit::StlDeflectionMm(tpu) <
        printfit::SmallestGapSideMm(*t) / 10.0);
  // A printed thread in TPU is not carried: refused, not read from PLA's.
  std::string why;
  CHECK_FALSE(
      printfit::ForBasic(tpu, printfit::Printed::InternalThread, 8.0, &why));
  CHECK_THAT(why, ContainsSubstring("tpu") &&
                      ContainsSubstring("no internal_thread row"));
  // A profile from it keeps the chords.
  const auto prof = printfit::ProfileFromBuiltin("TPU", "fdm", "tpu");
  REQUIRE(prof.has_value());
  CHECK(prof->stlDeflectionMm == 0.02);
  CHECK(printfit::ValidateProfile(*prof).empty());
}

TEST_CASE("a TPU plate fits the bed of its printer's profile",
          "[print][tpu][bed]") {
  // The bed is the printer's: a TPU profile carrying the P1S's bed fits a
  // TPU part as a PLA one does.
  auto prof = printfit::ProfileFromBuiltin("TPU on P1S", "fdm", "tpu");
  REQUIRE(prof.has_value());
  prof->bedMm = std::array<double, 3>{256.0, 256.0, 250.0};
  prof->layerHeightMm = 0.2;
  Document doc;
  doc.SetPrintProcess(printfit::FromProfile(*prof));
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::dvec3(0), 40, 30, 5));
  std::vector<const Body*> bodies{&doc.GetBodies()[0]};
  const auto fit =
      printcheck::FitOnBed(bodies, glm::dvec3(0, 0, 1),
                           glm::dvec3(256.0, 256.0, 250.0), prof->bedExcludeMm);
  CHECK(fit.status == printcheck::Status::Pass);
}
