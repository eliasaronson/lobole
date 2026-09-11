// Relations: gear, rack, screw, linear (docs/assemblies/relations-plan.md).
//
// A relation couples two mates' degrees of freedom, dofB = ratio * dofA +
// phase. In a mate tree it is enforced by propagation before the closed-form
// placement; inside a loop it is one residual row beside the six per cut
// mate. Both halves are pinned here, and so is the document round trip.

#include "core/Angles.h"
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

namespace {
// A fixed carrier and three more occurrences, each on the mate kind asked
// for, mated by origin.
struct Rig {
  AssemblyDocument d;
  uint64_t carrier = 0;
  uint64_t Add(const char* name, uint64_t partId, MateType type) {
    Occurrence o;
    o.name = name;
    o.partId = partId;
    const uint64_t occ = d.AddOccurrence(std::move(o));
    Mate m;
    m.type = type;
    m.a = Origin(carrier);
    m.b = Origin(occ);
    m.flipped = false;
    return d.AddMate(std::move(m));
  }
};
Rig MakeRig() {
  Rig r;
  Occurrence c;
  c.name = "Carrier";
  c.partId = 1;
  c.fixed = true;
  r.carrier = r.d.AddOccurrence(std::move(c));
  return r;
}
} // namespace

TEST_CASE("A cylindrical mate takes its turn from one relation and its slide "
          "from another",
          "[assembly][relations]") {
  // The watch's clutch: turned by its stem, slid by its yoke. The solve used
  // to settle a whole MATE the first time a relation reached it, so the
  // second relation was accepted by add_relation and never applied — the
  // clutch never slid.
  Rig r = MakeRig();
  const uint64_t stem = r.Add("Stem", 2, MateType::Revolute);
  const uint64_t yoke = r.Add("Yoke", 3, MateType::Slider);
  const uint64_t clutch = r.Add("Clutch", 4, MateType::Cylindrical);
  const uint64_t turn = r.d.AddRelation(Gear(stem, clutch, -2.0));
  Relation slide;
  slide.kind = RelationKind::Linear;
  slide.mateA = yoke;
  slide.mateB = clutch;
  slide.dofA = slide.dofB = MateDof::Offset;
  slide.ratio = 0.5;
  const uint64_t push = r.d.AddRelation(slide);

  r.d.FindMateById(stem)->rotation = 0.3f;
  r.d.FindMateById(yoke)->offset = 4.0f;
  assembly::Solve(r.d, NoParts());
  CHECK(r.d.FindRelationById(turn)->error.empty());
  CHECK(r.d.FindRelationById(push)->error.empty());
  CHECK_THAT(r.d.FindMateById(clutch)->rotation, WithinAbs(-0.6, 1e-6));
  CHECK_THAT(r.d.FindMateById(clutch)->offset, WithinAbs(2.0, 1e-6));
}

TEST_CASE("A plain solve applies a chain of relations end to end",
          "[assembly][relations]") {
  // With nothing driven, the solve seeded every relation's A at once and
  // marked each settled — so a mate that is both a follower and a driver
  // (the middle one here) was never written, and a chain held at its ends
  // only. A watch's keyless works sat off their relations' phases after a
  // solve until something drove the stem.
  Rig r = MakeRig();
  const uint64_t a = r.Add("A", 2, MateType::Revolute);
  const uint64_t b = r.Add("B", 3, MateType::Revolute);
  const uint64_t c = r.Add("C", 4, MateType::Revolute);
  r.d.AddRelation(Gear(a, b, -2.0));
  r.d.AddRelation(Gear(b, c, -3.0, 0.05));
  r.d.FindMateById(a)->rotation = 0.1f;
  assembly::Solve(r.d, NoParts());
  CHECK_THAT(r.d.FindMateById(b)->rotation, WithinAbs(-0.2, 1e-6));
  CHECK_THAT(r.d.FindMateById(c)->rotation, WithinAbs(0.65, 1e-6));
  for (const Relation& rel : r.d.GetRelations())
    CHECK(rel.error.empty());
}

