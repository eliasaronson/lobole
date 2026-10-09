// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/AssemblyLoopSolve.h" // MateAdjustFor, DeriveMateDofs
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/SketchFeature.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <memory>
#include <numbers>

using Catch::Matchers::WithinAbs;

namespace {

static const SketchPlane kXY(PlaneType::XY);

// A 4×4 rectangle on XY extruded to `height`. Its top cap ("<ext>/cap1") sits
// at z=height with centroid (2,2,height) — the face A3 mate connectors anchor
// on. Outputs the extrude feature's stable id (the ElementRef feature id).
std::unique_ptr<Document> BuildBox(double height, uint64_t& extrudeId) {
  auto doc = std::make_unique<Document>();
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  sk->AddRectangle({0, 0}, {4, 4});
  doc->AddFeature(std::move(sk));
  const uint64_t sketchId = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketchId, height,
                                                   ExtrudeMode::NewBody));
  extrudeId = doc->GetFeature(1)->GetId();
  doc->Regenerate();
  return doc;
}

// Same box, but the pad stamps a steel material so MassOfBodies reports a
// known mass (density 7850 kg/m^3). Volume is height * 16 mm^3.
std::unique_ptr<Document> BuildSteelBox(double height) {
  auto doc = std::make_unique<Document>();
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  sk->AddRectangle({0, 0}, {4, 4});
  doc->AddFeature(std::move(sk));
  const uint64_t sketchId = doc->GetFeature(0)->GetId();
  auto pad = std::make_unique<ExtrudeFeature>("Pad", sketchId, height,
                                              ExtrudeMode::NewBody);
  pad->SetMaterial("steel");
  doc->AddFeature(std::move(pad));
  doc->Regenerate();
  return doc;
}

Occurrence MakeOcc(uint64_t partId, glm::dvec3 pos, bool fixed = false) {
  Occurrence o;
  o.partId = partId;
  o.position = pos;
  o.fixed = fixed;
  return o;
}

} // namespace

TEST_CASE("Fastened mate aligns an origin connector to a face frame",
          "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, /*fixed=*/true)); // id 1
  asmDoc.AddOccurrence(MakeOcc(1, {100, 100, 100}));           // id 2

  Mate m;
  m.type = MateType::Fastened;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{ext, std::to_string(ext) + "/cap1"};
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  asmDoc.AddMate(m);

  const assembly::SolveReport rep = assembly::Solve(asmDoc, resolve);
  CHECK(rep.erroredMates == 0);

  // The arm's origin lands on the base's top-cap centroid (2, 2, 3).
  const Occurrence* arm = asmDoc.FindById(2);
  REQUIRE(arm != nullptr);
  CHECK_THAT(arm->position.x, WithinAbs(2.0, 1e-4));
  CHECK_THAT(arm->position.y, WithinAbs(2.0, 1e-4));
  CHECK_THAT(arm->position.z, WithinAbs(3.0, 1e-4));

  // The fixed anchor is never moved.
  CHECK(asmDoc.FindById(1)->position == glm::dvec3(0, 0, 0));
}

TEST_CASE("DeriveMateDofs inverts the adjust it is the inverse of",
          "[assembly][solve]") {
  // Every arrangement a fastened mate can hold, composed forwards and read
  // back. This is the arithmetic `add_mate derive:` runs, and the reason it
  // can answer "which way round" without trying all eight.
  const glm::dquat none(1.0, 0.0, 0.0, 0.0);
  for (const bool flipped : {false, true}) {
    for (const double turn : {0.0, 0.7, 1.5708, 3.0, -2.2}) {
      const glm::dmat4 rel =
          assembly::MateAdjustFor(flipped, turn, 12.0, 3.0, -4.0, none);
      bool gotFlip = !flipped;
      double gotTurn = 0.0, dx = 0.0, dy = 0.0, offset = 0.0;
      REQUIRE(assembly::DeriveMateDofs(rel, assembly::MateDerive::Pose, gotFlip,
                                       gotTurn, dx, dy, offset));
      CHECK(gotFlip == flipped);
      CHECK_THAT(std::sin(gotTurn), WithinAbs(std::sin(turn), 1e-4));
      CHECK_THAT(std::cos(gotTurn), WithinAbs(std::cos(turn), 1e-4));
      CHECK_THAT(dx, WithinAbs(3.0, 1e-4));
      CHECK_THAT(dy, WithinAbs(-4.0, 1e-4));
      CHECK_THAT(offset, WithinAbs(12.0, 1e-4));

      // `Twist` takes the same turn and drops the translation on purpose:
      // the anchors are meant to coincide, which is what makes it a joint
      // rather than a copy of a placement.
      bool tFlip = false;
      double tTurn = 0.0, tdx = 1.0, tdy = 1.0, toff = 1.0;
      REQUIRE(assembly::DeriveMateDofs(rel, assembly::MateDerive::Twist, tFlip,
                                       tTurn, tdx, tdy, toff));
      CHECK(tFlip == flipped);
      CHECK_THAT(std::sin(tTurn), WithinAbs(std::sin(turn), 1e-4));
      CHECK(tdx == 0.0);
      CHECK(tdy == 0.0);
      CHECK(toff == 0.0);
    }
  }
}

