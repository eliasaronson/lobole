#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/AssemblyLoopSolve.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/SketchFeature.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <memory>

using Catch::Matchers::WithinAbs;

namespace {

glm::mat4 AtX(float x) {
  return glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
}

// A revolute joint about Z between two connector frames, with no flip so its
// adjustment is exactly Rz(rotation) — the algebra in the four-bar below is
// then checkable by hand.
assembly::LoopMate Revolute(uint64_t id, uint64_t a, const glm::mat4& fa,
                            uint64_t b, const glm::mat4& fb, float seed) {
  assembly::LoopMate m;
  m.id = id;
  m.type = MateType::Revolute;
  m.aOcc = a;
  m.bOcc = b;
  m.fa = fa;
  m.fb = fb;
  m.flipped = false;
  m.rotation = seed;
  return m;
}

assembly::LoopMate Fastened(uint64_t id, uint64_t a, const glm::mat4& fa,
                            uint64_t b, const glm::mat4& fb) {
  assembly::LoopMate m;
  m.id = id;
  m.type = MateType::Fastened;
  m.aOcc = a;
  m.bOcc = b;
  m.fa = fa;
  m.fb = fb;
  m.flipped = false;
  return m;
}

bool Lists(const std::vector<uint64_t>& v, uint64_t id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}

static const SketchPlane kXY(PlaneType::XY);

std::unique_ptr<Document> BuildBox() {
  auto doc = std::make_unique<Document>();
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  sk->AddRectangle({0, 0}, {4, 4});
  doc->AddFeature(std::move(sk));
  const uint64_t sketchId = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketchId, 3.0,
                                                   ExtrudeMode::NewBody));
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

// A fastened mate between two occurrence ORIGINS — no geometry to resolve, so
// several of them between the same pair are exactly consistent by
// construction. The flange case in miniature.
Mate OriginMate(uint64_t a, uint64_t b, float offset = 0.0f) {
  Mate m;
  m.type = MateType::Fastened;
  m.a.occurrenceId = a;
  m.a.anchor.origin = true;
  m.b.occurrenceId = b;
  m.b.anchor.origin = true;
  m.flipped = false;
  m.offset = offset;
  return m;
}

} // namespace

// ---------------------------------------------------------------------------
// The numeric core, on hand-built problems (no geometry, no Document).
// ---------------------------------------------------------------------------

TEST_CASE("A loop-free problem never enters the numeric pass",
          "[assembly][solve][loops]") {
  assembly::LoopProblem p;
  p.mates.push_back(
      Revolute(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f), 0.0f));
  p.tree.push_back({0, 10, 20, false});
  p.anchor.emplace_back(10, glm::mat4(1.0f));

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  CHECK_FALSE(s.ran);
  CHECK(s.dof == 0);
}

TEST_CASE("EvaluateTree places a chain the way the A3 pass does",
          "[assembly][solve][loops]") {
  assembly::LoopProblem p;
  // Ground -> crank about the origin, crank's far end 40 mm out.
  p.mates.push_back(Revolute(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f),
                             glm::half_pi<float>()));
  p.tree.push_back({0, 10, 20, false});
  p.anchor.emplace_back(10, glm::mat4(1.0f));

  const auto world = assembly::EvaluateTree(p, p.mates);
  REQUIRE(world.count(20) == 1);
  // A quarter turn about Z takes the crank's local +X to world +Y.
  const glm::vec3 tip = glm::vec3(world.at(20) * glm::vec4(40, 0, 0, 1));
  CHECK_THAT(tip.x, WithinAbs(0.0, 1e-4));
  CHECK_THAT(tip.y, WithinAbs(40.0, 1e-4));
}