TEST_CASE("Relations that disagree on one degree of freedom are reported, "
          "not skipped",
          "[assembly][relations]") {
  // A drives B (x2) and C (x5), and B drives C (x3): C is asked to be 0.5
  // and 0.6 at once. The walk reaches C first from A and then skips B's
  // relation — which used to be the end of it, leaving the document claiming
  // a coupling the model lacked. (Two relations INTO one free mate are not a
  // conflict: the walk drives the free one backwards to satisfy both.)
  Rig r = MakeRig();
  const uint64_t a = r.Add("A", 2, MateType::Revolute);
  const uint64_t b = r.Add("B", 3, MateType::Revolute);
  const uint64_t c = r.Add("C", 4, MateType::Revolute);
  r.d.AddRelation(Gear(a, b, 2.0));
  r.d.AddRelation(Gear(b, c, 3.0));
  r.d.AddRelation(Gear(a, c, 5.0));
  r.d.FindMateById(a)->rotation = 0.1f;
  const assembly::SolveReport rep = assembly::Solve(r.d, NoParts());
  int unsatisfied = 0;
  for (const Relation& rel : r.d.GetRelations())
    if (rel.error.find("not satisfied") != std::string::npos)
      unsatisfied++;
  CHECK(unsatisfied == 1);
  CHECK(rep.erroredMates >= 1);
}

TEST_CASE("A configuration suppresses mates, relations and occurrences and "
          "holds a mate's DOF — as an overlay",
          "[assembly][relations][configurations]") {
  // A watch pulls its crown out: the clutch leaves one wheel for another and
  // the stem stands a millimetre further out. As a configuration that is a
  // relation out of the solve and a DOF held — and pushing the crown back in
  // must restore every authored value, so the hold is applied by the solve,
  // never written into the mate.
  Rig r = MakeRig();
  const uint64_t a = r.Add("A", 2, MateType::Revolute);
  const uint64_t b = r.Add("B", 3, MateType::Revolute);
  const uint64_t c = r.Add("C", 4, MateType::Cylindrical);
  const uint64_t ab = r.d.AddRelation(Gear(a, b, -2.0));
  const uint64_t ac = r.d.AddRelation(Gear(a, c, 1.0));
  r.d.FindMateById(a)->rotation = 0.2f;
  assembly::Solve(r.d, NoParts());
  REQUIRE_THAT(r.d.FindMateById(b)->rotation, WithinAbs(-0.4, 1e-6));
  REQUIRE_THAT(r.d.FindMateById(c)->rotation, WithinAbs(0.2, 1e-6));
  const uint64_t occA = r.d.FindMateById(a)->b.occurrenceId;
  const uint64_t occC = r.d.FindMateById(c)->b.occurrenceId;

  Configuration& cfg = r.d.AddConfiguration("Held");
  const uint64_t cfgId = cfg.id;
  cfg.suppressedRelations = {ab};
  cfg.mateOverrides = {{a, 0.5f, std::nullopt}, {c, std::nullopt, 3.0f}};
  REQUIRE(r.d.SetActiveConfiguration(cfgId));
  assembly::Solve(r.d, NoParts());
  // The hold drives: A stands at 0.5 for the placement and for its relation
  // to C, and C is held 3 mm out.
  CHECK_THAT(ZRotationOf(r.d, occA), WithinAbs(0.5, 1e-5));
  CHECK_THAT(r.d.FindMateById(c)->rotation, WithinAbs(0.5, 1e-6));
  CHECK_THAT(r.d.FindById(occC)->position.z, WithinAbs(3.0, 1e-5));
  // ...the suppressed relation is out, not broken...
  CHECK(r.d.FindRelationById(ab)->error.empty());
  CHECK_THAT(r.d.FindMateById(b)->rotation, WithinAbs(-0.4, 1e-6));
  // ...and nothing was written into the mates.
  CHECK_THAT(r.d.FindMateById(a)->rotation, WithinAbs(0.2, 1e-6));
  CHECK_THAT(r.d.FindMateById(c)->offset, WithinAbs(0.0, 1e-6));

  // Back to base: every authored value is where it was.
  r.d.SetActiveConfiguration(0);
  assembly::Solve(r.d, NoParts());
  CHECK_THAT(ZRotationOf(r.d, occA), WithinAbs(0.2, 1e-5));
  CHECK_THAT(r.d.FindMateById(c)->rotation, WithinAbs(0.2, 1e-6));
  CHECK_THAT(r.d.FindById(occC)->position.z, WithinAbs(0.0, 1e-5));

  // A hidden occurrence takes its mate and relations out with it: they used
  // to go on driving it.
  r.d.FindConfiguration(cfgId)->mateOverrides.clear();
  r.d.FindConfiguration(cfgId)->suppressedRelations.clear();
  r.d.FindConfiguration(cfgId)->suppressedFeatures = {occC};
  r.d.SetActiveConfiguration(cfgId);
  CHECK(r.d.MateSuppressed(c));
  CHECK(r.d.RelationSuppressed(ac));
  r.d.FindMateById(a)->rotation = 0.3f;
  assembly::Solve(r.d, NoParts());
  CHECK_THAT(r.d.FindMateById(c)->rotation, WithinAbs(0.2, 1e-6)); // untouched
  CHECK(r.d.FindRelationById(ac)->error.empty());
}

