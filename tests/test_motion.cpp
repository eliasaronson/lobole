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
#include <glm/gtc/quaternion.hpp>

#include "TestIO.h"
#include "core/AssemblySolve.h"
#include "core/CollisionCache.h"
#include "core/Document.h"
#include "core/InputHash.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/features/BearingFeature.h"
#include "core/features/FrameFeature.h"
#include "core/io/GlbAnimation.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <array>
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

  // The golden: written by this recipe (LOBOLE_WRITE_GOLDEN=<path>), and
  // loaded here every time.
  if (const char* out = std::getenv("LOBOLE_WRITE_GOLDEN"))
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
  setenv("LOBOLE_SWEEP_THREADS", "1", 1);
  std::atomic<bool> stop{false};
  const motion::Result res = motion::Run(
      r.asmDoc, r.Parts(), nullptr, study, [&stop] { return stop.load(); },
      [&stop](double f) {
        if (f > 0.1) // the first check landed
          stop.store(true);
      });
  unsetenv("LOBOLE_SWEEP_THREADS");
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

TEST_CASE("A traced point follows its occurrence through every sample",
          "[motion][trace]") {
  // docs/assemblies/README.md#tracing-a-point: the arm's tip at (30, 0, 0)
  // in its own coordinates sweeps a circle of radius 30 about the axle.
  Rig r = MakeRig(0.0, /*withStop=*/false);
  const MotionStudy study = FullTurn(r.axle, 9); // every 45 degrees
  const motion::Result res = motion::Run(r.asmDoc, r.Parts(), nullptr, study);
  REQUIRE(res.error.empty());
  std::vector<motion::TracePoint> pts = {
      {"tip", r.armOcc, 0, {30.0, 0.0, 0.0}},
      {"root", r.carrierOcc, 0, {5.0, 1.0, 2.0}}};
  CHECK(motion::WhyNotTrace(r.asmDoc, pts).empty());
  std::vector<motion::TracePath> paths;
  std::string why;
  REQUIRE(motion::TracePoints(res, pts, &paths, &why));
  REQUIRE(paths.size() == 2);
  REQUIRE(paths[0].points.size() == 9);
  for (size_t i = 0; i < 9; i++) {
    REQUIRE(paths[0].points[i]);
    const double a = res.samples[i].value;
    CHECK_THAT(paths[0].points[i]->x, WithinAbs(30.0 * std::cos(a), 1e-6));
    CHECK_THAT(paths[0].points[i]->y, WithinAbs(30.0 * std::sin(a), 1e-6));
    CHECK_THAT(paths[0].points[i]->z, WithinAbs(0.0, 1e-9));
    // The fixed carrier's point stands still, where its part puts it.
    REQUIRE(paths[1].points[i]);
    CHECK_THAT(paths[1].points[i]->x, WithinAbs(5.0, 1e-9));
  }
  CHECK(paths[0].any);
  CHECK(paths[0].unsolved == 0);
  CHECK_THAT(paths[0].min.x, WithinAbs(-30.0, 1e-6));
  CHECK_THAT(paths[0].max.y, WithinAbs(30.0, 1e-6));
  CHECK_THAT(paths[1].max.x - paths[1].min.x, WithinAbs(0.0, 1e-12));

  // An unsolved sample gives no point -- never the last good one's.
  motion::Result broken = res;
  broken.samples[4].solved = false;
  REQUIRE(motion::TracePoints(broken, pts, &paths, &why));
  CHECK_FALSE(paths[0].points[4]);
  CHECK(paths[0].unsolved == 1);
  CHECK(paths[0].points[3]);

  // Refused before a run: an unknown occurrence (naming those there are), an
  // empty or a repeated name, and a path into a placement that is not
  // flexible.
  CHECK_THAT(motion::WhyNotTrace(r.asmDoc, {{"x", 999, 0, {}}}),
             ContainsSubstring("no occurrence 999") &&
                 ContainsSubstring("Arm"));
  CHECK_THAT(motion::WhyNotTrace(r.asmDoc, {{"", r.armOcc, 0, {}}}),
             ContainsSubstring("needs a name"));
  CHECK_THAT(motion::WhyNotTrace(r.asmDoc, {{"a", r.armOcc, 0, {}},
                                            {"a", r.carrierOcc, 0, {}}}),
             ContainsSubstring("already traced"));
  CHECK_THAT(motion::WhyNotTrace(r.asmDoc, {{"a", r.armOcc, 7, {}}}),
             ContainsSubstring("not a FLEXIBLE"));
}