TEST_CASE("A four-bar linkage closes and reports one degree of freedom",
          "[assembly][solve][loops]") {
  // Parallelogram crank-rocker: ground pivots 100 apart, crank and rocker
  // both 40, coupler 100. Mobility 1, and the solution family is
  // theta2 = -theta1, theta3 = theta1, theta4 = theta1.
  //
  //   occ 10 ground (fixed)   20 crank   30 coupler   40 rocker
  assembly::LoopProblem p;
  p.mates.push_back( // J1 ground-crank
      Revolute(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f), 0.3f));
  p.mates.push_back( // J2 crank-coupler
      Revolute(2, 20, AtX(40.0f), 30, glm::mat4(1.0f), 0.0f));
  p.mates.push_back( // J3 coupler-rocker
      Revolute(3, 30, AtX(100.0f), 40, AtX(40.0f), 0.0f));
  p.mates.push_back( // J4 rocker-ground
      Revolute(4, 10, AtX(100.0f), 40, glm::mat4(1.0f), 0.0f));
  p.tree.push_back({0, 10, 20, false});
  p.tree.push_back({1, 20, 30, false});
  p.tree.push_back({2, 30, 40, false});
  p.cut.push_back(3); // J4 closes the loop
  p.anchor.emplace_back(10, glm::mat4(1.0f));

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.unavailable.empty());
  CHECK(s.ran);
  CHECK(s.converged);
  CHECK(s.residualMm < 1e-3);

  // Four joint angles, three independent loop equations (the other three rows
  // are identically zero because the axes are parallel) => one DOF. Grubler
  // would have said 6*3 - 5*4 = -2 here.
  CHECK(s.dof == 1);
  CHECK(s.conflicting.empty());

  // And the joint that CLOSES the loop is not called redundant. Three of its
  // six rows are structurally zero (the axes are parallel, so the loop cannot
  // say anything about z or tilt) — but remove it and there is no linkage
  // left, so "some dependent rows" must not read as "this mate does nothing".
  CHECK(s.redundant.empty());

  // Closure: the rocker's ground pivot must sit on the ground pivot at x=100.
  REQUIRE(s.world.count(40) == 1);
  const glm::vec3 pivot = glm::vec3(s.world.at(40)[3]);
  CHECK_THAT(pivot.x, WithinAbs(100.0, 1e-3));
  CHECK_THAT(pivot.y, WithinAbs(0.0, 1e-3));
  CHECK_THAT(pivot.z, WithinAbs(0.0, 1e-3));

  // Every joint axis is Z, so a closed loop must have its rotations sum: the
  // three tree joints carry the rocker exactly as far round as the cut joint
  // says it went. Branch-INDEPENDENT, which matters — a four-bar with equal
  // opposite links has two assembly modes (parallelogram and crossed) and
  // both are correct answers. Which one LM lands on is a property of the
  // seed, not of the mechanism.
  const double sum = static_cast<double>(s.solved[0].rotation) +
                     s.solved[1].rotation + s.solved[2].rotation;
  CHECK_THAT(sum, WithinAbs(s.solved[3].rotation, 1e-4));
}

TEST_CASE("A four-bar seeded on a branch stays on it",
          "[assembly][solve][loops]") {
  // The same linkage seeded at an exact parallelogram pose
  // (theta2 = -theta1, theta3 = theta4 = theta1). It is already closed, so
  // the solver must recognise that and leave it alone rather than wander to
  // the crossed branch — "turn the crank and the rocker follows" depends on
  // the solve tracking the configuration it was given.
  constexpr float kTheta = 0.3f;
  assembly::LoopProblem p;
  p.mates.push_back(
      Revolute(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f), kTheta));
  p.mates.push_back(Revolute(2, 20, AtX(40.0f), 30, glm::mat4(1.0f), -kTheta));
  p.mates.push_back(Revolute(3, 30, AtX(100.0f), 40, AtX(40.0f), kTheta));
  p.mates.push_back(Revolute(4, 10, AtX(100.0f), 40, glm::mat4(1.0f), kTheta));
  p.tree.push_back({0, 10, 20, false});
  p.tree.push_back({1, 20, 30, false});
  p.tree.push_back({2, 30, 40, false});
  p.cut.push_back(3);
  p.anchor.emplace_back(10, glm::mat4(1.0f));

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.unavailable.empty());
  CHECK(s.converged);
  CHECK(s.dof == 1);
  CHECK(s.conflicting.empty());

  const float t1 = s.solved[0].rotation;
  CHECK_THAT(t1, WithinAbs(kTheta, 1e-4));
  CHECK_THAT(s.solved[1].rotation, WithinAbs(-kTheta, 1e-4));
  CHECK_THAT(s.solved[2].rotation, WithinAbs(kTheta, 1e-4));
  CHECK_THAT(s.solved[3].rotation, WithinAbs(kTheta, 1e-4));

  // The coupler stays horizontal, which is what makes it a parallelogram.
  REQUIRE(s.world.count(30) == 1);
  const glm::vec3 along = glm::vec3(s.world.at(30) * glm::vec4(1, 0, 0, 0));
  CHECK_THAT(along.x, WithinAbs(1.0, 1e-4));
  CHECK_THAT(along.y, WithinAbs(0.0, 1e-4));
}

TEST_CASE("A redundant but consistent mate is named, not failed",
          "[assembly][solve][loops]") {
  // Two parts held by three identical fastened mates: the flange with three
  // bolts. Nothing is free, nothing conflicts, two mates say nothing new.
  assembly::LoopProblem p;
  p.mates.push_back(Fastened(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f)));
  p.mates.push_back(Fastened(2, 10, AtX(25.0f), 20, AtX(25.0f)));
  p.mates.push_back(Fastened(3, 10, AtX(50.0f), 20, AtX(50.0f)));
  p.tree.push_back({0, 10, 20, false});
  p.cut.push_back(1);
  p.cut.push_back(2);
  p.anchor.emplace_back(10, glm::mat4(1.0f));

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.unavailable.empty());
  CHECK(s.converged);
  CHECK(s.dof == 0);
  CHECK(s.conflicting.empty());
  CHECK(s.redundant.size() == 2);
  CHECK(Lists(s.redundant, 2));
  CHECK(Lists(s.redundant, 3));
}

