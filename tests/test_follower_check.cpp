// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The follower check (docs/assemblies/relations.md#the-follower-check):
// a cam cut by CamProfile for a roller, driven through a contact relation by
// that roller, reproduces the motion it was cut for; the speed at which
// gravity loses the follower against the hand-worked sqrt(g b^2 / (2 pi h));
// a drop's free fall; a follower gravity pushes off; and the 3D check of a
// sample, including a bump the 2D section cannot see.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Angles.h"
#include "core/AssemblySolve.h"
#include "core/ContactFollower.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/FollowerCheck.h"
#include "core/Occurrence.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CamProfileFeature.h"
#include "core/features/FrameFeature.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <map>

using cammech::Law;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kG = 9.81;

Connector Origin(uint64_t occ) {
  Connector c;
  c.occurrenceId = occ;
  c.anchor.origin = true;
  return c;
}

Mate MateOf(MateType type, uint64_t a, uint64_t b) {
  Mate m;
  m.type = type;
  m.a = Origin(a);
  m.b = Origin(b);
  m.flipped = false;
  return m;
}

// A cam cut by CamProfile on a Frame at the origin (Z up, 6 wide), turning
// about world Z, and a roller of `roller` radius on a slider along world X
// (its guide's Z), cut for exactly that roller.
struct Rig {
  std::map<uint64_t, std::unique_ptr<Document>> parts;
  AssemblyDocument asmDoc;
  CamProfileFeature* cam = nullptr;
  uint64_t frame = 0, camOcc = 0, folOcc = 0, axle = 0, slide = 0, rel = 0;
  assembly::PartResolver Resolver() {
    return [this](uint64_t id, uint64_t) -> const Document* {
      auto it = parts.find(id);
      return it == parts.end() ? nullptr : it->second.get();
    };
  }
  const contact::Model& Model() const {
    return *asmDoc.FindRelationById(rel)->contactModel;
  }
};

Rig MakeRig(const cammech::MotionSpec& motion, double roller,
            bool bump = false) {
  Rig t;
  t.parts[1] = std::make_unique<Document>();
  auto camPart = std::make_unique<Document>();
  auto fr = std::make_unique<FrameFeature>("Axis");
  const uint64_t frameId = fr->GetId();
  camPart->AddFeature(std::move(fr));
  auto c = std::make_unique<CamProfileFeature>("Cam", ElementRef{});
  c->frameId = camPart->GetFeature(0)->GetId();
  (void)frameId;
  c->baseRadius = 30.0;
  c->width = 6.0;
  c->motion = motion;
  c->followerSet = true;
  c->follower.kind = cammech::FollowerKind::Roller;
  c->follower.radiusMm = roller;
  t.cam = c.get();
  camPart->AddFeature(std::move(c));
  if (bump) {
    // A rim at the bottom of the width, wider than the lobe: the section at
    // the middle of the width does not see it.
    camPart->AddFeature(std::make_unique<CylinderFeature>(
        "Rim", glm::dvec3(0.0), glm::dvec3(0, 0, 1), 45.0, 1.0));
    camPart->AddFeature(std::make_unique<BooleanFeature>(
        "Fuse", t.cam->GetId(), camPart->GetFeature(2)->GetId(),
        BooleanOp::Union));
  }
  t.parts[2] = std::move(camPart);
  auto fol = std::make_unique<Document>();
  // In the guide's frame: local Z is the slide (world +X), local X world -Z.
  fol->AddFeature(
      std::make_unique<CylinderFeature>("Roller", glm::dvec3(1.0, 0.0, roller),
                                        glm::dvec3(-1, 0, 0), roller, 8.0));
  t.parts[3] = std::move(fol);
  Occurrence f;
  f.name = "Frame";
  f.partId = 1;
  f.fixed = true;
  const uint64_t frame = t.frame = t.asmDoc.AddOccurrence(std::move(f));
  Occurrence g;
  g.name = "Guide";
  g.partId = 1;
  g.fixed = true;
  g.orientation = glm::angleAxis(M_PI / 2, glm::dvec3(0, 1, 0));
  const uint64_t guide = t.asmDoc.AddOccurrence(std::move(g));
  Occurrence co;
  co.name = "Cam";
  co.partId = 2;
  t.camOcc = t.asmDoc.AddOccurrence(std::move(co));
  Occurrence fo;
  fo.name = "Roller";
  fo.partId = 3;
  t.folOcc = t.asmDoc.AddOccurrence(std::move(fo));
  t.axle = t.asmDoc.AddMate(MateOf(MateType::Revolute, frame, t.camOcc));
  t.slide = t.asmDoc.AddMate(MateOf(MateType::Slider, guide, t.folOcc));
  Relation r;
  r.kind = RelationKind::Contact;
  r.mateA = t.axle;
  r.mateB = t.slide;
  r.dofA = MateDof::Rotation;
  r.dofB = MateDof::Offset;
  r.toward = -1;
  t.rel = t.asmDoc.AddRelation(r);
  assembly::Solve(t.asmDoc, t.Resolver());
  return t;
}

