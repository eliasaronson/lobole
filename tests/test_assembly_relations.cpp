// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Relations: gear, rack, screw, linear (docs/assemblies/relations.md).
//
// A relation couples two mates' degrees of freedom, dofB = ratio * dofA +
// phase. In a mate tree it is enforced by propagation before the closed-form
// placement; inside a loop it is one residual row beside the six per cut
// mate. Both halves are pinned here, and so is the document round trip.

#include "TestPaths.h"
#include "core/Angles.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/AssemblyLoopSolve.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/features/ThreadFeature.h"
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
  return [](uint64_t, uint64_t) -> const Document* { return &empty; };
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
    if (rel.error.find("not satisfied") != std::string::npos) {
      unsatisfied++;
      // C has two drivers, and the message says so: only then.
      CHECK_THAT(rel.error, Catch::Matchers::ContainsSubstring(
                                "another relation drives it too"));
    }
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
      fs::path(TestSourceDir()) /
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

// A screw backed by a Thread feature reads the pitch off it. A printed
// thread is always a CUSTOM one (PrintedThreadFor says to model it so), so a
// custom thread's own pitch is taken -- the relation used to refuse every
// thread the tables do not carry -- and a left-hand thread runs the other
// way for the same turn.
TEST_CASE("A thread-backed screw takes a custom thread's pitch and hand",
          "[assembly][relations][thread]") {
  Document part;
  part.AddFeature(std::make_unique<CylinderFeature>(
      "Neck", glm::dvec3(0.0), glm::dvec3(0.0, 0.0, 1.0), 30.0, 10.0));
  const Body& body = part.GetBodies().at(0);
  std::string face;
  for (const auto& [index, name] : body.elementMap.names) {
    const auto f = Measure::DescribeFaceAt(body, index);
    if (f && f->kind == "cylinder")
      face = name;
  }
  REQUIRE(!face.empty());
  auto th = std::make_unique<ThreadFeature>(
      "Jar", ElementRef{body.createdBy, face}, "jar thread", 4.0);
  th->flankDeg = 45.0;
  part.AddFeature(std::move(th));
  auto* thread = dynamic_cast<ThreadFeature*>(part.GetFeature(1));
  REQUIRE(thread->GetError().empty());
  const assembly::PartResolver parts =
      [&](uint64_t, uint64_t) -> const Document* { return &part; };

  AssemblyDocument d;
  Occurrence jar;
  jar.name = "Jar";
  jar.partId = 1;
  jar.fixed = true;
  const uint64_t jarId = d.AddOccurrence(std::move(jar));
  Occurrence lid;
  lid.name = "Lid";
  lid.partId = 1;
  const uint64_t lidId = d.AddOccurrence(std::move(lid));
  Mate cyl;
  cyl.type = MateType::Cylindrical;
  cyl.a = Origin(jarId);
  cyl.b = Origin(lidId);
  cyl.flipped = false;
  const uint64_t screw = d.AddMate(std::move(cyl));
  Relation s;
  s.kind = RelationKind::Screw;
  s.mateA = s.mateB = screw;
  s.dofA = MateDof::Rotation;
  s.dofB = MateDof::Offset;
  s.threadPart = 1;
  s.threadFeature = thread->GetId();
  const uint64_t rel = d.AddRelation(s);
  d.FindMateById(screw)->rotation = static_cast<float>(3.0 * M_PI); // 1.5
  assembly::Solve(d, parts);
  CHECK(d.FindRelationById(rel)->error.empty());
  CHECK_THAT(d.FindMateById(screw)->offset, WithinAbs(6.0, 1e-4));

  thread->rightHanded = false;
  part.Regenerate();
  assembly::Solve(d, parts);
  CHECK(d.FindRelationById(rel)->error.empty());
  CHECK_THAT(d.FindMateById(screw)->offset, WithinAbs(-6.0, 1e-4));
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

  const fs::path golden = fs::path(TestSourceDir()) /
                          "tests/golden/assembly/v83-gear-relation.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->RelationCount() == 1);
  assembly::Solve(**g, NoParts());
  CHECK((*g)->GetRelations()[0].error.empty());
  // Pinion at 0.5 rad drives the wheel to -0.5/3.
  CHECK_THAT((*g)->FindMateById(5)->rotation, WithinAbs(-0.5 / 3.0, 1e-5));
}

// --- chain and cam (v88, docs/assemblies/relations.md#kinds) -----------------

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
  if (const char* out = std::getenv("LOBOLE_WRITE_GOLDEN"))
    REQUIRE(DocumentIO::SaveAssembly(t.asmDoc, out).has_value());

  const fs::path golden = fs::path(TestSourceDir()) /
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

// A freewheel on the two-axle train (docs/assemblies/relations.md#freewheel).
// Drives the named axle and propagates from it, as set_mate does.
struct Freewheel {
  Train t = MakeTrain();
  explicit Freewheel(int drives) {
    Relation fw;
    fw.kind = RelationKind::Freewheel;
    fw.mateA = t.axleA;
    fw.mateB = t.axleB;
    fw.dofA = fw.dofB = MateDof::Rotation;
    fw.ratio = 1.0;
    fw.drives = drives;
    t.asmDoc.AddRelation(fw);
  }
  float a() { return t.asmDoc.FindMateById(t.axleA)->rotation; }
  float b() { return t.asmDoc.FindMateById(t.axleB)->rotation; }
  double lead() { return t.asmDoc.GetRelations()[0].lead; }
  void drive(uint64_t mate, double v) {
    assembly::SetMateDof(*t.asmDoc.FindMateById(mate), MateDof::Rotation, v);
    assembly::PropagateRelations(t.asmDoc, mate);
  }
};

TEST_CASE("A freewheel drives one way and slips the other",
          "[assembly][relations][freewheel]") {
  Freewheel f(+1);
  // The input forward: engaged, the output follows.
  f.drive(f.t.axleA, 0.5);
  CHECK_THAT(f.b(), WithinAbs(0.5f, 1e-5f));
  CHECK_THAT(f.lead(), WithinAbs(0.0, 1e-6));
  // The input back: it slips. The output stays and the lead grows by
  // exactly the slip.
  f.drive(f.t.axleA, 0.2);
  CHECK_THAT(f.b(), WithinAbs(0.5f, 1e-5f));
  CHECK_THAT(f.lead(), WithinAbs(0.3, 1e-5));
  // Forward again: a roller clutch re-engages at once, from wherever it is.
  f.drive(f.t.axleA, 0.3);
  CHECK_THAT(f.b(), WithinAbs(0.6f, 1e-5f));
  CHECK_THAT(f.lead(), WithinAbs(0.3, 1e-5));
  // Propagating again — from the driver, from nowhere, and a whole solve,
  // which is what every action does after its own pass — changes nothing.
  assembly::PropagateRelations(f.t.asmDoc, f.t.axleA);
  assembly::PropagateRelations(f.t.asmDoc, 0);
  assembly::Solve(f.t.asmDoc, NoParts());
  CHECK_THAT(f.lead(), WithinAbs(0.3, 1e-5));
  CHECK_THAT(f.b(), WithinAbs(0.6f, 1e-5f));
  CHECK(f.t.asmDoc.GetRelations()[0].error.empty());
  // The OUTPUT driven on ahead overruns: the input stays.
  f.drive(f.t.axleB, 1.0);
  CHECK_THAT(f.a(), WithinAbs(0.3f, 1e-5f));
  CHECK_THAT(f.lead(), WithinAbs(0.7, 1e-5));
  // The output pushed BACK engages and drags the input with it.
  f.drive(f.t.axleB, 0.8);
  CHECK_THAT(f.a(), WithinAbs(0.1f, 1e-5f));
  CHECK_THAT(f.lead(), WithinAbs(0.7, 1e-5));
}