TEST_CASE("Configuration overlays round-trip, and the v101 golden loads",
          "[assembly][relations][configurations][golden]") {
  const fs::path golden =
      fs::path(DESIGNER_SOURCE_DIR) /
      "tests/golden/assembly/v101-configuration-overlay.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  AssemblyDocument& d = **g;
  REQUIRE(d.GetConfigurations().size() == 1);
  const Configuration& c = d.GetConfigurations()[0];
  CHECK(d.ActiveConfiguration() == c.id);
  CHECK(c.suppressedMates == std::vector<uint64_t>{5});
  CHECK(c.suppressedRelations == std::vector<uint64_t>{6});
  REQUIRE(c.mateOverrides.size() == 2);
  REQUIRE(c.mateOverrides[0].rotation.has_value());
  CHECK_THAT(*c.mateOverrides[0].rotation, WithinAbs(0.5, 1e-6));
  CHECK_FALSE(c.mateOverrides[0].offset.has_value());
  REQUIRE(c.mateOverrides[1].offset.has_value());
  CHECK_THAT(*c.mateOverrides[1].offset, WithinAbs(2.0, 1e-6));
  CHECK(d.MateSuppressed(5));
  CHECK(d.RelationSuppressed(6));
  // Active on load: the held pinion axle drives the placement, and the
  // suppressed relation is out rather than an error.
  assembly::Solve(d, NoParts());
  CHECK(d.GetRelations()[0].error.empty());
  CHECK_THAT(ZRotationOf(d, 2), WithinAbs(0.5, 1e-5));

  const fs::path out =
      fs::temp_directory_path() / "designer_config_overlay_rt.json";
  REQUIRE(DocumentIO::SaveAssembly(d, out.string()).has_value());
  auto back = DocumentIO::LoadAssembly(out.string());
  REQUIRE(back.has_value());
  const Configuration& bc = (*back)->GetConfigurations()[0];
  CHECK(bc.mateOverrides.size() == 2);
  CHECK(bc.suppressedMates == std::vector<uint64_t>{5});
  CHECK(bc.suppressedRelations == std::vector<uint64_t>{6});
  fs::remove(out);
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

// --- chain and cam (v88, docs/agent-scale-plan.md G4) -----------------------

TEST_CASE("A chain keeps the sign a gear reverses", "[assembly][relations]") {
  Train t = MakeTrain();
  Relation gear;
  gear.kind = RelationKind::Gear;
  gear.mateA = t.axleA;
  gear.mateB = t.axleB;
  gear.ratio = -12.0 / 36.0;
  const uint64_t gearId = t.asmDoc.AddRelation(gear);
  assembly::SetMateDof(*t.asmDoc.FindMateById(t.axleA), MateDof::Rotation, 0.6);
  assembly::PropagateRelations(t.asmDoc, t.axleA);
  CHECK_THAT(t.asmDoc.FindMateById(t.axleB)->rotation,
             WithinAbs(-0.2f, 1e-5f)); // meshing teeth counter-rotate

  // The same two sprockets on a chain turn the SAME way. Expressing that as
  // a gear with a negated tooth count works and makes the sign something to
  // remember rather than something to say.
  t.asmDoc.RemoveRelation(gearId);
  Relation chain;
  chain.kind = RelationKind::Chain;
  chain.mateA = t.axleA;
  chain.mateB = t.axleB;
  chain.ratio = 12.0 / 36.0;
  t.asmDoc.AddRelation(chain);
  assembly::PropagateRelations(t.asmDoc, t.axleA);
  CHECK_THAT(t.asmDoc.FindMateById(t.axleB)->rotation, WithinAbs(0.2f, 1e-5f));
}

TEST_CASE("The DRIVEN mate's chain settles before any other source",
          "[assembly][relations][cam]") {
  // A valve gear: a camshaft on a chain off the crank, a cam relation
  // lifting the valve's slide, and a rack that makes the ROCKER follow that
  // slide — backwards, which is the only way round a cam can be read.
  //
  // The rack's A is the rocker, and the rocker is free. Seeding every
  // relation's A at once and letting the walk order decide let the rack win
  // the race for the slide and write it from the rocker's LAST value, which
  // fed back on itself: an 8 mm cam lifted its valve 60 mm.
  Document empty;
  AssemblyDocument d;
  Occurrence head;
  head.name = "Head";
  head.partId = 1;
  head.fixed = true;
  const uint64_t headId = d.AddOccurrence(std::move(head));
  const auto add = [&](const char* name, uint64_t partId) {
    Occurrence o;
    o.name = name;
    o.partId = partId;
    return d.AddOccurrence(std::move(o));
  };
  const uint64_t crankId = add("Crank", 2), camId = add("Cam", 3);
  const uint64_t rockerId = add("Rocker", 4), valveId = add("Valve", 5);
  const uint64_t crank = d.AddMate(Revolute(headId, crankId));
  const uint64_t cam = d.AddMate(Revolute(headId, camId));
  const uint64_t rocker = d.AddMate(Revolute(headId, rockerId));
  Mate slide;
  slide.type = MateType::Slider;
  slide.a = Origin(headId);
  slide.b = Origin(valveId);
  slide.flipped = false;
  const uint64_t valve = d.AddMate(std::move(slide));

  Relation chain;
  chain.kind = RelationKind::Chain;
  chain.mateA = crank;
  chain.mateB = cam;
  chain.dofA = chain.dofB = MateDof::Rotation;
  chain.ratio = 0.5;
  d.AddRelation(chain);
  Relation lobe;
  lobe.kind = RelationKind::Cam;
  lobe.mateA = cam;
  lobe.mateB = valve;
  lobe.dofA = MateDof::Rotation;
  lobe.dofB = MateDof::Offset;
  for (int i = 0; i <= 18; i++) {
    const double f = i / 18.0;
    lobe.lift.emplace_back(f * 180.0,
                           8.0 * 0.5 * (1.0 - std::cos(2.0 * M_PI * f)));
  }
  lobe.lift.emplace_back(270.0, 0.0);
  d.AddRelation(lobe);
  Relation follow;
  follow.kind = RelationKind::Rack;
  follow.mateA = rocker;
  follow.mateB = valve;
  follow.dofA = MateDof::Rotation;
  follow.dofB = MateDof::Offset;
  follow.ratio = 50.0;
  d.AddRelation(follow);

  // Two turns of the crank, the way a motion study drives one.
  for (int step = 0; step <= 24; step++) {
    const double deg = step * 30.0;
    d.FindMateById(crank)->rotation = static_cast<float>(deg * M_PI / 180.0);
    assembly::Solve(d, NoParts(), nullptr, {crank, MateDof::Rotation});
    const double lift = d.FindMateById(valve)->offset;
    INFO("crank " << deg << " deg");
    CHECK(lift >= -1e-4);
    CHECK(lift <= 8.0 + 1e-4);
    // ...and the rocker follows the valve, not the other way about.
    CHECK_THAT(d.FindMateById(rocker)->rotation * 50.0, WithinAbs(lift, 1e-3));
  }
  // The peak: the cam turns at half crank speed, so 90 of cam is 180 of
  // crank, and that is where an 8 mm lobe is 8 mm up.
  d.FindMateById(crank)->rotation = static_cast<float>(M_PI);
  assembly::Solve(d, NoParts(), nullptr, {crank, MateDof::Rotation});
  CHECK_THAT(d.FindMateById(valve)->offset, WithinAbs(8.0, 1e-3));
}

TEST_CASE("A cam lifts its follower by a table, not a ratio",
          "[assembly][relations][cam]") {
  // A 6 mm lobe rising over the first 180 degrees and dwelling after.
  std::vector<std::pair<double, double>> lobe;
  for (int i = 0; i <= 18; i++) {
    const double f = i / 18.0;
    lobe.emplace_back(f * 180.0, 6.0 * 0.5 * (1.0 - std::cos(2.0 * M_PI * f)));
  }
  lobe.emplace_back(270.0, 0.0); // the dwell

  SECTION("the profile reads back where it was given") {
    CHECK_THAT(CamLift(lobe, 0.0), WithinAbs(0.0, 1e-9));
    CHECK_THAT(CamLift(lobe, M_PI / 2.0), WithinAbs(6.0, 1e-6)); // peak at 90
    CHECK_THAT(CamLift(lobe, M_PI), WithinAbs(0.0, 1e-6));       // closed
    CHECK_THAT(CamLift(lobe, 3.0 * M_PI / 2.0), WithinAbs(0.0, 1e-6));
    // Periodic: a crank past a full turn sees the profile again.
    CHECK_THAT(CamLift(lobe, 2.0 * M_PI + M_PI / 2.0), WithinAbs(6.0, 1e-6));
    CHECK_THAT(CamLift(lobe, -3.0 * M_PI / 2.0), WithinAbs(6.0, 1e-6));
  }
  SECTION("the slope is continuous — what the loop solver differentiates") {
    // Across a table point (10 degrees), the two one-sided slopes agree. A
    // polyline would step here, and the numeric Jacobian would step with it.
    double before = 0.0, after = 0.0;
    const double a = 10.0 * M_PI / 180.0;
    CamLift(lobe, a - 1e-4, &before);
    CamLift(lobe, a + 1e-4, &after);
    CHECK_THAT(after, WithinAbs(before, 0.05));
    // And it matches the profile's own derivative there.
    const double numeric =
        (CamLift(lobe, a + 1e-5) - CamLift(lobe, a - 1e-5)) / 2e-5;
    CHECK_THAT(before, WithinAbs(numeric, 0.02));
  }
  SECTION("a lobe that does not begin at zero is still shut before it") {
    // The engine's own case: a lobe phased to open at 74 degrees, whose
    // table therefore BEGINS at 74 and whose flat span wraps past the end of
    // the turn. Asking below the first angle is not asking before the start;
    // it is asking inside that wrapped span. Read as "before the table", the
    // Hermite cubic extrapolated backwards out of the interval its own
    // endpoints define and answered 60 mm of lift on an 8 mm lobe.
    std::vector<std::pair<double, double>> phased;
    for (int i = 0; i <= 18; i++) {
      const double f = i / 18.0;
      phased.emplace_back(74.0 + f * 112.0,
                          8.0 * 0.5 * (1.0 - std::cos(2.0 * M_PI * f)));
    }
    phased.emplace_back(310.0, 0.0); // the dwell, in the middle of the rest
    const auto at = [&](double deg) {
      return CamLift(phased, deg * M_PI / 180.0);
    };
    CHECK_THAT(at(0.0), WithinAbs(0.0, 1e-6));
    CHECK_THAT(at(74.0), WithinAbs(0.0, 1e-6));
    CHECK_THAT(at(130.0), WithinAbs(8.0, 1e-6)); // the peak, 56 in
    CHECK_THAT(at(186.0), WithinAbs(0.0, 1e-6));
    for (double deg = 0.0; deg < 360.0; deg += 1.0) {
      CHECK(at(deg) >= -1e-6);
      CHECK(at(deg) <= 8.0 + 1e-6);
    }
  }
  SECTION("an empty or one-point table is flat, never a crash") {
    CHECK_THAT(CamLift({}, 1.0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(CamLift({{0.0, 3.0}}, 1.0), WithinAbs(3.0, 1e-12));
  }
}

TEST_CASE("A cam drives its follower forward and refuses to be driven back",
          "[assembly][relations][cam]") {
  // The wheel's revolute becomes a SLIDER so it has an offset to lift.
  Train t = MakeTrain();
  t.asmDoc.FindMateById(t.axleB)->type = MateType::Slider;
  Relation cam;
  cam.kind = RelationKind::Cam;
  cam.mateA = t.axleA;
  cam.mateB = t.axleB;
  cam.dofA = MateDof::Rotation;
  cam.dofB = MateDof::Offset;
  cam.ratio = 1.0;
  for (int i = 0; i <= 18; i++) {
    const double f = i / 18.0;
    cam.lift.emplace_back(f * 180.0,
                          6.0 * 0.5 * (1.0 - std::cos(2.0 * M_PI * f)));
  }
  cam.lift.emplace_back(270.0, 0.0);
  t.asmDoc.AddRelation(cam);

  // Forward: the camshaft at 90 degrees lifts the follower to the peak.
  assembly::SetMateDof(*t.asmDoc.FindMateById(t.axleA), MateDof::Rotation,
                       M_PI / 2.0);
  assembly::PropagateRelations(t.asmDoc, t.axleA);
  CHECK_THAT(t.asmDoc.FindMateById(t.axleB)->offset, WithinAbs(6.0f, 1e-4f));
  assembly::SetMateDof(*t.asmDoc.FindMateById(t.axleA), MateDof::Rotation,
                       M_PI);
  assembly::PropagateRelations(t.asmDoc, t.axleA);
  CHECK_THAT(t.asmDoc.FindMateById(t.axleB)->offset, WithinAbs(0.0f, 1e-4f));

  // Backwards: pushing the FOLLOWER does not turn the camshaft. Many angles
  // give one lift, so there is no answer to give and a plausible one would
  // be a guess.
  const float camAngle = t.asmDoc.FindMateById(t.axleA)->rotation;
  assembly::SetMateDof(*t.asmDoc.FindMateById(t.axleB), MateDof::Offset, 3.0);
  assembly::PropagateRelations(t.asmDoc, t.axleB);
  CHECK_THAT(t.asmDoc.FindMateById(t.axleA)->rotation,
             WithinAbs(camAngle, 1e-6f));

  // A cam with no table is reported, not silently flat.
  t.asmDoc.GetRelations()[0].lift.clear();
  assembly::PropagateRelations(t.asmDoc, t.axleA);
  CHECK_THAT(t.asmDoc.GetRelations()[0].error,
             Catch::Matchers::ContainsSubstring("lift table"));
}

TEST_CASE("A cam turns a revolute follower by its table's DEGREES",
          "[assembly][relations][cam]") {
  // v102: a rocker, a pallet fork. The table's second column is in the
  // follower's unit, so a turning follower's lift is degrees and the mate's
  // rotation is the radians it converts to.
  Train t = MakeTrain(); // both axles revolute
  Relation rocker;
  rocker.kind = RelationKind::Cam;
  rocker.mateA = t.axleA;
  rocker.mateB = t.axleB;
  rocker.dofA = MateDof::Rotation;
  rocker.dofB = MateDof::Rotation;
  rocker.ratio = 1.0;
  for (int i = 0; i <= 18; i++) {
    const double f = i / 18.0;
    rocker.lift.emplace_back(f * 180.0,
                             30.0 * 0.5 * (1.0 - std::cos(2.0 * M_PI * f)));
  }
  rocker.lift.emplace_back(270.0, 0.0);
  t.asmDoc.AddRelation(rocker);
  assembly::SetMateDof(*t.asmDoc.FindMateById(t.axleA), MateDof::Rotation,
                       M_PI / 2.0);
  assembly::PropagateRelations(t.asmDoc, t.axleA);
  CHECK(t.asmDoc.GetRelations()[0].error.empty());
  CHECK_THAT(t.asmDoc.FindMateById(t.axleB)->rotation,
             WithinAbs(float(angles::ToRadians(30.0)), 1e-5f));

  // Inside a loop the relation is a row, and the row reads the table the
  // same way. The parallelogram four-bar holds theta4 = theta1; a cam whose
  // table is a flat 20 degrees pins theta4 there, so the linkage closes at
  // theta1 = 20 degrees too — in radians, 0.349, and not at 20 rad.
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
  p.relations.push_back({0,
                         3,
                         MateDof::Rotation,
                         MateDof::Rotation,
                         1.0,
                         0.0,
                         {{0.0, 20.0}, {180.0, 20.0}}});
  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.unavailable.empty());
  CHECK(s.converged);
  CHECK_THAT(s.solved[3].rotation, WithinAbs(angles::ToRadians(20.0), 1e-4));
  CHECK_THAT(s.solved[0].rotation, WithinAbs(angles::ToRadians(20.0), 1e-3));
}

TEST_CASE("A turning cam round-trips, and the v102 golden drives",
          "[assembly][relations][cam][golden]") {
  Train t = MakeTrain();
  Relation rocker;
  rocker.kind = RelationKind::Cam;
  rocker.name = "pallet fork";
  rocker.mateA = t.axleA;
  rocker.mateB = t.axleB;
  rocker.dofA = MateDof::Rotation;
  rocker.dofB = MateDof::Rotation;
  rocker.ratio = 1.0;
  rocker.lift = {{0.0, -10.0}, {90.0, 10.0}, {180.0, 10.0}, {270.0, -10.0}};
  t.asmDoc.AddRelation(rocker);
  if (const char* out = std::getenv("DESIGNER_WRITE_GOLDEN"))
    REQUIRE(DocumentIO::SaveAssembly(t.asmDoc, out).has_value());

  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) /
                          "tests/golden/assembly/v102-rotating-cam.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->RelationCount() == 1);
  const Relation& r = (*g)->GetRelations()[0];
  CHECK(r.kind == RelationKind::Cam);
  CHECK(r.dofB == MateDof::Rotation);
  // The table is stored as authored — degrees — and not pre-converted.
  CHECK_THAT(r.lift[1].second, WithinAbs(10.0, 1e-12));
  assembly::SetMateDof(*(*g)->FindMateById(t.axleA), MateDof::Rotation,
                       M_PI / 2.0);
  assembly::PropagateRelations(**g, t.axleA);
  CHECK_THAT((*g)->FindMateById(t.axleB)->rotation,
             WithinAbs(float(angles::ToRadians(10.0)), 1e-5f));
}

TEST_CASE("Chain and cam round-trip, and the v88 golden loads",
          "[assembly][relations][golden]") {
  Train t = MakeTrain();
  t.asmDoc.FindMateById(t.axleB)->type = MateType::Cylindrical;
  Relation chain;
  chain.kind = RelationKind::Chain;
  chain.name = "timing chain";
  chain.mateA = t.axleA;
  chain.mateB = t.axleB;
  chain.ratio = 1.0 / 2.0; // a camshaft at half crank speed
  t.asmDoc.AddRelation(chain);
  Relation cam;
  cam.kind = RelationKind::Cam;
  cam.name = "inlet lobe";
  cam.mateA = t.axleB;
  cam.mateB = t.axleB; // the cylindrical mate's own offset
  cam.dofA = MateDof::Rotation;
  cam.dofB = MateDof::Offset;
  cam.ratio = 1.0;
  cam.lift = {{0.0, 0.0},   {45.0, 4.0},  {90.0, 8.0},
              {135.0, 4.0}, {180.0, 0.0}, {270.0, 0.0}};
  t.asmDoc.AddRelation(cam);

  const fs::path tmp = fs::temp_directory_path() / "relations_v88.json";
  REQUIRE(DocumentIO::SaveAssembly(t.asmDoc, tmp.string()).has_value());
  auto back = DocumentIO::LoadAssembly(tmp.string());
  REQUIRE(back.has_value());
  REQUIRE((*back)->RelationCount() == 2);
  CHECK((*back)->GetRelations()[0].kind == RelationKind::Chain);
  CHECK_THAT((*back)->GetRelations()[0].ratio, WithinAbs(0.5, 1e-12));
  const Relation& rc = (*back)->GetRelations()[1];
  CHECK(rc.kind == RelationKind::Cam);
  REQUIRE(rc.lift.size() == 6);
  CHECK_THAT(rc.lift[2].second, WithinAbs(8.0, 1e-12));
  // A gear's `lift` is absent, not an empty array.
  CHECK_FALSE(
      DocumentIO::RelationToJson((*back)->GetRelations()[0]).contains("lift"));
  fs::remove(tmp);

  // Unknown kinds are still refused, and now name the two new ones.
  CHECK_THROWS_WITH(DocumentIO::RelationKindFromString("belt"),
                    Catch::Matchers::ContainsSubstring("chain, cam"));

  if (const char* out = std::getenv("DESIGNER_WRITE_GOLDEN"))
    REQUIRE(DocumentIO::SaveAssembly(t.asmDoc, out).has_value());
  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) /
                          "tests/golden/assembly/v88-chain-and-cam.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->RelationCount() == 2);
  // ...and it still DRIVES: the crank at 180 puts the camshaft at 90, which
  // is the lobe's peak.
  assembly::SetMateDof(*(*g)->FindMateById(4), MateDof::Rotation, M_PI);
  assembly::PropagateRelations(**g, 4);
  CHECK_THAT((*g)->FindMateById(5)->rotation,
             WithinAbs(float(M_PI / 2), 1e-5f));
  CHECK_THAT((*g)->FindMateById(5)->offset, WithinAbs(8.0f, 1e-3f));
}