// The study's samples, solved here: n over one turn.
std::vector<follower::Sample> Samples(Rig& t, int n) {
  std::vector<follower::Sample> out;
  for (int k = 0; k < n; k++) {
    const double a = 2.0 * M_PI * k / n;
    assembly::SetMateDof(*t.asmDoc.FindMateById(t.axle), MateDof::Rotation, a);
    assembly::Solve(t.asmDoc, t.Resolver(), nullptr,
                    {t.axle, MateDof::Rotation, {}});
    out.push_back({size_t(k), a, a, t.asmDoc.FindMateById(t.slide)->offset});
  }
  return out;
}

follower::Designed DesignOf(const CamProfileFeature& c) {
  follower::Designed d;
  d.motion = c.report.motion;
  d.follower = c.report.follower;
  d.ccw = c.ccw;
  d.baseRadius = c.report.baseRadius;
  d.fitDeviation = c.report.fitDeviation;
  d.frameInCamPart = c.report.frame;
  return d;
}

// Rise 10 mm over 90 degrees, dwell, fall 10 over 45, dwell: cycloidal.
cammech::MotionSpec Lobe() {
  cammech::MotionSpec m;
  m.segments = {{90, 10, Law::Cycloidal},
                {180, 0, Law::Dwell},
                {225, -10, Law::Cycloidal},
                {360, 0, Law::Dwell}};
  return m;
}

} // namespace

TEST_CASE("follower check: the cam cut for a roller gives back its motion",
          "[follower][contact][cam_profile]") {
  Rig t = MakeRig(Lobe(), 3.0);
  REQUIRE(t.cam->GetError().empty());
  REQUIRE(t.asmDoc.FindRelationById(t.rel)->error.empty());
  follower::Input in;
  in.model = &t.Model();
  in.samples = Samples(t, 360);
  in.designed = DesignOf(*t.cam);
  const follower::Report r = follower::Check(in);
  REQUIRE(r.deviationKnown);
  INFO("deviation " << r.deviation << " at " << r.deviationAtDeg << ", fit "
                    << r.fitDeviation << ", tolerance " << t.Model().tolerance);
  CHECK(r.deviation <= r.fitDeviation + t.Model().tolerance);
  // The roller's centre stands Rb + r from the axis at zero lift.
  CHECK_THAT(r.mateZero, WithinAbs(30.0, 1e-3));
  // The pressure angle it meets is the one the cam was designed for.
  CHECK_THAT(r.maxPressureDeg,
             WithinAbs(t.cam->report.profile.maxPressureAngleDeg, 0.05));
  CHECK_FALSE(r.pressureOk.has_value()); // no limit given: unchecked
  in.maxPressureDeg = 20.0;
  CHECK(follower::Check(in).pressureOk == (r.maxPressureDeg <= 20.0));
  // An edited cam no longer matches its design: a bigger roller in the
  // follower's place.
  Rig wrong = MakeRig(Lobe(), 3.0);
  dynamic_cast<CylinderFeature*>(wrong.parts[3]->GetFeature(0))->radius = 6.0;
  wrong.parts[3]->Regenerate();
  assembly::Solve(wrong.asmDoc, wrong.Resolver());
  follower::Input w = in;
  w.model = &wrong.Model();
  w.samples = Samples(wrong, 72);
  w.designed = DesignOf(*wrong.cam);
  CHECK(follower::Check(w).deviation > 0.01);
}

