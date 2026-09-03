// Relations: gear, rack, screw, linear (docs/assemblies/relations-plan.md).
//
// A relation couples two mates' degrees of freedom, dofB = ratio * dofA +
// phase. In a mate tree it is enforced by propagation before the closed-form
// placement; inside a loop it is one residual row beside the six per cut
// mate. Both halves are pinned here, and so is the document round trip.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/AssemblyLoopSolve.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

// A carrier (fixed) with two occurrences on revolute axles, each mated by
// origin so the connector frames are the occurrences' own frames.
struct Train {
  AssemblyDocument asmDoc;
  uint64_t carrier = 0, pinion = 0, wheel = 0;
  uint64_t axleA = 0, axleB = 0;
};

Connector Origin(uint64_t occ) {
  Connector c;
  c.occurrenceId = occ;
  c.anchor.origin = true;
  return c;
}

Mate Revolute(uint64_t a, uint64_t b, float offset = 0.0f) {
  Mate m;
  m.type = MateType::Revolute;
  m.a = Origin(a);
  m.b = Origin(b);
  m.flipped = false;
  m.offset = offset;
  return m;
}

Train MakeTrain() {
  Train t;
  Occurrence c;
  c.name = "Carrier";
  c.partId = 1;
  c.fixed = true;
  t.carrier = t.asmDoc.AddOccurrence(std::move(c));
  Occurrence p;
  p.name = "Pinion";
  p.partId = 2;
  t.pinion = t.asmDoc.AddOccurrence(std::move(p));
  Occurrence w;
  w.name = "Wheel";
  w.partId = 3;
  t.wheel = t.asmDoc.AddOccurrence(std::move(w));
  t.axleA = t.asmDoc.AddMate(Revolute(t.carrier, t.pinion));
  t.axleB = t.asmDoc.AddMate(Revolute(t.carrier, t.wheel, 48.0f));
  return t;
}

// The solve needs a part resolver; origin connectors never touch geometry,
// so an empty document per part is enough.
assembly::PartResolver NoParts() {
  static Document empty;
  return [](uint64_t) -> const Document* { return &empty; };
}

Relation Gear(uint64_t a, uint64_t b, double ratio, double phase = 0.0) {
  Relation r;
  r.kind = RelationKind::Gear;
  r.mateA = a;
  r.mateB = b;
  r.dofA = r.dofB = MateDof::Rotation;
  r.ratio = ratio;
  r.phase = phase;
  return r;
}

float ZRotationOf(const AssemblyDocument& d, uint64_t occ) {
  const Occurrence* o = d.FindById(occ);
  REQUIRE(o != nullptr);
  return glm::angle(o->orientation) * (o->orientation.z < 0.0f ? -1.0f : 1.0f);
}

} // namespace

TEST_CASE("Two gears on a carrier turn together, from either side",
          "[assembly][relations]") {
  Train t = MakeTrain();
  const uint64_t rel =
      t.asmDoc.AddRelation(Gear(t.axleA, t.axleB, -1.0 / 3.0, 0.1));
  REQUIRE(t.asmDoc.FindRelationById(rel) != nullptr);

  // A drives B on an unprompted solve.
  t.asmDoc.FindMateById(t.axleA)->rotation = 0.6f;
  assembly::Solve(t.asmDoc, NoParts());
  CHECK(t.asmDoc.FindRelationById(rel)->error.empty());
  CHECK_THAT(t.asmDoc.FindMateById(t.axleB)->rotation,
             WithinAbs(-0.6 / 3.0 + 0.1, 1e-6));
  // And the wheel's placement follows its mate, not just the number.
  CHECK_THAT(ZRotationOf(t.asmDoc, t.wheel), WithinAbs(-0.2 + 0.1, 1e-5));
  CHECK_THAT(ZRotationOf(t.asmDoc, t.pinion), WithinAbs(0.6, 1e-5));

  // Turning the WHEEL (a drag, set_mate) drives the pinion the other way.
  t.asmDoc.FindMateById(t.axleB)->rotation = 0.4f;
  CHECK(assembly::PropagateRelations(t.asmDoc, t.axleB) == 1);
  assembly::Solve(t.asmDoc, NoParts());
  CHECK_THAT(t.asmDoc.FindMateById(t.axleA)->rotation,
             WithinAbs((0.4 - 0.1) * -3.0, 1e-5));
  CHECK_THAT(t.asmDoc.FindMateById(t.axleB)->rotation, WithinAbs(0.4, 1e-6));
  // Propagation is idempotent: the unprompted pass inside Solve moved
  // nothing back.
  CHECK_THAT(ZRotationOf(t.asmDoc, t.wheel), WithinAbs(0.4, 1e-5));
}

