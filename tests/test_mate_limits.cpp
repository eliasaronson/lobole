// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Mate limits (docs/assemblies/README.md#mate-limits): an optional range on
// each free value of a mate, refused where it cannot stand, refused where a
// value would leave it, and stopping a motion study at the sample a
// follower reaches it. The loop half -- the bounded solve -- is in
// test_assembly_loops.cpp beside the rest of the loop solver's tests.

#include "TestPaths.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Angles.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <filesystem>
#include <unistd.h>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

double Rad(double deg) { return angles::ToRadians(deg); }

Connector Origin(uint64_t occ) {
  Connector c;
  c.occurrenceId = occ;
  c.anchor.origin = true;
  return c;
}

Mate MateOf(MateType t) {
  Mate m;
  m.id = 7;
  m.name = "joint";
  m.type = t;
  return m;
}

// A carrier with two revolute axles, origin to origin, and a gear between
// them: the second turns a third as far, the other way.
struct Train {
  AssemblyDocument asmDoc;
  uint64_t axleA = 0, axleB = 0, gear = 0;
};

Train MakeTrain() {
  Train t;
  Occurrence c;
  c.name = "Carrier";
  c.partId = 1;
  c.fixed = true;
  const uint64_t carrier = t.asmDoc.AddOccurrence(std::move(c));
  Occurrence p;
  p.name = "Pinion";
  p.partId = 1;
  const uint64_t pinion = t.asmDoc.AddOccurrence(std::move(p));
  Occurrence w;
  w.name = "Wheel";
  w.partId = 1;
  const uint64_t wheel = t.asmDoc.AddOccurrence(std::move(w));
  Mate a;
  a.type = MateType::Revolute;
  a.name = "pinion axle";
  a.a = Origin(carrier);
  a.b = Origin(pinion);
  a.flipped = false;
  t.axleA = t.asmDoc.AddMate(a);
  Mate b = a;
  b.name = "wheel axle";
  b.b = Origin(wheel);
  b.offset = 48.0;
  t.axleB = t.asmDoc.AddMate(b);
  Relation r;
  r.kind = RelationKind::Gear;
  r.mateA = t.axleA;
  r.mateB = t.axleB;
  r.dofA = r.dofB = MateDof::Rotation;
  r.ratio = -1.0 / 3.0;
  t.gear = t.asmDoc.AddRelation(r);
  return t;
}

assembly::PartResolver NoParts() {
  static Document empty;
  return [](uint64_t, uint64_t) -> const Document* { return &empty; };
}

} // namespace

TEST_CASE("A range on a value the type does not free is refused, naming "
          "what it frees",
          "[assembly][limits]") {
  Mate m = MateOf(MateType::Revolute);
  m.offsetRange = MateRange{0.0, 5.0}; // a revolute's offset is its seat
  CHECK_THAT(assembly::MateRangeProblem(m),
             ContainsSubstring("revolute") && ContainsSubstring("rotation") &&
                 ContainsSubstring("no offset range"));
  m.offsetRange.reset();
  m.rotationRange = MateRange{Rad(-30), Rad(30)};
  CHECK(assembly::MateRangeProblem(m).empty());

  Mate f = MateOf(MateType::Fastened);
  f.rotationRange = MateRange{0.0, 1.0};
  CHECK_THAT(assembly::MateRangeProblem(f), ContainsSubstring("frees nothing"));

  Mate b = MateOf(MateType::Ball);
  b.rotationRange = MateRange{0.0, 1.0};
  CHECK_THAT(assembly::MateRangeProblem(b), ContainsSubstring("cone"));
  b.rotationRange.reset();
  b.coneRad = Rad(190);
  CHECK_THAT(assembly::MateRangeProblem(b), ContainsSubstring("0..180"));
  b.coneRad = Rad(25);
  CHECK(assembly::MateRangeProblem(b).empty());

  // A planar frees its slide, its spin and (as a range) its gap.
  Mate p = MateOf(MateType::Planar);
  p.dxRange = MateRange{-1, 1};
  p.dyRange = MateRange{-1, 1};
  p.rotationRange = MateRange{-1, 1};
  p.offsetRange = MateRange{0, 0};
  CHECK(assembly::MateRangeProblem(p).empty());
}

TEST_CASE("A range upside down is refused; min equal to max is a lock",
          "[assembly][limits]") {
  Mate m = MateOf(MateType::Slider);
  m.offsetRange = MateRange{10.0, 2.0};
  CHECK_THAT(assembly::MateRangeProblem(m), ContainsSubstring("above its max"));
  m.offsetRange = MateRange{4.0, 4.0};
  CHECK(assembly::MateRangeProblem(m).empty());
  m.offset = 4.0;
  CHECK(assembly::MateOutsideRange(m).empty());
  m.offset = 4.5;
  CHECK_FALSE(assembly::MateOutsideRange(m).empty());
}