TEST_CASE("DeriveMateDofs refuses frames that are not square to one axis",
          "[assembly][solve]") {
  // A fastened adjust turns about Z and nothing else. Tilt one frame 20
  // degrees about X and no (flipped, rotation) pair composes to it — so the
  // answer is a refusal, not the nearest quarter turn, which would look
  // exactly like a joint that fits.
  glm::dmat4 skew = assembly::MateAdjustFor(false, 0.4, 5.0, 0.0, 0.0,
                                            glm::dquat(1.0, 0.0, 0.0, 0.0));
  skew = skew * glm::rotate(glm::dmat4(1.0), 0.35, glm::dvec3(1, 0, 0));
  bool flipped = false;
  double turn = 0.0, dx = 0.0, dy = 0.0, offset = 0.0;
  CHECK_FALSE(assembly::DeriveMateDofs(skew, assembly::MateDerive::Pose,
                                       flipped, turn, dx, dy, offset));
  CHECK_FALSE(assembly::DeriveMateDofs(skew, assembly::MateDerive::Twist,
                                       flipped, turn, dx, dy, offset));
  // Untouched, so a caller that ignores the return value cannot end up with
  // half a joint.
  CHECK(turn == 0.0);
  CHECK(offset == 0.0);
}

TEST_CASE("Slider offset slides the placement along the connector axis",
          "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));

  Mate m;
  m.type = MateType::Slider;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{ext, std::to_string(ext) + "/cap1"};
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  m.offset = 5.0; // along the cap's +Z normal
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);
  CHECK_THAT(arm->position.z, WithinAbs(8.0, 1e-4)); // 3 (cap) + 5 (offset)

  // Bumping the offset moves it further — the mechanism responds numerically.
  asmDoc.FindMateById(3)->offset = 9.0;
  assembly::Solve(asmDoc, resolve);
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(12.0, 1e-4));
}

TEST_CASE("Revolute mate seats the part axially by its offset (finding #6a)",
          "[assembly][solve]") {
  // A revolute mate must slide b along the shared axis by `offset` (the seat
  // depth up the shaft), not merely coincide origins — otherwise stacks
  // collapse. rotation stays the free DOF on top of that fixed seat.
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));
  Mate m;
  m.type = MateType::Revolute;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{ext, std::to_string(ext) + "/cap1"};
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  m.offset = 5.0; // seat 5 up the axis (cap at z=3 → z=8)
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(8.0, 1e-4));

  // Spinning the free DOF keeps the axial seat fixed.
  asmDoc.FindMateById(3)->rotation = glm::radians(90.0);
  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);
  CHECK_THAT(arm->position.z, WithinAbs(8.0, 1e-4)); // seat unchanged
  CHECK(std::abs(glm::dot(arm->orientation, glm::dquat(1, 0, 0, 0))) < 0.999);
}