TEST_CASE("A freewheel that drives negative mirrors all four",
          "[assembly][relations][freewheel]") {
  Freewheel f(-1);
  f.drive(f.t.axleA, -0.5); // drives b negative: engaged
  CHECK_THAT(f.b(), WithinAbs(-0.5f, 1e-5f));
  f.drive(f.t.axleA, -0.2); // back, which is positive here: slips
  CHECK_THAT(f.b(), WithinAbs(-0.5f, 1e-5f));
  CHECK_THAT(f.lead(), WithinAbs(-0.3, 1e-5));
  f.drive(f.t.axleB, -1.0); // the output on ahead: overruns
  CHECK_THAT(f.a(), WithinAbs(-0.2f, 1e-5f));
  CHECK_THAT(f.lead(), WithinAbs(-0.8, 1e-5));
  f.drive(f.t.axleB, -0.9); // pushed back: drags the input
  CHECK_THAT(f.a(), WithinAbs(-0.1f, 1e-5f));
}

TEST_CASE("Two freewheels drive one output: a reverser winds both ways",
          "[assembly][relations][freewheel]") {
  // The watch's reverser (docs/assemblies/relations.md#freewheel): the rotor
  // drives wheel A, wheel A drives wheel B the other way, each wheel reaches
  // its pinion through a one-way clutch, and both pinions mesh the ratchet.
  // Whichever way the rotor turns, one path drives and the other slips — and
  // the slipping one is reached with BOTH its ends already settled.
  AssemblyDocument d;
  const uint64_t carrier = d.AddOccurrence([] {
    Occurrence o;
    o.name = "Carrier";
    o.partId = 1;
    o.fixed = true;
    return o;
  }());
  const auto turning = [&](const char* name) {
    Occurrence o;
    o.name = name;
    o.partId = 1;
    return d.AddMate(Revolute(carrier, d.AddOccurrence(std::move(o))));
  };
  const uint64_t rotor = turning("Rotor");
  const uint64_t wheelA = turning("Wheel A");
  const uint64_t pinionA = turning("Pinion A");
  const uint64_t wheelB = turning("Wheel B");
  const uint64_t pinionB = turning("Pinion B");
  const uint64_t ratchet = turning("Ratchet");
  const auto gear = [&](uint64_t a, uint64_t b) {
    Relation r;
    r.kind = RelationKind::Gear;
    r.mateA = a;
    r.mateB = b;
    r.dofA = r.dofB = MateDof::Rotation;
    r.ratio = -1.0;
    d.AddRelation(r);
  };
  const auto clutch = [&](uint64_t a, uint64_t b) {
    Relation r;
    r.kind = RelationKind::Freewheel;
    r.mateA = a;
    r.mateB = b;
    r.dofA = r.dofB = MateDof::Rotation;
    r.ratio = 1.0;
    r.drives = 1;
    d.AddRelation(r);
  };
  gear(rotor, wheelA);
  gear(wheelA, wheelB); // wheel to wheel: B turns the other way
  clutch(wheelA, pinionA);
  clutch(wheelB, pinionB);
  gear(pinionA, ratchet);
  gear(pinionB, ratchet);

  const auto rot = [&](uint64_t m) {
    return double(d.FindMateById(m)->rotation);
  };
  const auto turn = [&](double v) {
    assembly::SetMateDof(*d.FindMateById(rotor), MateDof::Rotation, v);
    assembly::PropagateRelations(d, rotor);
  };
  // Every movement of the rotor winds the ratchet the SAME way, whichever
  // way it turns: 1 forward, then 2 back, then 1 forward again is 4 of
  // rotor travel and 4 of winding.
  turn(1.0);
  CHECK_THAT(rot(ratchet), WithinAbs(-1.0, 1e-4));
  turn(-1.0);
  CHECK_THAT(rot(ratchet), WithinAbs(-3.0, 1e-4));
  turn(0.0);
  CHECK_THAT(rot(ratchet), WithinAbs(-4.0, 1e-4));
  // Both clutches are consistent throughout: the slipping one takes the
  // difference up in its lead rather than reporting a disagreement.
  for (const Relation& r : d.GetRelations())
    CHECK(r.error.empty());
  // And it is idempotent: nothing moves when nothing has.
  const double before = rot(ratchet);
  assembly::PropagateRelations(d, rotor);
  assembly::PropagateRelations(d, 0);
  CHECK_THAT(rot(ratchet), WithinAbs(before, 1e-6));
  for (const Relation& r : d.GetRelations())
    CHECK(r.error.empty());

  // The output driven BACK against both clutches: they hold rather than
  // drag their wheels, and say so. Dragging here is what ran away — one
  // wheel dragged back turns the other forward, which drives the output
  // further the way that caused the drag.
  const double rotorAt = rot(rotor);
  assembly::SetMateDof(*d.FindMateById(ratchet), MateDof::Rotation,
                       before + 1.0);
  assembly::PropagateRelations(d, ratchet);
  CHECK_THAT(rot(rotor), WithinAbs(rotorAt, 1e-5));
  CHECK_THAT(rot(ratchet), WithinAbs(before + 1.0, 1e-5));
  int reported = 0;
  for (const Relation& r : d.GetRelations())
    if (r.kind == RelationKind::Freewheel && !r.error.empty())
      reported++;
  CHECK(reported > 0);
}

TEST_CASE("A freewheel whose output is driven BEHIND its line says so",
          "[assembly][relations][freewheel]") {
  // Both ends settled by other relations, with the output short of where
  // this clutch would have driven it: two drivers that disagree, which is
  // not slip and is not absorbed.
  Train t = MakeTrain();
  const uint64_t out = t.asmDoc.AddMate(Revolute(t.carrier, t.wheel));
  Relation g1;
  g1.kind = RelationKind::Gear;
  g1.mateA = t.axleA;
  g1.mateB = t.axleB;
  g1.dofA = g1.dofB = MateDof::Rotation;
  g1.ratio = 1.0;
  t.asmDoc.AddRelation(g1);
  Relation g2 = g1;
  g2.mateB = out;
  g2.ratio = 2.0;
  t.asmDoc.AddRelation(g2);
  Relation fw;
  fw.kind = RelationKind::Freewheel;
  fw.mateA = t.axleB;
  fw.mateB = out;
  fw.dofA = fw.dofB = MateDof::Rotation;
  fw.ratio = 1.0;
  fw.drives = 1;
  t.asmDoc.AddRelation(fw);
  assembly::SetMateDof(*t.asmDoc.FindMateById(t.axleA), MateDof::Rotation,
                       -0.5);
  assembly::PropagateRelations(t.asmDoc, t.axleA);
  CHECK_THAT(t.asmDoc.GetRelations()[2].error,
             Catch::Matchers::ContainsSubstring("behind the drive line"));
}

TEST_CASE("A click lets one DOF travel its way and holds it the other",
          "[assembly][relations][click]") {
  // A pawl on a watch's ratchet
  // (generators/watch/README.md#keyless-works-and-winding): ONE mate's
  // rotation, free the way it drives and held at its mark the other way.
  Train t = MakeTrain();
  Relation click;
  click.kind = RelationKind::Click;
  click.mateA = click.mateB = t.axleA;
  click.dofA = click.dofB = MateDof::Rotation;
  click.ratio = 1.0;
  click.drives = 1;
  t.asmDoc.AddRelation(click);
  const auto at = [&] {
    return double(t.asmDoc.FindMateById(t.axleA)->rotation);
  };
  const auto put = [&](double v) {
    assembly::SetMateDof(*t.asmDoc.FindMateById(t.axleA), MateDof::Rotation, v);
    assembly::PropagateRelations(t.asmDoc, t.axleA);
  };
  put(1.0);
  CHECK_THAT(at(), WithinAbs(1.0, 1e-5));
  CHECK_THAT(t.asmDoc.GetRelations()[0].lead, WithinAbs(1.0, 1e-5));
  put(0.4); // back: held at the mark, and the mark stands
  CHECK_THAT(at(), WithinAbs(1.0, 1e-5));
  CHECK_THAT(t.asmDoc.GetRelations()[0].lead, WithinAbs(1.0, 1e-5));
  put(1.6); // on again: the mark follows
  CHECK_THAT(at(), WithinAbs(1.6, 1e-5));
  // Re-solving changes nothing, and the mark survives a round trip.
  assembly::PropagateRelations(t.asmDoc, 0);
  CHECK_THAT(at(), WithinAbs(1.6, 1e-5));
  const fs::path tmp = fs::temp_directory_path() / "relations_click.json";
  REQUIRE(DocumentIO::SaveAssembly(t.asmDoc, tmp.string()).has_value());
  auto back = DocumentIO::LoadAssembly(tmp.string());
  REQUIRE(back.has_value());
  CHECK((*back)->GetRelations()[0].kind == RelationKind::Click);
  CHECK_THAT((*back)->GetRelations()[0].lead, WithinAbs(1.6, 1e-6));
  fs::remove(tmp);
}

