// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Motion studies (docs/assemblies/README.md#motion-studies): one mate DOF
// driven over a range, the assembly solved at every sample, every pair checked.
// The gate: "rotate through the range, confirm zero collision at
// every step, print the table" — and the honesty the table owes: a sample
// that does not solve is reported unsolved, never drawn as the last good
// pose.

#include "TestPaths.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "TestIO.h"
#include "core/AssemblySolve.h"
#include "core/CollisionCache.h"
#include "core/Document.h"
#include "core/InputHash.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/features/BearingFeature.h"
#include "core/io/GlbAnimation.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

Connector Origin(uint64_t occ) {
  Connector c;
  c.occurrenceId = occ;
  c.anchor.origin = true;
  return c;
}

// A carrier (fixed, an empty part), an ARM on a revolute at the origin —
// a 30 mm bar reaching out along +X — and a STOP: a small block fixed on
// the arm's circle at the angle `stopDeg`. Sweeping the arm through a turn
// must clash with the stop near that angle and nowhere else.
struct Rig {
  std::unique_ptr<Document> carrier, arm, stop;
  AssemblyDocument asmDoc;
  uint64_t carrierOcc = 0, armOcc = 0, stopOcc = 0, axle = 0;
  assembly::PartResolver Parts() {
    return [this](uint64_t id, uint64_t configuration) -> const Document* {
      if (id == 1)
        return carrier.get();
      if (id == 2)
        return arm.get();
      if (id == 3)
        return stop.get();
      return nullptr;
    };
  }
};

std::unique_ptr<Document> BoxPart(const glm::vec3& lo, const glm::vec3& size) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", lo, size.x, size.y, size.z));
  doc->Regenerate();
  return doc;
}

Rig MakeRig(double stopDeg, bool withStop = true) {
  Rig r;
  r.carrier = std::make_unique<Document>();
  // The arm: x from 2 to 30, 4 wide, 4 tall, centred on the axle's Z.
  r.arm = BoxPart({2, -2, -2}, {28, 4, 4});
  r.stop = BoxPart({-2, -2, -2}, {4, 4, 4});

  Occurrence c;
  c.name = "Carrier";
  c.partId = 1;
  c.fixed = true;
  r.carrierOcc = r.asmDoc.AddOccurrence(std::move(c));
  Occurrence a;
  a.name = "Arm";
  a.partId = 2;
  r.armOcc = r.asmDoc.AddOccurrence(std::move(a));
  if (withStop) {
    Occurrence s;
    s.name = "Stop";
    s.partId = 3;
    s.fixed = true;
    const double rad = stopDeg * kPi / 180.0;
    s.position = glm::vec3(20.0 * std::cos(rad), 20.0 * std::sin(rad), 0.0);
    r.stopOcc = r.asmDoc.AddOccurrence(std::move(s));
  }
  Mate m;
  m.type = MateType::Revolute;
  m.name = "axle";
  m.a = Origin(r.carrierOcc);
  m.b = Origin(r.armOcc);
  m.flipped = false;
  r.axle = r.asmDoc.AddMate(m);
  return r;
}

MotionStudy FullTurn(uint64_t axle, int samples = 25) {
  MotionStudy s;
  s.name = "turn";
  s.driverMate = axle;
  s.driverDof = MateDof::Rotation;
  s.from = 0.0;
  s.to = 2.0 * kPi;
  s.samples = samples;
  return s;
}

} // namespace

TEST_CASE("A motion study samples the driver and finds the clash", "[motion]") {
  Rig r = MakeRig(90.0);
  const MotionStudy study = FullTurn(r.axle, 25); // every 15 degrees
  const motion::Result res = motion::Run(r.asmDoc, r.Parts(), nullptr, study);
  INFO(res.error);
  REQUIRE(res.error.empty());
  REQUIRE(res.samples.size() == 25);
  // The driver value walks the range, `to` inclusive.
  CHECK_THAT(res.samples[0].value, WithinAbs(0.0, 1e-9));
  CHECK_THAT(res.samples[24].value, WithinAbs(2.0 * kPi, 1e-9));
  CHECK_THAT(res.samples[6].value, WithinAbs(kPi / 2.0, 1e-9)); // 90 deg
  for (const motion::Sample& s : res.samples)
    CHECK(s.solved);

  // The stop sits at 90 degrees, 4 mm wide on a 20 mm radius: the arm hits
  // it at 90 and at the neighbouring 15-degree steps it grazes (the arm is
  // 4 mm tall; at 75 degrees its far end is 5 mm off the stop's centre).
  std::vector<int> hits;
  for (size_t i = 0; i < res.samples.size(); i++)
    if (!res.samples[i].clashes.empty())
      hits.push_back(static_cast<int>(i));
  INFO(motion::Table(res, study));
  REQUIRE(!hits.empty());
  CHECK(std::find(hits.begin(), hits.end(), 6) != hits.end());
  for (int i : hits)
    CHECK((i >= 4 && i <= 8)); // nothing on the far side of the circle
  CHECK_FALSE(res.clean);
  CHECK(res.worstSample == 6);
  const motion::Clash& c = res.samples[6].clashes[0];
  CHECK(((c.aName == "Arm" && c.bName == "Stop") ||
         (c.aName == "Stop" && c.bName == "Arm")));
  // The table names the sample, the angle in degrees, and the pair.
  const std::string table = motion::Table(res, study);
  CHECK_THAT(table, ContainsSubstring("90.00") && ContainsSubstring("Arm") &&
                        ContainsSubstring("NOT CLEAN"));
  // The document handed in was not moved: the run drove a COPY.
  CHECK_THAT(r.asmDoc.FindMateById(r.axle)->rotation, WithinAbs(0.0, 1e-9));
}

TEST_CASE("A study with nothing in the way is clean, in both modes",
          "[motion]") {
  Rig r = MakeRig(0.0, /*withStop=*/false);
  MotionStudy study = FullTurn(r.axle, 13);
  for (const char* mode : {"mesh", "exact"}) {
    study.clashMode = mode;
    const motion::Result res = motion::Run(r.asmDoc, r.Parts(), nullptr, study);
    INFO(mode << ": " << res.error);
    CHECK(res.clean);
    CHECK(res.samples.size() == 13);
    CHECK(res.pairsChecked == 0); // only one moving body: nothing to pair
  }
}

