// Motion studies (docs/agent-scale-plan.md, G2): one mate DOF driven over a
// range, the assembly solved at every sample, every pair checked. The gate
// the plan asked for: "rotate through the range, confirm zero collision at
// every step, print the table" — and the honesty the table owes: a sample
// that does not solve is reported unsolved, never drawn as the last good
// pose.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/AssemblySolve.h"
#include "core/CollisionCache.h"
#include "core/Document.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/io/GlbAnimation.h"
#include "core/io/Serialization.h"

#include <cmath>
#include <filesystem>
#include <fstream>
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
    return [this](uint64_t id) -> const Document* {
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
  const fs::path golden = fs::path(DESIGNER_SOURCE_DIR) /
                          "tests/golden/assembly/v87-motion-study.json";
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
  CHECK(warm.pairsCached == warm.pairsChecked);
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
