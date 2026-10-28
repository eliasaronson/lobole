// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A disc cam (mechanism) cut from a motion (docs/modeling/cam-profiles.md):
// the laws against their textbook forms, the profile against shapes whose
// envelope is known in closed form (an eccentric circle under a flat face
// and under a roller), the pressure angle against tan(phi) = (s' - e) /
// (s + sqrt(Rp^2 - e^2)), and undercut refused with the base radius that
// clears it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestPaths.h"
#include "core/Angles.h"
#include "core/AssemblySolve.h"
#include "core/CamProfileMath.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/features/CamProfileFeature.h"
#include "core/features/FrameFeature.h"
#include "core/io/Serialization.h"

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <filesystem>
#include <random>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace cammech;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

MotionSpec RiseDwellFall(double h, Law law = Law::Cycloidal) {
  MotionSpec m;
  m.segments = {{120, h, law},
                {180, 0, Law::Dwell},
                {300, -h, law},
                {360, 0, Law::Dwell}};
  return m;
}

FollowerSpec Roller(double r, double e = 0) {
  FollowerSpec f;
  f.kind = FollowerKind::Roller;
  f.radiusMm = r;
  f.offsetMm = e;
  return f;
}

} // namespace

TEST_CASE("cam profile: the laws are their textbook forms", "[cam_profile]") {
  const double h = 10.0;
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> U(0.0, 120.0);
  for (Law law : {Law::Cycloidal, Law::Harmonic, Law::Poly345}) {
    const MotionSpec m = RiseDwellFall(h, law);
    const FollowerSpec f = Roller(4);
    CHECK(MotionError(m, f).empty());
    CHECK_THAT(EvaluateMotion(m, f, 0).s, WithinAbs(0, 1e-12));
    CHECK_THAT(EvaluateMotion(m, f, 150).s, WithinAbs(h, 1e-12));
    for (int k = 0; k < 200; k++) {
      const double deg = U(rng);
      const double beta = angles::ToRadians(120.0);
      const double u = deg / 120.0;
      double s = 0;
      switch (law) {
      case Law::Cycloidal:
        s = h * (u - std::sin(2 * M_PI * u) / (2 * M_PI));
        break;
      case Law::Harmonic:
        s = h / 2 * (1 - std::cos(M_PI * u));
        break;
      default:
        s = h *
            (10 * std::pow(u, 3) - 15 * std::pow(u, 4) + 6 * std::pow(u, 5));
      }
      const LiftAt l = EvaluateMotion(m, f, deg);
      CHECK_THAT(l.s, WithinAbs(s, 1e-9));
      // The derivatives are per radian: check them by differencing s.
      const double d = 1e-4;
      const double sp = EvaluateMotion(m, f, deg + angles::ToDegrees(d)).s;
      const double sm = EvaluateMotion(m, f, deg - angles::ToDegrees(d)).s;
      CHECK_THAT(l.ds, WithinAbs((sp - sm) / (2 * d), 1e-5));
      CHECK_THAT(l.dds, WithinAbs((sp - 2 * l.s + sm) / (d * d), 2e-3));
      (void)beta;
    }
  }
}

TEST_CASE("cam profile: an eccentric under a flat face is a circle",
          "[cam_profile]") {
  // s = e(1 - cos theta) is a flat face on a circle of radius R whose
  // centre is e off the axis: a harmonic rise of 2e over 180 and back.
  const double e = 6, R = 20;
  MotionSpec m;
  m.segments = {{180, 2 * e, Law::Harmonic}, {360, -2 * e, Law::Harmonic}};
  FollowerSpec f;
  f.kind = FollowerKind::Flat;
  for (bool ccw : {true, false}) {
    const Profile p = BuildProfile(m, f, R - e, ccw);
    REQUIRE(p.error.empty());
    REQUIRE(p.closed);
    // At theta = 0 the lift is least, so the centre lies away from the
    // follower: at (-e, 0) in the cam's frame.
    for (const auto& sp : p.spans)
      for (const auto& pts : {sp.points, sp.mid})
        for (glm::dvec2 q : pts)
          CHECK_THAT(glm::length(q - glm::dvec2{-e, 0}), WithinAbs(R, 1e-9));
    CHECK_THAT(p.minConvexRadiusMm, WithinAbs(R, 1e-6));
    CHECK_THAT(p.flatWidthMm, WithinAbs(2 * e, 1e-6)); // s' spans [-e, e]
    CHECK(p.maxPressureAngleDeg == 0.0);
  }
}