TEST_CASE("The exact and mesh checks agree on the rig", "[motion]") {
  Rig r = MakeRig(90.0);
  MotionStudy study = FullTurn(r.axle, 25);
  study.clashMode = "mesh";
  const motion::Result mesh = motion::Run(r.asmDoc, r.Parts(), nullptr, study);
  study.clashMode = "exact";
  const motion::Result exact = motion::Run(r.asmDoc, r.Parts(), nullptr, study);
  REQUIRE(mesh.samples.size() == exact.samples.size());
  for (size_t i = 0; i < mesh.samples.size(); i++) {
    INFO("sample " << i);
    CHECK(mesh.samples[i].clashes.size() == exact.samples[i].clashes.size());
  }
  CHECK(mesh.deflectionMm > 0.0);
  CHECK(exact.deflectionMm == 0.0);
}

TEST_CASE("A study refuses a driver it cannot drive, naming the mates",
          "[motion]") {
  Rig r = MakeRig(0.0, false);
  SECTION("a mate that is gone") {
    MotionStudy s = FullTurn(4242);
    CHECK_THAT(motion::WhyNot(r.asmDoc, s),
               ContainsSubstring("no longer exists") &&
                   ContainsSubstring("axle"));
    const motion::Result res = motion::Run(r.asmDoc, r.Parts(), nullptr, s);
    CHECK(!res.error.empty());
    CHECK(res.samples.empty());
  }
  SECTION("a DOF the mate does not have") {
    MotionStudy s = FullTurn(r.axle);
    s.driverDof = MateDof::Offset; // a revolute does not slide
    CHECK_THAT(motion::WhyNot(r.asmDoc, s),
               ContainsSubstring("no offset") && ContainsSubstring("slider"));
  }
  SECTION("too few samples") {
    MotionStudy s = FullTurn(r.axle, 1);
    CHECK_THAT(motion::WhyNot(r.asmDoc, s), ContainsSubstring("2 samples"));
  }
}

TEST_CASE("A motion study round-trips, and the v87 golden loads",
          "[motion][golden]") {
  Rig r = MakeRig(90.0);
  MotionStudy s = FullTurn(r.axle, 49);
  s.name = "crank";
  s.clashMode = "exact";
  s.checkClash = true;
  const uint64_t id = r.asmDoc.AddMotionStudy(s);

  const fs::path tmp = fs::temp_directory_path() / "motion_rt.json";
  REQUIRE(DocumentIO::SaveAssembly(r.asmDoc, tmp.string()).has_value());
  auto back = DocumentIO::LoadAssembly(tmp.string());
  REQUIRE(back.has_value());
  REQUIRE((*back)->GetMotionStudies().size() == 1);
  const MotionStudy& b = (*back)->GetMotionStudies()[0];
  CHECK(b.id == id);
  CHECK(b.name == "crank");
  CHECK(b.driverMate == r.axle);
  CHECK(b.driverDof == MateDof::Rotation);
  CHECK_THAT(b.to, WithinAbs(2.0 * kPi, 1e-12));
  CHECK(b.samples == 49);
  CHECK(b.clashMode == "exact");
  // The next id is pinned past it, like every other collection.
  CHECK((*back)->AddMotionStudy(FullTurn(r.axle)) > id);
  fs::remove(tmp);

  // An unknown mode is refused, not demoted.
  nlohmann::json bad = DocumentIO::MotionStudyToJson(s);
  bad["clash_mode"] = "fast";
  CHECK_THROWS_WITH(DocumentIO::MotionStudyFromJson(bad),
                    ContainsSubstring("mesh or exact"));

  // The golden: written by this recipe (DESIGNER_WRITE_GOLDEN=<path>), and
  // loaded here every time.
  if (const char* out = std::getenv("DESIGNER_WRITE_GOLDEN"))
    REQUIRE(DocumentIO::SaveAssembly(r.asmDoc, out).has_value());
  const fs::path golden =
      fs::path(TestSourceDir()) / "tests/golden/assembly/v87-motion-study.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->GetMotionStudies().size() == 1);
  CHECK((*g)->GetMotionStudies()[0].samples == 49);
  // ...and it still RUNS against the same parts: a golden that only parses
  // is half a golden.
  const motion::Result res =
      motion::Run(**g, r.Parts(), nullptr, (*g)->GetMotionStudies()[0]);
  REQUIRE(res.error.empty());
  CHECK(res.samples.size() == 49);
  CHECK_FALSE(res.clean); // the stop is still at 90 degrees
}

TEST_CASE("The GLB writer emits a valid animated scene", "[motion][export]") {
  Rig r = MakeRig(0.0, false);
  const MotionStudy study = FullTurn(r.axle, 5);
  const motion::Result res = motion::Run(r.asmDoc, r.Parts(), nullptr, study);
  REQUIRE(res.samples.size() == 5);

  glb::Node arm;
  arm.name = "Arm";
  glb::AppendShape(arm.mesh, r.arm->GetBodies()[0].shape, glm::mat4(1.0f));
  REQUIRE(!arm.mesh.positions.empty());
  std::vector<double> times;
  for (const motion::Sample& s : res.samples) {
    times.push_back(s.t);
    for (const auto& [occ, m] : s.worlds)
      if (occ == r.armOcc)
        arm.keyframes.push_back(m);
  }
  REQUIRE(arm.keyframes.size() == 5);

  const fs::path path = fs::temp_directory_path() / "motion_rt.glb";
  std::string err;
  REQUIRE(glb::Write(path.string(), {arm}, times, &err));

  // The container: magic, version 2, a JSON chunk then a BIN chunk, and a
  // JSON index that names one animation with two channels for the node.
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  uint32_t magic = 0, version = 0, total = 0, jsonLen = 0, jsonType = 0;
  in.read(reinterpret_cast<char*>(&magic), 4);
  in.read(reinterpret_cast<char*>(&version), 4);
  in.read(reinterpret_cast<char*>(&total), 4);
  in.read(reinterpret_cast<char*>(&jsonLen), 4);
  in.read(reinterpret_cast<char*>(&jsonType), 4);
  CHECK(magic == 0x46546C67u);
  CHECK(version == 2u);
  CHECK(jsonType == 0x4E4F534Au);
  CHECK(total == fs::file_size(path));
  std::string text(jsonLen, ' ');
  in.read(text.data(), jsonLen);
  const nlohmann::json doc = nlohmann::json::parse(text);
  CHECK(doc.at("asset").at("version") == "2.0");
  REQUIRE(doc.at("animations").size() == 1);
  CHECK(doc.at("animations")[0].at("channels").size() == 2);
  CHECK(doc.at("nodes").size() == 2); // the Y-up root and the arm
  CHECK(doc.at("nodes")[1].at("name") == "Arm");
  // glTF's unit is the metre: the root scales our millimetres, or the
  // mechanism opens 1000x too large in every viewer.
  const auto& rootScale = doc.at("nodes")[0].at("scale");
  for (int k = 0; k < 3; k++)
    CHECK_THAT(rootScale[k].get<double>(), WithinAbs(0.001, 1e-12));
  // The time accessor spans the study.
  const auto& sampler = doc.at("animations")[0].at("samplers")[0];
  const auto& timeAcc = doc.at("accessors")[sampler.at("input").get<int>()];
  CHECK(timeAcc.at("count") == 5);
  CHECK_THAT(timeAcc.at("max")[0].get<double>(), WithinAbs(1.0, 1e-6));

  // Refusals: a node with the wrong number of keyframes, no keyframes.
  glb::Node bad = arm;
  bad.keyframes.pop_back();
  CHECK_FALSE(glb::Write(path.string(), {bad}, times, &err));
  CHECK_THAT(err, ContainsSubstring("keyframes"));
  CHECK_FALSE(glb::Write(path.string(), {arm}, {0.0}, &err));
  CHECK_THAT(err, ContainsSubstring("two keyframes"));
  fs::remove(path);
}

