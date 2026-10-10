// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Flexible sub-assemblies (v129,
// docs/assemblies/README.md#flexible-sub-assemblies): a placed sub-assembly
// whose own mechanism moves in its parent — driven from the parent by a
// relation into its inner mate, posed per placement, checked leaf by leaf —
// while the sub-assembly's shared document is never written.

#include "TestPaths.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <memory>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint64_t kSubSlot = 100;

Connector Origin(uint64_t occ) {
  Connector c;
  c.occurrenceId = occ;
  c.anchor.origin = true;
  return c;
}

std::unique_ptr<Document> BoxPart(const glm::dvec3& lo,
                                  const glm::dvec3& size) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", lo, size.x, size.y, size.z));
  doc->Regenerate();
  return doc;
}

// The sub-assembly: a HOUSING (fixed) and a CRANK on a revolute at its
// origin — a 28 mm bar along +X — and, optionally, a STOP fixed in the
// housing on the crank's circle at 90 degrees. The parent: a FRAME (fixed),
// a DRIVE on a revolute, and the unit placed at (100, 0, 0), flexible,
// its crank chained 1:1 to the drive.
struct Rig {
  std::unique_ptr<Document> empty, crank, stop, housing, wall;
  AssemblyDocument sub, top;
  uint64_t housingOcc = 0, crankOcc = 0, stopOcc = 0, crankMate = 0;
  uint64_t frameOcc = 0, driveOcc = 0, unitOcc = 0, drive = 0, chain = 0;

  assembly::PartResolver Parts() {
    return [this](uint64_t id, uint64_t configuration) -> const Document* {
      switch (id) {
      case 1:
      case 2:
        return empty.get();
      case 10:
        return housing.get();
      case 11:
        return crank.get();
      case 12:
        return stop.get();
      case 13:
        return wall.get();
      default:
        return nullptr;
      }
    };
  }
  assembly::AssemblyResolver Subs() {
    return [this](uint64_t id) -> const AssemblyDocument* {
      return id == kSubSlot ? &sub : nullptr;
    };
  }
  const Occurrence& Unit() const { return *top.FindById(unitOcc); }
};

uint64_t PlaceUnit(Rig& r, glm::dvec3 at, bool flexible, std::string name) {
  Occurrence u;
  u.name = std::move(name);
  u.partId = kSubSlot;
  u.refKind = OccurrenceRef::Assembly;
  u.fixed = true;
  u.flexible = flexible;
  u.position = at;
  return r.top.AddOccurrence(std::move(u));
}

Rig MakeRig(bool withStop = false, bool withChain = true) {
  Rig r;
  r.empty = std::make_unique<Document>();
  r.crank = BoxPart({2, -2, -2}, {28, 4, 4});
  r.stop = BoxPart({-2, -2, -2}, {4, 4, 4});
  r.housing = BoxPart({-40, -5, -5}, {8, 10, 10});
  r.wall = BoxPart({-2, -2, -2}, {4, 4, 4});

  Occurrence h;
  h.name = "Housing";
  h.partId = 10;
  h.fixed = true;
  r.housingOcc = r.sub.AddOccurrence(std::move(h));
  Occurrence c;
  c.name = "Crank";
  c.partId = 11;
  r.crankOcc = r.sub.AddOccurrence(std::move(c));
  if (withStop) {
    Occurrence s;
    s.name = "Stop";
    s.partId = 12;
    s.fixed = true;
    s.position = glm::dvec3(0.0, 20.0, 0.0); // on the circle at 90 deg
    r.stopOcc = r.sub.AddOccurrence(std::move(s));
  }
  Mate cm;
  cm.type = MateType::Revolute;
  cm.name = "crank";
  cm.a = Origin(r.housingOcc);
  cm.b = Origin(r.crankOcc);
  cm.flipped = false;
  r.crankMate = r.sub.AddMate(cm);

  Occurrence f;
  f.name = "Frame";
  f.partId = 1;
  f.fixed = true;
  r.frameOcc = r.top.AddOccurrence(std::move(f));
  Occurrence d;
  d.name = "Drive";
  d.partId = 2;
  r.driveOcc = r.top.AddOccurrence(std::move(d));
  Mate dm;
  dm.type = MateType::Revolute;
  dm.name = "drive";
  dm.a = Origin(r.frameOcc);
  dm.b = Origin(r.driveOcc);
  dm.flipped = false;
  r.drive = r.top.AddMate(dm);
  r.unitOcc = PlaceUnit(r, {100, 0, 0}, true, "Unit");
  if (withChain) {
    Relation chain;
    chain.kind = RelationKind::Chain;
    chain.name = "drive to crank";
    chain.mateA = r.drive;
    chain.mateB = r.crankMate;
    chain.mateBPath = {r.unitOcc};
    chain.ratio = 1.0;
    r.chain = r.top.AddRelation(chain);
  }
  return r;
}

