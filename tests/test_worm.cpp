// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A worm and its wheel (docs/modeling/worm-gears.md): the worm's sizes and
// its exact volume, the wheel's hobbed envelope converging as the hob's steps
// shrink, the refusals, and the pair meshing through a motion study checked
// exactly at every sample — the relation's ratio and direction proved on the
// real geometry, right- and left-handed, with a wrong sign that clashes as
// the control.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cad/NonDestructiveBoolean.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/WormGear.h"
#include "core/features/BooleanFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/WormFeature.h"
#include "core/io/Serialization.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Circ.hxx>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = 3.14159265358979323846;

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

// A blank on the part's Z axis from z = 0, and its outside face.
std::unique_ptr<Document> Blank(double radius, double height) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Blank", glm::vec3(0.0f), glm::vec3(0, 0, 1), radius, height));
  doc->Regenerate();
  return doc;
}
ElementRef BlankFace(const Document& doc) {
  return ElementRef{doc.GetFeature(0)->GetId(),
                    CylFaceName(doc.GetBodies().at(0))};
}

// The head's pair (docs/modeling/worm-gears.md#sizing-a-pair): m 2, one
// start, 40 teeth, q 10 — a worm blank of 24 mm and a wheel blank of 86.
std::unique_ptr<Document> WormPart(bool right, double backlash) {
  auto doc = Blank(12.0, 40.0);
  auto w = std::make_unique<WormFeature>("Worm", BlankFace(*doc));
  w->rightHanded = right;
  w->backlash = backlash;
  doc->AddFeature(std::move(w));
  doc->Regenerate();
  return doc;
}
std::unique_ptr<Document> WheelPart(bool right) {
  auto doc = Blank(43.0, 13.0);
  auto w = std::make_unique<WormWheelFeature>("Wheel", BlankFace(*doc));
  w->rightHanded = right;
  doc->AddFeature(std::move(w));
  doc->Regenerate();
  return doc;
}

} // namespace

TEST_CASE("Worm pair sizes follow KHK's worked example", "[worm]") {
  worm::PairSpec s;
  s.module = 3.0;
  s.starts = 2;
  s.teeth = 30;
  s.wormPitchDia = 44.0;
  REQUIRE(worm::Check(s, true).empty());
  const worm::Sizes z = worm::Size(s);
  CHECK_THAT(z.centreDistance, WithinAbs(67.0, 1e-12));
  CHECK_THAT(2.0 * z.wheelThroatRadius, WithinAbs(96.0, 1e-12));
  CHECK_THAT(z.throatRadius, WithinAbs(19.0, 1e-12));
  CHECK_THAT(z.lead, WithinAbs(2.0 * kPi * 3.0, 1e-12));
  CHECK_THAT(std::tan(z.leadAngle), WithinAbs(6.0 / 44.0, 1e-12));

  // The head: gamma = atan(0.1) = 5.71 degrees, locked dry (mu 0.2 gives
  // 11.3), and exactly at the edge greased (mu 0.1 IS tan gamma).
  s = {};
  s.module = 2.0;
  s.teeth = 40;
  s.wormPitchDia = 20.0;
  const worm::Sizes h = worm::Size(s);
  CHECK_THAT(h.centreDistance, WithinAbs(50.0, 1e-12));
  CHECK_THAT(h.throatRadius, WithinAbs(8.0, 1e-12));
  CHECK_THAT(h.leadAngle * 180.0 / kPi, WithinAbs(5.7106, 1e-4));
  const worm::Locking dry = worm::SelfLocking(h.leadAngle, 0.2);
  CHECK(dry.locks);
  CHECK_THAT(dry.frictionAngleDeg, WithinAbs(11.3099, 1e-4));
  CHECK_THAT(dry.efficiency,
             WithinAbs(0.1 / std::tan(h.leadAngle + std::atan(0.2)), 1e-12));
  CHECK_FALSE(worm::SelfLocking(h.leadAngle, 0.05).locks);
  CHECK(worm::ThroatArcDeg(h, 13.0, h.wheelTipRadius) <= 90.0);
}