TEST_CASE(
    "Exact collision reuse preserves clashes and invalidates changed inputs",
    "[motion][cache]") {
  Rig rig = MakeRig(90.0);
  auto study = FullTurn(rig.axle);
  study.clashMode = "exact";
  collide::ExactContactCache cache;
  const auto cold = motion::Run(rig.asmDoc, rig.Parts(), nullptr, study,
                                nullptr, nullptr, nullptr, &cache);
  const auto warm = motion::Run(rig.asmDoc, rig.Parts(), nullptr, study,
                                nullptr, nullptr, nullptr, &cache);
  REQUIRE(cold.pairsChecked > 0);
  CHECK(warm.pairsCached + warm.pairsRepeated == warm.pairsChecked);
  CHECK(warm.clean == cold.clean);
  CHECK(warm.pairsUnknown == 0);
  for (size_t i = 0; i < cold.samples.size(); ++i) {
    REQUIRE(warm.samples[i].clashes.size() == cold.samples[i].clashes.size());
    for (size_t j = 0; j < cold.samples[i].clashes.size(); ++j) {
      CHECK(warm.samples[i].clashes[j].volume ==
            cold.samples[i].clashes[j].volume);
      CHECK(warm.samples[i].clashes[j].centroid ==
            cold.samples[i].clashes[j].centroid);
    }
  }
  rig.stop = BoxPart({-3, -3, -3}, {6, 6, 6});
  const auto edited = motion::Run(rig.asmDoc, rig.Parts(), nullptr, study,
                                  nullptr, nullptr, nullptr, &cache);
  CHECK(edited.pairsCached < edited.pairsChecked);
}

TEST_CASE("Exact contact cache distinguishes internal and world placements",
          "[motion][cache]") {
  auto doc = BoxPart({0, 0, 0}, {4, 4, 4});
  const auto a = doc->GetBodies()[0].shape;
  const glm::mat4 identity(1.0f);
  collide::ExactContactCache cache(2);
  const auto first = cache.Check(a, identity, a, identity);
  REQUIRE(first.contact);
  CHECK(first.contact->volume > 63.9);
  CHECK(cache.Check(a, identity, a, identity).cached);
  glm::mat4 moved(1.0f);
  moved[3][0] = 20.0f;
  const auto b = a.Transformed(moved);
  const auto internal = cache.Check(a, identity, b, identity);
  REQUIRE(internal.contact);
  CHECK_FALSE(internal.cached);
  CHECK(internal.contact->volume == 0.0);
  const auto external = cache.Check(a, identity, a, moved);
  REQUIRE(external.contact);
  CHECK_FALSE(external.cached);
  CHECK(external.contact->volume == 0.0);
  CHECK(cache.Size() == 2);
  CHECK_FALSE(cache.Check(a, identity, a, identity).cached); // bounded eviction
  cache.Clear();
  CHECK(cache.Size() == 0);
}

TEST_CASE("motion table explains unknown contacts and interrupted runs",
          "[motion]") {
  motion::Result r;
  MotionStudy study;
  r.checkedClashes = true;
  r.clean = false;
  r.pairsUnknown = 1;
  motion::Sample sample;
  sample.unknownContacts.push_back({10, 20, "screw", "jaw"});
  r.samples.push_back(sample);
  const auto table = motion::Table(r, study);
  REQUIRE(table.find("UNVERIFIED") != std::string::npos);
  REQUIRE(table.find("UNKNOWN screw x jaw") != std::string::npos);
  REQUIRE(table.find("exact contact calculation failed") != std::string::npos);
  r.cancelled = true;
  REQUIRE(motion::Table(r, study).find("INCOMPLETE") != std::string::npos);
  r.error = "driver missing";
  REQUIRE(motion::Table(r, study).find("Error: driver missing") !=
          std::string::npos);
}