// Where the crank's tip is, in the world, per the flattened tree.
glm::dvec3 CrankTip(Rig& r, uint64_t unit) {
  auto flat = assembly::Flatten(r.top, r.Parts(), r.Subs());
  REQUIRE(flat.has_value());
  for (const assembly::FlatInstance& inst : *flat)
    if (inst.rootOccurrenceId == unit && inst.partId == 11)
      return glm::dvec3(inst.world * glm::dvec4(30, 0, 0, 1));
  FAIL("no crank leaf under the unit");
  return {};
}

void Drive(Rig& r, double radians) {
  assembly::SetMateDof(*r.top.FindMateById(r.drive), MateDof::Rotation,
                       radians);
  const assembly::SolveReport rep =
      assembly::Solve(r.top, r.Parts(), r.Subs(), {r.drive, MateDof::Rotation});
  CHECK(rep.erroredMates == 0);
}

} // namespace

TEST_CASE("A parent relation turns a flexible unit's inner mate",
          "[assembly][solver]") {
  Rig r = MakeRig();
  // A RIGID placement of the same sub-assembly beside it.
  const uint64_t rigid = PlaceUnit(r, {0, 100, 0}, false, "Rigid unit");
  Drive(r, kPi / 2.0);

  // The crank turned a quarter, about the unit's own axis at (100, 0, 0).
  const glm::dvec3 tip = CrankTip(r, r.unitOcc);
  CHECK_THAT(tip.x, WithinAbs(100.0, 1e-3));
  CHECK_THAT(tip.y, WithinAbs(30.0, 1e-3));
  const InnerMateDof* d = r.Unit().FindInnerMateDof(r.crankMate);
  REQUIRE(d != nullptr);
  CHECK_THAT(d->rotation, WithinAbs(kPi / 2.0, 1e-5));
  REQUIRE(r.Unit().FindInnerPose(r.crankOcc) != nullptr);
  CHECK(r.Unit().FindInnerPose(r.housingOcc) == nullptr); // fixed: folds in
  CHECK(r.top.FindRelationById(r.chain)->error.empty());

  // The rigid placement did not move, and the shared document was not
  // written: its crank mate still stands where it was authored.
  const glm::dvec3 still = CrankTip(r, rigid);
  CHECK_THAT(still.x, WithinAbs(30.0, 1e-3));
  CHECK_THAT(still.y, WithinAbs(100.0, 1e-3));
  CHECK(r.sub.FindMateById(r.crankMate)->rotation == 0.0);
  CHECK(r.sub.FindById(r.crankOcc)->orientation ==
        glm::dquat(1.0, 0.0, 0.0, 0.0));

  // Solving again moves nothing: the poses re-solve to themselves.
  const std::vector<InnerPose> before = r.Unit().innerPoses;
  assembly::Solve(r.top, r.Parts(), r.Subs());
  CHECK_THAT(CrankTip(r, r.unitOcc).y, WithinAbs(30.0, 1e-3));
  REQUIRE(r.Unit().innerPoses.size() == before.size());
}

