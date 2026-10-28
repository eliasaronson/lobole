// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Heavy features off the frame, the core half
// (docs/ui/long-operations.md#heavy-features): a document that builds them
// so stops its regeneration at one, the request carries everything its
// Execute reads, the build runs on copies, the result lands through the
// regen cache and is found by every document of the session, and a cancel
// leaves the evaluation marked not built.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/HeavyBuild.h"
#include "core/Measure.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DeleteFaceFeature.h"
#include "core/io/Serialization.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBlock = 40.0 * 30.0 * 20.0;

const Body* BodyOf(const Document& doc, uint64_t createdBy) {
  for (const Body& b : doc.GetBodies())
    if (b.createdBy == createdBy)
      return &b;
  return nullptr;
}

ElementRef FaceAt(const Body& body, const glm::dvec3& p) {
  std::string best;
  double bestD = std::numeric_limits<double>::infinity();
  for (const auto& [idx, name] : body.elementMap.names) {
    const auto d = Measure::DistanceToElementAt(body, true, idx, p, 1e-3);
    if (d && *d < bestD) {
      bestD = *d;
      best = name;
    }
  }
  REQUIRE_FALSE(best.empty());
  return ElementRef{body.createdBy, best};
}

// The 40x30x20 block with a D10 through bore; the body's createdBy.
uint64_t BlockWithBore(Document& doc) {
  doc.AddFeature(
      std::make_unique<BoxFeature>("Block", glm::dvec3(0.0), 40, 30, 20));
  const uint64_t block = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::dvec3(20, 15, -1), glm::dvec3(0, 0, 1), 5.0, 22.0));
  const uint64_t tool = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(
      std::make_unique<BooleanFeature>("Op", block, tool, BooleanOp::Subtract));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

std::unique_ptr<Feature> FillBore(const Document& doc, uint64_t body) {
  return std::make_unique<DeleteFaceFeature>(
      "Fill", body,
      std::vector<ElementRef>{FaceAt(*BodyOf(doc, body), {25, 15, 10})});
}

// Every Delete Face builds off the frame, whatever it is estimated at, and
// nothing a previous case built is found.
struct Forced {
  Forced() {
    heavy::SetTestOptions({true, 0});
    heavy::ForgetAll();
  }
  ~Forced() {
    heavy::SetTestOptions({});
    heavy::ForgetAll();
  }
};

// What the worker does, here on the test's thread.
heavy::Result Build(const Document& doc) {
  std::shared_ptr<heavy::Request> req = doc.PendingBuildShared();
  REQUIRE(req != nullptr);
  heavy::Detach(*req);
  std::optional<heavy::Result> r = heavy::Run(*req, nullptr, nullptr);
  REQUIRE(r.has_value());
  return *r;
}

} // namespace

TEST_CASE("A heavy feature's estimate is derived from what it would act on",
          "[heavy]") {
  heavy::SetTestOptions({});
  Document doc;
  const uint64_t body = BlockWithBore(doc);
  const auto one = FillBore(doc, body);
  CHECK(one->MayBuildOffFrame());
  CHECK_FALSE(heavy::BuildsOffFrame(*one, doc.GetBodies()));
  // The grill's four holes, 916 faces: well past a frame.
  auto many = std::make_unique<DeleteFaceFeature>(
      "Many", body, std::vector<ElementRef>(916, ElementRef{body, "x"}));
  CHECK(many->BuildCostS(doc.GetBodies()) > 10.0);
  CHECK(heavy::BuildsOffFrame(*many, doc.GetBodies()));
  // A feature that does not declare itself heavy never is.
  CHECK_FALSE(doc.GetFeature(0)->MayBuildOffFrame());
}

TEST_CASE("A document that builds off the frame stops at a heavy feature and "
          "lands its build through the regen cache",
          "[heavy]") {
  const Forced forced;
  Document doc;
  doc.SetBuildsOffFrame(true);
  const uint64_t body = BlockWithBore(doc);
  const double before = BodyOf(doc, body)->shape.Volume();
  REQUIRE_THAT(before, WithinRel(kBlock - kPi * 25 * 20, 1e-9));
  doc.AddFeature(FillBore(doc, body));
  const uint64_t fill = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  // Stopped: building, the bodies those before it, no tip to bake.
  CHECK(doc.BuildStateOf(fill) == Document::BuildState::Building);
  REQUIRE(doc.PendingBuild() != nullptr);
  CHECK(doc.PendingBuild()->featureId == fill);
  CHECK(doc.TipChainHash() == 0);
  CHECK_THAT(BodyOf(doc, body)->shape.Volume(), WithinRel(before, 1e-12));
  // The request references the boolean whose body it edits, and nothing it
  // does not.
  REQUIRE(doc.PendingBuild()->refs.size() == 1);
  CHECK(doc.PendingBuild()->refs[0]->GetId() == body);

  // A feature after it waits.
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Later", glm::dvec3(5, 5, -1), glm::dvec3(0, 0, 1), 1.0, 22.0));
  const uint64_t later = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  CHECK(doc.BuildStateOf(later) == Document::BuildState::Waiting);

  // The build, on copies: the input shapes are not the document's.
  const size_t key = doc.PendingBuild()->hash;
  const heavy::Result built = Build(doc);
  CHECK(built.hash == key);
  doc.AdoptBuilt(built);
  REQUIRE(doc.Regenerate());
  CHECK(doc.PendingBuild() == nullptr);
  CHECK_FALSE(doc.HasUnbuilt());
  CHECK(doc.TipChainHash() != 0);
  CHECK_THAT(BodyOf(doc, body)->shape.Volume(), WithinRel(kBlock, 1e-9));
  CHECK(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());

  // The same as a build inline, on a document that does not opt in.
  Document inlineDoc;
  const uint64_t b2 = BlockWithBore(inlineDoc);
  heavy::SetTestOptions({});
  inlineDoc.AddFeature(FillBore(inlineDoc, b2));
  CHECK_THAT(BodyOf(inlineDoc, b2)->shape.Volume(), WithinRel(kBlock, 1e-12));
}