// Cancelling is a REQUEST honoured between samples, and what it leaves behind
// is a PARTIAL result rather than nothing: the panel's Cancel button promises
// "keeps the ones already checked", and the samples that landed are what
// makes that true. Only Table()'s rendering of an interrupted run was covered
// before — the predicate that produces one was not.
TEST_CASE("A cancelled motion run keeps the samples it finished", "[motion]") {
  Rig r = MakeRig(90.0);
  MotionStudy study = FullTurn(r.axle, 25);
  study.clashMode = "exact";

  // The same study, uninterrupted, is the control: cancelling changes what
  // was checked, not what checking finds.
  const motion::Result full = motion::Run(r.asmDoc, r.Parts(), nullptr, study);
  REQUIRE(full.samples.size() == 25);

  // A run solves every sample, then checks them all in one batch; a cancel
  // during the checks keeps the leading samples whose every check landed.
  // Cancelled at the first check, as progress reports them.
  // One worker, so the batch stops where the cancel lands rather than with
  // every candidate already handed out.
  setenv("DESIGNER_SWEEP_THREADS", "1", 1);
  std::atomic<bool> stop{false};
  const motion::Result res = motion::Run(
      r.asmDoc, r.Parts(), nullptr, study, [&stop] { return stop.load(); },
      [&stop](double f) {
        if (f > 0.1) // the first check landed
          stop.store(true);
      });
  unsetenv("DESIGNER_SWEEP_THREADS");
  REQUIRE(res.error.empty());
  CHECK(res.cancelled);
  CHECK(res.samples.size() < 25);
  // `clean` is false on an interrupted run whatever the samples showed: it
  // means "this mechanism is clear", and part of the poses cannot say that.
  CHECK_FALSE(res.clean);
  CHECK(motion::Table(res, study).find("INCOMPLETE") != std::string::npos);
  for (size_t i = 0; i < res.samples.size(); i++) {
    CHECK(res.samples[i].value == full.samples[i].value);
    CHECK(res.samples[i].clashes.size() == full.samples[i].clashes.size());
  }

  // Cancelled during the solves, nothing was checked, so nothing is kept: a
  // sample is finished when it is checked.
  int seen = 0;
  const motion::Result early = motion::Run(
      r.asmDoc, r.Parts(), nullptr, study, [&seen] { return ++seen > 7; },
      nullptr);
  CHECK(early.cancelled);
  CHECK(early.samples.empty());

  // A kinematics-only run's sample IS its solve: seven asks returned false,
  // the eighth stopped it, so seven samples.
  MotionStudy kinematics = study;
  kinematics.checkClash = false;
  seen = 0;
  const motion::Result moved = motion::Run(
      r.asmDoc, r.Parts(), nullptr, kinematics, [&seen] { return ++seen > 7; },
      nullptr);
  CHECK(moved.cancelled);
  CHECK(moved.samples.size() == 7);
}

TEST_CASE("A study runs under the configuration it names", "[motion]") {
  // A configuration that takes the stop out: the study that names it is
  // clean although the tab still has the base (stop in) active, and the one
  // that names none still hits the stop.
  Rig r = MakeRig(90.0);
  Configuration& noStop = r.asmDoc.AddConfiguration("no stop");
  noStop.suppressedFeatures.push_back(r.stopOcc);
  const uint64_t noStopId = noStop.id;
  REQUIRE(r.asmDoc.ActiveConfiguration() == 0);

  MotionStudy study = FullTurn(r.axle, 13);
  CHECK_FALSE(motion::Run(r.asmDoc, r.Parts(), nullptr, study).clean);
  study.configuration = noStopId;
  const motion::Result res = motion::Run(r.asmDoc, r.Parts(), nullptr, study);
  INFO(res.error);
  REQUIRE(res.error.empty());
  CHECK(res.clean);
  CHECK(r.asmDoc.ActiveConfiguration() == 0); // the tab was not switched

  // 0 is the BASE, not "whichever is active": with the stop-less
  // configuration active, a study naming 0 still has the stop in.
  REQUIRE(r.asmDoc.SetActiveConfiguration(noStopId));
  study.configuration = 0;
  CHECK_FALSE(motion::Run(r.asmDoc, r.Parts(), nullptr, study).clean);
  study.configuration.reset(); // unset: whichever is active — no stop
  CHECK(motion::Run(r.asmDoc, r.Parts(), nullptr, study).clean);
  REQUIRE(r.asmDoc.SetActiveConfiguration(0));

  // It is JUDGED under it too: a configuration holding the driver refuses.
  Configuration& held = r.asmDoc.AddConfiguration("axle held");
  MateOverride o;
  o.mate = r.axle;
  o.rotation = 0.5f;
  held.mateOverrides.push_back(o);
  study.configuration = held.id;
  CHECK_THAT(motion::WhyNot(r.asmDoc, study),
             ContainsSubstring("held by the active configuration"));
  // And one that no longer exists is named, with what does.
  study.configuration = 9999;
  CHECK_THAT(motion::WhyNot(r.asmDoc, study),
             ContainsSubstring("no longer exists") &&
                 ContainsSubstring("no stop"));

  // It is saved with the study, and a study that names none writes nothing.
  study.configuration = noStopId;
  const nlohmann::json j = DocumentIO::MotionStudyToJson(study);
  CHECK(j.at("configuration").get<uint64_t>() == noStopId);
  CHECK(DocumentIO::MotionStudyFromJson(j).configuration == noStopId);
  CHECK_FALSE(DocumentIO::MotionStudyToJson(FullTurn(r.axle))
                  .contains("configuration"));
  study.configuration = 0; // the base is written, and read back as the base
  const nlohmann::json base = DocumentIO::MotionStudyToJson(study);
  REQUIRE(base.contains("configuration"));
  CHECK(DocumentIO::MotionStudyFromJson(base).configuration ==
        std::optional<uint64_t>(0));
}

TEST_CASE("Every sample records the moving mates' values", "[motion]") {
  Rig r = MakeRig(0.0, /*withStop=*/false);
  const MotionStudy study = FullTurn(r.axle, 5); // 0, 90, 180, 270, 360
  const motion::Result res = motion::Run(r.asmDoc, r.Parts(), nullptr, study);
  REQUIRE(res.error.empty());
  REQUIRE(res.samples.size() == 5);
  for (size_t i = 0; i < res.samples.size(); i++) {
    const auto& mates = res.samples[i].mates;
    REQUIRE(mates.size() == 1); // the one revolute; nothing fastened
    CHECK(mates[0].mate == r.axle);
    CHECK_THAT(mates[0].rotation, WithinAbs(i * kPi / 2.0, 1e-6));
  }
}

// The v128 assembly golden: a study that names its configuration.
// DESIGNER_WRITE_GOLDEN_V128_ASM=<path> writes it.
TEST_CASE("The v128 golden: a study that names its configuration",
          "[motion][golden]") {
  Rig r = MakeRig(90.0);
  Configuration& noStop = r.asmDoc.AddConfiguration("no stop");
  noStop.suppressedFeatures.push_back(r.stopOcc);
  MotionStudy s = FullTurn(r.axle, 13);
  s.name = "turn without the stop";
  s.configuration = noStop.id;
  r.asmDoc.AddMotionStudy(s);
  if (const char* out = std::getenv("DESIGNER_WRITE_GOLDEN_V128_ASM"))
    REQUIRE(DocumentIO::SaveAssembly(r.asmDoc, out).has_value());

  const fs::path golden = fs::path(TestSourceDir()) /
                          "tests/golden/assembly/v128-study-configuration.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->GetMotionStudies().size() == 1);
  const MotionStudy& gs = (*g)->GetMotionStudies()[0];
  REQUIRE(gs.configuration.has_value());
  CHECK(*gs.configuration != 0);
  CHECK((*g)->ActiveConfiguration() == 0);
  const motion::Result res = motion::Run(**g, r.Parts(), nullptr, gs);
  REQUIRE(res.error.empty());
  CHECK(res.clean); // the stop is out, under the study's own configuration
}