TEST_CASE("Worm and wheel refuse what they cannot build", "[worm]") {
  worm::PairSpec s;
  s.module = 2.0;
  s.teeth = 40;
  s.wormPitchDia = 20.0;
  s.starts = 0;
  CHECK_THAT(worm::Check(s, false), ContainsSubstring("1 to 8 starts"));
  s.starts = 1;
  s.pressureDeg = 45.0;
  CHECK_THAT(worm::Check(s, false), ContainsSubstring("pressure angle"));
  s.pressureDeg = 20.0;
  s.wormPitchDia = 4.0; // no root left
  CHECK_THAT(worm::Check(s, false), ContainsSubstring("leaves no root"));
  s.wormPitchDia = 20.0;
  s.teeth = 6;
  CHECK_THAT(worm::Check(s, true), ContainsSubstring("at least 8 teeth"));
  s.teeth = 40;
  std::string why;
  CHECK(worm::WormSpaceProfile(s, 3.0, 13.0, &why).empty()); // pointed
  CHECK_THAT(why, ContainsSubstring("comes to a point"));

  // A hand we do not know is refused, not read as right.
  auto doc = Blank(12.0, 40.0);
  auto j = DocumentIO::FeatureToJson(WormFeature("W", BlankFace(*doc)));
  j["hand"] = "sinister";
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                    ContainsSubstring("accepted: right, left"));

  // A face wider than the worm can reach is refused, saying how wide.
  auto wide = Blank(43.0, 30.0);
  wide->AddFeature(
      std::make_unique<WormWheelFeature>("Wheel", BlankFace(*wide)));
  wide->Regenerate();
  CHECK_THAT(wide->GetFeature(1)->GetError(),
             ContainsSubstring("wider than the worm can cut"));
}

TEST_CASE("A worm is its blank less the ZA tooth space, to Pappus", "[worm]") {
  auto doc = WormPart(true, 0.2);
  const auto* w = dynamic_cast<const WormFeature*>(doc->GetFeature(1));
  REQUIRE(w != nullptr);
  INFO(w->GetError());
  REQUIRE(w->GetError().empty());
  REQUIRE(w->report.valid);
  CHECK_THAT(w->report.spec.module, WithinAbs(2.0, 1e-12));
  CHECK_THAT(w->report.spec.wormPitchDia, WithinAbs(20.0, 1e-12));
  const nlohmann::json rep = w->ReportJson();
  CHECK_THAT(rep.at("lead_angle_deg").get<double>(), WithinAbs(5.7106, 1e-4));
  CHECK(rep.at("locking").at(0).at("self_locking").get<bool>());

  // Both ends open onto air, so the space runs through both and the cut is
  // screw-invariant over the whole length: it removes exactly 2 pi rbar A
  // per axial pitch (Pappus for a screw motion), A being the part of the
  // space's profile inside the blank — a trapezoid 2h0 wide at the root x0
  // and 2h1 at the tip x1.
  const auto prof = worm::WormSpaceProfile(w->report.spec, 0.2, 13.0, nullptr);
  REQUIRE(prof.size() == 6);
  const double x0 = prof[0].X(), x1 = prof[1].X();
  const double h0 = prof[5].Y(), h1 = prof[4].Y();
  const double area = (x1 - x0) * (h0 + h1);
  const double rbar = x0 + (x1 - x0) * (h0 + 2.0 * h1) / (3.0 * (h0 + h1));
  const double expected = 2.0 * kPi * rbar * area * 40.0 / (kPi * 2.0);
  const double blank = kPi * 12.0 * 12.0 * 40.0;
  const double removed = blank - doc->GetBodies().at(0).shape.Volume();
  CHECK_THAT(removed, WithinRel(expected, 2e-3));
  CHECK(doc->GetBodies().at(0).shape.SolidCount() == 1);
}