TEST_CASE("A hinge hanging off a loop keeps the angle it was posed at",
          "[assembly][solve][loops]") {
  // The flange above, with a lid on it: two parts bolted twice (a loop the
  // solver has to close) and a revolute joining a third part to one of them
  // — a bridge, on no cycle, so nothing about the loop depends on its angle.
  //
  // It came out of the grill, where the lid's hinge is nowhere near the two
  // bolts that hold its handle on. Damping is RELATIVE to the diagonal, and
  // an unconstrained column has none, so the solve gave that DOF whatever
  // satisfied 0 = 0: the lid stood 443 degrees open while the loop closed to
  // a ten-thousandth of a millimetre.
  assembly::LoopProblem p;
  p.mates.push_back(Fastened(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f)));
  p.mates.push_back(Fastened(2, 10, AtX(25.0f), 20, AtX(25.0f)));
  p.mates.push_back(
      Revolute(3, 30, glm::mat4(1.0f), 10, glm::mat4(1.0f), 0.0f));
  p.tree.push_back({0, 10, 20, false});
  p.tree.push_back({2, 10, 30, true});
  p.cut.push_back(1);
  p.anchor.emplace_back(10, glm::mat4(1.0f));

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.unavailable.empty());
  CHECK(s.converged);
  CHECK_THAT(s.solved[2].rotation, WithinAbs(0.0, 1e-9));
  // And a pose that is NOT zero is just as much a pose.
  p.mates[2].rotation = 0.75f;
  const assembly::LoopSolution posed = assembly::SolveLoops(p);
  REQUIRE(posed.unavailable.empty());
  CHECK(posed.converged);
  CHECK_THAT(posed.solved[2].rotation, WithinAbs(0.75, 1e-9));
}

TEST_CASE("A loop that cannot close reports how far off it is",
          "[assembly][solve][loops]") {
  // Same pair, two fastened mates demanding placements 10 mm apart.
  assembly::LoopProblem p;
  p.mates.push_back(Fastened(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f)));
  p.mates.push_back(Fastened(2, 10, AtX(10.0f), 20, glm::mat4(1.0f)));
  p.tree.push_back({0, 10, 20, false});
  p.cut.push_back(1);
  p.anchor.emplace_back(10, glm::mat4(1.0f));

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.unavailable.empty());
  CHECK_FALSE(s.converged);
  CHECK_THAT(s.residualMm, WithinAbs(10.0, 1e-6));
  CHECK(s.conflicting.size() == 1);
  CHECK(Lists(s.conflicting, 2));
  // A mate that did not close is a conflict, never a redundancy.
  CHECK(s.redundant.empty());
}

TEST_CASE("A slider loop resolves its own offset", "[assembly][solve][loops]") {
  // Ground -> slider -> block, and a second fastened mate pinning the block
  // 12 mm along. The slider's offset is an UNKNOWN, so the loop closes by
  // solving for it rather than by failing.
  assembly::LoopProblem p;
  assembly::LoopMate slide;
  slide.id = 1;
  slide.type = MateType::Slider;
  slide.aOcc = 10;
  slide.bOcc = 20;
  slide.flipped = false;
  slide.offset = 0.0f; // seeded wrong on purpose
  p.mates.push_back(slide);
  p.mates.push_back(
      Fastened(2, 10, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 12.0f)),
               20, glm::mat4(1.0f)));
  p.tree.push_back({0, 10, 20, false});
  p.cut.push_back(1);
  p.anchor.emplace_back(10, glm::mat4(1.0f));

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.unavailable.empty());
  CHECK(s.converged);
  CHECK_THAT(s.solved[0].offset, WithinAbs(12.0, 1e-3));
  CHECK(s.dof == 0); // the one DOF was consumed closing the loop
  CHECK(s.conflicting.empty());
}

// ---------------------------------------------------------------------------
// Through the real placement pass.
// ---------------------------------------------------------------------------

TEST_CASE("Solve reports a consistent loop instead of erroring on it",
          "[assembly][solve][loops]") {
  auto box = BuildBox();
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  const uint64_t a = asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  const uint64_t b = asmDoc.AddOccurrence(MakeOcc(1, {50, 50, 50}));
  asmDoc.AddMate(OriginMate(a, b));
  const uint64_t m2 = asmDoc.AddMate(OriginMate(a, b));
  const uint64_t m3 = asmDoc.AddMate(OriginMate(a, b));

  const assembly::SolveReport r = assembly::Solve(asmDoc, resolve);

  CHECK(r.loopMates == 2);
  CHECK(r.loopsClosed);
  CHECK(r.dof == 0);
  CHECK(r.erroredMates == 0); // the A3 pass called this "over-constrained"
  CHECK(r.redundantMates.size() == 2);
  CHECK(Lists(r.redundantMates, m2));
  CHECK(Lists(r.redundantMates, m3));
  for (const Mate& m : asmDoc.GetMates())
    CHECK(m.error.empty());
}

TEST_CASE("Solve still errors when a loop genuinely conflicts",
          "[assembly][solve][loops]") {
  auto box = BuildBox();
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  const uint64_t a = asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  const uint64_t b = asmDoc.AddOccurrence(MakeOcc(1, {50, 50, 50}));
  asmDoc.AddMate(OriginMate(a, b, 0.0f));
  const uint64_t bad = asmDoc.AddMate(OriginMate(a, b, 7.0f));

  const assembly::SolveReport r = assembly::Solve(asmDoc, resolve);

  CHECK(r.loopMates == 1);
  CHECK_FALSE(r.loopsClosed);
  CHECK(r.erroredMates == 1);
  CHECK_THAT(r.residualMm, WithinAbs(7.0, 1e-3));
  const Mate* m = asmDoc.FindMateById(bad);
  REQUIRE(m != nullptr);
  // The number is the point: 7 mm is a modelling error, 0.02 mm is tolerance.
  CHECK(m->error.find("does not close") != std::string::npos);
  CHECK(m->error.find("7") != std::string::npos);
}