TEST_CASE("The v167 golden: a study that states its clash deflection",
          "[motion][golden]") {
  Rig r = MakeRig(90.0);
  MotionStudy s = FullTurn(r.axle, 13);
  s.name = "turn, finely";
  s.clashDeflectionMm = 0.05;
  r.asmDoc.AddMotionStudy(s);
  if (const char* out = std::getenv("DESIGNER_WRITE_GOLDEN_V167_ASM"))
    REQUIRE(DocumentIO::SaveAssembly(r.asmDoc, out).has_value());

  // What the run meshes at is what it was asked for, and it still finds the
  // stop the derived deflection finds.
  const motion::Result here = motion::Run(r.asmDoc, r.Parts(), nullptr, s);
  REQUIRE(here.error.empty());
  CHECK_THAT(here.deflectionMm, WithinAbs(0.05, 1e-12));
  CHECK_FALSE(here.clean);
  MotionStudy derived = s;
  derived.clashDeflectionMm = 0.0;
  const motion::Result coarse =
      motion::Run(r.asmDoc, r.Parts(), nullptr, derived);
  CHECK(coarse.deflectionMm > 0.05);
  // ...and they are different inputs: a stored answer is not handed across.
  CHECK(motion::InputKey(r.asmDoc, r.Parts(), nullptr, s) !=
        motion::InputKey(r.asmDoc, r.Parts(), nullptr, derived));

  const fs::path golden = fs::path(TestSourceDir()) /
                          "tests/golden/assembly/v167-clash-deflection.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->GetMotionStudies().size() == 1);
  const MotionStudy& gs = (*g)->GetMotionStudies()[0];
  CHECK_THAT(gs.clashDeflectionMm, WithinAbs(0.05, 1e-12));
  const motion::Result res = motion::Run(**g, r.Parts(), nullptr, gs);
  REQUIRE(res.error.empty());
  CHECK_THAT(res.deflectionMm, WithinAbs(0.05, 1e-12));
  CHECK_FALSE(res.clean);

  // Absent is derived; a bad one, or one on an exact study, is refused.
  nlohmann::json j = DocumentIO::MotionStudyToJson(derived);
  CHECK_FALSE(j.contains("clash_deflection_mm"));
  j["clash_deflection_mm"] = -0.1;
  CHECK_THROWS_WITH(DocumentIO::MotionStudyFromJson(j),
                    ContainsSubstring("positive"));
  j["clash_deflection_mm"] = 0.05;
  j["clash_mode"] = "exact";
  CHECK_THROWS_WITH(DocumentIO::MotionStudyFromJson(j),
                    ContainsSubstring("MESH"));
}

TEST_CASE("The verdict asks whether it moved before it asks about clashes",
          "[motion]") {
  MotionStudy s;
  s.driverDof = MateDof::Rotation;
  s.samples = 5;
  const auto samples = [](int n) {
    motion::Result r;
    for (int i = 0; i < n; i++) {
      motion::Sample x;
      x.value = i * kPi / 2.0; // 0, 90, 180 ... degrees
      r.samples.push_back(x);
    }
    return r;
  };

  // A preview that moves: says so, and that collisions were not checked.
  motion::Result preview = samples(5);
  preview.checkedClashes = false;
  motion::Verdict v = motion::Summarize(preview, s, false);
  CHECK(v.level == motion::Verdict::Level::Neutral);
  CHECK_THAT(v.text, ContainsSubstring("Moves through all 5 samples") &&
                         ContainsSubstring("not checked"));

  // A preview that locks at 90 degrees: THE case the old panel hid behind
  // "kinematics only".
  preview.samples[1].solved = false;
  preview.samples[1].residualMm = 0.4;
  preview.samples[1].erroredMates = 1;
  preview.clean = false;
  preview.worstSample = 1;
  v = motion::Summarize(preview, s, false);
  CHECK(v.level == motion::Verdict::Level::Bad);
  CHECK_THAT(v.text, ContainsSubstring("Does not solve at sample 1 (90 deg)") &&
                         ContainsSubstring("0.4 mm") &&
                         ContainsSubstring("1 errored mate;"));

  // Validation: clean, and not clean naming the pair.
  motion::Result checked = samples(5);
  checked.checkedClashes = true;
  CHECK(motion::Summarize(checked, s, false).level ==
        motion::Verdict::Level::Good);
  checked.clean = false;
  checked.worstSample = 2;
  motion::Clash c;
  c.aName = "Arm";
  c.bName = "Stop";
  checked.samples[2].clashes.push_back(c);
  v = motion::Summarize(checked, s, false);
  CHECK(v.level == motion::Verdict::Level::Bad);
  CHECK_THAT(v.text, ContainsSubstring("Arm x Stop at sample 2 (180 deg)"));

  // Stale and cancelled come first, whatever the samples say.
  CHECK_THAT(motion::Summarize(checked, s, true).text,
             ContainsSubstring("Stale"));
  checked.cancelled = true;
  CHECK_THAT(motion::Summarize(checked, s, false).text,
             ContainsSubstring("Cancelled"));
}

namespace {

// The rig, plus a standing fault it never moves: PLATE A fixed, PLATE B
// fastened to it and overlapping it by 5 x 10 x 4 mm, and PLATE C fastened
// to B face-to-face — touching, as a bolted joint does. None of the three
// moves relative to the others while the arm turns, so the study used to
// skip every pair among them; the pump's clamp plates overlapped that way
// through four exact studies.
struct PlatedRig {
  Rig rig = MakeRig(90.0);
  std::unique_ptr<Document> a = BoxPart({40, 40, -2}, {10, 10, 4});
  std::unique_ptr<Document> b = BoxPart({45, 40, -2}, {10, 10, 4});
  std::unique_ptr<Document> c = BoxPart({55, 40, -2}, {10, 10, 4});
  assembly::PartResolver Parts() {
    return [this](uint64_t id, uint64_t configuration) -> const Document* {
      if (id == 4)
        return a.get();
      if (id == 5)
        return b.get();
      if (id == 6)
        return c.get();
      return rig.Parts()(id, configuration);
    };
  }
  PlatedRig() {
    const auto place = [&](const char* name, uint64_t part, bool fixed) {
      Occurrence o;
      o.name = name;
      o.partId = part;
      o.fixed = fixed;
      return rig.asmDoc.AddOccurrence(std::move(o));
    };
    const uint64_t oa = place("Plate A", 4, true);
    const uint64_t ob = place("Plate B", 5, false);
    const uint64_t oc = place("Plate C", 6, false);
    for (const auto& [from, to] : {std::pair{oa, ob}, std::pair{ob, oc}}) {
      Mate m;
      m.type = MateType::Fastened;
      m.a = Origin(from);
      m.b = Origin(to);
      m.flipped = false;
      rig.asmDoc.AddMate(m);
    }
  }
};

bool IsPair(const motion::Clash& c, const char* x, const char* y) {
  return (c.aName == x && c.bName == y) || (c.aName == y && c.bName == x);
}

} // namespace