TEST_CASE(
    "A positive offset moves b along +Z of a's connector frame (finding #6c)",
    "[assembly][solve]") {
  // The sign convention: a POSITIVE offset moves b in the +Z direction of a's
  // connector frame (a's outward face normal). The top cap's normal is world
  // +Z, so +offset lifts b and -offset lowers it — no inversion. Guards every
  // offset-using mate type (slider/cylindrical/revolute share MateAdjust).
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext); // top cap "cap1" at z=3, normal +Z
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  auto placeWithOffset = [&](double offset) {
    AssemblyDocument asmDoc;
    asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
    asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));
    Mate m;
    m.type = MateType::Slider;
    m.a.occurrenceId = 1;
    m.a.anchor.face = ElementRef{ext, std::to_string(ext) + "/cap1"};
    m.b.occurrenceId = 2;
    m.b.anchor.origin = true;
    m.flipped = false;
    m.offset = offset;
    asmDoc.AddMate(m);
    assembly::Solve(asmDoc, resolve);
    return asmDoc.FindById(2)->position.z;
  };

  CHECK_THAT(placeWithOffset(5.0), WithinAbs(8.0, 1e-4));   // 3 + (+5)
  CHECK_THAT(placeWithOffset(-5.0), WithinAbs(-2.0, 1e-4)); // 3 + (-5)
  CHECK_THAT(placeWithOffset(0.0), WithinAbs(3.0, 1e-4));   // on the cap
}

TEST_CASE("Assembly mass properties aggregate occurrences in world space "
          "(finding #5)",
          "[assembly][solve]") {
  auto box = BuildSteelBox(3.0); // 4x4x3 steel box: vol 48 mm^3
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true)); // local CoM (2,2,1.5)
  asmDoc.AddOccurrence(MakeOcc(1, {10, 0, 0}));      // shifted → (12,2,1.5)

  const assembly::AssemblyMass am = assembly::MassProperties(asmDoc, resolve);

  REQUIRE(am.occurrences.size() == 2);
  CHECK(am.skipped.empty());
  // Total volume is the sum; mass is known (steel) and doubles.
  CHECK_THAT(am.volume, WithinAbs(96.0, 1e-6));
  REQUIRE(am.massKnown);
  const double oneMass = 7850.0 * 48.0 * 1e-6; // density * volume(mm^3) * 1e-6
  CHECK_THAT(am.massG, WithinAbs(2.0 * oneMass, 1e-9));
  // World CoM sits at the midpoint between the two placed boxes.
  CHECK_THAT(am.centerOfMass.x, WithinAbs(7.0, 1e-5));
  CHECK_THAT(am.centerOfMass.y, WithinAbs(2.0, 1e-5));
  CHECK_THAT(am.centerOfMass.z, WithinAbs(1.5, 1e-5));

  // Per-occurrence rows carry their own WORLD CoM (the shifted box at x=12).
  CHECK_THAT(am.occurrences[1].centerOfMass.x, WithinAbs(12.0, 1e-5));

  // An invisible occurrence drops out of the aggregate (BOM convention).
  asmDoc.FindById(2)->visible = false;
  const assembly::AssemblyMass hidden =
      assembly::MassProperties(asmDoc, resolve);
  REQUIRE(hidden.occurrences.size() == 1);
  CHECK_THAT(hidden.volume, WithinAbs(48.0, 1e-6));
  CHECK_THAT(hidden.centerOfMass.x, WithinAbs(2.0, 1e-5));
}

TEST_CASE("Revolute rotation spins without translating an origin connector",
          "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));

  Mate m;
  m.type = MateType::Revolute;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{ext, std::to_string(ext) + "/cap1"};
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  const glm::dquat before = asmDoc.FindById(2)->orientation;
  const glm::dvec3 pos0 = asmDoc.FindById(2)->position;

  asmDoc.FindMateById(3)->rotation = glm::radians(90.0);
  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);

  // Rotation about the axis through the connector origin: position pinned.
  CHECK_THAT(arm->position.x, WithinAbs(pos0.x, 1e-4));
  CHECK_THAT(arm->position.y, WithinAbs(pos0.y, 1e-4));
  CHECK_THAT(arm->position.z, WithinAbs(pos0.z, 1e-4));
  // ...but the orientation changed (the DOF moved).
  const double dot = std::abs(glm::dot(before, arm->orientation));
  CHECK(dot < 0.999);
}

TEST_CASE("Payoff: editing the base part moves the mated occurrence",
          "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));

  Mate m;
  m.type = MateType::Fastened;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{ext, std::to_string(ext) + "/cap1"};
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(3.0, 1e-4));

  // Edit the base: raise the pad 3 → 7. The named cap moves with the geometry,
  // and a re-solve carries the mated occurrence with it — the whole point of
  // mating faces, not coordinates.
  auto* pad = dynamic_cast<ExtrudeFeature*>(box->GetFeature(1));
  REQUIRE(pad != nullptr);
  pad->distance = 7.0;
  box->Regenerate();

  assembly::Solve(asmDoc, resolve);
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(7.0, 1e-4));
}