TEST_CASE("A value outside its range is refused, inside and at a bound "
          "accepted",
          "[assembly][limits]") {
  Mate m = MateOf(MateType::Revolute);
  m.rotationRange = MateRange{Rad(-5), Rad(95)};
  CHECK(assembly::RangeRefusal(m, MateDof::Rotation, Rad(30)).empty());
  CHECK(assembly::RangeRefusal(m, MateDof::Rotation, Rad(95)).empty());
  CHECK(assembly::RangeRefusal(m, MateDof::Rotation, Rad(-5)).empty());
  const std::string why =
      assembly::RangeRefusal(m, MateDof::Rotation, Rad(100));
  CHECK_THAT(why, ContainsSubstring("100") && ContainsSubstring("-5") &&
                      ContainsSubstring("95") && ContainsSubstring("joint"));
  // A free value says nothing.
  CHECK(assembly::RangeRefusal(m, MateDof::Offset, 1e6).empty());
}

TEST_CASE("A ball's cone holds the tilt and keeps the twist",
          "[assembly][limits]") {
  Mate b = MateOf(MateType::Ball);
  b.coneRad = Rad(20);
  // Tilted 35 degrees about X, twisted 50 about Z first.
  const glm::dquat twist = glm::angleAxis(Rad(50), glm::dvec3(0, 0, 1));
  const glm::dquat tilt = glm::angleAxis(Rad(35), glm::dvec3(1, 0, 0));
  b.ballRotation = tilt * twist;
  CHECK_THAT(assembly::RangedValueOf(b, RangedValue::Cone),
             WithinAbs(Rad(35), 1e-9));
  CHECK_FALSE(assembly::MateOutsideRange(b).empty());
  const glm::dquat held = assembly::ClampToCone(b.coneRad, b.ballRotation);
  Mate h = b;
  h.ballRotation = held;
  CHECK_THAT(assembly::RangedValueOf(h, RangedValue::Cone),
             WithinAbs(Rad(20), 1e-9));
  // Same direction of tilt, same twist: X still maps into the plane the
  // unclamped rotation put it in.
  const glm::dvec3 zWas = b.ballRotation * glm::dvec3(0, 0, 1);
  const glm::dvec3 zNow = held * glm::dvec3(0, 0, 1);
  CHECK_THAT(glm::dot(glm::normalize(glm::dvec3(zWas.x, zWas.y, 0)),
                      glm::normalize(glm::dvec3(zNow.x, zNow.y, 0))),
             WithinAbs(1.0, 1e-9));
  // Inside the cone nothing moves.
  b.ballRotation = glm::angleAxis(Rad(10), glm::dvec3(0, 1, 0));
  CHECK(assembly::ClampToCone(b.coneRad, b.ballRotation) == b.ballRotation);
}

TEST_CASE("Save and reopen keeps every range, and the v218 golden loads",
          "[assembly][limits][serialization]") {
  const auto root = std::filesystem::path(TestSourceDir()) / "tests/golden";
  auto loaded = DocumentIO::LoadAssembly(
      (root / "assembly/v218-mate-limits.json").string());
  REQUIRE(loaded.has_value());
  const AssemblyDocument& doc = **loaded;
  const Mate* hinge = doc.FindMateById(11);
  REQUIRE(hinge != nullptr);
  REQUIRE(hinge->rotationRange);
  CHECK_THAT(hinge->rotationRange->min, WithinAbs(Rad(-5), 1e-12));
  CHECK_THAT(hinge->rotationRange->max, WithinAbs(Rad(95), 1e-12));
  REQUIRE(doc.FindMateById(12)->offsetRange);
  const Mate* plate = doc.FindMateById(13);
  CHECK(plate->dxRange);
  CHECK(plate->dyRange);
  CHECK(plate->rotationRange);
  CHECK(plate->offsetRange);
  CHECK(plate->offsetRange->min == plate->offsetRange->max); // a lock
  REQUIRE(doc.FindMateById(14)->coneRad);
  CHECK_THAT(*doc.FindMateById(14)->coneRad, WithinAbs(Rad(20), 1e-12));
  CHECK(doc.FindMateById(15)->rotationRange);
  CHECK(doc.FindMateById(15)->offsetRange);

  const auto saved =
      std::filesystem::temp_directory_path() /
      ("lobole_mate_limits_" + std::to_string(::getpid()) + ".json");
  REQUIRE(DocumentIO::SaveAssembly(doc, saved.string()).has_value());
  auto again = DocumentIO::LoadAssembly(saved.string());
  std::filesystem::remove(saved);
  REQUIRE(again.has_value());
  for (const Mate& m : doc.GetMates()) {
    const Mate* n = (*again)->FindMateById(m.id);
    REQUIRE(n != nullptr);
    CHECK(n->rotationRange == m.rotationRange);
    CHECK(n->offsetRange == m.offsetRange);
    CHECK(n->dxRange == m.dxRange);
    CHECK(n->dyRange == m.dyRange);
    CHECK(n->coneRad == m.coneRad);
  }
  // A mate without limits writes none: older files stay byte-identical.
  Mate plain = MateOf(MateType::Revolute);
  CHECK_FALSE(DocumentIO::MateToJson(plain).contains("limits"));
}