TEST_CASE("A study checks rigid pairs once, at the first sample, and reports "
          "their clash",
          "[motion]") {
  for (const char* mode : {"exact", "mesh"}) {
    PlatedRig p;
    MotionStudy study = FullTurn(p.rig.axle, 13);
    study.clashMode = mode;
    const motion::Result res =
        motion::Run(p.rig.asmDoc, p.Parts(), nullptr, study);
    INFO(mode << "\n" << motion::Table(res, study));
    REQUIRE(res.error.empty());
    CHECK_FALSE(res.clean);
    CHECK(res.rigidCheckedAt == 0);
    CHECK(res.pairsRigidChecked > 0);
    CHECK(res.pairsRigid > 0); // and skipped at every later sample

    // Sample 0 names the fastened pair's overlap, marked rigid, and says
    // where it is — exactly, even in mesh mode, since a rigid pair is
    // always asked exactly.
    const auto& first = res.samples[0].clashes;
    const auto ab = std::find_if(first.begin(), first.end(), [](const auto& c) {
      return IsPair(c, "Plate A", "Plate B");
    });
    REQUIRE(ab != first.end());
    CHECK(ab->rigid);
    REQUIRE(ab->known);
    CHECK_THAT(ab->volume, WithinAbs(5.0 * 10.0 * 4.0, 1e-3));
    CHECK_THAT(ab->centroid.x, WithinAbs(47.5, 1e-3));
    CHECK_THAT(ab->centroid.y, WithinAbs(45.0, 1e-3));
    CHECK_THAT(ab->centroid.z, WithinAbs(0.0, 1e-3));
    CHECK_THAT(ab->bboxMin.x, WithinAbs(45.0, 1e-3));
    CHECK_THAT(ab->bboxMax.x, WithinAbs(50.0, 1e-3));
    CHECK_THAT(ab->bboxMin.z, WithinAbs(-2.0, 1e-3));
    CHECK_THAT(ab->bboxMax.z, WithinAbs(2.0, 1e-3));

    // B and C only touch: a bolted joint is not a clash — which in mesh
    // mode it would be, had the rigid pair been asked by triangles.
    for (const motion::Sample& s : res.samples)
      for (const motion::Clash& c : s.clashes)
        CHECK_FALSE(IsPair(c, "Plate B", "Plate C"));
    // A standing clash is reported once, not at every sample.
    for (size_t i = 1; i < res.samples.size(); i++)
      for (const motion::Clash& c : res.samples[i].clashes)
        CHECK_FALSE(c.rigid);
    // The arm still hits the stop, as a moving clash.
    bool armHit = false;
    for (const motion::Sample& s : res.samples)
      for (const motion::Clash& c : s.clashes)
        armHit = armHit || (IsPair(c, "Arm", "Stop") && !c.rigid);
    CHECK(armHit);

    const std::string table = motion::Table(res, study);
    CHECK_THAT(table, ContainsSubstring("(rigid)") &&
                          ContainsSubstring("checked once, exactly, at "
                                            "sample 0"));
  }
}

TEST_CASE("The verdict calls a rigid pair's clash standing", "[motion]") {
  const MotionStudy s = FullTurn(1, 3);
  motion::Result r;
  r.checkedClashes = true;
  r.clean = false;
  r.samples.resize(3);
  motion::Clash c;
  c.aName = "Plate A";
  c.bName = "Plate B";
  c.rigid = true;
  r.samples[0].clashes.push_back(c);
  r.worstSample = 0;
  CHECK_THAT(motion::Summarize(r, s, false).text,
             ContainsSubstring("Plate A x Plate B at sample 0") &&
                 ContainsSubstring("standing"));
}

// A run is answered from a stored one only when its input hash matches
// (docs/simulation/README.md#studies-as-jobs), so the hash must move on every
// input that can change an answer -- each class checked here on its own --
// and on nothing that cannot.
TEST_CASE("A motion run's input hash moves with every input",
          "[motion][studyjobs]") {
  Rig r = MakeRig(90.0);
  const MotionStudy study = FullTurn(r.axle, 13);
  const auto key = [&](const MotionStudy& s) {
    return motion::InputKey(r.asmDoc, r.Parts(), nullptr, s);
  };
  const uint64_t base = key(study);
  REQUIRE(base != 0);
  CHECK(key(study) == base); // deterministic

  // The study's name is a label, not an input.
  MotionStudy renamed = study;
  renamed.name = "another name";
  CHECK(key(renamed) == base);

  // Settings.
  MotionStudy more = study;
  more.samples = 14;
  CHECK(key(more) != base);
  MotionStudy exact = study;
  exact.clashMode = "exact";
  CHECK(key(exact) != base);
  MotionStudy shorter = study;
  shorter.to = kPi;
  CHECK(key(shorter) != base);

  // A mate's value (the run starts from the pose the document is in).
  r.asmDoc.FindMateById(r.axle)->rotation = 0.25;
  CHECK(key(study) != base);
  r.asmDoc.FindMateById(r.axle)->rotation = 0.0;
  REQUIRE(key(study) == base);

  // A placement.
  r.asmDoc.FindById(r.stopOcc)->position.z = 1.0;
  CHECK(key(study) != base);
  r.asmDoc.FindById(r.stopOcc)->position.z = 0.0;
  REQUIRE(key(study) == base);

  // A relation.
  Relation rel;
  rel.kind = RelationKind::Gear;
  rel.mateA = r.axle;
  rel.mateB = r.axle;
  rel.ratio = 1.0;
  const uint64_t relId = r.asmDoc.AddRelation(rel);
  CHECK(key(study) != base);
  r.asmDoc.RemoveRelation(relId);

  // A configuration, and which one is active.
  const uint64_t beforeConfig = key(study);
  Configuration& noStop = r.asmDoc.AddConfiguration("no stop");
  noStop.suppressedFeatures.push_back(r.stopOcc);
  const uint64_t withConfig = key(study);
  CHECK(withConfig != beforeConfig);
  r.asmDoc.SetActiveConfigurationRaw(noStop.id);
  CHECK(key(study) != withConfig);
  r.asmDoc.SetActiveConfigurationRaw(0);

  // The geometry of a placed part, and its material.
  const uint64_t beforeGeometry = key(study);
  r.stop = BoxPart({-2, -2, -2}, {5, 4, 4});
  CHECK(key(study) != beforeGeometry);
  const uint64_t beforeMaterial = key(study);
  r.stop->GetFeature(0)->SetMaterial("steel");
  r.stop->Regenerate();
  CHECK(key(study) != beforeMaterial);
}