// The v128 assembly golden: a study that names its configuration.
// LOBOLE_WRITE_GOLDEN_V128_ASM=<path> writes it.
TEST_CASE("The v128 golden: a study that names its configuration",
          "[motion][golden]") {
  Rig r = MakeRig(90.0);
  Configuration& noStop = r.asmDoc.AddConfiguration("no stop");
  noStop.suppressedFeatures.push_back(r.stopOcc);
  MotionStudy s = FullTurn(r.axle, 13);
  s.name = "turn without the stop";
  s.configuration = noStop.id;
  r.asmDoc.AddMotionStudy(s);
  if (const char* out = std::getenv("LOBOLE_WRITE_GOLDEN_V128_ASM"))
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
  if (const char* out = std::getenv("LOBOLE_WRITE_GOLDEN_V167_ASM"))
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

    // B and C only touch: a bolted joint is not a clash.
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

// ---------------------------------------------------------------------------
// Branches (docs/assemblies/solver.md#branches): a study does not jump to a
// mirror branch between samples without saying so.
// ---------------------------------------------------------------------------

namespace {

// A link part: nothing but named Frame features at its joints.
std::unique_ptr<Document> Link(const std::vector<glm::dvec2>& joints) {
  auto doc = std::make_unique<Document>();
  for (size_t i = 0; i < joints.size(); i++) {
    auto fr = std::make_unique<FrameFeature>("j" + std::to_string(i),
                                             FrameKind::Explicit);
    fr->origin[0] = joints[i].x;
    fr->origin[1] = joints[i].y;
    doc->AddFeature(std::move(fr));
  }
  doc->Regenerate();
  return doc;
}

// A planar linkage of links whose frames sit at their joints in WORLD
// coordinates at the built pose: every occurrence at the origin, every
// revolute at zero. Mate k joins link a's joint i to link b's joint j.
struct Linkage {
  std::vector<std::unique_ptr<Document>> parts;
  AssemblyDocument asmDoc;
  std::vector<uint64_t> occ;
  std::vector<uint64_t> mates;
  assembly::PartResolver Parts() {
    return [this](uint64_t id, uint64_t) -> const Document* {
      return id >= 1 && id <= parts.size() ? parts[id - 1].get() : nullptr;
    };
  }
  // Each link's own frame has its origin at its first joint and its x axis
  // toward its second (the fixed link's is the world's), as a part drawn
  // along its bar is; `angle` remembers where it was placed.
  std::vector<double> angle;
  void AddLink(const char* name, const std::vector<glm::dvec2>& joints,
               bool fixed = false) {
    const glm::dvec2 o0 = fixed ? glm::dvec2(0.0) : joints[0];
    const double phi = fixed ? 0.0
                             : std::atan2(joints[1].y - joints[0].y,
                                          joints[1].x - joints[0].x);
    const glm::dvec2 x(std::cos(phi), std::sin(phi)), y(-x.y, x.x);
    std::vector<glm::dvec2> local;
    for (const glm::dvec2& j : joints)
      local.push_back({glm::dot(j - o0, x), glm::dot(j - o0, y)});
    parts.push_back(Link(local));
    Occurrence o;
    o.name = name;
    o.partId = parts.size();
    o.fixed = fixed;
    o.position = glm::dvec3(o0, 0.0);
    o.orientation = glm::angleAxis(phi, glm::dvec3(0.0, 0.0, 1.0));
    occ.push_back(asmDoc.AddOccurrence(std::move(o)));
    angle.push_back(phi);
  }
  // A world point of the built pose in link `k`'s own coordinates.
  glm::dvec3 Local(size_t k, glm::dvec2 world) const {
    const Occurrence* o = asmDoc.FindById(occ[k]);
    const glm::dvec2 x(std::cos(angle[k]), std::sin(angle[k])), y(-x.y, x.x);
    const glm::dvec2 d = world - glm::dvec2(o->position);
    return {glm::dot(d, x), glm::dot(d, y), 0.0};
  }
  uint64_t Join(const char* name, size_t a, size_t ja, size_t b, size_t jb) {
    Mate m;
    m.rotation = angle[b] - angle[a];
    m.name = name;
    m.type = MateType::Revolute;
    m.a.occurrenceId = occ[a];
    m.a.anchor.frameId = parts[a]->GetFeature(ja)->GetId();
    m.b.occurrenceId = occ[b];
    m.b.anchor.frameId = parts[b]->GetFeature(jb)->GetId();
    m.flipped = false;
    mates.push_back(asmDoc.AddMate(m));
    return mates.back();
  }
};

// Jansen's "holy numbers", mm (a crank of 15).
struct JansenLengths {
  double a = 38.0, b = 41.5, c = 39.3, d = 40.1, e = 55.8, f = 39.4, g = 36.7,
         h = 65.7, i = 49.0, j = 50.0, k = 61.9, l = 7.8, m = 15.0;
};

glm::dvec2 Dyad(glm::dvec2 p, double r1, glm::dvec2 q, double r2, double side) {
  const glm::dvec2 d = q - p;
  const double L = glm::length(d);
  const double along = (r1 * r1 - r2 * r2 + L * L) / (2.0 * L);
  const double h = std::sqrt(std::max(0.0, r1 * r1 - along * along));
  const glm::dvec2 u = d / L;
  return p + along * u + side * h * glm::dvec2(-u.y, u.x);
}

double Cross2(glm::dvec2 u, glm::dvec2 v) { return u.x * v.y - u.y * v.x; }

// The leg at crank angle 0, by circle intersections on the built branch;
// links 0 frame, 1 crank, 2 j, 3 k, 4 BCD, 5 c, 6 f, 7 EFG.
struct JansenLeg {
  Linkage l;
  glm::dvec2 O, A, B, C, D, E, F, G;
  uint64_t crank = 0;
};
std::unique_ptr<JansenLeg> BuildJansen() {
  const JansenLengths L;
  auto leg = std::make_unique<JansenLeg>();
  JansenLeg& p = *leg;
  p.O = {0.0, 0.0};
  p.B = {-L.a, -L.l};
  p.A = {L.m, 0.0};
  p.C = Dyad(p.A, L.j, p.B, L.b, -1.0);
  p.E = Dyad(p.A, L.k, p.B, L.c, +1.0);
  p.D = Dyad(p.B, L.d, p.C, L.e, +1.0);
  p.F = Dyad(p.D, L.f, p.E, L.g, -1.0);
  p.G = Dyad(p.E, L.i, p.F, L.h, +1.0);
  Linkage& k = p.l;
  k.AddLink("frame", {p.O, p.B}, /*fixed=*/true);
  k.AddLink("crank", {p.O, p.A});
  k.AddLink("j", {p.A, p.C});
  k.AddLink("k", {p.A, p.E});
  k.AddLink("BCD", {p.B, p.C, p.D});
  k.AddLink("c", {p.B, p.E});
  k.AddLink("f", {p.D, p.F});
  k.AddLink("EFG", {p.E, p.F, p.G});
  p.crank = k.Join("O", 0, 0, 1, 0);
  k.Join("A j", 1, 1, 2, 0);
  k.Join("A k", 1, 1, 3, 0);
  k.Join("B BCD", 0, 1, 4, 0);
  k.Join("B c", 0, 1, 5, 0);
  k.Join("D", 4, 2, 6, 0);
  k.Join("E c", 5, 1, 7, 0);
  k.Join("C", 2, 1, 4, 1);
  k.Join("E k", 3, 1, 5, 1);
  k.Join("F", 6, 1, 7, 1);
  return leg;
}

// The knees' signs at every sample of `res`, traced: C, E and F, each the
// side of the line through its two other joints its vertex is on.
std::vector<std::array<double, 3>> JansenKnees(JansenLeg& leg,
                                               const motion::Result& res) {
  const std::vector<motion::TracePoint> pts = {
      {"A", leg.l.occ[1], 0, leg.l.Local(1, leg.A)},
      {"C", leg.l.occ[2], 0, leg.l.Local(2, leg.C)},
      {"E", leg.l.occ[3], 0, leg.l.Local(3, leg.E)},
      {"D", leg.l.occ[6], 0, leg.l.Local(6, leg.D)},
      {"F", leg.l.occ[6], 0, leg.l.Local(6, leg.F)}};
  std::vector<motion::TracePath> paths;
  std::string why;
  REQUIRE(motion::TracePoints(res, pts, &paths, &why));
  std::vector<std::array<double, 3>> out;
  for (size_t i = 0; i < res.samples.size(); i++) {
    const auto at = [&](size_t k) { return glm::dvec2(*paths[k].points[i]); };
    const glm::dvec2 A = at(0), C = at(1), E = at(2), D = at(3), F = at(4);
    out.push_back({Cross2(A - C, leg.B - C), Cross2(A - E, leg.B - E),
                   Cross2(D - F, E - F)});
  }
  return out;
}

} // namespace

// A four-bar: ground pivots O (0, 0) and Q (d, 0), crank O-A (a), coupler
// A-B (b), rocker Q-B (c), built at crank angle th0 with the elbow on
// `side`. Links 0 ground, 1 crank, 2 coupler, 3 rocker.
std::unique_ptr<Linkage> FourBar(double d, double a, double b, double c,
                                 double th0, double side, uint64_t* crank) {
  auto l = std::make_unique<Linkage>();
  const glm::dvec2 O(0, 0), Q(d, 0), A(a * std::cos(th0), a * std::sin(th0));
  const glm::dvec2 B = Dyad(A, b, Q, c, side);
  l->AddLink("ground", {O, Q}, true);
  l->AddLink("crank", {O, A});
  l->AddLink("coupler", {A, B});
  l->AddLink("rocker", {Q, B});
  *crank = l->Join("O", 0, 0, 1, 0);
  l->Join("A", 1, 1, 2, 0);
  l->Join("B", 2, 1, 3, 1);
  l->Join("Q", 0, 1, 3, 0);
  return l;
}

TEST_CASE("A Jansen leg at 45 degrees a step stays on its branch",
          "[motion][branch][jansen]") {
  // Seeded from the last sample, a 45-degree step lands the leg on a
  // mirror branch at 360 degrees, every loop closed
  // (docs/assemblies/solver.md#branches). The run refines the steps whose
  // joint rates jump, and comes back on the built branch.
  auto leg = BuildJansen();
  MotionStudy study = FullTurn(leg->crank, 9);
  study.checkClash = false;
  const motion::Result res =
      motion::Run(leg->l.asmDoc, leg->l.Parts(), nullptr, study);
  REQUIRE(res.error.empty());
  INFO(motion::Table(res, study));
  const auto knees = JansenKnees(*leg, res);
  for (size_t i = 0; i < res.samples.size(); i++) {
    INFO("sample " << i);
    CHECK(res.samples[i].solved);
    CHECK_FALSE(res.samples[i].branchJump);
    CHECK(knees[i][0] * knees[0][0] > 0.0);
    CHECK(knees[i][1] * knees[0][1] > 0.0);
    CHECK(knees[i][2] * knees[0][2] > 0.0);
  }
  CHECK(res.refinedSamples > 0);
  CHECK(res.branchJumps == 0);
  CHECK(res.clean);
  std::printf("Jansen at 45 deg a step: %zu of %zu samples refined\n",
              res.refinedSamples, res.samples.size());

  // Without refinement the same study lands on the mirror branch, and
  // says so rather than calling it solved.
  study.maxRefine = 0;
  const motion::Result coarse =
      motion::Run(leg->l.asmDoc, leg->l.Parts(), nullptr, study);
  CHECK(coarse.branchJumps > 0);
  CHECK_FALSE(coarse.clean);

  // At 2 degrees a step nothing is refined: the watch costs nothing there.
  study.maxRefine = MotionStudy::kDefaultMaxRefine;
  study.samples = 181;
  const motion::Result fine =
      motion::Run(leg->l.asmDoc, leg->l.Parts(), nullptr, study);
  CHECK(fine.refinedSamples == 0);
  CHECK(fine.branchJumps == 0);
}

TEST_CASE("A four-bar driven through its dead point reports the branch jump",
          "[motion][branch]") {
  // A parallelogram four-bar (ground = coupler, crank = rocker) has a dead
  // point where all four links lie in line. There it can leave as a
  // parallelogram or as an anti-parallelogram, and the solver takes the
  // second: the coupler, which never turned, starts to. No step is fine
  // enough to make that go away, so the sample says so.
  uint64_t crank = 0;
  auto l = FourBar(100, 30, 100, 30, 1.0, 1.0, &crank);
  MotionStudy study = FullTurn(crank, 73);
  study.from = 1.0;
  study.to = 1.0 + 2.0 * kPi;
  study.checkClash = false;
  const motion::Result res = motion::Run(l->asmDoc, l->Parts(), nullptr, study);
  REQUIRE(res.error.empty());
  INFO(motion::Table(res, study));
  REQUIRE(res.branchJumps > 0);
  CHECK_FALSE(res.clean);
  size_t first = res.samples.size();
  for (size_t i = 0; i < res.samples.size(); i++)
    if (res.samples[i].branchJump) {
      first = i;
      break;
    }
  REQUIRE(first < res.samples.size());
  const motion::Sample::BranchJump& j = *res.samples[first].branchJump;
  std::printf("four-bar: jump at sample %zu, '%s' %.4f deg where its rate "
              "gave %.4f, over %.4f deg to %.3f deg\n",
              first, j.name.c_str(), j.jump * 180 / kPi, j.expected * 180 / kPi,
              j.stepValue * 180 / kPi, j.atValue * 180 / kPi);
  // At the dead point, half a turn on from the crank's start at 0 in line.
  CHECK_THAT(j.atValue, WithinAbs(kPi, 0.01));
  // Over the smallest step: a 64th of the study's.
  CHECK_THAT(std::abs(j.stepValue),
             WithinAbs(std::abs(study.to - study.from) / 72.0 / 64.0, 1e-9));
  CHECK(res.worstSample == first);
  const motion::Verdict v = motion::Summarize(res, study, false);
  CHECK(v.level == motion::Verdict::Level::Bad);
  CHECK_THAT(v.text, ContainsSubstring("Jumps branch at sample"));
  CHECK_THAT(motion::Table(res, study), ContainsSubstring("BRANCH JUMP"));
}

TEST_CASE("A study's max_refine and a run's branch jump round-trip",
          "[motion][branch][serialize]") {
  MotionStudy s = FullTurn(7, 9);
  CHECK_FALSE(DocumentIO::MotionStudyToJson(s).contains("max_refine"));
  s.maxRefine = 3;
  nlohmann::json j = DocumentIO::MotionStudyToJson(s);
  CHECK(j.at("max_refine") == 3);
  CHECK(DocumentIO::MotionStudyFromJson(j).maxRefine == 3);
  j["max_refine"] = 13;
  CHECK_THROWS_WITH(DocumentIO::MotionStudyFromJson(j),
                    ContainsSubstring("from 0 to 12"));

  uint64_t crank = 0;
  auto l = FourBar(100, 30, 100, 30, 1.0, 1.0, &crank);
  MotionStudy study = FullTurn(crank, 73);
  study.from = 1.0;
  study.to = 1.0 + 2.0 * kPi;
  study.checkClash = false;
  const motion::Result res = motion::Run(l->asmDoc, l->Parts(), nullptr, study);
  REQUIRE(res.branchJumps > 0);
  const motion::Result back =
      DocumentIO::MotionResultFromJson(DocumentIO::MotionResultToJson(res));
  CHECK(back.branchJumps == res.branchJumps);
  CHECK(back.refinedSamples == res.refinedSamples);
  for (size_t i = 0; i < res.samples.size(); i++) {
    CHECK(back.samples[i].refined == res.samples[i].refined);
    REQUIRE(back.samples[i].branchJump.has_value() ==
            res.samples[i].branchJump.has_value());
    if (res.samples[i].branchJump) {
      CHECK(back.samples[i].branchJump->name ==
            res.samples[i].branchJump->name);
      CHECK(back.samples[i].branchJump->jump ==
            res.samples[i].branchJump->jump);
    }
  }
  CHECK(motion::Summarize(back, study, false).text ==
        motion::Summarize(res, study, false).text);
}

TEST_CASE("A cam follower leaving its dwell at a coarse step is no branch "
          "jump",
          "[motion][branch]") {
  // A valve on a slider, lifted by a cam that dwells half a turn and then
  // rises: at 22.5 degrees a step the lobe leaving its base circle changes
  // the valve's rate at once. That is the lift table, a function of the
  // driver the relation sets forward -- it has no branch to flip to, and
  // the watcher judged it as if the loop solve had chosen it (the engine's
  // belt study). Only what the solve chooses is watched; the dead-point
  // four-bar above still reports its jump.
  using Parts = std::map<uint64_t, std::unique_ptr<Document>>;
  Parts parts;
  parts[1] = BoxPart({-5, -5, -5}, {10, 10, 10});
  const assembly::PartResolver resolver(
      [&parts](uint64_t id, uint64_t) -> const Document* {
        const auto it = parts.find(id);
        return it == parts.end() ? nullptr : it->second.get();
      });
  AssemblyDocument d;
  const auto add = [&d](const char* name, bool fixed) {
    Occurrence o;
    o.name = name;
    o.partId = 1;
    o.fixed = fixed;
    return d.AddOccurrence(std::move(o));
  };
  const uint64_t head = add("Head", true), camOcc = add("Cam", false),
                 valveOcc = add("Valve", false);
  Mate turn;
  turn.type = MateType::Revolute;
  turn.name = "cam axle";
  turn.a = Origin(head);
  turn.b = Origin(camOcc);
  turn.flipped = false;
  const uint64_t cam = d.AddMate(turn);
  Mate slide;
  slide.type = MateType::Slider;
  slide.name = "valve guide";
  slide.a = Origin(head);
  slide.b = Origin(valveOcc);
  slide.flipped = false;
  const uint64_t valve = d.AddMate(slide);
  Relation lobe;
  lobe.kind = RelationKind::Cam;
  lobe.mateA = cam;
  lobe.mateB = valve;
  lobe.dofA = MateDof::Rotation;
  lobe.dofB = MateDof::Offset;
  // Base circle 0..180, then up 8 mm by 270 and back by 360, tabulated
  // every degree: a tangent cam's lift leaves its base circle with a slope,
  // a velocity step, as the engine's flat-follower lobes do.
  for (int deg = 0; deg < 360; deg++)
    lobe.lift.emplace_back(deg, deg <= 180   ? 0.0
                                : deg <= 270 ? 8.0 * (deg - 180) / 90.0
                                             : 8.0 * (360 - deg) / 90.0);
  d.AddRelation(lobe);

  MotionStudy study = FullTurn(cam, 17); // 22.5 degrees a step
  study.checkClash = false;
  const motion::Result res = motion::Run(d, resolver, nullptr, study);
  REQUIRE(res.error.empty());
  INFO(motion::Table(res, study));
  double peak = 0.0;
  for (const motion::Sample& s : res.samples)
    for (const motion::Sample::MateValue& v : s.mates)
      if (v.mate == valve)
        peak = std::max(peak, v.offset);
  CHECK_THAT(peak, WithinAbs(8.0, 1e-3)); // it did lift
  CHECK(res.branchJumps == 0);
  for (const motion::Sample& s : res.samples)
    CHECK_FALSE(s.branchJump);
}

TEST_CASE("The v180 golden: a mesh study lists a seated pair as touching, "
          "and its result keeps it",
          "[motion][golden]") {
  // A block turning on a plate it is seated on: its underside on the
  // plate's top face at every pose. A mesh check called that a clash at
  // every sample; it is touching, as exact interference calls it
  // (docs/inspect/README.md#touching-is-not-a-clash), and a stored result
  // keeps the list (v180).
  using Parts = std::map<uint64_t, std::unique_ptr<Document>>;
  Parts parts;
  parts[1] = BoxPart({-50, -10, -5}, {100, 20, 5});
  parts[2] = BoxPart({-5, -5, 0}, {10, 10, 10});
  const assembly::PartResolver resolver(
      [&parts](uint64_t id, uint64_t) -> const Document* {
        const auto it = parts.find(id);
        return it == parts.end() ? nullptr : it->second.get();
      });
  AssemblyDocument d;
  Occurrence plate;
  plate.name = "Plate";
  plate.partId = 1;
  plate.fixed = true;
  const uint64_t plateOcc = d.AddOccurrence(std::move(plate));
  Occurrence block;
  block.name = "Block";
  block.partId = 2;
  const uint64_t blockOcc = d.AddOccurrence(std::move(block));
  Mate turn;
  turn.type = MateType::Revolute;
  turn.name = "turntable";
  turn.a = Origin(plateOcc);
  turn.b = Origin(blockOcc);
  turn.flipped = false;
  const uint64_t axle = d.AddMate(turn);
  MotionStudy study = FullTurn(axle, 9);
  study.name = "turn on the plate";
  study.clashMode = "mesh";
  d.AddMotionStudy(study);
  if (const char* out = std::getenv("LOBOLE_WRITE_GOLDEN_VNEXT_ASM"))
    REQUIRE(DocumentIO::SaveAssembly(d, out).has_value());

  const fs::path golden = fs::path(TestSourceDir()) /
                          "tests/golden/assembly/v180-touching-sample.json";
  auto g = DocumentIO::LoadAssembly(golden.string());
  REQUIRE(g.has_value());
  REQUIRE((*g)->GetMotionStudies().size() == 1);
  const motion::Result res =
      motion::Run(**g, resolver, nullptr, (*g)->GetMotionStudies()[0]);
  REQUIRE(res.error.empty());
  INFO(motion::Table(res, (*g)->GetMotionStudies()[0]));
  CHECK(res.clean);
  REQUIRE(res.samples.size() == 9);
  for (const motion::Sample& s : res.samples) {
    CHECK(s.clashes.empty());
    REQUIRE(s.touching.size() == 1);
    CHECK(((s.touching[0].aName == "Plate" && s.touching[0].bName == "Block") ||
           (s.touching[0].aName == "Block" && s.touching[0].bName == "Plate")));
  }
  const motion::Result back =
      DocumentIO::MotionResultFromJson(DocumentIO::MotionResultToJson(res));
  REQUIRE(back.samples.size() == res.samples.size());
  for (size_t i = 0; i < back.samples.size(); i++) {
    REQUIRE(back.samples[i].touching.size() == 1);
    CHECK(back.samples[i].touching[0].a == res.samples[i].touching[0].a);
    CHECK(back.samples[i].touching[0].bName ==
          res.samples[i].touching[0].bName);
  }
}