TEST_CASE("A rack follows its pinion, and a screw follows its own turn",
          "[assembly][relations]") {
  AssemblyDocument d;
  Occurrence base;
  base.name = "Base";
  base.partId = 1;
  base.fixed = true;
  const uint64_t baseId = d.AddOccurrence(std::move(base));
  Occurrence pin;
  pin.name = "Pinion";
  pin.partId = 2;
  const uint64_t pinId = d.AddOccurrence(std::move(pin));
  Occurrence rack;
  rack.name = "Rack";
  rack.partId = 3;
  const uint64_t rackId = d.AddOccurrence(std::move(rack));
  const uint64_t axle = d.AddMate(Revolute(baseId, pinId));
  Mate slide;
  slide.type = MateType::Slider;
  slide.a = Origin(baseId);
  slide.b = Origin(rackId);
  slide.flipped = false;
  const uint64_t slider = d.AddMate(std::move(slide));

  Relation r;
  r.kind = RelationKind::Rack;
  r.mateA = axle;
  r.mateB = slider;
  r.dofA = MateDof::Rotation;
  r.dofB = MateDof::Offset;
  r.ratio = 12.0; // pitch radius, mm per radian
  d.AddRelation(r);
  d.FindMateById(axle)->rotation = 0.5f;
  assembly::Solve(d, NoParts());
  CHECK_THAT(d.FindMateById(slider)->offset, WithinAbs(6.0, 1e-5));
  // Push the rack: the pinion turns.
  d.FindMateById(slider)->offset = 24.0f;
  assembly::PropagateRelations(d, slider);
  CHECK_THAT(d.FindMateById(axle)->rotation, WithinAbs(2.0, 1e-5));

  // A screw: one cylindrical mate, rotation driving offset.
  Occurrence nut;
  nut.name = "Nut";
  nut.partId = 4;
  const uint64_t nutId = d.AddOccurrence(std::move(nut));
  Mate cyl;
  cyl.type = MateType::Cylindrical;
  cyl.a = Origin(baseId);
  cyl.b = Origin(nutId);
  cyl.flipped = false;
  const uint64_t thread = d.AddMate(std::move(cyl));
  Relation s;
  s.kind = RelationKind::Screw;
  s.mateA = s.mateB = thread;
  s.dofA = MateDof::Rotation;
  s.dofB = MateDof::Offset;
  s.ratio = 1.25 / (2.0 * M_PI); // M8 coarse
  d.AddRelation(s);
  d.FindMateById(thread)->rotation = static_cast<float>(4.0 * M_PI); // 2 turns
  assembly::Solve(d, NoParts());
  CHECK_THAT(d.FindMateById(thread)->offset, WithinAbs(2.5, 1e-5));
}

TEST_CASE("A relation inside a loop is a row: a four-bar drops to zero DOF",
          "[assembly][relations][loops]") {
  // The parallelogram four-bar of test_assembly_loops: mobility 1, and its
  // solution family has theta4 = theta1. A gear relation theta4 = 1 * theta1
  // is consistent with it and takes the last degree of freedom away.
  const auto AtX = [](float x) {
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
  };
  const auto Rev = [](uint64_t id, uint64_t a, const glm::mat4& fa, uint64_t b,
                      const glm::mat4& fb, float seed) {
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
  };
  assembly::LoopProblem p;
  p.mates.push_back(Rev(1, 10, glm::mat4(1.0f), 20, glm::mat4(1.0f), 0.3f));
  p.mates.push_back(Rev(2, 20, AtX(40.0f), 30, glm::mat4(1.0f), 0.0f));
  p.mates.push_back(Rev(3, 30, AtX(100.0f), 40, AtX(40.0f), 0.0f));
  p.mates.push_back(Rev(4, 10, AtX(100.0f), 40, glm::mat4(1.0f), 0.0f));
  p.tree.push_back({0, 10, 20, false});
  p.tree.push_back({1, 20, 30, false});
  p.tree.push_back({2, 30, 40, false});
  p.cut.push_back(3);
  p.anchor.emplace_back(10, glm::mat4(1.0f));

  const assembly::LoopSolution free = assembly::SolveLoops(p);
  REQUIRE(free.converged);
  CHECK(free.dof == 1);

  p.relations.push_back({0, 3, MateDof::Rotation, MateDof::Rotation, 1.0, 0.0});
  const assembly::LoopSolution geared = assembly::SolveLoops(p);
  REQUIRE(geared.unavailable.empty());
  CHECK(geared.converged);
  CHECK(geared.dof == 0);
  CHECK_THAT(geared.solved[3].rotation,
             WithinAbs(geared.solved[0].rotation, 1e-4));

  // An INCONSISTENT relation (theta4 = 2 * theta1) cannot close with the
  // linkage's own family unless both angles go to zero — which they do, and
  // that is the honest answer: the loop still closes, at the one pose that
  // satisfies both.
  p.relations[0].ratio = 2.0;
  const assembly::LoopSolution forced = assembly::SolveLoops(p);
  CHECK(forced.converged);
  CHECK_THAT(forced.solved[3].rotation,
             WithinAbs(2.0 * forced.solved[0].rotation, 1e-3));
}