TEST_CASE("A loop-free assembly reports nothing from the loop pass",
          "[assembly][solve][loops]") {
  auto box = BuildBox();
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  const uint64_t a = asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  const uint64_t b = asmDoc.AddOccurrence(MakeOcc(1, {50, 50, 50}));
  asmDoc.AddMate(OriginMate(a, b));

  const assembly::SolveReport r = assembly::Solve(asmDoc, resolve);
  CHECK(r.loopMates == 0);
  CHECK(r.loopsClosed);
  CHECK(r.redundantMates.empty());
  CHECK_THAT(r.residualMm, WithinAbs(0.0, 1e-12));
}

TEST_CASE("Two rails on one slide leave a degree of freedom",
          "[assembly][solve][loops]") {
  // The same pair joined by two sliders along the same axis: a carriage on two
  // parallel rails. The loop closes only when the two offsets agree, which
  // leaves one free DOF (slide them together) — and the cut mate is only
  // PARTLY dependent (five of its six rows are structurally zero), so it must
  // not be reported redundant either.
  const auto rail = [](uint64_t id, float seed) {
    assembly::LoopMate m;
    m.id = id;
    m.type = MateType::Slider;
    m.aOcc = 10;
    m.bOcc = 20;
    m.flipped = false;
    m.offset = seed;
    return m;
  };
  assembly::LoopProblem p;
  p.mates.push_back(rail(1, 0.0f));
  p.mates.push_back(rail(2, 10.0f)); // disagrees with the first on purpose
  p.tree.push_back({0, 10, 20, false});
  p.cut.push_back(1);
  p.anchor.emplace_back(10, glm::mat4(1.0f));

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.unavailable.empty());
  CHECK(s.converged);
  CHECK(s.dof == 1); // two offsets, one independent equation between them
  CHECK(s.conflicting.empty());
  CHECK(s.redundant.empty());
  // They had to meet somewhere.
  CHECK_THAT(s.solved[0].offset, WithinAbs(s.solved[1].offset, 1e-4));
}

TEST_CASE("Independent loops are solved independently",
          "[assembly][solve][loops]") {
  // Two separate carriages, each on its own pair of rails. They share no
  // occurrence, so they are two components and must not be assembled into one
  // system — a Jacobian spanning both is quadratic in the size of the model
  // for loops that are each four parts wide.
  const auto rail = [](uint64_t id, uint64_t a, uint64_t b, float seed) {
    assembly::LoopMate m;
    m.id = id;
    m.type = MateType::Slider;
    m.aOcc = a;
    m.bOcc = b;
    m.flipped = false;
    m.offset = seed;
    return m;
  };
  assembly::LoopProblem p;
  p.mates.push_back(rail(1, 10, 20, 0.0f));
  p.mates.push_back(rail(2, 10, 20, 10.0f));
  p.mates.push_back(rail(3, 30, 40, 0.0f));
  p.mates.push_back(rail(4, 30, 40, -6.0f));
  p.tree.push_back({0, 10, 20, false});
  p.tree.push_back({2, 30, 40, false});
  p.cut.push_back(1);
  p.cut.push_back(3);
  p.anchor.emplace_back(10, glm::mat4(1.0f));
  p.anchor.emplace_back(30, glm::mat4(1.0f));

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.unavailable.empty());
  CHECK(s.converged);
  CHECK(s.conflicting.empty());
  CHECK(s.dof == 2); // one per carriage, summed over the components
  // Each pair met on its own, and neither was dragged toward the other.
  CHECK_THAT(s.solved[0].offset, WithinAbs(s.solved[1].offset, 1e-4));
  CHECK_THAT(s.solved[2].offset, WithinAbs(s.solved[3].offset, 1e-4));
  CHECK(s.world.count(20) == 1);
  CHECK(s.world.count(40) == 1);
}

TEST_CASE("A loop leaves a mate tree elsewhere in the document alone",
          "[assembly][solve][loops]") {
  // Two disjoint groups: one with a loop, one a plain tree. The loop pass
  // only returns placements for components it actually solved, so Solve has
  // to MERGE them over what the walk produced — replacing would silently
  // strand every occurrence in the other group at its stored transform.
  auto box = BuildBox();
  const assembly::PartResolver resolve = [&](uint64_t, uint64_t) {
    return box.get();
  };

  AssemblyDocument asmDoc;
  const uint64_t a = asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  const uint64_t b = asmDoc.AddOccurrence(MakeOcc(1, {50, 50, 50}));
  const uint64_t c = asmDoc.AddOccurrence(MakeOcc(1, {0, 0, 0}, true));
  const uint64_t d = asmDoc.AddOccurrence(MakeOcc(1, {90, 90, 90}));
  asmDoc.AddMate(OriginMate(a, b)); // the looped group
  asmDoc.AddMate(OriginMate(a, b));
  asmDoc.AddMate(OriginMate(c, d)); // an untouched tree

  const assembly::SolveReport r = assembly::Solve(asmDoc, resolve);
  CHECK(r.loopMates == 1);
  CHECK(r.loopsClosed);
  CHECK(r.erroredMates == 0);

  // Both followers were pulled onto their anchors: the one in the loop AND
  // the one in the tree.
  const Occurrence* ob = asmDoc.FindById(b);
  const Occurrence* od = asmDoc.FindById(d);
  REQUIRE(ob != nullptr);
  REQUIRE(od != nullptr);
  CHECK(glm::length(ob->position) < 1e-3f);
  CHECK(glm::length(od->position) < 1e-3f);
}

