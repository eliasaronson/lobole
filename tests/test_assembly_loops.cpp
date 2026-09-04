#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/AssemblyLoopSolve.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"

#include <glm/gtc/matrix_transform.hpp>

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
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  const assembly::PartResolver resolve = [&](uint64_t) { return box.get(); };

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
  // (docs/modeling/single-cylinder-dogfood.md): a closed loop has one more
  // unknown than it has equations, so the least-squares step is free to move
  // ANY of them — including the one the caller just drove. Setting a crank to
  // 90 degrees and reading back 67 is not a tolerance; it is the solver
  // answering a different question.
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