TEST_CASE("The hobbed envelope converges as the hob's steps shrink", "[worm]") {
  // The measure: one space at n, 2n, 4n and continuous, read on its
  // mid-plane section's area (the loft between sections adds a noise of its
  // own that the finer steps would drown in).
  // The scallops a discrete hob leaves shrink as 1/n^2, so the gap to the
  // continuous envelope falls about fourfold per doubling.
  worm::PairSpec s;
  s.module = 2.0;
  s.teeth = 40;
  s.wormPitchDia = 20.0;
  const auto space = [&](int steps) {
    worm::HobOptions o;
    o.stepsPerPitch = steps;
    worm::WheelTools t;
    worm::HobStats st;
    std::string why;
    const bool ok =
        worm::WheelCutTools(gp_Ax3(), 6.5, 43.0, s, 0.1, o, &t, &st, &why);
    INFO(why);
    REQUIRE(ok);
    CHECK(st.slices % 2 == 0);
    return st.midArea;
  };
  const double v8 = space(8), v16 = space(16), v32 = space(32), vc = space(0);
  CHECK(v8 < v16);
  CHECK(v16 < vc);
  const double ratio = (vc - v8) / (vc - v16);
  CHECK(ratio > 3.0);
  CHECK(ratio < 5.0);
  CHECK((vc - v32) / vc < 2e-4);
}

TEST_CASE("A wheel is hobbed from its blank: throat, tip and teeth",
          "[worm][slow]") {
  auto doc = WheelPart(true);
  const auto* w = dynamic_cast<const WormWheelFeature*>(doc->GetFeature(1));
  REQUIRE(w != nullptr);
  INFO(w->GetError());
  REQUIRE(w->GetError().empty());
  const nlohmann::json rep = w->ReportJson();
  CHECK_THAT(rep.at("module").get<double>(), WithinAbs(2.0, 1e-12));
  CHECK_THAT(rep.at("centre_distance").get<double>(), WithinAbs(50.0, 1e-12));
  CHECK_THAT(rep.at("throat_diameter").get<double>(), WithinAbs(84.0, 1e-12));
  CHECK(rep.at("throat_arc_deg").get<double>() <= 90.0);
  CHECK(rep.at("tip_crest").get<double>() >= 0.5);
  CHECK(rep.at("hob_seconds").get<double>() < 30.0);
  const Body& b = doc->GetBodies().at(0);
  CHECK(b.shape.SolidCount() == 1);
  // The teeth left the rim: well under the blank, well over the root disc.
  const double blank = kPi * 43.0 * 43.0 * 13.0;
  const double root = kPi * 35.0 * 35.0 * 13.0;
  CHECK(b.shape.Volume() < blank - 0.05 * blank);
  CHECK(b.shape.Volume() > root);
}

// The wheel's crest is read where it is thinnest: at one radius a tooth
// narrows from the face ends inward, to where the throat comes up to meet the
// tip -- sqrt(ri^2 - (a - tip)^2) = sqrt(8^2 - 7^2) off the mid-plane on the
// head's pair. It used to be read at the face ends, and said 2.1 mm of a
// crest that is 1.0 where the throat meets it. Proved on the body: a circle
// just under the tip, cut by the solid, is z2 crests long.
TEST_CASE("A wheel's tip crest is read where the tooth is thinnest",
          "[worm][slow]") {
  auto doc = WheelPart(true);
  const auto* w = dynamic_cast<const WormWheelFeature*>(doc->GetFeature(1));
  REQUIRE(w != nullptr);
  INFO(w->GetError());
  REQUIRE(w->GetError().empty());
  const nlohmann::json rep = w->ReportJson();
  const double kink = std::sqrt(8.0 * 8.0 - 7.0 * 7.0);
  const double crest = rep.at("tip_crest").get<double>();
  CHECK(crest >= 0.5); // the m/4 floor
  CHECK_THAT(std::abs(rep.at("tip_crest_z").get<double>()),
             WithinAbs(kink, 0.02));
  CHECK_THAT(rep.at("tip_crest_diameter").get<double>(), WithinAbs(86.0, 1e-6));

  // The crest on the body, at a radius just under the tip: the length of a
  // circle there inside the solid, over the teeth. The blank runs z 0..13,
  // its mid-plane at 6.5.
  const TopoDS_Shape& body = doc->GetBodies().at(0).shape.GetShape();
  const auto crestOnBody = [&](double R, double zOff) {
    const TopoDS_Edge ring = BRepBuilderAPI_MakeEdge(
        gp_Circ(gp_Ax2(gp_Pnt(0, 0, 6.5 + zOff), gp_Dir(0, 0, 1)), R));
    BRepAlgoAPI_Common common;
    REQUIRE(cad::BuildNonDestructive(common, ring, body));
    GProp_GProps props;
    BRepGProp::LinearProperties(common.Shape(), props);
    return props.Mass() / 40.0;
  };
  const double R = 42.98; // the throat reaches it 3.84 off the mid-plane
  const double atKink = crestOnBody(R, -3.9);
  const double atEnd = crestOnBody(R, -6.4);
  INFO("at the kink " << atKink << ", at the face end " << atEnd);
  CHECK_THAT(atKink, WithinAbs(crest, 0.1));
  CHECK(atEnd > crest + 0.8);
  // Both sides of the mid-plane: the thinnest is within a tenth of either.
  CHECK(std::abs(crestOnBody(R, 3.9) - atKink) < 0.1);
}

