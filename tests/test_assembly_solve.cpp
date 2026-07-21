#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/Occurrence.h"
#include "core/features/SketchFeature.h"

#include <glm/gtc/quaternion.hpp>

#include <memory>

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

Occurrence MakeOcc(uint64_t partId, glm::vec3 pos, bool fixed = false) {
  Occurrence o;
  o.partId = partId;
  o.position = pos;
  o.fixed = fixed;
  return o;
}

} // namespace

TEST_CASE("Fastened mate aligns an origin connector to a face frame", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, /*fixed=*/true));   // id 1
  asmDoc.AddOccurrence(MakeOcc(1, {100, 100, 100}));            // id 2

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
  CHECK_THAT(arm->position.x, WithinAbs(2.0f, 1e-4f));
  CHECK_THAT(arm->position.y, WithinAbs(2.0f, 1e-4f));
  CHECK_THAT(arm->position.z, WithinAbs(3.0f, 1e-4f));

  // The fixed anchor is never moved.
  CHECK(asmDoc.FindById(1)->position == glm::vec3(0, 0, 0));
}

TEST_CASE("Slider offset slides the placement along the connector axis", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  m.offset = 5.0f; // along the cap's +Z normal
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);
  CHECK_THAT(arm->position.z, WithinAbs(8.0f, 1e-4f)); // 3 (cap) + 5 (offset)

  // Bumping the offset moves it further — the mechanism responds numerically.
  asmDoc.FindMateById(3)->offset = 9.0f;
  assembly::Solve(asmDoc, resolve);
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(12.0f, 1e-4f));
}

TEST_CASE("Revolute mate seats the part axially by its offset (finding #6a)",
          "[assembly][solve]") {
  // A revolute mate must slide b along the shared axis by `offset` (the seat
  // depth up the shaft), not merely coincide origins — otherwise stacks
  // collapse. rotation stays the free DOF on top of that fixed seat.
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  m.offset = 5.0f; // seat 5 up the axis (cap at z=3 → z=8)
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(8.0f, 1e-4f));

  // Spinning the free DOF keeps the axial seat fixed.
  asmDoc.FindMateById(3)->rotation = glm::radians(90.0f);
  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);
  CHECK_THAT(arm->position.z, WithinAbs(8.0f, 1e-4f)); // seat unchanged
  CHECK(std::abs(glm::dot(arm->orientation, glm::quat(1, 0, 0, 0))) < 0.999f);
}

TEST_CASE("A positive offset moves b along +Z of a's connector frame (finding #6c)",
          "[assembly][solve]") {
  // The sign convention: a POSITIVE offset moves b in the +Z direction of a's
  // connector frame (a's outward face normal). The top cap's normal is world
  // +Z, so +offset lifts b and -offset lowers it — no inversion. Guards every
  // offset-using mate type (slider/cylindrical/revolute share MateAdjust).
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext); // top cap "cap1" at z=3, normal +Z
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

  auto placeWithOffset = [&](float offset) {
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

  CHECK_THAT(placeWithOffset(5.0f), WithinAbs(8.0f, 1e-4f));  // 3 + (+5)
  CHECK_THAT(placeWithOffset(-5.0f), WithinAbs(-2.0f, 1e-4f)); // 3 + (-5)
  CHECK_THAT(placeWithOffset(0.0f), WithinAbs(3.0f, 1e-4f));   // on the cap
}

TEST_CASE("Assembly mass properties aggregate occurrences in world space (finding #5)",
          "[assembly][solve]") {
  auto box = BuildSteelBox(3.0); // 4x4x3 steel box: vol 48 mm^3
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  const assembly::AssemblyMass hidden = assembly::MassProperties(asmDoc, resolve);
  REQUIRE(hidden.occurrences.size() == 1);
  CHECK_THAT(hidden.volume, WithinAbs(48.0, 1e-6));
  CHECK_THAT(hidden.centerOfMass.x, WithinAbs(2.0, 1e-5));
}