TEST_CASE("A finished build is found by every document of the session",
          "[heavy]") {
  const Forced forced;
  Document doc;
  doc.SetBuildsOffFrame(true);
  const uint64_t body = BlockWithBore(doc);
  doc.AddFeature(FillBore(doc, body));
  heavy::Put(Build(doc));
  REQUIRE(heavy::StoreSize() == 1);

  // A second part with the same recipe -- a reopen, a copy -- is built at
  // once, executing nothing for it.
  Document again;
  again.SetBuildsOffFrame(true);
  const uint64_t b2 = BlockWithBore(again);
  again.AddFeature(FillBore(again, b2));
  CHECK(again.PendingBuild() == nullptr);
  CHECK_FALSE(again.HasUnbuilt());
  CHECK(again.LastRegenStats().executed == 0);
  CHECK_THAT(BodyOf(again, b2)->shape.Volume(), WithinRel(kBlock, 1e-9));
}

TEST_CASE("An edit before a building feature asks for a new build; one after "
          "it keeps the request",
          "[heavy]") {
  const Forced forced;
  Document doc;
  doc.SetBuildsOffFrame(true);
  const uint64_t body = BlockWithBore(doc);
  doc.AddFeature(FillBore(doc, body));
  const uint64_t serial = doc.PendingSerial();
  const size_t key = doc.PendingBuild()->hash;
  // Downstream: the same evaluation is still what is asked for.
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Later", glm::dvec3(5, 5, -1), glm::dvec3(0, 0, 1), 1.0, 22.0));
  CHECK(doc.PendingSerial() == serial);
  CHECK(doc.PendingBuild()->hash == key);
  // Upstream: the bore moves, so the fill builds from another body.
  auto* bore = dynamic_cast<CylinderFeature*>(doc.GetFeature(1));
  REQUIRE(bore != nullptr);
  auto moved = std::make_unique<CylinderFeature>(*bore);
  moved->radius = 4.0;
  doc.ReplaceFeature(1, std::move(moved));
  REQUIRE(doc.PendingBuild() != nullptr);
  CHECK(doc.PendingSerial() != serial);
  CHECK(doc.PendingBuild()->hash != key);
}

TEST_CASE("A cancelled build leaves the feature not built until it is asked "
          "for, and an edit to its inputs builds it again",
          "[heavy]") {
  const Forced forced;
  Document doc;
  doc.SetBuildsOffFrame(true);
  const uint64_t body = BlockWithBore(doc);
  doc.AddFeature(FillBore(doc, body));
  const uint64_t fill = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.CancelBuild(doc.PendingBuild()->hash);
  CHECK(doc.BuildStateOf(fill) == Document::BuildState::Cancelled);
  CHECK(doc.PendingBuild() == nullptr);
  CHECK(doc.TipChainHash() == 0); // never baked as the recipe's answer
  // Again and again: a regeneration does not ask for it on its own.
  REQUIRE(doc.Regenerate());
  CHECK(doc.BuildStateOf(fill) == Document::BuildState::Cancelled);
  // Asked for.
  CHECK(doc.RequestBuild(fill));
  CHECK(doc.BuildStateOf(fill) == Document::BuildState::Building);
  CHECK(doc.PendingBuild() != nullptr);
  CHECK_FALSE(doc.RequestBuild(fill)); // not cancelled now

  // Cancelled again, then an upstream edit: another evaluation, built.
  doc.CancelBuild(doc.PendingBuild()->hash);
  auto* bore = dynamic_cast<CylinderFeature*>(doc.GetFeature(1));
  auto moved = std::make_unique<CylinderFeature>(*bore);
  moved->radius = 4.0;
  doc.ReplaceFeature(1, std::move(moved));
  CHECK(doc.BuildStateOf(fill) == Document::BuildState::Building);
}

TEST_CASE("A build stops on its cancel and keeps nothing", "[heavy]") {
  const Forced forced;
  Document doc;
  doc.SetBuildsOffFrame(true);
  const uint64_t body = BlockWithBore(doc);
  doc.AddFeature(FillBore(doc, body));
  std::shared_ptr<heavy::Request> req = doc.PendingBuildShared();
  heavy::Detach(*req);
  const std::atomic<bool> cancel{true};
  CHECK_FALSE(heavy::Run(*req, &cancel, nullptr).has_value());
  // Slowed by the knob, the cancel is seen while it waits.
  heavy::SetTestOptions({true, 60000});
  std::atomic<bool> later{false};
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    later.store(true);
  });
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(heavy::Run(*req, &later, nullptr).has_value());
  stopper.join();
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
}

TEST_CASE("A copy of a document builds its heavy features inline", "[heavy]") {
  const Forced forced;
  Document doc;
  doc.SetBuildsOffFrame(true);
  const uint64_t body = BlockWithBore(doc);
  doc.AddFeature(FillBore(doc, body));
  REQUIRE(doc.PendingBuild() != nullptr);
  const Document copy(doc);
  CHECK_FALSE(copy.BuildsOffFrame());
  CHECK_FALSE(copy.HasUnbuilt());
  CHECK_THAT(BodyOf(copy, body)->shape.Volume(), WithinRel(kBlock, 1e-9));
  // ...and files what it built, which the document then finds.
  REQUIRE(doc.Regenerate());
  CHECK_FALSE(doc.HasUnbuilt());
}