TEST_CASE("A driven DOF is HELD while the loop closes around it",
          "[assembly][solve][loops]") {
  // The defect the single-cylinder engine found
  // (generators/README.md#single-cylinder-and-air-cooled-engines): a closed
  // loop has one more unknown than it has equations, so the least-squares step
  // is free to move ANY of them — including the one the caller just drove.
  // Setting a crank to 90 degrees and reading back 67 is not a tolerance; it is
  // the solver answering a different question.
  //
  // The same four-bar, with J1 driven to a quarter turn.
  const auto build = [](double drive, bool pin) {
    assembly::LoopProblem p;
    p.mates.push_back(Revolute(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f),
                               static_cast<float>(drive)));
    p.mates.push_back(Revolute(2, 20, AtX(40.0f), 30, glm::mat4(1.0f), 0.0f));
    p.mates.push_back(Revolute(3, 30, AtX(100.0f), 40, AtX(40.0f), 0.0f));
    p.mates.push_back(Revolute(4, 10, AtX(100.0f), 40, glm::mat4(1.0f), 0.0f));
    p.tree.push_back({0, 10, 20, false});
    p.tree.push_back({1, 20, 30, false});
    p.tree.push_back({2, 30, 40, false});
    p.cut.push_back(3);
    p.anchor.emplace_back(10, glm::mat4(1.0f));
    if (pin)
      p.pinned.push_back({0, MateDof::Rotation, drive});
    return p;
  };

  const double drive = 3.14159265358979 / 2.0; // 90 degrees
  SECTION("pinned, it stays where it was driven") {
    const assembly::LoopSolution s = assembly::SolveLoops(build(drive, true));
    REQUIRE(s.ran);
    CHECK(s.converged);
    CHECK(s.residualMm < 1e-3);
    CHECK_THAT(static_cast<double>(s.solved[0].rotation),
               WithinAbs(drive, 1e-4));
    // And the linkage really closed AROUND it. Checked branch-independently:
    // a four-bar with equal opposite links has two assembly modes and both
    // are correct, so what is pinned is the closure — the rocker's ground
    // pivot back on the ground pivot — not which mode it landed in.
    REQUIRE(s.world.count(40) == 1);
    const glm::vec3 pivot = glm::vec3(s.world.at(40)[3]);
    CHECK_THAT(pivot.x, WithinAbs(100.0, 1e-3));
    CHECK_THAT(pivot.y, WithinAbs(0.0, 1e-3));
  }
  SECTION("a pin does not break a loop that has nothing to pin") {
    assembly::LoopProblem p = build(drive, true);
    p.pinned[0].dof = MateDof::Offset; // a revolute does not slide
    const assembly::LoopSolution s = assembly::SolveLoops(p);
    CHECK(s.ran);
    CHECK(s.converged); // the row is skipped, not a bad column
  }
}

// ---------------------------------------------------------------------------
// Naming the mate that keeps a loop open
// (docs/assemblies/solver.md#naming-the-mate-that-keeps-a-loop-open).
// ---------------------------------------------------------------------------

namespace {

glm::mat4 Tilted(glm::vec3 axis, float degrees) {
  return glm::rotate(glm::mat4(1.0f), glm::radians(degrees),
                     glm::normalize(axis));
}

// A crank-rocker that is NOT a parallelogram, so it has no special poses to
// hide in: ground pivots 100 apart, crank 30, coupler 90, rocker 60.
//
//   occ 10 ground (fixed)   20 crank   30 coupler   40 rocker
//   J1 ground-crank, J2 crank-coupler, J3 coupler-rocker: the tree
//   J4 ground-rocker: the cut
//
// `fault` is composed onto one connector frame; index = mate, side = a|b.
assembly::LoopProblem CrankRocker(size_t faultMate = 99, char side = 'a',
                                  const glm::mat4& fault = glm::mat4(1.0f)) {
  assembly::LoopProblem p;
  p.mates.push_back(
      Revolute(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f), 1.2f));
  p.mates.push_back(Revolute(2, 20, AtX(30.0f), 30, glm::mat4(1.0f), -0.8f));
  p.mates.push_back(Revolute(3, 30, AtX(90.0f), 40, AtX(60.0f), 1.0f));
  p.mates.push_back(Revolute(4, 10, AtX(100.0f), 40, glm::mat4(1.0f), 1.0f));
  p.tree.push_back({0, 10, 20, false});
  p.tree.push_back({1, 20, 30, false});
  p.tree.push_back({2, 30, 40, false});
  p.cut.push_back(3);
  p.anchor.emplace_back(10, glm::mat4(1.0f));
  // Seed at a closed pose of the CORRECT linkage, as a document that was
  // right until one connector moved would be.
  const assembly::LoopSolution closed = assembly::SolveLoops(p);
  for (size_t i = 0; i < p.mates.size(); i++)
    p.mates[i].rotation = closed.solved[i].rotation;
  if (faultMate < p.mates.size()) {
    glm::mat4& f = side == 'b' ? p.mates[faultMate].fb : p.mates[faultMate].fa;
    f = f * fault;
  }
  p.diagnose = true;
  return p;
}

} // namespace