TEST_CASE("cam profile: an eccentric under a roller is a circle",
          "[cam_profile]") {
  // A radial roller of radius r on a circle of radius R, e off the axis:
  // the roller's centre stands at rho = -e cos t + sqrt((R+r)^2 - e^2
  // sin^2 t) from the axis. Cut from that table, the profile is the circle.
  const double e = 5, R = 18, r = 4;
  MotionSpec m;
  for (int k = 0; k < 360; k++) {
    const double t = angles::ToRadians(k);
    const double rho =
        -e * std::cos(t) +
        std::sqrt((R + r) * (R + r) - e * e * std::sin(t) * std::sin(t));
    m.lift.push_back({double(k), rho - (R + r - e)});
  }
  const Profile p = BuildProfile(m, Roller(r), R - e, true);
  REQUIRE(p.error.empty());
  double worst = 0;
  for (const auto& sp : p.spans)
    for (glm::dvec2 q : sp.points)
      worst = std::max(worst, std::abs(glm::length(q - glm::dvec2{-e, 0}) - R));
  // The table is interpolated (the cam relation's monotone cubic), so the
  // slope that places the contact carries the interpolation's error.
  CHECK(worst < 2e-3);
}

TEST_CASE("cam profile: the pressure angle is the textbook's",
          "[cam_profile]") {
  // tan(phi) = (s' - e) / (s + sqrt(Rp^2 - e^2)) for a cam turning
  // clockwise with the follower's line e to +Y (the textbook's sense).
  const double Rb = 25, r = 5, e = 4, h = 12;
  const MotionSpec m = RiseDwellFall(h);
  const FollowerSpec f = Roller(r, e);
  const double Rp = Rb + r;
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> U(0.0, 360.0);
  for (int k = 0; k < 500; k++) {
    const double deg = U(rng);
    const LiftAt l = EvaluateMotion(m, f, deg);
    const double tanPhi = (l.ds - e) / (l.s + std::sqrt(Rp * Rp - e * e));
    CHECK_THAT(DesignedPressureAngle(m, f, Rb, /*ccw=*/false, deg),
               WithinAbs(std::atan(std::abs(tanPhi)), 1e-9));
  }
}

TEST_CASE("cam profile: undercut is refused with the radius that clears it",
          "[cam_profile]") {
  // A steep 20 mm rise over 40 degrees on a small cam.
  MotionSpec m;
  m.segments = {{40, 20, Law::Cycloidal},
                {180, 0, Law::Dwell},
                {220, -20, Law::Cycloidal},
                {360, 0, Law::Dwell}};
  for (FollowerKind kind : {FollowerKind::Roller, FollowerKind::Flat}) {
    FollowerSpec f = Roller(8);
    f.kind = kind;
    const Profile p = BuildProfile(m, f, 10, true);
    REQUIRE_FALSE(p.error.empty());
    CHECK_THAT(p.error, ContainsSubstring("undercut"));
    REQUIRE_FALSE(p.undercut.empty());
    REQUIRE(p.minBaseRadiusMm > 10);
    const Profile ok = BuildProfile(m, f, p.minBaseRadiusMm, true);
    CHECK(ok.error.empty());
    const Profile under = BuildProfile(m, f, p.minBaseRadiusMm - 0.1, true);
    CHECK_FALSE(under.error.empty());
  }
}

TEST_CASE("cam profile: a drop is a step between two spans", "[cam_profile]") {
  MotionSpec m;
  m.segments = {
      {300, 10, Law::Cycloidal}, {300, -10, Law::Drop}, {360, 0, Law::Dwell}};
  FollowerSpec f;
  f.kind = FollowerKind::Knife;
  f.radiusMm = 1.0;
  REQUIRE(MotionError(m, f).empty());
  const auto drops = MotionDrops(m, f);
  REQUIRE(drops.size() == 1);
  CHECK(drops[0].atDeg == 300);
  CHECK(drops[0].from == 10);
  CHECK(drops[0].to == 0);
  const Profile p = BuildProfile(m, f, 20, true);
  REQUIRE(p.error.empty());
  CHECK_FALSE(p.closed);
  REQUIRE(p.spans.size() == 1);
  // The span starts low just after the step and ends high just before it,
  // both on the follower's line (radial: no offset) at 300 degrees.
  const glm::dvec2 a = p.spans[0].points.front();
  const glm::dvec2 b = p.spans[0].points.back();
  CHECK_THAT(glm::length(a), WithinAbs(20, 1e-9));
  CHECK_THAT(glm::length(b), WithinAbs(30, 1e-9));
  CHECK_THAT(std::atan2(a.y, a.x), WithinAbs(std::atan2(b.y, b.x), 1e-9));
}