namespace {

// The pair on a carrier: the wheel on a revolute at the carrier's origin
// (its axis Z), the worm on a revolute at a Frame 50 mm out along +X, its
// axis along +Y and its X towards the wheel — so the worm's tooth (on its
// own X side, mid-span) stands in the wheel's space 0 (on the wheel's X
// side, mid-face) at zero turn (docs/modeling/worm-gears.md#phase).
struct Pair {
  std::unique_ptr<Document> carrier, worm, wheel;
  AssemblyDocument asmDoc;
  uint64_t carrierOcc = 0, wormOcc = 0, wheelOcc = 0;
  uint64_t wormMate = 0, wheelMate = 0;
  assembly::PartResolver Parts() {
    return [this](uint64_t id, uint64_t) -> const Document* {
      return id == 1   ? carrier.get()
             : id == 2 ? worm.get()
             : id == 3 ? wheel.get()
                       : nullptr;
    };
  }
};

Connector Origin(uint64_t occ) {
  Connector c;
  c.occurrenceId = occ;
  c.anchor.origin = true;
  return c;
}

std::unique_ptr<Pair> MakePair(bool right) {
  auto p = std::make_unique<Pair>();
  p->carrier = std::make_unique<Document>();
  auto fr = std::make_unique<FrameFeature>("Worm axis", FrameKind::Explicit);
  fr->origin[0] = 50.0;
  fr->origin[1] = -20.0; // the worm's mid-span (z = 20 on it) at y = 0
  fr->origin[2] = 6.5;   // the wheel's mid-plane
  fr->zAxis = glm::vec3(0, 1, 0);
  fr->xAxis = glm::vec3(-1, 0, 0);
  p->carrier->AddFeature(std::move(fr));
  p->carrier->Regenerate();
  p->worm = WormPart(right, 0.2);
  p->wheel = WheelPart(right);
  Occurrence c;
  c.name = "Carrier";
  c.partId = 1;
  c.fixed = true;
  p->carrierOcc = p->asmDoc.AddOccurrence(std::move(c));
  Occurrence wo;
  wo.name = "Worm";
  wo.partId = 2;
  p->wormOcc = p->asmDoc.AddOccurrence(std::move(wo));
  Occurrence wh;
  wh.name = "Wheel";
  wh.partId = 3;
  p->wheelOcc = p->asmDoc.AddOccurrence(std::move(wh));
  Mate m;
  m.type = MateType::Revolute;
  m.flipped = false;
  m.name = "worm shaft";
  m.a = Origin(p->carrierOcc);
  m.a.anchor.origin = false;
  m.a.anchor.frameId = p->carrier->GetFeature(0)->GetId();
  m.b = Origin(p->wormOcc);
  p->wormMate = p->asmDoc.AddMate(m);
  m.name = "wheel shaft";
  m.a = Origin(p->carrierOcc);
  m.b = Origin(p->wheelOcc);
  p->wheelMate = p->asmDoc.AddMate(m);
  return p;
}

// The worm relation as add_relation makes it: its ratio from the two mates'
// frames in world space (the carrier is fixed at the origin).
double WormRatio(Pair& p, bool right) {
  const Mate* a = p.asmDoc.FindMateById(p.wormMate);
  const Mate* b = p.asmDoc.FindMateById(p.wheelMate);
  const auto fa = assembly::ConnectorLocalFrame(a->a, p.asmDoc, p.Parts());
  const auto fb = assembly::ConnectorLocalFrame(b->a, p.asmDoc, p.Parts());
  REQUIRE(fa.has_value());
  REQUIRE(fb.has_value());
  std::string why;
  const double r = worm::RelationRatio(p.asmDoc, *a, *b, true, true, *fa, *fb,
                                       1, 40, right, &why);
  INFO(why);
  REQUIRE(r != 0.0);
  return r;
}

motion::Result Turn(Pair& p, double ratio, double turns, int samples) {
  Relation r;
  r.kind = RelationKind::Worm;
  r.mateA = p.wormMate;
  r.mateB = p.wheelMate;
  r.dofA = r.dofB = MateDof::Rotation;
  r.ratio = ratio;
  p.asmDoc.GetRelations().clear();
  p.asmDoc.AddRelation(r);
  MotionStudy s;
  s.name = "worm";
  s.driverMate = p.wormMate;
  s.driverDof = MateDof::Rotation;
  s.from = 0.0;
  s.to = turns * 2.0 * kPi;
  s.samples = samples;
  s.clashMode = "exact";
  return motion::Run(p.asmDoc, p.Parts(), nullptr, s);
}

double WorstClash(const motion::Result& res) {
  double worst = 0.0;
  for (const auto& sm : res.samples)
    for (const auto& c : sm.clashes)
      worst = std::max(worst, c.known ? c.volume : 1e9);
  return worst;
}

} // namespace