TEST_CASE("follower check: gravity loses a follower at sqrt(g b^2 / 2 pi h)",
          "[follower][contact]") {
  // One slider of mass m held down by gravity: M = m, Q = -m g along +q.
  // The cycloidal fall's peak deceleration is 2 pi h / b^2 per rad^2 of
  // cam, so the follower leaves above w = sqrt(g b^2 / (2 pi h)).
  Rig t = MakeRig(Lobe(), 3.0);
  const double m = 0.012;
  follower::Input in;
  in.model = &t.Model();
  in.mass = [&](double) { return follower::MassAt{m, -m * kG, 0.0}; };
  in.driverPerCam = 1.0;
  const follower::Report r = follower::Check(in);
  REQUIRE(r.gravityKnown);
  REQUIRE(r.liftoffRpm.has_value());
  const double beta = M_PI / 4, h = 0.010;
  const double w = std::sqrt(kG * beta * beta / (2 * M_PI * h));
  INFO("liftoff " << *r.liftoffRpm << " rpm at " << r.liftoffAtDeg);
  CHECK_THAT(*r.liftoffRpm, WithinRel(w * 60.0 / (2 * M_PI), 0.01));
  // ...over the fall, where the cam pulls away fastest.
  CHECK(r.liftoffAtDeg > 180.0);
  CHECK(r.liftoffAtDeg < 225.0);
  CHECK(r.notHeld.empty());
  // The worked number: 93.8 cam rpm, and a 2:1 drive halves the crank's.
  CHECK_THAT(*r.liftoffRpm, WithinRel(93.8, 0.01));
  in.driverPerCam = 2.0;
  CHECK_THAT(*follower::Check(in).liftoffRpm, WithinRel(2 * 93.8, 0.01));
  in.driverPerCam = 1.0;
  in.speedRpm = 60.0;
  CHECK(follower::Check(in).holdsAtSpeed == true);
  in.speedRpm = 120.0;
  CHECK(follower::Check(in).holdsAtSpeed == false);
}