TEST_CASE("A floating component with no fixed anchor stays put",
          "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {5, 0, 0}));    // id 1, NOT fixed
  asmDoc.AddOccurrence(MakeOcc(1, {99, 99, 99})); // id 2, NOT fixed

  Mate m; // origin-to-origin fastened → the two origins coincide
  m.type = MateType::Fastened;
  m.a.occurrenceId = 1;
  m.a.anchor.origin = true;
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  // Lowest-id occurrence (1) anchors at its stored transform; 2 follows onto
  // it.
  CHECK(asmDoc.FindById(1)->position == glm::dvec3(5, 0, 0));
  CHECK_THAT(asmDoc.FindById(2)->position.x, WithinAbs(5.0, 1e-4));
  CHECK_THAT(asmDoc.FindById(2)->position.y, WithinAbs(0.0, 1e-4));
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(0.0, 1e-4));
}

TEST_CASE("An over-constrained loop is reported, not solved",
          "[assembly][solve]") {
  auto box = std::unique_ptr<Document>();
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  // Two fixed occurrences at conflicting positions...
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(MakeOcc(1, {100, 0, 0}, true));

  Mate m; // ...that a fastened origin-origin mate cannot possibly satisfy.
  m.type = MateType::Fastened;
  m.a.occurrenceId = 1;
  m.a.anchor.origin = true;
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  const uint64_t mateId = asmDoc.AddMate(m);

  const assembly::SolveReport rep = assembly::Solve(asmDoc, resolve);
  CHECK(rep.erroredMates == 1);
  CHECK_FALSE(asmDoc.FindMateById(mateId)->error.empty());
  // Placements stand (the fixed anchors are untouched).
  CHECK(asmDoc.FindById(2)->position == glm::dvec3(100, 0, 0));
}

TEST_CASE("A dangling connector ref errors the mate", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));

  Mate m;
  m.type = MateType::Fastened;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{ext, "9999/nonexistent"}; // no such name
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  const uint64_t mateId = asmDoc.AddMate(m);

  const assembly::SolveReport rep = assembly::Solve(asmDoc, resolve);
  CHECK(rep.erroredMates == 1);
  CHECK_FALSE(asmDoc.FindMateById(mateId)->error.empty());
}

// ---- Wider mate set (v25): cylindrical / planar / ball --------------------

namespace {
// A face-anchored connector on both sides (base cap1, arm cap0) with the given
// mate type; base fixed at origin, arm parked away. Returns the assembly ready
// to solve. `box` is shared as both parts (origin anchors unused here).
Mate FaceMate(uint64_t ext, MateType type) {
  Mate m;
  m.type = type;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{ext, std::to_string(ext) + "/cap1"};
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true; // arm origin onto the base's top face
  m.flipped = false;
  return m;
}
} // namespace

TEST_CASE("Cylindrical mate frees both rotation and offset",
          "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));
  Mate m = FaceMate(ext, MateType::Cylindrical);
  m.offset = 4.0;                  // slides along the cap normal (+Z)
  m.rotation = glm::radians(30.0); // and spins about it
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);
  CHECK_THAT(arm->position.z, WithinAbs(7.0, 1e-4)); // 3 + 4
  // Rotation took effect (non-identity orientation).
  CHECK(std::abs(glm::dot(arm->orientation, glm::dquat(1, 0, 0, 0))) < 0.999);
}

TEST_CASE("Planar mate slides in-plane by dx/dy", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));
  Mate m = FaceMate(ext, MateType::Planar);
  m.dx = 5.0; // along the cap frame's in-plane X (world +X for a +Z cap)
  m.dy = 0.0;
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);
  // Base cap centroid is (2,2,3); +5 along the frame X axis.
  CHECK_THAT(arm->position.z, WithinAbs(3.0, 1e-4)); // stays on the plane
  const double inPlane =
      glm::length(glm::dvec2(arm->position.x - 2.0, arm->position.y - 2.0));
  CHECK_THAT(inPlane, WithinAbs(5.0, 1e-4)); // slid 5 units within the plane
}

