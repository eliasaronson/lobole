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