// The exact check reuses work between samples: a pair at a relative pose it
// was already found clear at -- here the full turn's last sample, which is
// its first -- takes that answer instead of a second boolean, and the table
// is the same as the answer each sample would have got on its own.
TEST_CASE("An exact motion run checks a repeated relative pose once",
          "[motion][studyjobs]") {
  // A stop just off the arm's line at 0 degrees: the arm clashes with it
  // at 0 (and 360), and at 30 degrees their boxes overlap with the two
  // clear of each other -- a clear answer to repeat.
  Rig r = MakeRig(0.0);
  r.asmDoc.FindById(r.stopOcc)->position = glm::dvec3(20.0, 2.0, 0.0);
  MotionStudy study = FullTurn(r.axle, 13);
  study.clashMode = "exact";
  // Two full turns: every pose of the second is one of the first.
  study.to = 4.0 * kPi;
  study.samples = 25;
  const motion::Result res = motion::Run(r.asmDoc, r.Parts(), nullptr, study);
  REQUIRE(res.error.empty());
  REQUIRE(res.samples.size() == 25);
  CHECK(res.pairsRepeated > 0);
  // Sample k and sample k + 12 are the same pose: the same clashes.
  for (size_t k = 0; k + 12 < res.samples.size(); k++)
    CHECK(res.samples[k].clashes.size() == res.samples[k + 12].clashes.size());
  // And the same as one turn checked on its own.
  MotionStudy once = study;
  once.to = 2.0 * kPi;
  once.samples = 13;
  const motion::Result single = motion::Run(r.asmDoc, r.Parts(), nullptr, once);
  for (size_t k = 0; k < single.samples.size(); k++)
    CHECK(single.samples[k].clashes.size() == res.samples[k].clashes.size());
  CHECK(res.clean == single.clean);
}

// The session's exact cache finds a clear pair by its RELATIVE pose: two
// bodies moved together, or a re-run whose solve landed a hair away, is a
// hit. A clash is not reused across world poses (its location is in world
// space).
TEST_CASE("The exact contact cache matches a clear pair by relative pose",
          "[motion][cache][studyjobs]") {
  auto a = BoxPart({0, 0, 0}, {4, 4, 4});
  auto b = BoxPart({10, 0, 0}, {4, 4, 4});
  const CADShape& sa = a->GetBodies()[0].shape;
  const CADShape& sb = b->GetBodies()[0].shape;
  collide::ExactContactCache cache;
  const glm::dmat4 id(1.0);
  const auto first = cache.Check(sa, id, sb, id);
  REQUIRE(first.contact);
  CHECK_FALSE(first.cached);
  CHECK(first.contact->volume == 0.0);
  // Both carried 50 mm and turned together: the same question.
  const glm::dmat4 carried =
      glm::rotate(glm::translate(glm::dmat4(1.0), glm::dvec3(50, 0, 0)), 0.7,
                  glm::dvec3(0, 0, 1));
  const auto moved = cache.Check(sa, carried, sb, carried);
  CHECK(moved.cached);
  // Apart by a different amount: a different question.
  const glm::dmat4 nearer =
      glm::translate(glm::dmat4(1.0), glm::dvec3(-7, 0, 0));
  const auto closer = cache.Check(sa, id, sb, nearer);
  CHECK_FALSE(closer.cached);
  REQUIRE(closer.contact);
  CHECK(closer.contact->volume > 0.0); // they overlap now
  // An overlap is not found by relative pose from another world pose.
  const auto overlapMoved = cache.Check(sa, carried, sb, carried * nearer);
  CHECK_FALSE(overlapMoved.cached);
}

// A motion result is saved with the project beside its study (v153), with
// the input hash that produced it: the reopened assembly hashes the same, so
// a run of it is answered from the saved table, and every sample comes back
// as it was run.
TEST_CASE("A saved motion result reopens with its hash and its samples",
          "[motion][studyjobs][serialize]") {
  namespace fs = std::filesystem;
  Rig r = MakeRig(90.0);
  MotionStudy study = FullTurn(r.axle, 13);
  study.id = r.asmDoc.AddMotionStudy(study);
  const MotionStudy& stored = *r.asmDoc.FindMotionStudyById(study.id);
  motion::Result result = motion::Run(r.asmDoc, r.Parts(), nullptr, stored);
  REQUIRE(result.error.empty());
  REQUIRE_FALSE(result.clean); // the stop at 90 degrees
  result.inputHash = motion::InputKey(r.asmDoc, r.Parts(), nullptr, stored);
  REQUIRE(result.inputHash != 0);
  r.asmDoc.StoreMotionResult(study.id,
                             std::make_shared<const motion::Result>(result));

  const fs::path proj = fs::temp_directory_path() / "motion_result_reopen.dsgn";
  fs::remove_all(proj);
  REQUIRE(testio::SaveAssembly(r.asmDoc, proj.string()));
  auto reloaded = testio::LoadAssembly(proj.string());
  REQUIRE(reloaded != nullptr);
  const std::shared_ptr<const motion::Result> back =
      reloaded->MotionResult(study.id);
  REQUIRE(back != nullptr);
  CHECK(back->inputHash == result.inputHash);
  const MotionStudy& reread = *reloaded->FindMotionStudyById(study.id);
  CHECK(motion::InputKey(*reloaded, r.Parts(), nullptr, reread) ==
        result.inputHash);
  // The table and the poses, as run.
  CHECK(motion::Table(*back, reread) == motion::Table(result, stored));
  REQUIRE(back->samples.size() == result.samples.size());
  CHECK(back->clean == result.clean);
  CHECK(back->worstSample == result.worstSample);
  CHECK(back->pairsChecked == result.pairsChecked);
  for (size_t i = 0; i < result.samples.size(); ++i) {
    const motion::Sample& a = result.samples[i];
    const motion::Sample& b = back->samples[i];
    CHECK(a.value == b.value);
    REQUIRE(a.worlds.size() == b.worlds.size());
    for (size_t k = 0; k < a.worlds.size(); ++k) {
      CHECK(a.worlds[k].first == b.worlds[k].first);
      CHECK(a.worlds[k].second == b.worlds[k].second);
    }
    CHECK(a.clashes.size() == b.clashes.size());
    CHECK(a.mates.size() == b.mates.size());
  }

  // An input moved after the save: the reopened one hashes otherwise, and
  // is not the answer to it.
  reloaded->FindById(r.stopOcc)->position.z = 1.0;
  CHECK(motion::InputKey(*reloaded, r.Parts(), nullptr, reread) !=
        back->inputHash);

  // A result gone from the study is gone from the next save.
  r.asmDoc.StoreMotionResult(study.id, nullptr);
  REQUIRE(testio::SaveAssembly(r.asmDoc, proj.string()));
  auto without = testio::LoadAssembly(proj.string());
  REQUIRE(without != nullptr);
  CHECK(without->MotionResult(study.id) == nullptr);
  fs::remove_all(proj);
}