TEST_CASE("A tilted tree revolute is named, not the cut mate that fails",
          "[assembly][solve][loops][diagnose]") {
  // J2's crank-side connector tilted 30 degrees about its own Y: the coupler
  // leaves the plane and the loop fails at J4, three joints away. A tilt at
  // J3 or J4 closes it TOO — a loop with two extra unknowns has as many
  // unknowns as equations — but only as a rigid spatial linkage at one
  // pose. Undoing the fault at J2 gives back the four-bar's one DOF.
  const assembly::LoopSolution s =
      assembly::SolveLoops(CrankRocker(1, 'a', Tilted({0, 1, 0}, 30.0f)));
  REQUIRE(s.unavailable.empty());
  CHECK_FALSE(s.converged);
  REQUIRE(s.conflicting.size() == 1);
  CHECK(s.conflicting[0] == 4); // the error lands on the cut...
  REQUIRE(s.diagnoses.size() == 1);
  const assembly::LoopDiagnosis& d = s.diagnoses[0];
  REQUIRE(d.suspects.size() == 1);
  CHECK(d.suspects[0].mate == 2); // ...and the diagnosis names the fault
  CHECK(d.suspects[0].connector == 'a');
  CHECK_THAT(glm::degrees(d.suspects[0].tiltRad), WithinAbs(30.0, 0.5));
  CHECK(d.suspects[0].dof == 1);

  // The others that closed did so rigidly — that is what ruled them out.
  int closedRigid = 0;
  for (const assembly::LoopTiltTrial& t : d.trials)
    if (t.closes && t.mate != 2)
      closedRigid += t.dof == 0 ? 1 : 0;
  CHECK(closedRigid >= 2);
}

TEST_CASE("The diagnosis finds a tilt on either connector of any joint",
          "[assembly][solve][loops][diagnose]") {
  SECTION("the cut mate's own connector") {
    const assembly::LoopSolution s =
        assembly::SolveLoops(CrankRocker(3, 'a', Tilted({1, 1, 0}, 30.0f)));
    REQUIRE(s.diagnoses.size() == 1);
    REQUIRE(s.diagnoses[0].suspects.size() == 1);
    CHECK(s.diagnoses[0].suspects[0].mate == 4);
    CHECK(s.diagnoses[0].suspects[0].connector == 'a');
  }
  SECTION("a B connector, which a tilt on A cannot express") {
    // The rocker's end of J3. Tilting only A sides named J1 and J2 here —
    // both wrong — because neither can make the axes parallel again.
    const assembly::LoopSolution s =
        assembly::SolveLoops(CrankRocker(2, 'b', Tilted({0, 1, 0}, 20.0f)));
    REQUIRE(s.diagnoses.size() == 1);
    REQUIRE(s.diagnoses[0].suspects.size() == 1);
    CHECK(s.diagnoses[0].suspects[0].mate == 3);
    CHECK(s.diagnoses[0].suspects[0].connector == 'b');
    CHECK_THAT(glm::degrees(s.diagnoses[0].suspects[0].tiltRad),
               WithinAbs(20.0, 0.5));
  }
  SECTION("a tilt about the link's own line is a real tie") {
    // Tilting J2's crank connector about the crank's own length is the same
    // as J1's crank connector turned the same way: both lie on that line.
    // Two suspects, and neither is claimed.
    const assembly::LoopSolution s =
        assembly::SolveLoops(CrankRocker(1, 'a', Tilted({1, 0, 0}, 30.0f)));
    REQUIRE(s.diagnoses.size() == 1);
    const auto& sus = s.diagnoses[0].suspects;
    REQUIRE(sus.size() == 2);
    CHECK(((sus[0].mate == 1 && sus[1].mate == 2) ||
           (sus[0].mate == 2 && sus[1].mate == 1)));
  }
}

TEST_CASE("A loop that closes runs no diagnosis",
          "[assembly][solve][loops][diagnose]") {
  const assembly::LoopSolution s = assembly::SolveLoops(CrankRocker());
  REQUIRE(s.unavailable.empty());
  CHECK(s.converged);
  CHECK(s.conflicting.empty());
  CHECK(s.diagnoses.empty());
}