TEST_CASE("Two flexible placements of one sub-assembly pose independently",
          "[assembly][solver]") {
  Rig r = MakeRig(false, /*withChain=*/false);
  const uint64_t second = PlaceUnit(r, {0, 100, 0}, true, "Second unit");
  const std::vector<uint64_t> p1{r.unitOcc}, p2{second};
  REQUIRE(assembly::SetMateDofAt(r.top, p1, r.crankMate, MateDof::Rotation,
                                 kPi / 6.0, r.Subs()));
  assembly::Solve(r.top, r.Parts(), r.Subs(),
                  {r.crankMate, MateDof::Rotation, p1});
  // The undo snapshot of the parent is all the state there is.
  const AssemblyDocument snapshot = r.top;
  REQUIRE(assembly::SetMateDofAt(r.top, p2, r.crankMate, MateDof::Rotation,
                                 kPi / 3.0, r.Subs()));
  assembly::Solve(r.top, r.Parts(), r.Subs(),
                  {r.crankMate, MateDof::Rotation, p2});

  const glm::dvec3 a = CrankTip(r, r.unitOcc) - glm::dvec3(100, 0, 0);
  const glm::dvec3 b = CrankTip(r, second) - glm::dvec3(0, 100, 0);
  CHECK_THAT(std::atan2(a.y, a.x), WithinAbs(kPi / 6.0, 1e-4)); // no fighting
  CHECK_THAT(std::atan2(b.y, b.x), WithinAbs(kPi / 3.0, 1e-4));
  CHECK_THAT(
      *assembly::MateDofAt(r.top, p2, r.crankMate, MateDof::Rotation, r.Subs()),
      WithinAbs(kPi / 3.0, 1e-5));

  // Undo: the snapshot brings the second unit back to where it stood.
  r.top = snapshot;
  const glm::dvec3 back = CrankTip(r, second) - glm::dvec3(0, 100, 0);
  CHECK_THAT(std::atan2(back.y, back.x), WithinAbs(0.0, 1e-4));
  const glm::dvec3 kept = CrankTip(r, r.unitOcc) - glm::dvec3(100, 0, 0);
  CHECK_THAT(std::atan2(kept.y, kept.x), WithinAbs(kPi / 6.0, 1e-4));
}

TEST_CASE("A flexible unit runs under its own configuration",
          "[assembly][solver][motion]") {
  Rig r = MakeRig(false, /*withChain=*/false);
  Configuration& held = r.sub.AddConfiguration("Held");
  held.mateOverrides.push_back({r.crankMate, kPi / 4.0, std::nullopt});
  const uint64_t heldId = held.id;
  r.top.FindById(r.unitOcc)->configurationId = heldId;
  assembly::Solve(r.top, r.Parts(), r.Subs());
  const glm::dvec3 a = CrankTip(r, r.unitOcc) - glm::dvec3(100, 0, 0);
  CHECK_THAT(std::atan2(a.y, a.x), WithinAbs(kPi / 4.0, 1e-4));
  // The sub's own active configuration is untouched: the placement chose.
  CHECK(r.sub.ActiveConfiguration() == 0);

  // A study cannot drive what that configuration holds.
  MotionStudy s;
  s.driverMate = r.crankMate;
  s.driverPath = {r.unitOcc};
  s.from = 0.0;
  s.to = kPi;
  s.samples = 5;
  CHECK_THAT(motion::WhyNot(r.top, s, r.Subs()), ContainsSubstring("held"));
  // Under the base it can.
  r.top.FindById(r.unitOcc)->configurationId = 0;
  CHECK(motion::WhyNot(r.top, s, r.Subs()).empty());
}