// The parts a motion hash reads come back from the project too, and a shape
// read back is not bit for bit the one that was saved: reading a direction
// normalises it again, and the 6005's and 609's chamfer lines came back a
// unit in the last place away. The pan/tilt head's pan and tilt studies read
// stale after every reopen for it. The hash reads a shape as the project
// reads it back, so a saved project reopens with its results current.
TEST_CASE("A saved motion study on an assembly with relations reopens "
          "current, its parts read back from the project",
          "[motion][studyjobs][serialize]") {
  const fs::path dir = fs::temp_directory_path() / "motion_relations_reopen";
  fs::remove_all(dir);
  fs::create_directories(dir);
  using Parts = std::map<uint64_t, std::unique_ptr<Document>>;
  // Part 1 a block; 2 and 3 the two bearings that came back otherwise.
  Parts parts;
  parts[1] = BoxPart({-40, -40, -20}, {80, 80, 10});
  for (const auto& [id, designation] :
       {std::pair<uint64_t, const char*>{2, "6005"}, {3, "609"}}) {
    auto doc = std::make_unique<Document>();
    doc->AddFeature(std::make_unique<BearingFeature>("Bearing", designation));
    doc->Regenerate();
    REQUIRE(doc->GetFeature(0)->GetError().empty());
    parts[id] = std::move(doc);
  }
  const auto over = [](Parts& docs) {
    return assembly::PartResolver(
        [&docs](uint64_t id, uint64_t) -> const Document* {
          const auto it = docs.find(id);
          return it == docs.end() ? nullptr : it->second.get();
        });
  };

  AssemblyDocument asmDoc;
  Occurrence base;
  base.name = "Base";
  base.partId = 1;
  base.fixed = true;
  const uint64_t baseOcc = asmDoc.AddOccurrence(std::move(base));
  Occurrence big;
  big.name = "Big";
  big.partId = 2;
  const uint64_t bigOcc = asmDoc.AddOccurrence(std::move(big));
  Occurrence small;
  small.name = "Small";
  small.partId = 3;
  const uint64_t smallOcc = asmDoc.AddOccurrence(std::move(small));
  Mate axle;
  axle.type = MateType::Revolute;
  axle.name = "big axle";
  axle.a = Origin(baseOcc);
  axle.b = Origin(bigOcc);
  axle.flipped = false;
  const uint64_t bigAxle = asmDoc.AddMate(axle);
  axle.name = "small axle";
  axle.b = Origin(smallOcc);
  axle.offset = 30.0;
  const uint64_t smallAxle = asmDoc.AddMate(axle);
  Relation pair;
  pair.kind = RelationKind::Gear;
  pair.name = "pair";
  pair.mateA = bigAxle;
  pair.mateB = smallAxle;
  pair.ratio = -0.025;
  const uint64_t pairId = asmDoc.AddRelation(pair);

  MotionStudy study = FullTurn(bigAxle, 5);
  study.checkClash = false;
  study.id = asmDoc.AddMotionStudy(study);
  const MotionStudy& stored = *asmDoc.FindMotionStudyById(study.id);
  motion::Result result = motion::Run(asmDoc, over(parts), nullptr, stored);
  REQUIRE(result.error.empty());
  result.inputHash = motion::InputKey(asmDoc, over(parts), nullptr, stored);
  asmDoc.StoreMotionResult(study.id,
                           std::make_shared<const motion::Result>(result));

  for (const auto& [id, doc] : parts)
    REQUIRE(
        testio::Save(*doc, (dir / (std::to_string(id) + ".dsgn")).string()));
  REQUIRE(testio::SaveAssembly(asmDoc, (dir / "asm.dsgn").string()));
  Parts back;
  for (const auto& [id, doc] : parts) {
    back[id] = testio::Load((dir / (std::to_string(id) + ".dsgn")).string());
    REQUIRE(back[id] != nullptr);
    INFO("part " << id);
    inputhash::Hasher was, is;
    was.Bodies(doc->GetBodies());
    is.Bodies(back[id]->GetBodies());
    CHECK(was.Digest() == is.Digest());
  }
  auto reloaded = testio::LoadAssembly((dir / "asm.dsgn").string());
  REQUIRE(reloaded != nullptr);
  const MotionStudy& reread = *reloaded->FindMotionStudyById(study.id);
  REQUIRE(reloaded->MotionResult(study.id) != nullptr);
  CHECK(reloaded->MotionResult(study.id)->inputHash == result.inputHash);
  CHECK(motion::InputKey(*reloaded, over(back), nullptr, reread) ==
        result.inputHash);

  // An input moved after the reopen reads otherwise: a part's material,
  // then the relation's ratio.
  back[3]->GetFeature(0)->SetMaterial("aluminum");
  back[3]->Regenerate();
  CHECK(motion::InputKey(*reloaded, over(back), nullptr, reread) !=
        result.inputHash);
  back[3]->GetFeature(0)->SetMaterial("steel");
  back[3]->Regenerate();
  CHECK(motion::InputKey(*reloaded, over(back), nullptr, reread) ==
        result.inputHash);
  reloaded->FindRelationById(pairId)->ratio = -0.05;
  CHECK(motion::InputKey(*reloaded, over(back), nullptr, reread) !=
        result.inputHash);
  fs::remove_all(dir);
}