TEST_CASE("A rotation mismatch at one point names every joint that absorbs it",
          "[assembly][solve][loops][diagnose]") {
  // Three revolutes all at the origin, the loop's own connector tilted 30
  // degrees. Every joint sits at the same point, so a tilt of either
  // ground-side connector is the same correction: the numbers cannot tell
  // them apart, and the diagnosis says so rather than picking one.
  assembly::LoopProblem p;
  const glm::mat4 I(1.0f);
  p.mates.push_back(Revolute(1, 10, I, 20, I, 0.0f));
  p.mates.push_back(Revolute(2, 20, I, 30, I, 0.0f));
  p.mates.push_back(Revolute(3, 10, Tilted({1, 0, 0}, 30.0f), 30, I, 0.0f));
  p.tree.push_back({0, 10, 20, false});
  p.tree.push_back({1, 20, 30, false});
  p.cut.push_back(2);
  p.anchor.emplace_back(10, I);
  p.diagnose = true;

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.diagnoses.size() == 1);
  const auto& sus = s.diagnoses[0].suspects;
  REQUIRE(sus.size() >= 2);
  bool hasFault = false;
  for (const assembly::LoopTiltTrial& t : sus) {
    CHECK_THAT(glm::degrees(t.tiltRad), WithinAbs(30.0, 0.5));
    hasFault |= t.mate == 3;
  }
  CHECK(hasFault);
}

TEST_CASE("A translation fault is not blamed on any axis",
          "[assembly][solve][loops][diagnose]") {
  // J4's pin shifted 5 mm along its own axis: no tilt anywhere puts the
  // rocker's plane back on the ground's, so there is no suspect and the
  // message stays the plain one.
  const assembly::LoopSolution s = assembly::SolveLoops(CrankRocker(
      3, 'b', glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 5.0f))));
  REQUIRE(s.unavailable.empty());
  CHECK_FALSE(s.converged);
  CHECK_THAT(s.residualMm, WithinAbs(5.0, 1e-3));
  REQUIRE(s.diagnoses.size() == 1);
  CHECK(s.diagnoses[0].suspects.empty());
  for (const assembly::LoopTiltTrial& t : s.diagnoses[0].trials)
    CHECK_FALSE(t.closes);
}

TEST_CASE("Each conflicting mate reports its own residual",
          "[assembly][solve][loops]") {
  // One pair, three fastened mates: the first places it, the other two
  // demand 7 mm and 3 mm. Each is off by its own amount, not the worst.
  assembly::LoopProblem p;
  p.mates.push_back(Fastened(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f)));
  p.mates.push_back(Fastened(2, 10, AtX(7.0f), 20, glm::mat4(1.0f)));
  p.mates.push_back(Fastened(3, 10, AtX(3.0f), 20, glm::mat4(1.0f)));
  p.tree.push_back({0, 10, 20, false});
  p.cut.push_back(1);
  p.cut.push_back(2);
  p.anchor.emplace_back(10, glm::mat4(1.0f));
  p.diagnose = true;

  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.conflicting.size() == 2);
  REQUIRE(s.conflictingMm.size() == 2);
  for (size_t k = 0; k < 2; k++)
    CHECK_THAT(s.conflictingMm[k],
               WithinAbs(s.conflicting[k] == 2 ? 7.0 : 3.0, 1e-6));
  CHECK_THAT(s.residualMm, WithinAbs(7.0, 1e-6));
  // No axis-carrying mate, so nothing to tilt and nothing to suspect.
  REQUIRE(s.diagnoses.size() == 1);
  CHECK(s.diagnoses[0].trials.empty());
  CHECK(s.diagnoses[0].suspects.empty());
}

// ---- Through the real placement pass -------------------------------------

namespace {

// A link part: nothing but named Frame features, which is all a mate needs.
std::unique_ptr<Document> Link(const std::vector<glm::vec3>& origins) {
  auto doc = std::make_unique<Document>();
  for (size_t i = 0; i < origins.size(); i++) {
    auto fr = std::make_unique<FrameFeature>("f" + std::to_string(i),
                                             FrameKind::Explicit);
    for (int c = 0; c < 3; c++)
      fr->origin[c] = origins[i][c];
    doc->AddFeature(std::move(fr));
  }
  doc->Regenerate();
  return doc;
}

FrameFeature& FrameOf(Document& part, size_t index) {
  return *dynamic_cast<FrameFeature*>(part.GetFeature(index));
}

Mate FrameMate(const std::string& name, uint64_t a, uint64_t fa, uint64_t b,
               uint64_t fb) {
  Mate m;
  m.name = name;
  m.type = MateType::Revolute;
  m.a.occurrenceId = a;
  m.a.anchor.frameId = fa;
  m.b.occurrenceId = b;
  m.b.anchor.frameId = fb;
  m.flipped = false;
  return m;
}

} // namespace