TEST_CASE("cam profile: refusals name what is accepted", "[cam_profile]") {
  FollowerSpec f = Roller(4);
  MotionSpec m;
  CHECK_THAT(MotionError(m, f), ContainsSubstring("EITHER"));
  m.segments = {{180, 10, Law::Cycloidal}, {350, -10, Law::Cycloidal}};
  CHECK_THAT(MotionError(m, f), ContainsSubstring("ending at 360"));
  m.segments = {{180, 10, Law::Cycloidal}, {360, -9, Law::Cycloidal}};
  CHECK_THAT(MotionError(m, f), ContainsSubstring("sum to zero"));
  m.segments = {{180, 10, Law::Cycloidal},
                {200, 5, Law::Drop},
                {360, -15, Law::Cycloidal}};
  CHECK_THAT(MotionError(m, f), ContainsSubstring("to_deg is where"));
  m.segments.clear();
  m.lift = {{0, 0}, {400, 1}};
  CHECK_THAT(MotionError(m, f), ContainsSubstring("[0, 360)"));
  FollowerSpec flatTurn = f;
  flatTurn.kind = FollowerKind::Flat;
  flatTurn.motion = FollowerMotion::Turn;
  CHECK_THAT(FollowerError(flatTurn, 20), ContainsSubstring("roller"));
  FollowerSpec far = Roller(2, 30);
  CHECK_THAT(FollowerError(far, 20), ContainsSubstring("misses"));
  Law l;
  CHECK_FALSE(LawFromName("cycloid", &l));
}

// ---- the feature -----------------------------------------------------------

namespace {

std::string CylinderFace(const Body& body) {
  for (const auto& [index, name] : body.elementMap.names) {
    const auto f = Measure::DescribeFaceAt(body, index);
    if (f && f->kind == "cylinder")
      return name;
  }
  return {};
}

// A hub on Z: radius 20, 8 long.
std::unique_ptr<Document> Hub(double r = 20.0, double h = 8.0) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>("Hub", glm::dvec3(0.0),
                                                    glm::dvec3(0, 0, 1), r, h));
  return doc;
}

CamProfileFeature* AddCam(Document& doc, std::unique_ptr<CamProfileFeature> c) {
  CamProfileFeature* raw = c.get();
  doc.AddFeature(std::move(c));
  return raw;
}

} // namespace

TEST_CASE("CamProfile: picked on a hub, every default comes from the hub",
          "[cam_profile]") {
  auto doc = Hub();
  const Body& hub = doc->GetBodies().at(0);
  const std::string face = CylinderFace(hub);
  REQUIRE_FALSE(face.empty());
  auto* cam = AddCam(*doc, std::make_unique<CamProfileFeature>(
                               "Cam", ElementRef{hub.createdBy, face}));
  REQUIRE(cam->GetError().empty());
  REQUIRE(cam->report.valid);
  CHECK(cam->report.baseRadius == 20.0);
  CHECK(cam->report.width == 8.0);
  CHECK(cam->report.derived == std::vector<std::string>{"base_radius_mm",
                                                        "width_mm", "follower",
                                                        "segments"});
  REQUIRE(doc->GetBodies().size() == 2);
  // Rise half the base radius: the lobe's top at 30.
  CHECK_THAT(cam->report.profile.maxRadiusMm, WithinAbs(30.0, 1e-9));
  glm::dvec3 lo, hi;
  doc->GetBodies().at(1).shape.GetBoundingBox(lo, hi, true);
  CHECK_THAT(std::max({hi.x, -lo.x, hi.y, -lo.y}), WithinAbs(30.0, 1e-3));
  CHECK_THAT(hi.z - lo.z, WithinAbs(8.0, 1e-6));
  CHECK(cam->report.fitDeviation < 1e-4);
  const json rep = cam->ReportJson();
  CHECK(rep.at("rise_mm") == 10.0);
  CHECK(rep.at("follower").at("kind") == "roller");
}