TEST_CASE("A file with a range the type does not free is refused, not "
          "loaded",
          "[assembly][limits][serialization]") {
  Mate m = MateOf(MateType::Slider);
  nlohmann::json j = DocumentIO::MateToJson(m);
  j["limits"] = {{"rotation", {0.0, 1.0}}};
  CHECK_THROWS_WITH(DocumentIO::MateFromJson(j),
                    ContainsSubstring("slider") &&
                        ContainsSubstring("no rotation range"));
  j["limits"] = {{"offset", {3.0, 1.0}}};
  CHECK_THROWS_WITH(DocumentIO::MateFromJson(j),
                    ContainsSubstring("above its max"));
  j["limits"] = {{"twist", {0.0, 1.0}}};
  CHECK_THROWS(DocumentIO::MateFromJson(j));
}

TEST_CASE("A relation that drives a follower past its limit is said on the "
          "follower, not clamped",
          "[assembly][limits]") {
  Train t = MakeTrain();
  t.asmDoc.FindMateById(t.axleB)->rotationRange = MateRange{Rad(-20), Rad(20)};
  t.asmDoc.FindMateById(t.axleA)->rotation = Rad(30); // wheel at -10: inside
  assembly::SolveReport ok = assembly::Solve(t.asmDoc, NoParts());
  CHECK(ok.limits.empty());
  CHECK(t.asmDoc.FindMateById(t.axleB)->error.empty());

  t.asmDoc.FindMateById(t.axleA)->rotation = Rad(90); // wheel at -30
  assembly::SolveReport past = assembly::Solve(t.asmDoc, NoParts());
  REQUIRE(past.limits.size() == 1);
  CHECK(past.limits[0].mate == t.axleB);
  CHECK_FALSE(past.limits[0].loop);
  CHECK_THAT(past.limits[0].bound, WithinAbs(Rad(-20), 1e-12));
  CHECK_THAT(past.limits[0].at, WithinAbs(Rad(-30), 1e-9));
  const Mate* wheel = t.asmDoc.FindMateById(t.axleB);
  CHECK_THAT(wheel->error, ContainsSubstring("outside its range"));
  // Placed where the relation said, never quietly held at the limit.
  CHECK_THAT(wheel->rotation, WithinAbs(Rad(-30), 1e-9));
  CHECK(past.erroredMates >= 1);
  CHECK_THAT(assembly::AssemblyOutsideRange(t.asmDoc),
             ContainsSubstring("wheel axle"));
}

TEST_CASE("A motion study across its driver's limit is refused before it "
          "runs; a follower's limit stops it there",
          "[assembly][limits][motion]") {
  Train t = MakeTrain();
  MotionStudy s;
  s.name = "turn";
  s.driverMate = t.axleA;
  s.driverDof = MateDof::Rotation;
  s.from = 0.0;
  s.to = Rad(180);
  s.samples = 19; // every 10 degrees
  s.checkClash = false;

  SECTION("the driver's own range") {
    t.asmDoc.FindMateById(t.axleA)->rotationRange = MateRange{0.0, Rad(120)};
    const std::string why = motion::WhyNot(t.asmDoc, s);
    CHECK_THAT(why, ContainsSubstring("past the driver's limit") &&
                        ContainsSubstring("120"));
    const motion::Result r = motion::Run(t.asmDoc, NoParts(), nullptr, s);
    CHECK(r.error == why);
    CHECK(r.samples.empty());
    // Exactly the whole range is the mate's whole travel: it runs.
    s.to = Rad(120);
    CHECK(motion::WhyNot(t.asmDoc, s).empty());
  }
  SECTION("a follower's range") {
    // The wheel turns a third as far, backwards: -20 degrees at 60.
    t.asmDoc.FindMateById(t.axleB)->rotationRange =
        MateRange{Rad(-20), Rad(20)};
    const motion::Result r = motion::Run(t.asmDoc, NoParts(), nullptr, s);
    CHECK(r.error.empty());
    REQUIRE(r.stoppedBy);
    CHECK(r.stoppedBy->mate == t.axleB);
    CHECK(r.stoppedBy->name == "wheel axle");
    CHECK(r.stoppedBy->value == RangedValue::Rotation);
    CHECK_THAT(r.stoppedBy->bound, WithinAbs(Rad(-20), 1e-12));
    // The first sample past 60 degrees is 70: the run stops there, and
    // that sample is the last and unsolved.
    CHECK_THAT(r.stoppedBy->atValue, WithinAbs(Rad(70), 1e-9));
    REQUIRE(r.samples.size() == 8);
    CHECK_FALSE(r.samples.back().solved);
    for (size_t i = 0; i + 1 < r.samples.size(); i++)
      CHECK(r.samples[i].solved);
    CHECK_FALSE(r.clean);
    const motion::Verdict v = motion::Summarize(r, s, false);
    CHECK(v.level == motion::Verdict::Level::Bad);
    CHECK_THAT(v.text,
               ContainsSubstring("wheel axle") && ContainsSubstring("limit"));
    CHECK_THAT(motion::Table(r, s), ContainsSubstring("STOPPED AT A LIMIT"));
  }
}