TEST_CASE("A toothed freewheel rides over its dogs and lifts what it names",
          "[assembly][relations][freewheel]") {
  // A watch's clutch: sawtooth dogs slip only because the clutch LIFTS along
  // the stem as they ride over each other. The lift is applied after the
  // walk, so it ADDS to the slide the yoke already gave it.
  Train t = MakeTrain();
  t.asmDoc.FindMateById(t.axleB)->type = MateType::Cylindrical;
  Relation fw;
  fw.kind = RelationKind::Freewheel;
  fw.mateA = t.axleA;
  fw.mateB = t.axleB;
  fw.dofA = fw.dofB = MateDof::Rotation;
  fw.ratio = 1.0;
  fw.drives = 1;
  fw.teeth = 4; // a quarter turn between dogs
  fw.rideAmount = 1.0;
  fw.rideMate = t.axleB;
  fw.rideDof = MateDof::Offset;
  t.asmDoc.AddRelation(fw);
  // The fixture's cylindrical mate stands at 48: the lift ADDS to it.
  const double base = double(t.asmDoc.FindMateById(t.axleB)->offset);
  const auto lift = [&] {
    return double(t.asmDoc.FindMateById(t.axleB)->offset) - base;
  };
  const auto turn = [&](double v) {
    assembly::SetMateDof(*t.asmDoc.FindMateById(t.axleA), MateDof::Rotation, v);
    assembly::PropagateRelations(t.asmDoc, t.axleA);
  };
  turn(0.0);
  CHECK_THAT(lift(), WithinAbs(0.0, 1e-5)); // engaged: sitting in a notch
  turn(-M_PI / 4.0); // half a tooth of slip: standing clear of the dogs
  CHECK_THAT(lift(), WithinAbs(1.0, 1e-4));
  CHECK_THAT(double(t.asmDoc.FindMateById(t.axleB)->rotation),
             WithinAbs(0.0, 1e-5)); // the output stayed: it slipped
  turn(-M_PI / 2.0);                // a whole tooth: dropped into the next
  CHECK_THAT(lift(), WithinAbs(0.0, 1e-4));
  // Re-solving does not stack another lift on top of the last one.
  turn(-3.0 * M_PI / 8.0); // three quarters of a tooth: still riding
  const double half = lift();
  assembly::PropagateRelations(t.asmDoc, 0);
  assembly::PropagateRelations(t.asmDoc, 0);
  CHECK_THAT(lift(), WithinAbs(half, 1e-6));
  // Seated in a notch, the input drives the output at once.
  turn(-3.0 * M_PI / 8.0);
  const double seated = double(t.asmDoc.FindMateById(t.axleB)->rotation);
  CHECK_THAT(seated, WithinAbs(M_PI / 8.0, 1e-4));
  // The dead band: slip a quarter of a tooth back, and coming forward again
  // moves nothing until the input has made that quarter up.
  turn(-M_PI / 2.0);
  CHECK_THAT(double(t.asmDoc.FindMateById(t.axleB)->rotation),
             WithinAbs(seated, 1e-5));
  turn(-7.0 * M_PI / 16.0);
  CHECK_THAT(double(t.asmDoc.FindMateById(t.axleB)->rotation),
             WithinAbs(seated, 1e-5));
  // ...and past it, it drives again.
  turn(-3.0 * M_PI / 8.0);
  CHECK_THAT(double(t.asmDoc.FindMateById(t.axleB)->rotation),
             WithinAbs(M_PI / 8.0, 1e-4));
}

TEST_CASE("A click and a toothed clutch round-trip, and the v105 golden "
          "drives",
          "[assembly][relations][click][golden]") {
  Train t = MakeTrain();
  t.asmDoc.FindMateById(t.axleB)->type = MateType::Cylindrical;
  Relation dogs;
  dogs.kind = RelationKind::Freewheel;
  dogs.name = "dogs";
  dogs.mateA = t.axleA;
  dogs.mateB = t.axleB;
  dogs.dofA = dogs.dofB = MateDof::Rotation;
  dogs.ratio = 1.0;
  dogs.drives = 1;
  dogs.teeth = 4;
  dogs.rideAmount = 0.3;
  dogs.rideMate = t.axleB;
  dogs.rideDof = MateDof::Offset;
  t.asmDoc.AddRelation(dogs);
  Relation click;
  click.kind = RelationKind::Click;
  click.name = "click";
  click.mateA = click.mateB = t.axleB;
  click.dofA = click.dofB = MateDof::Rotation;
  click.ratio = 1.0;
  click.drives = 1;
  t.asmDoc.AddRelation(click);
  if (const char* out = std::getenv("LOBOLE_WRITE_GOLDEN"))
    REQUIRE(DocumentIO::SaveAssembly(t.asmDoc, out).has_value());
  // `teeth` and `ride` are a freewheel's; a click writes neither.
  const nlohmann::json dj = DocumentIO::RelationToJson(dogs);
  CHECK(dj.at("teeth") == 4);
  CHECK(dj.at("ride").at("dof") == "offset");
  CHECK_FALSE(DocumentIO::RelationToJson(click).contains("teeth"));

  const fs::path golden = fs::path(TestSourceDir()) /
                          "tests/golden/assembly/v105-click-and-dogs.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->RelationCount() == 2);
  const Relation& rd = (*g)->GetRelations()[0];
  CHECK(rd.teeth == 4);
  CHECK(rd.rideMate == t.axleB);
  CHECK(rd.rideDof == MateDof::Offset);
  CHECK_THAT(rd.rideAmount, WithinAbs(0.3, 1e-12));
  CHECK((*g)->GetRelations()[1].kind == RelationKind::Click);
  // And it still rides: off its notch, the clutch stands clear.
  assembly::SetMateDof(*(*g)->FindMateById(t.axleA), MateDof::Rotation,
                       -M_PI / 4.0);
  assembly::PropagateRelations(**g, t.axleA);
  CHECK_THAT((*g)->FindMateById(t.axleB)->offset, WithinAbs(48.3f, 1e-3f));
}

TEST_CASE("A freewheel inside a mate loop is reported, not held",
          "[assembly][relations][freewheel][loops]") {
  // Two revolutes between the same pair close a loop: both must give the
  // pinion one orientation. A freewheel at ratio 2 between them asks for
  // another, so the loop solve moves one of its mates — and a freewheel is
  // an inequality the loop solve cannot hold, so it was left out of the
  // rows and says so rather than being quietly treated as engaged.
  Train t = MakeTrain();
  const uint64_t second = t.asmDoc.AddMate(Revolute(t.carrier, t.pinion));
  Relation fw;
  fw.kind = RelationKind::Freewheel;
  fw.mateA = t.axleA;
  fw.mateB = second;
  fw.dofA = fw.dofB = MateDof::Rotation;
  fw.ratio = 2.0;
  fw.drives = 1;
  t.asmDoc.AddRelation(fw);
  assembly::SetMateDof(*t.asmDoc.FindMateById(t.axleA), MateDof::Rotation, 0.4);
  const assembly::SolveReport rep = assembly::Solve(t.asmDoc, NoParts());
  CHECK(rep.loopMates >= 1);
  CHECK_THAT(t.asmDoc.GetRelations()[0].error,
             Catch::Matchers::ContainsSubstring("inside a mate loop"));
}