TEST_CASE("Solve names the tilted connector in the failing mate's error",
          "[assembly][solve][loops][diagnose]") {
  auto ground = Link({{0, 0, 0}, {100, 0, 0}});
  auto crank = Link({{0, 0, 0}, {30, 0, 0}});
  auto coupler = Link({{0, 0, 0}, {90, 0, 0}});
  auto rocker = Link({{0, 0, 0}, {60, 0, 0}});
  const std::vector<Document*> parts{ground.get(), crank.get(), coupler.get(),
                                     rocker.get()};
  const assembly::PartResolver resolve = [&](uint64_t id,
                                             uint64_t) -> const Document* {
    return id >= 1 && id <= parts.size() ? parts[id - 1] : nullptr;
  };
  const auto frameId = [&](size_t part, size_t i) {
    return parts[part]->GetFeature(i)->GetId();
  };

  AssemblyDocument asmDoc;
  Occurrence o = MakeOcc(1, {0, 0, 0}, true);
  o.name = "ground";
  const uint64_t g = asmDoc.AddOccurrence(o);
  o = MakeOcc(2, {0, 40, 0});
  o.name = "crank";
  const uint64_t c = asmDoc.AddOccurrence(o);
  o = MakeOcc(3, {0, 80, 0});
  o.name = "coupler";
  const uint64_t k = asmDoc.AddOccurrence(o);
  o = MakeOcc(4, {0, 120, 0});
  o.name = "rocker";
  const uint64_t r = asmDoc.AddOccurrence(o);
  asmDoc.AddMate(FrameMate("main bearing", g, frameId(0, 0), c, frameId(1, 0)));
  asmDoc.AddMate(FrameMate("crank pin", c, frameId(1, 1), k, frameId(2, 0)));
  asmDoc.AddMate(FrameMate("rocker pin", k, frameId(2, 1), r, frameId(3, 1)));
  Mate seed = FrameMate("rocker pivot", g, frameId(0, 1), r, frameId(3, 0));
  const uint64_t cut = asmDoc.AddMate(seed);
  // Start the crank up and the rocker back so the solve lands on an open
  // (non-folded) pose.
  asmDoc.GetMates()[0].rotation = 1.2f;
  asmDoc.GetMates()[1].rotation = -0.8f;
  asmDoc.GetMates()[2].rotation = 1.0f;
  asmDoc.GetMates()[3].rotation = 1.0f;

  // Built right: it closes, and the diagnosis has nothing to say.
  const assembly::SolveReport ok = assembly::Solve(asmDoc, resolve);
  REQUIRE(ok.loopMates == 1);
  REQUIRE(ok.loopsClosed);
  CHECK(ok.diagnoses.empty());
  for (const Mate& m : asmDoc.GetMates()) {
    CHECK(m.error.empty());
    CHECK(m.residualMm == 0.0);
  }
  // Closed as APPLIED: the occurrences written back put each mate's two
  // frames on one another -- the cut mate's too -- read from the placements
  // everything downstream uses, not from the loop solver's own matrices.
  const auto worldFrame = [&](uint64_t occ, size_t part, size_t i) {
    const Occurrence* o = asmDoc.FindById(occ);
    REQUIRE(o != nullptr);
    return glm::translate(glm::mat4(1.0f), o->position) *
           glm::mat4_cast(o->orientation) * FrameOf(*parts[part], i).Matrix();
  };
  const struct {
    uint64_t oa;
    size_t pa, fa;
    uint64_t ob;
    size_t pb, fb;
  } joints[] = {{g, 0, 0, c, 1, 0},
                {c, 1, 1, k, 2, 0},
                {k, 2, 1, r, 3, 1},
                {g, 0, 1, r, 3, 0}};
  for (const auto& j : joints) {
    const glm::mat4 a = worldFrame(j.oa, j.pa, j.fa);
    const glm::mat4 b = worldFrame(j.ob, j.pb, j.fb);
    INFO("joint " << j.oa << "/" << j.fa << " to " << j.ob << "/" << j.fb);
    CHECK(glm::length(glm::vec3(a[3]) - glm::vec3(b[3])) < 1e-3f);
    CHECK(glm::dot(glm::vec3(a[2]), glm::vec3(b[2])) > 0.9999f);
  }

  // Now the crank pin's frame is tilted 30 degrees about its Y.
  FrameFeature& pin = FrameOf(*crank, 1);
  const float t = glm::radians(30.0f);
  pin.zAxis = glm::vec3(std::sin(t), 0.0f, std::cos(t));
  pin.xAxis = glm::vec3(std::cos(t), 0.0f, -std::sin(t));
  crank->Regenerate();

  const assembly::SolveReport bad = assembly::Solve(asmDoc, resolve);
  CHECK_FALSE(bad.loopsClosed);
  const Mate* m = asmDoc.FindMateById(cut);
  REQUIRE(m != nullptr);
  INFO(m->error);
  // The existing words and number stay...
  CHECK(m->error.find("does not close") != std::string::npos);
  CHECK(m->residualMm > 1.0);
  CHECK_THAT(m->residualMm, WithinAbs(bad.residualMm, 1e-9));
  // ...and the mate that is wrong is named, with the tilt that fixes it.
  CHECK(m->error.find("'crank pin' connector A (crank)") != std::string::npos);
  CHECK(m->error.find("30.0") != std::string::npos);
  REQUIRE(bad.diagnoses.size() == 1);
  REQUIRE(bad.diagnoses[0].suspects.size() == 1);
  CHECK(bad.diagnoses[0].suspects[0].mate == asmDoc.GetMates()[1].id);

  // A motion sample's solve does not diagnose: the plain message only.
  const assembly::SolveReport quiet =
      assembly::Solve(asmDoc, resolve, nullptr, {}, /*diagnose=*/false);
  CHECK(quiet.diagnoses.empty());
  CHECK(m->error.find("does not close") != std::string::npos);
  CHECK(m->error.find("crank pin") == std::string::npos);
  (void)c;
  (void)k;
}