TEST_CASE("Ball mate rotates freely without translating", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {7, 8, 9}, true)); // fixed somewhere
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));

  Mate m;
  m.type = MateType::Ball;
  m.a.occurrenceId = 1;
  m.a.anchor.origin = true;
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  m.ballRotation = glm::angleAxis(glm::radians(60.0), glm::dvec3(0, 1, 0));
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);
  // Origins coincide (ball is a point joint) — arm sits on the fixed origin.
  CHECK_THAT(arm->position.x, WithinAbs(7.0, 1e-4));
  CHECK_THAT(arm->position.y, WithinAbs(8.0, 1e-4));
  CHECK_THAT(arm->position.z, WithinAbs(9.0, 1e-4));
  // ...but it's rotated by the ball DOF.
  CHECK(std::abs(glm::dot(arm->orientation, glm::dquat(1, 0, 0, 0))) < 0.999);
}

TEST_CASE("Unresolvable connector names its side and suggests near misses",
          "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, /*fixed=*/true)); // id 1
  asmDoc.AddOccurrence(MakeOcc(1, {10, 0, 0}));                // id 2
  asmDoc.FindById(2)->name = "LobeBearing1";

  // Connector B references a STALE face name that is a strict prefix of the
  // current names ("<ext>/cap" vs "<ext>/cap0"/"<ext>/cap1") — the
  // modified-by-a-later-feature shape of failure.
  Mate m;
  m.type = MateType::Fastened;
  m.a.occurrenceId = 1;
  m.a.anchor.origin = true;
  m.b.occurrenceId = 2;
  m.b.anchor.face = ElementRef{ext, std::to_string(ext) + "/cap"};
  asmDoc.AddMate(m);

  const assembly::SolveReport rep = assembly::Solve(asmDoc, resolve);
  CHECK(rep.erroredMates == 1);
  const std::string& err = asmDoc.GetMates()[0].error;
  INFO(err);
  // Names WHICH side failed, with the occurrence and the failing face ref.
  CHECK(err.find("connector B (LobeBearing1)") != std::string::npos);
  CHECK(err.find("face '" + std::to_string(ext) + "/cap' not found") !=
        std::string::npos);
  // ...and only that side: connector A resolved fine.
  CHECK(err.find("connector A") == std::string::npos);
  // Suggests the current names sharing the stale name as a prefix.
  CHECK(err.find("similar:") != std::string::npos);
  CHECK(err.find(std::to_string(ext) + "/cap1") != std::string::npos);

  // A name with no near miss reports without a suggestion list.
  asmDoc.GetMates()[0].b.anchor.face = ElementRef{ext, "zzz/nope"};
  assembly::Solve(asmDoc, resolve);
  const std::string& err2 = asmDoc.GetMates()[0].error;
  INFO(err2);
  CHECK(err2.find("connector B (LobeBearing1)") != std::string::npos);
  CHECK(err2.find("similar:") == std::string::npos);
}

namespace {
// The planar face of `doc`'s one body whose outward normal is `dir`.
ElementRef FaceFacingDir(const Document& doc, const glm::dvec3& dir) {
  const Body& b = doc.GetBodies().front();
  for (const auto& [index, name] : b.elementMap.names) {
    glm::dvec3 c(0.0), n(0.0), u(0.0);
    if (b.shape.FacePlane(index, c, n, u) && glm::dot(n, dir) > 0.999)
      return ElementRef{b.createdBy, name};
  }
  return {};
}

glm::dmat4 PlacementOf(const Occurrence& o) {
  return glm::translate(glm::dmat4(1.0), o.position) *
         glm::mat4_cast(o.orientation);
}
} // namespace