TEST_CASE("A freewheel round-trips with its history, and the v104 golden "
          "drives",
          "[assembly][relations][freewheel][golden]") {
  Freewheel f(-1);
  f.t.asmDoc.GetRelations()[0].name = "one-way wheel";
  f.t.asmDoc.GetRelations()[0].lead = -0.25;
  if (const char* out = std::getenv("LOBOLE_WRITE_GOLDEN"))
    REQUIRE(DocumentIO::SaveAssembly(f.t.asmDoc, out).has_value());
  // `drives` and `lead` are a freewheel's; no other kind writes them.
  const nlohmann::json gj =
      DocumentIO::RelationToJson(f.t.asmDoc.GetRelations()[0]);
  CHECK(gj.at("drives") == "negative");
  CHECK_THAT(gj.at("lead").get<double>(), WithinAbs(-0.25, 1e-12));
  Relation gear;
  CHECK_FALSE(DocumentIO::RelationToJson(gear).contains("lead"));
  // The direction is required, and anything but the two is refused.
  nlohmann::json nodir = gj;
  nodir.erase("drives");
  CHECK_THROWS_WITH(DocumentIO::RelationFromJson(nodir),
                    Catch::Matchers::ContainsSubstring("drives"));
  CHECK_THROWS_WITH(DocumentIO::FreewheelDrivesFromString("forward"),
                    Catch::Matchers::ContainsSubstring("positive or negative"));

  const fs::path golden =
      fs::path(TestSourceDir()) / "tests/golden/assembly/v104-freewheel.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->RelationCount() == 1);
  const Relation& r = (*g)->GetRelations()[0];
  CHECK(r.kind == RelationKind::Freewheel);
  CHECK(r.drives == -1);
  CHECK_THAT(r.lead, WithinAbs(-0.25, 1e-12));
  // It drives from the history it was saved with: the input at -0.5 puts
  // the output at -0.75.
  assembly::SetMateDof(*(*g)->FindMateById(f.t.axleA), MateDof::Rotation, -0.5);
  assembly::PropagateRelations(**g, f.t.axleA);
  CHECK_THAT((*g)->FindMateById(f.t.axleB)->rotation, WithinAbs(-0.75f, 1e-5f));
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

  if (const char* out = std::getenv("LOBOLE_WRITE_GOLDEN"))
    REQUIRE(DocumentIO::SaveAssembly(t.asmDoc, out).has_value());
  const fs::path golden = fs::path(TestSourceDir()) /
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

// --- relations over several mates (v171,
// docs/assemblies/relations.md#relations-over-several-mates) ----------------

namespace {
// A housing (fixed) with a sun, a ring and a carrier each on a revolute
// about one axis, and two planets on revolutes on the carrier. Tooth counts
// are the planetary generator's kind: 14-tooth sun, 40-tooth ring, 13-tooth
// planets.
constexpr int kZs = 14, kZr = 40, kZp = 13;
struct Epi {
  Rig r = MakeRig();
  uint64_t sun = 0, ring = 0, carrier = 0, planet1 = 0, planet2 = 0;
  uint64_t willis = 0, p1 = 0, p2 = 0;
  uint64_t carrierOcc = 0;
  double Rot(uint64_t mate) { return r.d.FindMateById(mate)->rotation; }
  void Drive(uint64_t mate, double value) {
    r.d.FindMateById(mate)->rotation = value;
    assembly::PropagateRelations(r.d, mate);
    assembly::Solve(r.d, NoParts(), nullptr, {mate, MateDof::Rotation, {}});
  }
};
RelationTerm Term(uint64_t mate) {
  RelationTerm t;
  t.mate = mate;
  return t;
}
Epi MakeEpi(bool planets = true) {
  Epi e;
  e.sun = e.r.Add("Sun", 2, MateType::Revolute);
  e.ring = e.r.Add("Ring", 3, MateType::Revolute);
  e.carrier = e.r.Add("Carrier arm", 4, MateType::Revolute);
  e.carrierOcc = e.r.d.FindMateById(e.carrier)->b.occurrenceId;
  e.willis = e.r.d.AddRelation(assembly::MakeEpicyclic(
      Term(e.sun), Term(e.ring), Term(e.carrier), kZs, kZr));
  if (planets)
    for (uint64_t* mate : {&e.planet1, &e.planet2}) {
      Occurrence o;
      o.name = "Planet";
      o.partId = 5;
      const uint64_t occ = e.r.d.AddOccurrence(std::move(o));
      *mate = e.r.d.AddMate(Revolute(e.carrierOcc, occ));
      (mate == &e.planet1 ? e.p1 : e.p2) =
          e.r.d.AddRelation(assembly::MakePlanet(Term(*mate), Term(e.sun),
                                                 Term(e.carrier), kZp, kZs));
    }
  return e;
}
} // namespace

TEST_CASE("An internal gear turns the same way, at teeth_a / teeth_b",
          "[assembly][relations]") {
  Train t = MakeTrain();
  Relation g;
  g.kind = RelationKind::Gear;
  g.mateA = t.axleA;
  g.mateB = t.axleB;
  g.teethA = 13;
  g.teethB = 40;
  g.internal = true;
  REQUIRE(assembly::DeriveFromTeeth(g));
  CHECK_THAT(g.ratio, WithinAbs(13.0 / 40.0, 1e-12));
  const uint64_t rel = t.asmDoc.AddRelation(g);
  t.asmDoc.FindMateById(t.axleA)->rotation = 0.8;
  assembly::Solve(t.asmDoc, NoParts());
  CHECK(t.asmDoc.FindRelationById(rel)->error.empty());
  // Same sense: a planet in its ring.
  CHECK_THAT(t.asmDoc.FindMateById(t.axleB)->rotation,
             WithinAbs(0.8 * 13.0 / 40.0, 1e-9));
  // ...and the external pair, from the same teeth, the other way.
  g.internal = false;
  REQUIRE(assembly::DeriveFromTeeth(g));
  CHECK_THAT(g.ratio, WithinAbs(-13.0 / 40.0, 1e-12));
  Relation chain = g;
  chain.kind = RelationKind::Chain;
  REQUIRE(assembly::DeriveFromTeeth(chain));
  CHECK(chain.ratio > 0.0);
}

TEST_CASE("Willis holds with each member of an epicyclic held in turn",
          "[assembly][relations][epicyclic]") {
  const double zs = kZs, zr = kZr;
  SECTION("ring held: the carrier turns zs/(zs+zr) of the sun") {
    Epi e = MakeEpi(false);
    e.Drive(e.sun, 0.7);
    CHECK_THAT(e.Rot(e.ring), WithinAbs(0.0, 1e-12)); // nothing drove it
    CHECK_THAT(e.Rot(e.carrier), WithinAbs(0.7 * zs / (zs + zr), 1e-9));
    const Relation* w = e.r.d.FindRelationById(e.willis);
    CHECK(w->error.empty());
    // ...and it says which member it held, and why.
    CHECK_THAT(w->held, Catch::Matchers::ContainsSubstring("ring"));
    // The carrier's placement follows its mate, not just the number.
    CHECK_THAT(ZRotationOf(e.r.d, e.carrierOcc),
               WithinAbs(0.7 * zs / (zs + zr), 1e-5));
  }
  SECTION("carrier held: the ring turns -zs/zr of the sun") {
    Epi e = MakeEpi(false);
    Configuration& cfg = e.r.d.AddConfiguration("Carrier held");
    cfg.mateOverrides = {{e.carrier, 0.0, std::nullopt}};
    REQUIRE(e.r.d.SetActiveConfiguration(cfg.id));
    e.Drive(e.sun, 0.7);
    CHECK_THAT(e.Rot(e.ring), WithinAbs(-0.7 * zs / zr, 1e-9));
    CHECK(e.r.d.FindRelationById(e.willis)->error.empty());
    CHECK(e.r.d.FindRelationById(e.willis)->held.empty()); // all driven
  }
  SECTION("sun held: the carrier turns zr/(zs+zr) of the ring") {
    Epi e = MakeEpi(false);
    Configuration& cfg = e.r.d.AddConfiguration("Sun held");
    cfg.mateOverrides = {{e.sun, 0.0, std::nullopt}};
    REQUIRE(e.r.d.SetActiveConfiguration(cfg.id));
    e.Drive(e.ring, 0.5);
    CHECK_THAT(e.Rot(e.carrier), WithinAbs(0.5 * zr / (zs + zr), 1e-9));
    CHECK(e.r.d.FindRelationById(e.willis)->error.empty());
  }
}

TEST_CASE("A planet turns on its carrier by -(zs/zp) of the sun's turn "
          "relative to the carrier",
          "[assembly][relations][epicyclic]") {
  Epi e = MakeEpi();
  e.Drive(e.sun, 0.9);
  const double carrier = 0.9 * kZs / double(kZs + kZr);
  CHECK_THAT(e.Rot(e.carrier), WithinAbs(carrier, 1e-9));
  for (uint64_t planet : {e.planet1, e.planet2})
    CHECK_THAT(e.Rot(planet),
               WithinAbs(-double(kZs) / kZp * (0.9 - carrier), 1e-9));
  // Absolute, that is -(zs/zp)(zr/(zs+zr)) of the sun — the hand-derived
  // bare ratio the planetary generator used to carry.
  CHECK_THAT(e.Rot(e.planet1),
             WithinAbs(-double(kZs) / kZp * kZr / (kZs + kZr) * 0.9, 1e-9));
  for (const Relation& rel : e.r.d.GetRelations())
    CHECK(rel.error.empty());
  // A plain solve afterwards changes nothing: propagation is idempotent.
  assembly::Solve(e.r.d, NoParts());
  CHECK_THAT(e.Rot(e.sun), WithinAbs(0.9, 1e-12));
  CHECK_THAT(e.Rot(e.carrier), WithinAbs(carrier, 1e-9));
}

TEST_CASE("A differential: two members driven set the third, one driven "
          "holds the next and says so",
          "[assembly][relations][epicyclic]") {
  Epi e = MakeEpi(false);
  const double zs = kZs, zr = kZr;
  // The ring held at 0.3 by a configuration, the sun driven to 0.7: Willis
  // sets the carrier between them.
  Configuration& cfg = e.r.d.AddConfiguration("Ring at 0.3");
  cfg.mateOverrides = {{e.ring, 0.3, std::nullopt}};
  REQUIRE(e.r.d.SetActiveConfiguration(cfg.id));
  e.Drive(e.sun, 0.7);
  CHECK_THAT(e.Rot(e.carrier),
             WithinAbs((zs * 0.7 + zr * 0.3) / (zs + zr), 1e-9));
  CHECK(e.r.d.FindRelationById(e.willis)->held.empty());
  // Driving the CARRIER alone: the sun, first in term order, stays where it
  // stands and the ring takes the turn — and the relation names the sun.
  e.r.d.SetActiveConfiguration(0);
  const double sun = e.Rot(e.sun);
  e.Drive(e.carrier, 1.0);
  CHECK_THAT(e.Rot(e.sun), WithinAbs(sun, 1e-12));
  CHECK_THAT(e.Rot(e.ring), WithinAbs(((zs + zr) * 1.0 - zs * sun) / zr, 1e-9));
  CHECK_THAT(e.r.d.FindRelationById(e.willis)->held,
             Catch::Matchers::ContainsSubstring("sun"));
}

TEST_CASE("Two epicyclic relations that disagree report 'not satisfied'",
          "[assembly][relations][epicyclic]") {
  Epi e = MakeEpi(false);
  e.r.d.AddRelation(assembly::MakeEpicyclic(Term(e.sun), Term(e.ring),
                                            Term(e.carrier), 20, kZr));
  e.Drive(e.sun, 0.4);
  int unsatisfied = 0;
  for (const Relation& rel : e.r.d.GetRelations())
    if (rel.error.find("not satisfied") != std::string::npos)
      unsatisfied++;
  CHECK(unsatisfied == 1);
}

TEST_CASE("A relation over several mates is ONE row in a loop: rank sees it",
          "[assembly][relations][loops][epicyclic]") {
  // The parallelogram four-bar (theta4 = theta1, one DOF) and a wheel on
  // the ground, pinned at 0.4. The row 3*wheel + 5*theta1 - 8*theta4 = 0
  // with the linkage's own theta4 = theta1 leaves theta1 = wheel: the loop
  // solve must MOVE the linkage there, from a seed of 0.3.
  const auto AtX = [](double x) {
    return glm::translate(glm::dmat4(1.0), glm::dvec3(x, 0.0, 0.0));
  };
  const auto Rev = [](uint64_t id, uint64_t a, const glm::dmat4& fa, uint64_t b,
                      const glm::dmat4& fb, double seed) {
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
  // Seeded ON the parallelogram branch (crank and rocker at 0.3, the
  // coupler level): the linkage has a crossed branch through zero too.
  p.mates.push_back(Rev(1, 10, glm::dmat4(1.0), 20, glm::dmat4(1.0), 0.3));
  p.mates.push_back(Rev(2, 20, AtX(40.0), 30, glm::dmat4(1.0), -0.3));
  p.mates.push_back(Rev(3, 30, AtX(100.0), 40, AtX(40.0), 0.3));
  p.mates.push_back(Rev(4, 10, AtX(100.0), 40, glm::dmat4(1.0), 0.3));
  p.mates.push_back(Rev(5, 10, AtX(-50.0), 50, glm::dmat4(1.0), 0.4));
  p.tree.push_back({0, 10, 20, false});
  p.tree.push_back({1, 20, 30, false});
  p.tree.push_back({2, 30, 40, false});
  p.tree.push_back({4, 10, 50, false});
  p.cut.push_back(3);
  p.anchor.emplace_back(10, glm::dmat4(1.0));
  p.pinned.push_back({4, MateDof::Rotation, 0.4});
  assembly::LoopRelation lr;
  lr.terms = {{4, MateDof::Rotation, 3.0},
              {0, MateDof::Rotation, 5.0},
              {3, MateDof::Rotation, -8.0}};
  p.relations.push_back(lr);
  const assembly::LoopSolution s = assembly::SolveLoops(p);
  REQUIRE(s.unavailable.empty());
  CHECK(s.converged);
  CHECK(s.dof == 0);
  CHECK_THAT(s.solved[0].rotation, WithinAbs(0.4, 1e-4));
  CHECK_THAT(s.solved[3].rotation, WithinAbs(0.4, 1e-4));
}

TEST_CASE("An epicyclic inside a closed loop solves to propagation's answer",
          "[assembly][relations][loops][epicyclic]") {
  // A second bearing on the carrier's axis closes a loop through the
  // housing, so the solve takes the loop path with the Willis and planet
  // rows in it. Its answer must be the tree's.
  Epi tree = MakeEpi();
  tree.Drive(tree.sun, 0.6);
  Epi looped = MakeEpi();
  looped.r.d.AddMate(Revolute(looped.r.carrier, looped.carrierOcc));
  looped.r.d.FindMateById(looped.sun)->rotation = 0.6;
  assembly::PropagateRelations(looped.r.d, looped.sun);
  const assembly::SolveReport rep = assembly::Solve(
      looped.r.d, NoParts(), nullptr, {looped.sun, MateDof::Rotation, {}});
  CHECK(rep.loopMates >= 1);
  CHECK(rep.loopsClosed);
  for (uint64_t Epi::* m :
       {&Epi::ring, &Epi::carrier, &Epi::planet1, &Epi::planet2})
    CHECK_THAT(looped.Rot(looped.*m), WithinAbs(tree.Rot(tree.*m), 1e-6));
  for (const Relation& rel : looped.r.d.GetRelations())
    CHECK(rel.error.empty());
}

TEST_CASE("A relation over several mates is refused before it is made, "
          "naming what would do",
          "[assembly][relations][epicyclic]") {
  Epi e = MakeEpi(false);
  using Catch::Matchers::ContainsSubstring;
  // A mate used twice.
  Relation twice = assembly::MakeEpicyclic(Term(e.sun), Term(e.sun),
                                           Term(e.carrier), kZs, kZr);
  CHECK_THAT(assembly::RelationProblem(e.r.d, twice),
             ContainsSubstring("both the sun and the ring"));
  // A member missing.
  Relation missing =
      assembly::MakeEpicyclic(Term(e.sun), Term(0), Term(e.carrier), kZs, kZr);
  CHECK_THAT(assembly::RelationProblem(e.r.d, missing),
             ContainsSubstring("no ring picked"));
  // A member that does not turn.
  const uint64_t slide = e.r.Add("Slide", 6, MateType::Slider);
  Relation flat = assembly::MakeEpicyclic(Term(e.sun), Term(slide),
                                          Term(e.carrier), kZs, kZr);
  CHECK_THAT(assembly::RelationProblem(e.r.d, flat),
             ContainsSubstring("has no rotation"));
  // A linear_n of one term.
  Relation one;
  one.kind = RelationKind::LinearN;
  one.terms = {Term(e.sun)};
  CHECK_THAT(assembly::RelationProblem(e.r.d, one),
             ContainsSubstring("at least two terms"));
  // No teeth.
  Relation toothless = assembly::MakeEpicyclic(Term(e.sun), Term(e.ring),
                                               Term(e.carrier), 0, kZr);
  CHECK_THAT(assembly::RelationProblem(e.r.d, toothless),
             ContainsSubstring("tooth count"));
  // And the good one passes.
  CHECK(assembly::RelationProblem(e.r.d, *e.r.d.FindRelationById(e.willis))
            .empty());
  // A gear pair named on one mate is a mate used twice too.
  Relation self = Gear(e.sun, e.sun, -1.0);
  CHECK_THAT(assembly::RelationProblem(e.r.d, self),
             ContainsSubstring("both the a and the b"));
}

TEST_CASE("A configuration suppressing any member takes the whole relation "
          "out, and a removed member is an error",
          "[assembly][relations][epicyclic][configurations]") {
  Epi e = MakeEpi();
  Configuration& cfg = e.r.d.AddConfiguration("No ring");
  cfg.suppressedMates = {e.ring};
  const uint64_t cfgId = cfg.id;
  REQUIRE(e.r.d.SetActiveConfiguration(cfgId));
  CHECK(e.r.d.RelationSuppressed(e.willis));
  CHECK_FALSE(e.r.d.RelationSuppressed(e.p1));
  e.Drive(e.sun, 0.5);
  CHECK(e.r.d.FindRelationById(e.willis)->error.empty());
  CHECK_THAT(e.Rot(e.carrier), WithinAbs(0.0, 1e-12)); // nothing drove it
  e.r.d.SetActiveConfiguration(0);
  REQUIRE(e.r.d.RemoveMate(e.ring));
  assembly::Solve(e.r.d, NoParts());
  CHECK_THAT(e.r.d.FindRelationById(e.willis)->error,
             Catch::Matchers::ContainsSubstring("ring"));
}

TEST_CASE("A freewheel tied to its input through a relation over several "
          "mates holds rather than drags",
          "[assembly][relations][freewheel]") {
  // The cycle search walks every member of a hyperedge: X drives Y through
  // a clutch, and a linear_n over {Y, X, Z} ties them another way. Pushing
  // Y back must not drag X (the clutch HOLDS, as in a reverser).
  Rig r = MakeRig();
  const uint64_t x = r.Add("X", 2, MateType::Revolute);
  const uint64_t y = r.Add("Y", 3, MateType::Revolute);
  const uint64_t z = r.Add("Z", 4, MateType::Revolute);
  Relation fw;
  fw.kind = RelationKind::Freewheel;
  fw.mateA = x;
  fw.mateB = y;
  fw.ratio = 1.0;
  fw.drives = 1;
  r.d.AddRelation(fw);
  assembly::Solve(r.d, NoParts());
  r.d.FindMateById(y)->rotation = -0.5;
  assembly::PropagateRelations(r.d, y);
  // Alone, the clutch drags its input back with it (a bicycle's pedals).
  CHECK_THAT(r.d.FindMateById(x)->rotation, WithinAbs(-0.5, 1e-9));

  Rig t = MakeRig();
  const uint64_t tx = t.Add("X", 2, MateType::Revolute);
  const uint64_t ty = t.Add("Y", 3, MateType::Revolute);
  const uint64_t tz = t.Add("Z", 4, MateType::Revolute);
  fw.mateA = tx;
  fw.mateB = ty;
  t.d.AddRelation(fw);
  Relation tie;
  tie.kind = RelationKind::LinearN;
  tie.terms = {Term(ty), Term(tx), Term(tz)};
  tie.terms[1].coefficient = -1.0;
  t.d.AddRelation(tie);
  assembly::Solve(t.d, NoParts());
  t.d.FindMateById(ty)->rotation = -0.5;
  assembly::PropagateRelations(t.d, ty);
  CHECK_THAT(t.d.FindMateById(tx)->rotation, WithinAbs(0.0, 1e-12));
  CHECK_THAT(t.d.FindMateById(tz)->rotation, WithinAbs(0.5, 1e-9));
  (void)z;
}

TEST_CASE("Relations over several mates round-trip, and the v171 golden "
          "drives",
          "[assembly][relations][epicyclic][golden]") {
  Epi e = MakeEpi();
  e.r.d.FindRelationById(e.p1)->phase = 0.25;
  Relation g = Gear(e.sun, e.ring, 0.0);
  g.teethA = kZp;
  g.teethB = kZr;
  g.internal = true;
  REQUIRE(assembly::DeriveFromTeeth(g));
  e.r.d.AddRelation(g);
  const fs::path tmp = fs::temp_directory_path() / "epicyclic_rt.json";
  REQUIRE(DocumentIO::SaveAssembly(e.r.d, tmp.string()).has_value());
  auto back = DocumentIO::LoadAssembly(tmp.string());
  REQUIRE(back.has_value());
  const Relation* w = (*back)->FindRelationById(e.willis);
  REQUIRE(w != nullptr);
  CHECK(w->kind == RelationKind::Epicyclic);
  REQUIRE(w->terms.size() == 3);
  CHECK(w->terms[0].mate == e.sun);
  CHECK(w->terms[0].teeth == kZs);
  CHECK(w->terms[1].teeth == kZr);
  CHECK_THAT(w->terms[2].coefficient, WithinAbs(-(kZs + kZr), 1e-12));
  const Relation* p = (*back)->FindRelationById(e.p1);
  REQUIRE(p != nullptr);
  CHECK(p->kind == RelationKind::Planet);
  CHECK(p->terms[2].teeth == kZp);
  CHECK_THAT(p->phase, WithinAbs(0.25, 1e-12));
  const Relation& gb = (*back)->GetRelations().back();
  CHECK(gb.internal);
  CHECK(gb.teethA == kZp);
  CHECK_THAT(gb.ratio, WithinAbs(double(kZp) / kZr, 1e-12));
  fs::remove(tmp);

  // An unknown kind is refused, naming the new ones among those accepted.
  CHECK_THROWS_WITH(DocumentIO::RelationKindFromString("epicylic"),
                    Catch::Matchers::ContainsSubstring("epicyclic"));
  // `internal` on anything but a gear is refused, not ignored.
  nlohmann::json bad = DocumentIO::RelationToJson(gb);
  bad["kind"] = "chain";
  CHECK_THROWS(DocumentIO::RelationFromJson(bad));

  // The golden: the sun at 0.7 rad, the ring held, the carrier and the
  // planet following.
  const fs::path golden = fs::path(TestSourceDir()) /
                          "tests/golden/assembly/v171-epicyclic-relation.json";
  auto gd = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(gd.has_value());
  assembly::Solve(**gd, NoParts());
  for (const Relation& rel : (*gd)->GetRelations())
    CHECK(rel.error.empty());
  const auto rot = [&](uint64_t mate) {
    return (*gd)->FindMateById(mate)->rotation;
  };
  CHECK_THAT(rot(3), WithinAbs(0.7, 1e-12));
  CHECK_THAT(rot(5), WithinAbs(0.7 * kZs / (kZs + kZr), 1e-9));
}

namespace {
// A carry (docs/assemblies/relations.md#intermittent): `units` turning
// steps `transfer` once a turn, and the transfer turns `tens` through a
// 9:30 gear. Geneva: 3 slots, the window opening at -12 degrees.
Relation Geneva(uint64_t a, uint64_t b, int slots, double startDeg) {
  Relation r;
  r.kind = RelationKind::Intermittent;
  r.mateA = a;
  r.mateB = b;
  r.dofA = r.dofB = MateDof::Rotation;
  r.ratio = 1.0;
  r.intermittent.slots = slots;
  r.intermittent.step = -360.0 / slots;
  r.intermittent.durationDeg = 180.0 - 360.0 / slots;
  r.intermittent.periodDeg = 360.0;
  r.intermittent.startDeg = startDeg;
  return r;
}

double Deg(double rad) { return rad * 180.0 / M_PI; }
double Rad(double deg) { return deg * M_PI / 180.0; }
} // namespace

TEST_CASE("An intermittent relation steps once a period and stands still "
          "between",
          "[assembly][relations][intermittent]") {
  Relation r;
  r.kind = RelationKind::Intermittent;
  r.dofA = r.dofB = MateDof::Rotation;
  r.intermittent.step = 36.0;
  r.intermittent.durationDeg = 36.0;
  r.intermittent.startDeg = 300.0;
  const auto at = [&](double deg) {
    return Deg(IntermittentValue(r, Rad(deg)));
  };
  // Before the first window nothing has moved; through it, a uniform step
  // is linear; after it, b stands at one step until the next turn's window.
  CHECK_THAT(at(0.0), WithinAbs(0.0, 1e-9));
  CHECK_THAT(at(299.0), WithinAbs(0.0, 1e-9));
  CHECK_THAT(at(318.0), WithinAbs(18.0, 1e-9));
  CHECK_THAT(at(336.0), WithinAbs(36.0, 1e-9));
  CHECK_THAT(at(359.0), WithinAbs(36.0, 1e-9));
  CHECK_THAT(at(660.0), WithinAbs(36.0, 1e-9));
  CHECK_THAT(at(696.0), WithinAbs(72.0, 1e-9));
  // The count is a's turns, not history: a thousand turns on is a thousand
  // steps, and turning back runs the count back through zero.
  CHECK_THAT(at(360.0 * 1000.0), WithinAbs(36.0 * 1000.0, 1e-6));
  CHECK_THAT(at(-30.0), WithinAbs(-6.0, 1e-9)); // last turn's window
  CHECK_THAT(at(-60.0), WithinAbs(-36.0, 1e-9));
  CHECK_THAT(at(-70.0), WithinAbs(-36.0, 1e-9));
  CHECK_THAT(at(-42.0), WithinAbs(-18.0, 1e-9));
  // Continuous everywhere, including the window's two ends.
  for (double deg = -800.0; deg < 800.0; deg += 0.25)
    CHECK(std::abs(at(deg + 0.25) - at(deg)) <= 0.25 + 1e-9);
  // A slide steps in mm, untouched by any degree conversion.
  r.dofB = MateDof::Offset;
  r.intermittent.step = 2.5;
  CHECK_THAT(IntermittentValue(r, Rad(700.0)), WithinAbs(5.0, 1e-12));
}

TEST_CASE("A Geneva's law is the pin in its slot",
          "[assembly][relations][intermittent]") {
  for (int n : {3, 4, 6}) {
    CAPTURE(n);
    Relation r = Geneva(1, 2, n, 0.0);
    const double half = M_PI / n;
    const double c = 1.0, rp = std::sin(half) * c; // centres 1 apart
    const double window = M_PI - 2.0 * half;
    double last = -1.0;
    for (int i = 0; i <= 200; i++) {
      const double t = i / 200.0;
      const double s = IntermittentLaw(r.intermittent, t);
      CHECK(s >= last - 1e-12); // never back
      last = s;
      // The wheel, turned by the relation, puts a slot through the pin:
      // the driver at alpha from the line of centres has its pin at
      // rp (cos, sin); the wheel at O2 = (c, 0) has turned -step * s from
      // the slot that faced the driver's axis at entry, which stood at
      // pi - half... seen from O2 the slot's direction is pi - (s * 2 half
      // - half), and the pin must lie on it.
      const double alpha = (t - 0.5) * window;
      const double px = rp * std::cos(alpha) - c, py = rp * std::sin(alpha);
      const double slot = M_PI - (s * 2.0 * half - half);
      CHECK_THAT(px * std::sin(slot) - py * std::cos(slot),
                 WithinAbs(0.0, 1e-12));
      CHECK(px * std::cos(slot) + py * std::sin(slot) > 0.0); // not behind
    }
    CHECK_THAT(IntermittentLaw(r.intermittent, 0.0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(IntermittentLaw(r.intermittent, 1.0), WithinAbs(1.0, 1e-12));
    CHECK_THAT(IntermittentLaw(r.intermittent, 0.5), WithinAbs(0.5, 1e-12));
    // The pin enters and leaves radially: the wheel starts and stops at
    // rest, so it moves under a fifth of its step in the window's first
    // tenth.
    CHECK(IntermittentLaw(r.intermittent, 1e-4) < 1e-6);
    CHECK(IntermittentLaw(r.intermittent, 0.1) < 0.2);
  }
}

TEST_CASE("A carry turns the next wheel one digit per turn, and a locked "
          "wheel says so",
          "[assembly][relations][intermittent]") {
  Rig r = MakeRig();
  const uint64_t units = r.Add("units", 2, MateType::Revolute);
  const uint64_t transfer = r.Add("transfer", 3, MateType::Revolute);
  const uint64_t tens = r.Add("tens", 4, MateType::Revolute);
  const uint64_t carry = r.d.AddRelation(Geneva(units, transfer, 3, -12.0));
  r.d.AddRelation(Gear(transfer, tens, -9.0 / 30.0));
  const auto turn = [&](uint64_t mate) {
    return Deg(r.d.FindMateById(mate)->rotation);
  };
  const auto drive = [&](double deg) {
    assembly::SetMateDof(*r.d.FindMateById(units), MateDof::Rotation, Rad(deg));
    assembly::Solve(r.d, NoParts(), nullptr, {units, MateDof::Rotation});
  };
  // Counting is the units turning back 36 degrees a digit; the window, 60
  // degrees centred between 9 (36) and 0 (0), steps the transfer +120 and
  // the tens -36.
  drive(-180.0);
  CHECK_THAT(turn(tens), WithinAbs(0.0, 1e-9));
  drive(-360.0 + 72.0); // 08: before the window, still
  CHECK_THAT(turn(tens), WithinAbs(0.0, 1e-9));
  drive(-360.0 - 72.0); // 12: after it, one digit on
  CHECK_THAT(turn(tens), WithinAbs(-36.0, 1e-9));
  // At 9 and 0 the next wheel is a degree off its digit: the Geneva's law
  // has done 97 % of the step by then. 29 reads 2, a degree on toward 3;
  // 30 reads 3, a degree short of it.
  drive(-360.0 * 3.0 + 36.0);
  CHECK(std::abs(turn(tens) - (-36.0 * 2.0)) < 1.1);
  CHECK(std::abs(turn(tens) - (-36.0 * 2.0)) > 0.5);
  drive(-360.0 * 3.0);
  CHECK(std::abs(turn(tens) - (-36.0 * 3.0)) < 1.1);
  // Ninety-nine turns on: 995.
  drive(-360.0 * 99.0 - 180.0);
  CHECK_THAT(turn(tens), WithinAbs(-36.0 * 99.0, 1e-6));
  for (const Relation& rel : r.d.GetRelations())
    CHECK(rel.error.empty());

  // The tens wheel turned on its own: the gear drives the transfer back,
  // the carry is never inverted (a locked wheel says nothing of where its
  // driver stands), so the units stay and the carry reports it.
  const double unitsBefore = turn(units);
  assembly::SetMateDof(*r.d.FindMateById(tens), MateDof::Rotation,
                       r.d.FindMateById(tens)->rotation + Rad(3.0));
  assembly::Solve(r.d, NoParts(), nullptr, {tens, MateDof::Rotation});
  CHECK_THAT(turn(units), WithinAbs(unitsBefore, 1e-9));
  CHECK_THAT(r.d.FindRelationById(carry)->error,
             Catch::Matchers::ContainsSubstring("not satisfied"));
}

TEST_CASE("An intermittent relation round-trips, refuses what it does not "
          "know, and the v174 golden counts",
          "[assembly][relations][intermittent][golden]") {
  Rig r = MakeRig();
  const uint64_t a = r.Add("units", 2, MateType::Revolute);
  const uint64_t b = r.Add("slide", 3, MateType::Slider);
  Relation u;
  u.kind = RelationKind::Intermittent;
  u.mateA = a;
  u.mateB = b;
  u.dofA = MateDof::Rotation;
  u.dofB = MateDof::Offset;
  u.ratio = 1.0;
  u.phase = 0.5;
  u.intermittent = {2.5, 180.0, 10.0, 30.0, 0};
  r.d.AddRelation(u);
  r.d.AddRelation(Geneva(a, a, 4, -12.0)); // shape only: never solved
  const fs::path tmp = fs::temp_directory_path() / "intermittent_rt.json";
  REQUIRE(DocumentIO::SaveAssembly(r.d, tmp.string()).has_value());
  auto back = DocumentIO::LoadAssembly(tmp.string());
  REQUIRE(back.has_value());
  const Relation& ub = (*back)->GetRelations()[0];
  CHECK(ub.kind == RelationKind::Intermittent);
  CHECK(ub.dofB == MateDof::Offset);
  CHECK_THAT(ub.intermittent.step, WithinAbs(2.5, 1e-12));
  CHECK_THAT(ub.intermittent.periodDeg, WithinAbs(180.0, 1e-12));
  CHECK_THAT(ub.intermittent.startDeg, WithinAbs(10.0, 1e-12));
  CHECK_THAT(ub.intermittent.durationDeg, WithinAbs(30.0, 1e-12));
  CHECK(ub.intermittent.slots == 0);
  CHECK_THAT(ub.phase, WithinAbs(0.5, 1e-12));
  const Relation& gb = (*back)->GetRelations()[1];
  CHECK(gb.intermittent.slots == 4);
  CHECK_THAT(gb.intermittent.step, WithinAbs(-90.0, 1e-12));
  fs::remove(tmp);

  // An unknown motion is refused naming both; a Geneva needs three slots.
  nlohmann::json bad = DocumentIO::RelationToJson(gb);
  bad["motion"] = "genva";
  CHECK_THROWS_WITH(
      DocumentIO::RelationFromJson(bad),
      Catch::Matchers::ContainsSubstring("uniform, geneva or table"));
  bad["motion"] = "geneva";
  bad["slots"] = 2;
  CHECK_THROWS(DocumentIO::RelationFromJson(bad));
  bad = DocumentIO::RelationToJson(ub);
  bad.erase("start_deg");
  CHECK_THROWS_WITH(DocumentIO::RelationFromJson(bad),
                    Catch::Matchers::ContainsSubstring("start_deg"));

  // The golden: a units wheel ten and a half turns back, a Geneva of three
  // slots and a 9:30 gear — the tens wheel ten digits back, exactly.
  const fs::path golden =
      fs::path(TestSourceDir()) /
      "tests/golden/assembly/v174-intermittent-relation.json";
  auto gd = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(gd.has_value());
  assembly::Solve(**gd, NoParts());
  for (const Relation& rel : (*gd)->GetRelations())
    CHECK(rel.error.empty());
  CHECK_THAT(Deg((*gd)->FindMateById(6)->rotation), WithinAbs(1200.0, 1e-6));
  CHECK_THAT(Deg((*gd)->FindMateById(7)->rotation), WithinAbs(-360.0, 1e-6));
}

TEST_CASE("A table law follows its points, and a table that is not a law is "
          "refused",
          "[assembly][relations][intermittent]") {
  Relation r;
  r.kind = RelationKind::Intermittent;
  r.dofA = r.dofB = MateDof::Rotation;
  r.intermittent.step = 36.0;
  r.intermittent.durationDeg = 36.0;
  r.intermittent.table = {{0.0, 0.0}, {0.5, 0.2}, {1.0, 1.0}};
  const auto at = [&](double deg) {
    return Deg(IntermittentValue(r, Rad(deg)));
  };
  // Linear between the points: a fifth of the step at half the window,
  // the rest in the second half; whole steps outside it, as any carry.
  CHECK_THAT(at(9.0), WithinAbs(3.6, 1e-9));
  CHECK_THAT(at(18.0), WithinAbs(7.2, 1e-9));
  CHECK_THAT(at(27.0), WithinAbs(21.6, 1e-9));
  CHECK_THAT(at(36.0), WithinAbs(36.0, 1e-9));
  CHECK_THAT(at(200.0), WithinAbs(36.0, 1e-9));
  CHECK_THAT(at(360.0 + 9.0), WithinAbs(39.6, 1e-9));

  using T = std::vector<std::pair<double, double>>;
  CHECK(IntermittentTableError(r.intermittent.table).empty());
  CHECK_THAT(IntermittentTableError(T{{0, 0}}),
             Catch::Matchers::ContainsSubstring("two"));
  CHECK_THAT(IntermittentTableError(T{{0, 0}, {0.5, 0.6}, {1, 0.9}}),
             Catch::Matchers::ContainsSubstring("ends at (1, 1)"));
  CHECK_THAT(IntermittentTableError(T{{0.1, 0}, {1, 1}}),
             Catch::Matchers::ContainsSubstring("starts at (0, 0)"));
  CHECK_THAT(IntermittentTableError(T{{0, 0}, {0.5, 0.6}, {0.7, 0.4}, {1, 1}}),
             Catch::Matchers::ContainsSubstring("monotone"));
  CHECK_THAT(IntermittentTableError(T{{0, 0}, {0.5, 0.2}, {0.5, 0.4}, {1, 1}}),
             Catch::Matchers::ContainsSubstring("rise"));

  // The file: written as motion table with its points, read back; a table
  // that is not a law, a table motion without one and a table under
  // another motion are refused on load.
  r.mateA = 1;
  r.mateB = 2;
  r.ratio = 1.0;
  const nlohmann::json j = DocumentIO::RelationToJson(r);
  CHECK(j.at("motion") == "table");
  const Relation back = DocumentIO::RelationFromJson(j);
  CHECK(back.intermittent.table == r.intermittent.table);
  CHECK(back.intermittent.slots == 0);
  nlohmann::json bad = j;
  bad["table"] = {{0, 0}, {0.6, 0.5}, {0.4, 0.7}, {1, 1}};
  CHECK_THROWS_WITH(DocumentIO::RelationFromJson(bad),
                    Catch::Matchers::ContainsSubstring("rise"));
  bad = j;
  bad.erase("table");
  CHECK_THROWS_WITH(DocumentIO::RelationFromJson(bad),
                    Catch::Matchers::ContainsSubstring("needs its `table`"));
  bad = j;
  bad["motion"] = "uniform";
  CHECK_THROWS_WITH(DocumentIO::RelationFromJson(bad),
                    Catch::Matchers::ContainsSubstring("table motion's"));

  // The golden: the v174 carry, its law a table, counts the same digits.
  const fs::path golden = fs::path(TestSourceDir()) /
                          "tests/golden/assembly/v184-intermittent-table.json";
  auto gd = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(gd.has_value());
  const Relation& carry = (*gd)->GetRelations()[0];
  REQUIRE(carry.intermittent.table.size() == 4);
  CHECK_THAT(IntermittentLaw(carry.intermittent, 0.5), WithinAbs(0.5, 1e-12));
  assembly::Solve(**gd, NoParts());
  for (const Relation& rel : (*gd)->GetRelations())
    CHECK(rel.error.empty());
  CHECK_THAT(Deg((*gd)->FindMateById(6)->rotation), WithinAbs(1200.0, 1e-6));
  CHECK_THAT(Deg((*gd)->FindMateById(7)->rotation), WithinAbs(-360.0, 1e-6));
}