TEST_CASE("The worm relation's sign follows the hand and the side", "[worm]") {
  // Frames only: the worm's axis +Y at x = 50, the wheel's +Z at the
  // origin, both gears on their mates' B sides. u x w = +X, and the worm
  // sits on +X: a right-hand worm turns the wheel negatively.
  AssemblyDocument asmDoc;
  Occurrence c;
  c.fixed = true;
  const uint64_t carrier = asmDoc.AddOccurrence(std::move(c));
  const uint64_t wo = asmDoc.AddOccurrence(Occurrence{});
  const uint64_t wh = asmDoc.AddOccurrence(Occurrence{});
  Mate a, b;
  a.a = Origin(carrier);
  a.b = Origin(wo);
  b.a = Origin(carrier);
  b.b = Origin(wh);
  glm::mat4 fu(1.0f), fw(1.0f);
  fu[0] = glm::vec4(-1, 0, 0, 0);
  fu[1] = glm::vec4(0, 0, 1, 0);
  fu[2] = glm::vec4(0, 1, 0, 0);
  fu[3] = glm::vec4(50, 0, 0, 1);
  std::string why;
  const auto ratio = [&](bool right) {
    return worm::RelationRatio(asmDoc, a, b, true, true, fu, fw, 1, 40, right,
                               &why);
  };
  CHECK_THAT(ratio(true), WithinAbs(-1.0 / 40.0, 1e-15));
  CHECK_THAT(ratio(false), WithinAbs(1.0 / 40.0, 1e-15));
  // The worm on the other side reverses it.
  fu[3] = glm::vec4(-50, 0, 0, 1);
  CHECK_THAT(ratio(true), WithinAbs(1.0 / 40.0, 1e-15));
  // So does the worm on its mate's A side (the carrier on B).
  std::swap(a.a, a.b);
  CHECK_THAT(ratio(true), WithinAbs(-1.0 / 40.0, 1e-15));
  // Parallel or meeting axes are refused.
  fu[2] = glm::vec4(0, 0, 1, 0);
  CHECK(ratio(true) == 0.0);
  CHECK_THAT(why, ContainsSubstring("parallel or meet"));
}