// The mate solve's contract is not the transform it writes but what that
// transform does to the faces: two faces mated face to face -- `flipped`,
// the RPC's default -- TOUCH, measured by the measure tool in world space,
// with opposed normals, and the mated part sits on the other rather than
// inside it. And a revolute's `rotation` turns the part right-handed about
// the seat's outward normal, measured on a side face. Every other test here
// mates a face to an origin and reads back `position`.
TEST_CASE("A face-to-face mate puts the faces together as measured",
          "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext); // 4 x 4 x 3
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };
  const std::vector<Body>& bodies = box->GetBodies();
  const ElementRef top = FaceFacingDir(*box, {0, 0, 1});
  const ElementRef bottom = FaceFacingDir(*box, {0, 0, -1});
  const ElementRef side = FaceFacingDir(*box, {1, 0, 0});
  REQUIRE(top.IsSet());
  REQUIRE(bottom.IsSet());
  REQUIRE(side.IsSet());

  for (const MateType type : {MateType::Fastened, MateType::Revolute}) {
    AssemblyDocument asmDoc;
    asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, /*fixed=*/true));
    Occurrence loose = MakeOcc(1, {100, 50, 20});
    loose.orientation =
        glm::angleAxis(glm::radians(70.0), glm::normalize(glm::dvec3(1, 2, 3)));
    asmDoc.AddOccurrence(loose);
    Mate m;
    m.type = type;
    m.a.occurrenceId = 1;
    m.a.anchor.face = top;
    m.b.occurrenceId = 2;
    m.b.anchor.face = bottom;
    m.flipped = true;
    asmDoc.AddMate(m);
    const assembly::SolveReport rep = assembly::Solve(asmDoc, resolve);
    INFO("mate type " << static_cast<int>(type));
    REQUIRE(rep.erroredMates == 0);

    const glm::dmat4 xa = PlacementOf(*asmDoc.FindById(1));
    const glm::dmat4 xb = PlacementOf(*asmDoc.FindById(2));
    const auto gap =
        Measure::DistanceDetailed(bodies, top, xa, bodies, bottom, xb);
    REQUIRE(gap.has_value());
    CHECK(gap->distance < 1e-4);
    const auto angle = Measure::Angle(bodies, top, xa, bodies, bottom, xb);
    REQUIRE(angle.has_value());
    CHECK_THAT(angle->degrees, WithinAbs(180.0, 1e-3)); // face to face
    // On it, not through it: b's own top is a box height above a's.
    const auto stack =
        Measure::DistanceDetailed(bodies, top, xa, bodies, top, xb);
    REQUIRE(stack.has_value());
    CHECK_THAT(stack->distance, WithinAbs(3.0, 1e-4));

    if (type != MateType::Revolute)
      continue;
    // A quarter turn of the free rotation: b's side face turns +90 about
    // a's seat normal (world +Z), right-handed.
    const auto sideDir = Measure::DirectionOf(bodies, side);
    REQUIRE(sideDir.has_value());
    const glm::dvec3 before = glm::dmat3(xb) * glm::dvec3(sideDir->dir);
    asmDoc.FindMateById(3)->rotation = glm::radians(90.0);
    REQUIRE(assembly::Solve(asmDoc, resolve).erroredMates == 0);
    const glm::dmat4 turned = PlacementOf(*asmDoc.FindById(2));
    const glm::dvec3 after = glm::dmat3(turned) * glm::dvec3(sideDir->dir);
    INFO("side normal before " << before.x << " " << before.y << " " << before.z
                               << " after " << after.x << " " << after.y << " "
                               << after.z);
    CHECK(glm::length(after - glm::dvec3(-before.y, before.x, before.z)) <
          1e-4);
    // ...and still seated.
    const auto still =
        Measure::DistanceDetailed(bodies, top, xa, bodies, bottom, turned);
    REQUIRE(still.has_value());
    CHECK(still->distance < 1e-4);
  }
}