TEST_CASE("follower check: a drop falls freely and must land in its dwell",
          "[follower][contact]") {
  // A snail: rise 10 mm over 300 degrees, drop, dwell 60. A 10 mm free fall
  // takes sqrt(2 h / g) = 45 ms: 32.5 degrees of cam at 2 rev/s.
  cammech::MotionSpec snail;
  snail.segments = {
      {300, 10, Law::Cycloidal}, {300, -10, Law::Drop}, {360, 0, Law::Dwell}};
  Rig t = MakeRig(snail, 0.5);
  REQUIRE(t.cam->GetError().empty());
  REQUIRE(t.asmDoc.FindRelationById(t.rel)->error.empty());
  REQUIRE(t.Model().table.drops.size() == 1);
  const double m = 0.012;
  follower::Input in;
  in.model = &t.Model();
  in.mass = [&](double) { return follower::MassAt{m, -m * kG, 0.0}; };
  in.speedRpm = 120.0;
  const follower::Report r = follower::Check(in);
  REQUIRE(r.drops.size() == 1);
  const follower::DropOut& d = r.drops[0];
  INFO("height " << d.height << " fall " << d.fallS << " dwell " << d.dwellDeg
                 << " drop rpm " << d.dropRpm);
  // The pin rounds the step's corner before it falls, so it drops a little
  // less than the whole step.
  CHECK(d.height > 9.5);
  CHECK(d.height <= 10.0 + 1e-6);
  CHECK_THAT(d.fallS, WithinRel(std::sqrt(2 * d.height * 1e-3 / kG), 1e-9));
  // The worked number: 10 mm in 45.2 ms, 32.5 degrees of cam at 2 rev/s
  // (scaled to the height it actually falls).
  CHECK_THAT(d.fallS, WithinRel(0.0452 * std::sqrt(d.height / 10.0), 0.005));
  CHECK_THAT(*d.turnDeg, WithinRel(720.0 * d.fallS, 1e-9));
  CHECK_THAT(*d.turnDeg, WithinRel(32.5 * std::sqrt(d.height / 10.0), 0.005));
  // The 60 degree dwell, and the first few of the rise that stand within a
  // tolerance of it.
  CHECK(d.dwellDeg > 59.0);
  CHECK(d.dwellDeg < 65.0);
  CHECK_THAT(d.dropRpm, WithinRel(d.dwellDeg / 360.0 / d.fallS * 60.0, 1e-9));
  CHECK(*d.lands == (*d.turnDeg <= d.dwellDeg));
  CHECK(*d.lands);
  // Too fast, and it lands on the next rise.
  in.speedRpm = d.dropRpm * 1.1;
  const follower::Report fast = follower::Check(in);
  CHECK_FALSE(*fast.drops[0].lands);
  CHECK(fast.holdsAtSpeed == false);
}

TEST_CASE("follower check: where gravity pushes it off, it is not held",
          "[follower][contact]") {
  // A rocker past vertical, as its gravity moment reads: m g L sin(q - q0)
  // changes sign at the top of the lift, where its weight goes over centre.
  Rig t = MakeRig(Lobe(), 3.0);
  const double m = 0.05, L = 0.06, q0 = 37.0;
  follower::Input in;
  in.model = &t.Model();
  in.mass = [&](double q) {
    return follower::MassAt{m * L * L, -m * kG * L * std::sin((q0 - q) / 10.0),
                            0.0};
  };
  in.speedRpm = 10.0;
  const follower::Report r = follower::Check(in);
  REQUIRE_FALSE(r.notHeld.empty());
  // The lobe stands above 37 mm (lift 7 of 10) from about 60 to 200 deg.
  CHECK(r.notHeld[0].fromDeg > 40.0);
  CHECK(r.notHeld[0].toDeg < 215.0);
  CHECK(r.holdsAtSpeed == false);
}

TEST_CASE("follower check: across the width it sees what the middle cut "
          "cannot",
          "[follower][contact]") {
  for (bool bump : {false, true}) {
    CAPTURE(bump);
    Rig t = MakeRig(Lobe(), 3.0, bump);
    REQUIRE(t.asmDoc.FindRelationById(t.rel)->error.empty());
    const contact::Model& m = t.Model();
    std::string why;
    const auto levels = follower::Levels(m, &why);
    REQUIRE(levels.size() == size_t(follower::kLevels));
    int fails = 0;
    for (const follower::Sample& s : Samples(t, 24)) {
      const follower::Across x = follower::AcrossWidth(m, levels, s.a, s.b);
      INFO(s.a << " clash " << x.clash << " at " << x.atZ << " gap " << x.gap);
      if (!bump)
        CHECK(x.ok);
      fails += x.ok ? 0 : 1;
      if (bump) {
        // The rim (radius 45, the bottom millimetre) is where it clashes.
        CHECK(x.atZ < 1.0);
        CHECK(x.clash > 4.9);
      }
    }
    if (bump)
      CHECK(fails == 24);
    else {
      // A stored pose the geometry no longer gives: off by 0.01 mm.
      const follower::Across off =
          follower::AcrossWidth(m, levels, 0.0, Samples(t, 1)[0].b + 0.01);
      CHECK_FALSE(off.ok);
    }
  }
}