TEST_CASE("A worm pair meshes through a turn with no clash", "[worm][slow]") {
  for (const bool right : {true, false}) {
    INFO((right ? "right-hand" : "left-hand"));
    auto p = MakePair(right);
    REQUIRE(p->worm->GetFeature(1)->GetError().empty());
    REQUIRE(p->wheel->GetFeature(1)->GetError().empty());
    const double ratio = WormRatio(*p, right);
    CHECK_THAT(std::abs(ratio), WithinAbs(1.0 / 40.0, 1e-15));
    // Through a wheel pitch and more (a worm turn is a wheel pitch at one
    // start), every sample checked exactly: a quarter turn apart, so the
    // worm's tooth is seen at every side of its turn.
    const motion::Result res = Turn(*p, ratio, 1.25, 6);
    INFO(res.error);
    REQUIRE(res.error.empty());
    CHECK(res.pairsChecked > 0);
    for (const auto& sm : res.samples)
      CHECK(sm.solved);
    CHECK(WorstClash(res) < 1e-3);
    CHECK(res.clean);
    // The control: the sign a gear relation would give the other hand.
    const motion::Result wrong = Turn(*p, -ratio, 0.25, 2);
    CHECK_FALSE(wrong.clean);
    CHECK(WorstClash(wrong) > 0.5);
  }
}

namespace {

// The throated blank of the head's wheel, the way the feature throats it,
// with a bore and five lightening holes drilled through its web (all inside
// the root) when `holes`, and the tools that cut its teeth.
struct Throated {
  gp_Ax3 frame;
  worm::WheelTools tools;
  worm::HobStats stats;
  TopoDS_Shape body;
};
Throated HeadWheel(bool holes) {
  Throated t;
  worm::PairSpec spec;
  spec.teeth = 40;
  spec.module = 2.0;
  spec.wormPitchDia = 20.0;
  t.frame = gp_Ax3(gp_Pnt(0, 0, 6.5), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0));
  t.frame.Rotate(gp_Ax1(gp_Pnt(0, 0, 6.5), gp_Dir(0, 0, 1)), kPi / 40);
  std::string why;
  REQUIRE(worm::WheelCutTools(t.frame, 6.5, 43.0, spec, 0.1, {}, &t.tools,
                              &t.stats, &why));
  TopoDS_Shape blank = BRepPrimAPI_MakeCylinder(
                           gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 43.0, 13.0)
                           .Shape();
  if (holes) {
    TopoDS_Compound drill;
    BRep_Builder bb;
    bb.MakeCompound(drill);
    bb.Add(drill, BRepPrimAPI_MakeCylinder(
                      gp_Ax2(gp_Pnt(0, 0, -1), gp_Dir(0, 0, 1)), 10.0, 15.0)
                      .Shape());
    for (int k = 0; k < 5; k++)
      bb.Add(drill, BRepPrimAPI_MakeCylinder(
                        gp_Ax2(gp_Pnt(22.0 * std::cos(k * 2 * kPi / 5),
                                      22.0 * std::sin(k * 2 * kPi / 5), -1),
                               gp_Dir(0, 0, 1)),
                        3.0, 15.0)
                        .Shape());
    BRepAlgoAPI_Cut drilled;
    REQUIRE(cad::BuildNonDestructive(drilled, blank, drill));
    blank = drilled.Shape();
  }
  BRepAlgoAPI_Cut throat;
  REQUIRE(cad::BuildNonDestructive(throat, blank, t.tools.rim.GetShape()));
  t.body = throat.Shape();
  return t;
}

double VolumeOf(const TopoDS_Shape& s) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(s, props);
  return props.Mass();
}

int FaceCount(const TopoDS_Shape& s) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(s, TopAbs_FACE, faces);
  return faces.Extent();
}

} // namespace