TEST_CASE("Revolute rotation spins without translating an origin connector", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  const glm::quat before = asmDoc.FindById(2)->orientation;
  const glm::vec3 pos0 = asmDoc.FindById(2)->position;

  asmDoc.FindMateById(3)->rotation = glm::radians(90.0f);
  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);

  // Rotation about the axis through the connector origin: position pinned.
  CHECK_THAT(arm->position.x, WithinAbs(pos0.x, 1e-4f));
  CHECK_THAT(arm->position.y, WithinAbs(pos0.y, 1e-4f));
  CHECK_THAT(arm->position.z, WithinAbs(pos0.z, 1e-4f));
  // ...but the orientation changed (the DOF moved).
  const float dot = std::abs(glm::dot(before, arm->orientation));
  CHECK(dot < 0.999f);
}

TEST_CASE("Payoff: editing the base part moves the mated occurrence", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(3.0f, 1e-4f));

  // Edit the base: raise the pad 3 → 7. The named cap moves with the geometry,
  // and a re-solve carries the mated occurrence with it — the whole point of
  // mating faces, not coordinates.
  auto* pad = dynamic_cast<ExtrudeFeature*>(box->GetFeature(1));
  REQUIRE(pad != nullptr);
  pad->distance = 7.0;
  box->Regenerate();

  assembly::Solve(asmDoc, resolve);
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(7.0f, 1e-4f));
}

TEST_CASE("A floating component with no fixed anchor stays put", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {5, 0, 0}));   // id 1, NOT fixed
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
  // Lowest-id occurrence (1) anchors at its stored transform; 2 follows onto it.
  CHECK(asmDoc.FindById(1)->position == glm::vec3(5, 0, 0));
  CHECK_THAT(asmDoc.FindById(2)->position.x, WithinAbs(5.0f, 1e-4f));
  CHECK_THAT(asmDoc.FindById(2)->position.y, WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(asmDoc.FindById(2)->position.z, WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("An over-constrained loop is reported, not solved", "[assembly][solve]") {
  auto box = std::unique_ptr<Document>();
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  CHECK(asmDoc.FindById(2)->position == glm::vec3(100, 0, 0));
}

TEST_CASE("A dangling connector ref errors the mate", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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

TEST_CASE("Cylindrical mate frees both rotation and offset", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));
  Mate m = FaceMate(ext, MateType::Cylindrical);
  m.offset = 4.0f;                     // slides along the cap normal (+Z)
  m.rotation = glm::radians(30.0f);    // and spins about it
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);
  CHECK_THAT(arm->position.z, WithinAbs(7.0f, 1e-4f)); // 3 + 4
  // Rotation took effect (non-identity orientation).
  CHECK(std::abs(glm::dot(arm->orientation, glm::quat(1, 0, 0, 0))) < 0.999f);
}

TEST_CASE("Planar mate slides in-plane by dx/dy", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}));
  Mate m = FaceMate(ext, MateType::Planar);
  m.dx = 5.0f; // along the cap frame's in-plane X (world +X for a +Z cap)
  m.dy = 0.0f;
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);
  // Base cap centroid is (2,2,3); +5 along the frame X axis.
  CHECK_THAT(arm->position.z, WithinAbs(3.0f, 1e-4f)); // stays on the plane
  const float inPlane = glm::length(glm::vec2(arm->position.x - 2.0f,
                                              arm->position.y - 2.0f));
  CHECK_THAT(inPlane, WithinAbs(5.0f, 1e-4f)); // slid 5 units within the plane
}

TEST_CASE("Ball mate rotates freely without translating", "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  m.ballRotation = glm::angleAxis(glm::radians(60.0f), glm::vec3(0, 1, 0));
  asmDoc.AddMate(m);

  assembly::Solve(asmDoc, resolve);
  const Occurrence* arm = asmDoc.FindById(2);
  // Origins coincide (ball is a point joint) — arm sits on the fixed origin.
  CHECK_THAT(arm->position.x, WithinAbs(7.0f, 1e-4f));
  CHECK_THAT(arm->position.y, WithinAbs(8.0f, 1e-4f));
  CHECK_THAT(arm->position.z, WithinAbs(9.0f, 1e-4f));
  // ...but it's rotated by the ball DOF.
  CHECK(std::abs(glm::dot(arm->orientation, glm::quat(1, 0, 0, 0))) < 0.999f);
}

TEST_CASE("Unresolvable connector names its side and suggests near misses",
          "[assembly][solve]") {
  uint64_t ext = 0;
  auto box = BuildBox(3.0, ext);
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