// A Frame's `rotation_deg` is read by whatever mates to it, so its contract
// is where a part mated there ends up facing: Rx then Ry then Rz in the
// parent's axes (parent . T . Rx . Ry . Rz), right-handed. [0, 0, 90] turns a
// mated part's +X side to +Y; [90, 0, 90] turns it to +Z, which the other
// order (+Y) and the other hand (-Z) both miss. Nothing tested the field.
TEST_CASE("A part mated to a turned frame faces the way the frame says",
          "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const ElementRef side = FaceFacingDir(*box, {1, 0, 0});
  REQUIRE(side.IsSet());
  const auto sideDir = Measure::DirectionOf(box->GetBodies(), side);
  REQUIRE(sideDir.has_value());

  struct Case {
    glm::dvec3 rotationDeg;
    glm::dvec3 want;
  };
  for (const Case& c :
       {Case{{0, 0, 90}, {0, 1, 0}}, Case{{90, 0, 90}, {0, 0, 1}},
        Case{{0, -90, 0}, {0, 0, 1}}}) {
    Document base;
    auto fr = std::make_unique<FrameFeature>("Seat", FrameKind::Offset);
    fr->offset[0] = 10.0;
    for (int i = 0; i < 3; i++)
      fr->rotationDeg[i] = c.rotationDeg[i];
    base.AddFeature(std::move(fr));
    REQUIRE(base.Regenerate());
    const uint64_t frameId = base.GetFeature(0)->GetId();
    const assembly::PartResolver resolve = [&](uint64_t partId, uint64_t) {
      return partId == 1 ? &base : box.get();
    };

    AssemblyDocument asmDoc;
    asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, /*fixed=*/true));
    asmDoc.AddOccurrence(MakeOcc(2, {50, 60, 70}));
    Mate m;
    m.type = MateType::Fastened;
    m.a.occurrenceId = 1;
    m.a.anchor.frameId = frameId;
    m.b.occurrenceId = 2;
    m.b.anchor.origin = true;
    m.flipped = false;
    asmDoc.AddMate(m);
    INFO("rotation_deg " << c.rotationDeg.x << " " << c.rotationDeg.y << " "
                         << c.rotationDeg.z);
    REQUIRE(assembly::Solve(asmDoc, resolve).erroredMates == 0);
    const Occurrence& placed = *asmDoc.FindById(2);
    const glm::dvec3 faces =
        glm::dmat3(PlacementOf(placed)) * glm::dvec3(sideDir->dir);
    INFO("side faces " << faces.x << " " << faces.y << " " << faces.z);
    CHECK(glm::length(faces - c.want) < 1e-4);
    CHECK(glm::length(placed.position - glm::dvec3(10, 0, 0)) < 1e-4);
  }
}

// A round face's connector X is the surface's seam (u = 0)
// (docs/assemblies/solver.md#connector-frames): for an extruded sketch
// circle, the sketch plane's U axis. It is the part's, not the world's, so it
// follows the part and stays put through a regeneration and a boolean that
// trims the face. Until v150 every Z-axis cylinder got X = world +Y.
TEST_CASE("A cylinder's frame takes its seam as X", "[assembly][solve]") {
  const auto seamX = [](const Document& doc) {
    for (const Body& b : doc.GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        const auto face = Measure::DescribeFaceAt(b, index);
        if (!face || face->kind != "cylinder")
          continue;
        glm::dvec3 o, z, x;
        REQUIRE(b.shape.FaceAxis(index, o, z, x));
        CHECK(std::abs(glm::dot(x, z)) < 1e-9);
        return x;
      }
    FAIL("no cylinder");
    return glm::dvec3(0.0);
  };
  const double a = 30.0 * std::numbers::pi / 180.0;
  const glm::dvec3 u(std::cos(a), std::sin(a), 0.0);
  Document doc;
  auto sk = std::make_unique<SketchFeature>(
      "Circle", SketchPlane(glm::dvec3(0.0), glm::dvec3(0, 0, 1), u));
  sk->AddEntity(sketch::Circle{{0, 0}, 5.0});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Shaft", doc.GetFeature(0)->GetId(), 20.0, ExtrudeMode::NewBody));
  REQUIRE(doc.Regenerate());
  const glm::dvec3 x = seamX(doc);
  INFO(x.x << ", " << x.y << ", " << x.z);
  CHECK(glm::length(x - u) < 1e-9);

  // Reproducible: a regeneration with another length leaves it.
  static_cast<ExtrudeFeature*>(doc.GetFeature(1))->distance = 35.0;
  REQUIRE(doc.Regenerate());
  CHECK(glm::length(seamX(doc) - u) < 1e-9);

  // A flat cut along the shaft trims the face (and moves its edges); the
  // surface, and so X, stays.
  auto flat = std::make_unique<SketchFeature>("Flat", kXY);
  flat->AddRectangle({4, -10}, {10, 10});
  doc.AddFeature(std::move(flat));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Cut", doc.GetFeature(2)->GetId(), 35.0, ExtrudeMode::Cut));
  REQUIRE(doc.Regenerate());
  CHECK(glm::length(seamX(doc) - u) < 1e-9);

  // And it is the part's: turn the sketch and X turns with it.
  auto* circle = static_cast<SketchFeature*>(doc.GetFeature(0));
  const double b = 75.0 * std::numbers::pi / 180.0;
  const glm::dvec3 u2(std::cos(b), std::sin(b), 0.0);
  circle->plane = SketchPlane(glm::dvec3(0.0), glm::dvec3(0, 0, 1), u2);
  REQUIRE(doc.Regenerate());
  CHECK(glm::length(seamX(doc) - u2) < 1e-9);
}