TEST_CASE("A motion study checks a flexible unit leaf by leaf",
          "[assembly][motion]") {
  Rig r = MakeRig(/*withStop=*/true);
  // A wall in the PARENT on the crank's circle at 180 degrees.
  Occurrence w;
  w.name = "Wall";
  w.partId = 13;
  w.fixed = true;
  w.position = glm::dvec3(80.0, 0.0, 0.0);
  const uint64_t wall = r.top.AddOccurrence(std::move(w));

  MotionStudy s;
  s.name = "turn";
  s.driverMate = r.drive;
  s.from = 0.0;
  s.to = 2.0 * kPi;
  s.samples = 25; // every 15 degrees
  s.clashMode = "exact";
  const motion::Result res = motion::Run(r.top, r.Parts(), r.Subs(), s);
  REQUIRE(res.error.empty());
  CHECK_FALSE(res.clean);

  bool wallHit = false, stopHit = false;
  for (const motion::Sample& sm : res.samples) {
    CHECK(sm.solved);
    // Every sample carries the crank's pose by path and its mate by path.
    bool leaf = false, mate = false;
    for (const auto& l : sm.leaves)
      leaf = leaf || (l.root == r.unitOcc && l.occurrence == r.crankOcc);
    for (const auto& m : sm.mates)
      mate = mate ||
             (m.path == std::vector<uint64_t>{r.unitOcc} &&
              m.mate == r.crankMate && std::abs(m.rotation - sm.value) < 1e-4);
    CHECK(leaf);
    CHECK(mate);
    const double deg = sm.value * 180.0 / kPi;
    for (const motion::Clash& c : sm.clashes) {
      // The inner moving part against a PARENT part.
      if ((c.a == wall || c.b == wall) &&
          (c.aInner == r.crankOcc || c.bInner == r.crankOcc)) {
        wallHit = true;
        CHECK_THAT(deg, WithinAbs(180.0, 20.0));
      }
      // ...and against its OWN housing's stop: both sides are the unit.
      if (c.a == r.unitOcc && c.b == r.unitOcc) {
        CHECK(((c.aInner == r.crankOcc && c.bInner == r.stopOcc) ||
               (c.aInner == r.stopOcc && c.bInner == r.crankOcc)));
        CHECK_FALSE(c.rigid);
        CHECK_THAT(c.aName, ContainsSubstring("Unit/"));
        stopHit = true;
        CHECK_THAT(deg, WithinAbs(90.0, 20.0));
      }
    }
  }
  CHECK(wallHit);
  CHECK(stopHit);
  // The stop and the housing fold into the unit, so they are rigid to each
  // other and to it: checked once, at the first sample.
  CHECK(res.rigidCheckedAt == 0);
  // The document handed in is not left at the last sample.
  CHECK(r.Unit().innerPoses.empty());
}

TEST_CASE("A study can drive a flexible unit's inner mate by path",
          "[assembly][motion]") {
  Rig r = MakeRig(false, /*withChain=*/true);
  MotionStudy s;
  s.driverMate = r.crankMate;
  s.driverPath = {r.unitOcc};
  s.from = 0.0;
  s.to = kPi;
  s.samples = 5;
  s.checkClash = false;
  const motion::Result res = motion::Run(r.top, r.Parts(), r.Subs(), s);
  REQUIRE(res.error.empty());
  // Driving the crank drives the parent's drive back through the chain.
  const motion::Sample& last = res.samples.back();
  bool driveTurned = false;
  for (const auto& m : last.mates)
    if (m.path.empty() && m.mate == r.drive)
      driveTurned = std::abs(m.rotation - kPi) < 1e-4;
  CHECK(driveTurned);
}

TEST_CASE("What cannot be flexible is refused, naming why", "[assembly]") {
  Rig r = MakeRig();
  Occurrence part;
  part.name = "A part";
  part.partId = 11;
  CHECK_THAT(assembly::FlexibleRefusal(part, r.Subs()),
             ContainsSubstring("places a part"));
  CHECK(assembly::FlexibleRefusal(r.Unit(), r.Subs()).empty());

  SECTION("a freewheel inside") {
    Relation fw;
    fw.kind = RelationKind::Freewheel;
    fw.name = "clutch";
    fw.mateA = fw.mateB = r.crankMate;
    r.sub.AddRelation(fw);
    CHECK_THAT(assembly::FlexibleRefusal(r.Unit(), r.Subs()),
               ContainsSubstring("freewheel"));
    // A solve reports it on the placement rather than moving it.
    const assembly::SolveReport rep =
        assembly::Solve(r.top, r.Parts(), r.Subs());
    bool reported = false;
    for (const auto& im : rep.innerMates)
      reported = reported || (im.occurrence == r.unitOcc && im.mate == 0 &&
                              im.error.find("freewheel") != std::string::npos);
    CHECK(reported);
    CHECK(rep.erroredMates > 0);
  }
  SECTION("nothing fixed inside") {
    r.sub.FindById(r.housingOcc)->fixed = false;
    CHECK_THAT(assembly::FlexibleRefusal(r.Unit(), r.Subs()),
               ContainsSubstring("nothing inside it is fixed"));
  }
  SECTION("flexible inside flexible") {
    r.sub.FindById(r.crankOcc)->flexible = true;
    CHECK_THAT(assembly::FlexibleRefusal(r.Unit(), r.Subs()),
               ContainsSubstring("only one"));
  }
  SECTION("a relation into a rigid placement") {
    r.top.FindById(r.unitOcc)->flexible = false;
    assembly::Solve(r.top, r.Parts(), r.Subs());
    CHECK_THAT(r.top.FindRelationById(r.chain)->error,
               ContainsSubstring("not flexible"));
  }
  SECTION("a relation into a mate that is not there") {
    r.top.FindRelationById(r.chain)->mateB = 999;
    assembly::Solve(r.top, r.Parts(), r.Subs());
    CHECK_THAT(r.top.FindRelationById(r.chain)->error,
               ContainsSubstring("not one of"));
  }
}