TEST_CASE("CamProfile: an eccentric cut for a flat face is that circle",
          "[cam_profile]") {
  // Placed by a Frame, so the base radius and the width are stated.
  const double e = 6, R = 20, w = 5;
  Document doc;
  auto fr = std::make_unique<FrameFeature>("Axle");
  fr->origin[0] = 10.0;
  FrameFeature* frame = fr.get();
  doc.AddFeature(std::move(fr));
  auto c = std::make_unique<CamProfileFeature>("Eccentric", ElementRef{});
  c->frameId = frame->GetId();
  c->baseRadius = R - e;
  c->width = w;
  c->motion.segments = {{180, 2 * e, Law::Harmonic},
                        {360, -2 * e, Law::Harmonic}};
  c->followerSet = true;
  c->follower.kind = FollowerKind::Flat;
  auto* cam = AddCam(doc, std::move(c));
  REQUIRE(cam->GetError().empty());
  const CADShape& s = doc.GetBodies().back().shape;
  CHECK_THAT(s.Volume(), WithinRel(M_PI * R * R * w, 1e-6));
  CHECK(cam->report.fitDeviation < 1e-5);
  const json rep = cam->ReportJson();
  CHECK_THAT(rep.at("flat_face_width_mm").get<double>(),
             WithinAbs(2 * e, 1e-6));
  // Without a hub, the base radius and the width are refused, not guessed.
  cam->baseRadius = 0;
  doc.Regenerate();
  CHECK_THAT(cam->GetError(), ContainsSubstring("needs base_radius_mm"));
}

TEST_CASE("CamProfile: added to its hub, a drop makes the step face",
          "[cam_profile]") {
  auto doc = Hub(12.0, 6.0);
  const Body& hub = doc->GetBodies().at(0);
  const double hubVolume = hub.shape.Volume();
  auto c = std::make_unique<CamProfileFeature>(
      "Axe", ElementRef{hub.createdBy, CylinderFace(hub)});
  c->baseRadius = 14.0;
  c->newBody = false;
  c->motion.segments = {
      {300, 8, Law::Cycloidal}, {300, -8, Law::Drop}, {360, 0, Law::Dwell}};
  c->followerSet = true;
  c->follower.kind = FollowerKind::Knife;
  c->follower.radiusMm = 1.5;
  auto* cam = AddCam(*doc, std::move(c));
  REQUIRE(cam->GetError().empty());
  REQUIRE(doc->GetBodies().size() == 1);
  CHECK(doc->GetBodies().at(0).shape.Volume() > hubVolume);
  REQUIRE(cam->report.profile.drops.size() == 1);
  CHECK(cam->ReportJson().at("drops").at(0).at("height_mm") == 8.0);
  // The step: one planar face across the profile, the drop's height long.
  int planarSides = 0;
  for (TopExp_Explorer ex(doc->GetBodies().at(0).shape.GetShape(), TopAbs_FACE);
       ex.More(); ex.Next()) {
    BRepAdaptor_Surface su(TopoDS::Face(ex.Current()));
    if (su.GetType() == GeomAbs_Plane &&
        std::abs(su.Plane().Axis().Direction().Z()) < 1e-6)
      planarSides++;
  }
  CHECK(planarSides == 1);
}

TEST_CASE("CamProfile: undercut and wrong inputs are refused by name",
          "[cam_profile]") {
  auto doc = Hub(8.0, 6.0);
  const Body& hub = doc->GetBodies().at(0);
  auto c = std::make_unique<CamProfileFeature>(
      "Steep", ElementRef{hub.createdBy, CylinderFace(hub)});
  c->motion.segments = {{40, 20, Law::Cycloidal},
                        {180, 0, Law::Dwell},
                        {220, -20, Law::Cycloidal},
                        {360, 0, Law::Dwell}};
  c->followerSet = true;
  c->follower.radiusMm = 8.0;
  auto* cam = AddCam(*doc, std::move(c));
  CHECK_THAT(cam->GetError(), ContainsSubstring("undercut"));
  CHECK_THAT(cam->GetError(), ContainsSubstring("a base radius of at least"));

  // The JSON form refuses what it does not know, naming what it does.
  const json base = {
      {"type", "CamProfile"},   {"name", "C"},    {"id", 9}, {"frame", 1},
      {"base_radius_mm", 10.0}, {"width_mm", 5.0}};
  json j = base;
  j["segments"] = {{{"to_deg", 360}, {"rise_mm", 0}, {"law", "sine"}}};
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                    ContainsSubstring("cycloidal, harmonic, poly345"));
  j = base;
  j["follower"] = {{"kind", "roller"},
                   {"motion", "turn"},
                   {"radius_mm", 3},
                   {"pivot_mm", {30, 10}},
                   {"arm_mm", 25}};
  j["segments"] = {{{"to_deg", 180}, {"rise_mm", 10}, {"law", "harmonic"}},
                   {{"to_deg", 360}, {"rise_mm", -10}, {"law", "harmonic"}}};
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                    ContainsSubstring("its rise is rise_deg"));
  j = base;
  j["turn"] = "clockwise";
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                    ContainsSubstring("accepted: ccw, cw"));
  j = base;
  j["segments"] = {{{"to_deg", 180}, {"rise_mm", 10}, {"law", "harmonic"}}};
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(j),
                    ContainsSubstring("ending at 360"));
}