TEST_CASE("A relation says when its mate is gone or lacks the DOF",
          "[assembly][relations]") {
  Train t = MakeTrain();
  const uint64_t rel = t.asmDoc.AddRelation(Gear(t.axleA, t.axleB, -2.0));
  assembly::Solve(t.asmDoc, NoParts());
  CHECK(t.asmDoc.FindRelationById(rel)->error.empty());

  t.asmDoc.FindMateById(t.axleB)->type = MateType::Fastened;
  assembly::Solve(t.asmDoc, NoParts());
  CHECK(t.asmDoc.FindRelationById(rel)->error.find("degree of freedom") !=
        std::string::npos);

  REQUIRE(t.asmDoc.RemoveMate(t.axleB));
  assembly::Solve(t.asmDoc, NoParts());
  CHECK(t.asmDoc.FindRelationById(rel)->error.find("gone") !=
        std::string::npos);
  CHECK(assembly::MateHasDof(MateType::Cylindrical, MateDof::Offset));
  CHECK_FALSE(assembly::MateHasDof(MateType::Slider, MateDof::Rotation));
}

TEST_CASE("Relations round-trip through the file, and the v83 golden loads",
          "[assembly][relations][golden]") {
  Train t = MakeTrain();
  t.asmDoc.AddRelation(Gear(t.axleA, t.axleB, -1.0 / 3.0, 0.25));
  const fs::path tmp = fs::temp_directory_path() / "relations_rt.json";
  REQUIRE(DocumentIO::SaveAssembly(t.asmDoc, tmp.string()).has_value());
  auto back = DocumentIO::LoadAssembly(tmp.string());
  REQUIRE(back.has_value());
  REQUIRE((*back)->RelationCount() == 1);
  const Relation& r = (*back)->GetRelations()[0];
  CHECK(r.kind == RelationKind::Gear);
  CHECK(r.mateA == t.axleA);
  CHECK(r.mateB == t.axleB);
  CHECK_THAT(r.ratio, WithinAbs(-1.0 / 3.0, 1e-12));
  CHECK_THAT(r.phase, WithinAbs(0.25, 1e-12));
  // The next id is pinned past the relation's, like mates and views: a
  // relation added after the load does not reuse it.
  const uint64_t relId = r.id;
  Relation more;
  more.mateA = t.axleA;
  more.mateB = t.axleB;
  more.ratio = -2.0;
  CHECK((*back)->AddRelation(more) > relId);
  fs::remove(tmp);

  // An unknown kind is refused, not demoted.
  CHECK_THROWS_WITH(DocumentIO::RelationKindFromString("gears"),
                    Catch::Matchers::ContainsSubstring("gear, rack, screw"));
  CHECK_THROWS(DocumentIO::MateDofFromString("twist"));

  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) /
                          "tests/golden/assembly/v83-gear-relation.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->RelationCount() == 1);
  assembly::Solve(**g, NoParts());
  CHECK((*g)->GetRelations()[0].error.empty());
  // Pinion at 0.5 rad drives the wheel to -0.5/3.
  CHECK_THAT((*g)->FindMateById(5)->rotation, WithinAbs(-0.5 / 3.0, 1e-5));
}