TEST_CASE("An origin anchor with an inner path is the origin it names",
          "[assembly][solver]") {
  // Found on the way (v129): ConnectorLocalFrame answered the origin case
  // before it walked the path, so it gave the UNIT's origin.
  Rig r = MakeRig(false, false);
  r.top.FindById(r.unitOcc)->flexible = false;
  r.sub.FindById(r.crankOcc)->position = glm::dvec3(5.0, 7.0, 0.0);
  Connector c = Origin(r.unitOcc);
  c.innerPath = {r.crankOcc};
  const auto f =
      assembly::ConnectorLocalFrame(c, r.top, r.Parts(), nullptr, r.Subs());
  REQUIRE(f.has_value());
  CHECK_THAT((*f)[3].x, WithinAbs(5.0, 1e-6));
  CHECK_THAT((*f)[3].y, WithinAbs(7.0, 1e-6));
}

TEST_CASE("The v129 golden: a flexible placement and mates named by path",
          "[assembly][io]") {
  const fs::path golden =
      fs::path(TestSourceDir()) /
      "tests/golden/assembly/v129-flexible-subassembly.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  const AssemblyDocument& doc = **g;
  const Occurrence* unit = doc.FindById(3);
  REQUIRE(unit != nullptr);
  CHECK(unit->flexible);
  CHECK(unit->configurationId == 9);
  REQUIRE(unit->innerPoses.size() == 1);
  CHECK(unit->innerPoses[0].occurrence == 2);
  REQUIRE(unit->FindInnerMateDof(7) != nullptr);
  CHECK_THAT(unit->FindInnerMateDof(7)->rotation, WithinAbs(0.5, 1e-6));
  const Relation* chain = doc.FindRelationById(5);
  REQUIRE(chain != nullptr);
  CHECK(chain->mateAPath.empty());
  CHECK(chain->mateBPath == std::vector<uint64_t>{3});
  const MotionStudy* s = doc.FindMotionStudyById(6);
  REQUIRE(s != nullptr);
  CHECK(s->driverPath == std::vector<uint64_t>{3});

  // Round trip: the same fields come back.
  const fs::path tmp =
      fs::temp_directory_path() / "designer-v129-roundtrip.json";
  REQUIRE(testio::SaveAssembly(doc, tmp.string()));
  auto back = DocumentIO::LoadAssembly(tmp.string());
  REQUIRE(back.has_value());
  CHECK((*back)->FindById(3)->flexible);
  CHECK((*back)->FindById(3)->innerPoses.size() == 1);
  CHECK((*back)->FindRelationById(5)->mateBPath == std::vector<uint64_t>{3});
  CHECK((*back)->FindMotionStudyById(6)->driverPath ==
        std::vector<uint64_t>{3});
  // A rigid occurrence carries no inner state, and saying otherwise is
  // refused on load rather than half-applied.
  CHECK_FALSE((*back)->FindById(1)->flexible);
  fs::remove(tmp);
}