TEST_CASE("CamProfile: the golden loads and round-trips",
          "[cam_profile][golden]") {
  const fs::path golden =
      fs::path(TestSourceDir()) / "tests/golden/v198-cam-profile.json";
  auto doc = DocumentIO::Load(golden.string());
  REQUIRE(doc.has_value());
  const auto* cam =
      dynamic_cast<const CamProfileFeature*>((*doc)->GetFeature(1));
  REQUIRE(cam);
  CHECK(cam->GetError().empty());
  CHECK_FALSE(cam->newBody);
  CHECK(cam->follower.kind == FollowerKind::Knife);
  const json back = DocumentIO::FeatureToJson(*cam);
  CHECK(back.at("segments").at(1).at("law") == "drop");
  CHECK(back.at("segments").at(1).at("rise_mm") == -8.0);
  CHECK_FALSE(back.at("segments").at(2).contains("rise_mm"));
}

TEST_CASE("CamProfile: a cam relation reads the motion the cam was cut for",
          "[cam_profile][assembly][relations]") {
  auto part = Hub();
  const Body& hub = part->GetBodies().at(0);
  auto c = std::make_unique<CamProfileFeature>(
      "Cam", ElementRef{hub.createdBy, CylinderFace(hub)});
  auto* cam = AddCam(*part, std::move(c));
  REQUIRE(cam->report.valid);
  const assembly::PartResolver parts =
      [&](uint64_t, uint64_t) -> const Document* { return part.get(); };

  AssemblyDocument d;
  Occurrence f;
  f.name = "Frame";
  f.partId = 1;
  f.fixed = true;
  const uint64_t frame = d.AddOccurrence(std::move(f));
  Occurrence a;
  a.name = "Cam";
  a.partId = 1;
  const uint64_t camOcc = d.AddOccurrence(std::move(a));
  Occurrence b;
  b.name = "Rod";
  b.partId = 1;
  const uint64_t rod = d.AddOccurrence(std::move(b));
  const auto origin = [](uint64_t occ) {
    Connector cn;
    cn.occurrenceId = occ;
    cn.anchor.origin = true;
    return cn;
  };
  Mate axle;
  axle.type = MateType::Revolute;
  axle.a = origin(frame);
  axle.b = origin(camOcc);
  axle.flipped = false;
  const uint64_t ax = d.AddMate(axle);
  Mate slide = axle;
  slide.type = MateType::Slider;
  slide.b = origin(rod);
  const uint64_t sl = d.AddMate(slide);
  Relation r;
  r.kind = RelationKind::Cam;
  r.mateA = ax;
  r.mateB = sl;
  r.dofA = MateDof::Rotation;
  r.dofB = MateDof::Offset;
  r.ratio = 1.0;
  r.profilePart = 1;
  r.profileFeature = cam->GetId();
  const uint64_t rel = d.AddRelation(r);
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> U(0.0, 360.0);
  for (int k = 0; k < 50; k++) {
    const double deg = U(rng);
    assembly::SetMateDof(*d.FindMateById(ax), MateDof::Rotation,
                         angles::ToRadians(deg));
    assembly::Solve(d, parts);
    REQUIRE(d.FindRelationById(rel)->error.empty());
    CHECK_THAT(
        d.FindMateById(sl)->offset,
        WithinAbs(
            EvaluateMotion(cam->report.motion, cam->report.follower, deg).s,
            1e-9));
  }
  // Edit the cam and the follower follows: a bigger hub, a bigger rise.
  auto* cyl = dynamic_cast<CylinderFeature*>(part->GetFeature(0));
  cyl->radius = 24.0;
  part->Regenerate();
  assembly::SetMateDof(*d.FindMateById(ax), MateDof::Rotation,
                       angles::ToRadians(150.0));
  assembly::Solve(d, parts);
  CHECK_THAT(d.FindMateById(sl)->offset, WithinAbs(12.0, 1e-9));
  // A turning b is refused for a cam cut for a sliding follower.
  d.FindRelationById(rel)->dofB = MateDof::Rotation;
  d.FindMateById(sl)->type = MateType::Revolute;
  assembly::Solve(d, parts);
  CHECK_THAT(d.FindRelationById(rel)->error,
             ContainsSubstring("cut for a sliding follower"));
}