TEST_CASE("Teeth cut by replication are the teeth cut whole", "[worm][slow]") {
  // The whole cut, forty spaces at once, is the reference: the same volume
  // to a part in a million, and the same points inside it.
  const Throated plain = HeadWheel(false);
  BRepAlgoAPI_Cut whole;
  REQUIRE(cad::BuildNonDestructive(whole, plain.body,
                                   plain.tools.spaces.GetShape()));
  const double wholeVolume = VolumeOf(whole.Shape());

  worm::SectorCut cut;
  std::string why;
  const bool replicated = worm::CutSpacesBySectors(plain.body, plain.frame, 40,
                                                   plain.tools, &cut, &why);
  INFO(why);
  REQUIRE(replicated);
  CHECK(BRepCheck_Analyzer(cut.result).IsValid());
  CHECK(CADShape(cut.result).SolidCount() == 1);
  const double v = VolumeOf(cut.result);
  CHECK_THAT(v, WithinRel(wholeVolume, 1e-6));
  CHECK_THAT(VolumeOf(plain.body) - v, WithinRel(cut.removed, 1e-6));
  // Splitting faces at the sectors and merging them back leaves the faces
  // the whole cut has, but for the root of the one space where the ring of
  // copies closes, and the blank's seam.
  CHECK(std::abs(FaceCount(cut.result) - FaceCount(whole.Shape())) <= 2);
  // The same solid: points all through the teeth classify alike.
  BRepClass3d_SolidClassifier a(whole.Shape()), b(cut.result);
  int differ = 0, inside = 0, outside = 0;
  for (int i = 0; i < 400; i++) {
    const double ang = 2 * kPi * i / 400.0 + 0.37 * i;
    const double rad = 36.0 + 7.0 * std::fmod(0.618 * i, 1.0);
    const double z = 13.0 * std::fmod(0.414 * i, 1.0);
    const gp_Pnt p(rad * std::cos(ang), rad * std::sin(ang), z);
    a.Perform(p, 1e-6);
    b.Perform(p, 1e-6);
    inside += a.State() == TopAbs_IN;
    outside += a.State() == TopAbs_OUT;
    differ += a.State() != b.State();
  }
  CHECK(inside > 100);
  CHECK(outside > 50);
  CHECK(differ == 0);

  // The history carries every space's faces onto the wheel.
  int spaceFacesOnWheel = 0;
  TopTools_IndexedMapOfShape resultFaces;
  TopExp::MapShapes(cut.result, TopAbs_FACE, resultFaces);
  for (TopExp_Explorer ex(plain.tools.spaces.GetShape(), TopAbs_FACE);
       ex.More(); ex.Next())
    for (const TopoDS_Shape& m : cut.history->Modified(ex.Current()))
      spaceFacesOnWheel += resultFaces.Contains(m);
  CHECK(spaceFacesOnWheel >= 40 * 3);

  // A blank drilled through its web is split: the ring is replicated, the
  // rest kept, and the holes take exactly their own volume.
  const Throated drilled = HeadWheel(true);
  worm::SectorCut split;
  REQUIRE(worm::CutSpacesBySectors(drilled.body, drilled.frame, 40,
                                   drilled.tools, &split, &why));
  CHECK(BRepCheck_Analyzer(split.result).IsValid());
  CHECK(CADShape(split.result).SolidCount() == 1);
  const double holes = kPi * 13.0 * (10.0 * 10.0 + 5 * 3.0 * 3.0);
  CHECK_THAT(VolumeOf(split.result), WithinRel(wholeVolume - holes, 1e-6));
}

namespace {

// The stable name of the body's largest cylindrical face: the rim.
std::string RimFaceName(const Body& body) {
  int i = 0, best = -1;
  double radius = 0.0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
    if (s.GetType() == GeomAbs_Cylinder && s.Cylinder().Radius() > radius) {
      radius = s.Cylinder().Radius();
      best = i;
    }
  }
  return best < 0 ? std::string{} : body.elementMap.names.at(best);
}

// The head's wheel blank with each of `drills` (a point on the axis of a
// hole along Z, and its radius) cut from it first, then the wheel.
std::unique_ptr<Document>
DrilledWheel(const std::vector<std::pair<glm::vec3, double>>& drills) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Blank", glm::vec3(0.0f), glm::vec3(0, 0, 1), 43.0, 13.0));
  // A boolean's result is the body of the boolean that made it.
  uint64_t owner = doc->GetFeature(0)->GetId();
  for (const auto& [at, radius] : drills) {
    doc->AddFeature(std::make_unique<CylinderFeature>(
        "Drill", at, glm::vec3(0, 0, 1), radius, 15.0));
    doc->AddFeature(std::make_unique<BooleanFeature>(
        "Cut", owner, doc->GetFeature(doc->FeatureCount() - 1)->GetId(),
        BooleanOp::Subtract));
    owner = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
  }
  const Body& body = doc->GetBodies().at(0);
  doc->AddFeature(std::make_unique<WormWheelFeature>(
      "Wheel", ElementRef{owner, RimFaceName(body)}));
  return doc;
}