TEST_CASE("The v130 golden: an explode step inside a flexible placement",
          "[assembly][io]") {
  const fs::path golden = fs::path(TestSourceDir()) /
                          "tests/golden/assembly/v130-explode-inner-part.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  const AssemblyDocument& doc = **g;
  const ExplodedView* v = doc.FindExplodedViewById(8);
  REQUIRE(v != nullptr);
  REQUIRE(v->steps.size() == 2);
  // The unit's own step names a top-level occurrence, with no path...
  CHECK(v->steps[0].occurrenceId == 3);
  CHECK(v->steps[0].path.empty());
  // ...and the crank's names the sub-assembly's occurrence by path, as a
  // mate inside a flexible placement is named.
  CHECK(v->steps[1].occurrenceId == 2);
  CHECK(v->steps[1].path == std::vector<uint64_t>{3});
  CHECK_THAT(v->steps[1].offset.z, WithinAbs(15.0, 1e-6));

  const fs::path tmp =
      fs::temp_directory_path() / "designer-v130-roundtrip.json";
  REQUIRE(testio::SaveAssembly(doc, tmp.string()));
  auto back = DocumentIO::LoadAssembly(tmp.string());
  REQUIRE(back.has_value());
  const ExplodedView* w = (*back)->FindExplodedViewById(8);
  REQUIRE(w != nullptr);
  REQUIRE(w->steps.size() == 2);
  CHECK(w->steps[0].path.empty());
  CHECK(w->steps[1].path == std::vector<uint64_t>{3});
  fs::remove(tmp);

  // A path more than one level down is refused on load, not half-applied:
  // only one level moves.
  nlohmann::json j = DocumentIO::ExplodedViewToJson(*v);
  j["steps"][1]["path"] = {3, 2};
  CHECK_THROWS_WITH(DocumentIO::ExplodedViewFromJson(j),
                    ContainsSubstring("one flexible placement"));
}

TEST_CASE("Relations over several mates cross into a flexible unit and move "
          "inside one",
          "[assembly][solver][relations]") {
  // v171: a parent's linear_n names the unit's crank by path, and the
  // sub-assembly's own linear_n couples its crank to a second crank — both
  // expanded with the placement, every member remapped.
  Rig r = MakeRig(false, /*withChain=*/false);
  Occurrence c2;
  c2.name = "Second crank";
  c2.partId = 11;
  const uint64_t crank2Occ = r.sub.AddOccurrence(std::move(c2));
  Mate m2;
  m2.type = MateType::Revolute;
  m2.name = "crank 2";
  m2.a = Origin(r.housingOcc);
  m2.b = Origin(crank2Occ);
  m2.flipped = false;
  const uint64_t crank2 = r.sub.AddMate(m2);
  const auto term = [](uint64_t mate, double c, std::vector<uint64_t> path) {
    RelationTerm t;
    t.mate = mate;
    t.coefficient = c;
    t.path = std::move(path);
    return t;
  };
  Relation inner;
  inner.kind = RelationKind::LinearN;
  inner.terms = {term(r.crankMate, 1.0, {}), term(crank2, 1.0, {})};
  r.sub.AddRelation(inner); // crank2 = -crank
  Relation outer;
  outer.kind = RelationKind::LinearN;
  outer.terms = {term(r.drive, 2.0, {}), term(r.crankMate, -1.0, {r.unitOcc})};
  const uint64_t rel = r.top.AddRelation(outer); // crank = 2 * drive
  CHECK_FALSE(r.top.RelationSuppressed(rel));

  Drive(r, 0.25);
  CHECK(r.top.FindRelationById(rel)->error.empty());
  const std::vector<uint64_t> path{r.unitOcc};
  CHECK_THAT(*assembly::MateDofAt(r.top, path, r.crankMate, MateDof::Rotation,
                                  r.Subs()),
             WithinAbs(0.5, 1e-6));
  CHECK_THAT(
      *assembly::MateDofAt(r.top, path, crank2, MateDof::Rotation, r.Subs()),
      WithinAbs(-0.5, 1e-6));

  // A configuration hiding the unit takes the parent's relation out with it.
  Configuration& cfg = r.top.AddConfiguration("No unit");
  cfg.suppressedFeatures = {r.unitOcc};
  REQUIRE(r.top.SetActiveConfiguration(cfg.id));
  CHECK(r.top.RelationSuppressed(rel));
}