TEST_CASE("A cam profile never overshoots its own table",
          "[assembly][relations][cam]") {
  // The single-cylinder engine's finding: a Catmull-Rom through a 9 mm lobe
  // with a long dwell dipped 1.86 mm BELOW zero between the lobe closing and
  // reopening, so a closed valve travelled backwards into the head and the
  // motion study reported 10.86 mm of lift on a 9 mm cam. A lift table is
  // data; an interpolant that leaves the interval its endpoints define is
  // inventing motion the cam does not have.
  std::vector<std::pair<double, double>> lobe;
  const double rise = 9.0, duration = 150.0, start = 20.0;
  for (int i = 0; i < 18; i++) {
    const double f = i / 18.0;
    lobe.emplace_back(start + f * duration,
                      rise * 0.5 * (1.0 - std::cos(2.0 * M_PI * f)));
  }
  lobe.emplace_back(start + duration + (360.0 - duration) * 0.5, 0.0);

  double lo = 1e9, hi = -1e9;
  for (int deg = 0; deg < 3600; deg++) {
    const double v = CamLift(lobe, deg * 0.1 * M_PI / 180.0);
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  INFO("profile ranges " << lo << " .. " << hi << " on a " << rise
                         << " mm lobe");
  CHECK(lo >= -1e-6);       // a closed valve does not lift backwards
  CHECK(hi <= rise + 1e-6); // and an open one does not exceed its lobe

  // Still smooth: the slope is continuous across a table point, which is
  // what the loop solver differentiates.
  const double a = lobe[3].first * M_PI / 180.0;
  double before = 0.0, after = 0.0;
  CamLift(lobe, a - 1e-4, &before);
  CamLift(lobe, a + 1e-4, &after);
  CHECK_THAT(after, WithinAbs(before, 0.05));
}