const WormWheelFeature& Wheel(const Document& doc) {
  const auto* w = dynamic_cast<const WormWheelFeature*>(
      doc.GetFeature(doc.FeatureCount() - 1));
  REQUIRE(w != nullptr);
  return *w;
}

} // namespace

TEST_CASE("A drilled wheel is replicated and keeps the blank's names",
          "[worm][slow]") {
  // A bore and a lightening hole, both inside the root: the web is not
  // revolved, the rim is.
  auto doc =
      DrilledWheel({{glm::vec3(0, 0, -1), 10.0}, {glm::vec3(22, 0, -1), 3.0}});
  const WormWheelFeature& w = Wheel(*doc);
  INFO(w.GetError());
  REQUIRE(w.GetError().empty());
  CHECK(w.ReportJson().at("teeth_cut") == "replicated");
  const Body& body = doc->GetBodies().at(0);
  CHECK(body.shape.SolidCount() == 1);
  // Every face is named, and no two alike; the bore and the hole, which the
  // wheel never touched, keep the names they had.
  std::vector<std::string> names;
  int faces = 0, holes = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), faces++) {
    const std::string* name = body.elementMap.NameOf(faces);
    REQUIRE(name != nullptr);
    names.push_back(*name);
    BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
    if (s.GetType() == GeomAbs_Cylinder && s.Cylinder().Radius() < 20.0) {
      holes++;
      INFO(*name);
      CHECK(name->find(";M" + std::to_string(w.GetId())) == std::string::npos);
    }
  }
  CHECK(holes == 2);
  std::sort(names.begin(), names.end());
  CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
}

TEST_CASE("A rim that is not revolved has its teeth cut whole",
          "[worm][slow]") {
  // A hole through the rim, where the teeth go: replicating a sector would
  // copy it round, so the wheel cuts the forty spaces at once.
  auto doc = DrilledWheel({{glm::vec3(39.0, 0, -1), 1.0}});
  const WormWheelFeature& w = Wheel(*doc);
  INFO(w.GetError());
  REQUIRE(w.GetError().empty());
  CHECK(w.ReportJson().at("teeth_cut") == "whole");
  CHECK(doc->GetBodies().at(0).shape.SolidCount() == 1);
}

TEST_CASE("An undone wheel edit comes back from its inputs", "[worm][slow]") {
  auto doc = WheelPart(true);
  const double v0 = doc->GetBodies().at(0).shape.Volume();
  const ElementMap names0 = doc->GetBodies().at(0).elementMap;
  CHECK_FALSE(Wheel(*doc).ReportJson().at("reused").get<bool>());
  const auto edit = [&](int teeth) {
    auto w = Wheel(*doc).Clone();
    dynamic_cast<WormWheelFeature&>(*w).teeth = teeth;
    doc->ReplaceFeature(1, std::move(w));
  };
  edit(41);
  CHECK_FALSE(Wheel(*doc).ReportJson().at("reused").get<bool>());
  CHECK(doc->GetBodies().at(0).shape.Volume() != v0);
  edit(40); // as an undo does
  // The regeneration cache remembers the wheel at 40 teeth, so the undo
  // executes nothing (docs/modeling/README.md#regeneration)...
  CHECK(doc->LastRegenStats().executed == 0);
  CHECK(doc->GetBodies().at(0).shape.Volume() == v0);
  // ...and when the wheel does execute on the same blank with the same
  // numbers -- a change it does not read, like its name -- its own memo
  // serves it.
  {
    auto w = Wheel(*doc).Clone();
    w->SetName("renamed wheel");
    doc->ReplaceFeature(1, std::move(w));
  }
  CHECK(doc->LastRegenStats().executed == 1);
  CHECK(Wheel(*doc).ReportJson().at("reused").get<bool>());
  CHECK(doc->GetBodies().at(0).shape.Volume() == v0);
  CHECK(doc->GetBodies().at(0).elementMap.names == names0.names);
  CHECK(doc->GetBodies().at(0).elementMap.edgeNames == names0.edgeNames);
}
