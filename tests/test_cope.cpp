#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Angles.h"
#include "core/Document.h"
#include "core/features/CopeFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/PipeFeature.h"

#include <cmath>
#include <string>

using Catch::Matchers::WithinRel;

namespace {

// A straight member from `a` to `b`, as a bent path of two corners — which is
// what a straight tube IS to a bender, and what puts it in the cut list.
uint64_t AddTube(Document& doc, const std::string& name, glm::vec3 a,
                 glm::vec3 b, double od, double wall) {
  auto path =
      std::make_unique<CurveFeature>(name + " CL", Curve3DKind::BentPath);
  path->points = {a, b};
  path->radius = 60.0;
  doc.AddFeature(std::move(path));
  const uint64_t pathId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<PipeFeature>(name, pathId, od, wall));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

double VolumeOf(const Document& doc, uint64_t createdBy) {
  for (const Body& b : doc.GetBodies()) {
    if (b.createdBy == createdBy)
      return b.shape.Volume();
  }
  return -1.0;
}

} // namespace

TEST_CASE("A cope cuts the member and leaves the neighbour standing",
          "[tube][cope]") {
  // The thing a Boolean cannot do. At a node every member is coped to every
  // other and they all survive; a subtract consumes both, so coping with one
  // needs a throwaway copy of the neighbour per joint — and every throwaway
  // is a tube in the cut list nobody ordered.
  for (const double angleDeg : {90.0, 60.0, 45.0, 30.0}) {
    DYNAMIC_SECTION("joint at " << angleDeg << " degrees") {
      Document doc;
      const uint64_t run =
          AddTube(doc, "Run", {0, -300, 0}, {0, 300, 0}, 25.4, 2.4);
      const double t = angles::ToRadians(angleDeg);
      const uint64_t brace =
          AddTube(doc, "Brace",
                  {static_cast<float>(400.0 * std::cos(t)), 0.0f,
                   static_cast<float>(400.0 * std::sin(t))},
                  {0, 0, 0}, 19.05, 1.65);
      REQUIRE(doc.Regenerate());
      const double runBefore = VolumeOf(doc, run);
      const double braceBefore = VolumeOf(doc, brace);
      REQUIRE(runBefore > 0.0);
      REQUIRE(braceBefore > 0.0);

      doc.AddFeature(std::make_unique<CopeFeature>("Cope", brace,
                                                   std::vector<uint64_t>{run}));
      REQUIRE(doc.Regenerate());

      // Three things, and the first is the whole point.
      CHECK_THAT(VolumeOf(doc, run), WithinRel(runBefore, 1e-12));
      CHECK(VolumeOf(doc, brace) < braceBefore);
      CHECK(doc.GetBodies().size() == 2);
      // The member is still the member: a cope replaces it in place rather
      // than consuming it into a new body, so the cut list goes on seeing one
      // member where there is one member.
      CHECK(VolumeOf(doc, brace) > 0.0);
    }
  }
}

TEST_CASE("A cope cuts to the OUTSIDE of a hollow neighbour", "[tube][cope]") {
  // Subtract the tube itself and the member is left filling its bore — a
  // plug of metal where the run's air is, which welds to nothing and weighs
  // what it should not.
  Document doc;
  const uint64_t run =
      AddTube(doc, "Run", {0, -300, 0}, {0, 300, 0}, 25.4, 2.4);
  const uint64_t brace =
      AddTube(doc, "Brace", {400, 0, 0}, {0, 0, 0}, 19.05, 1.65);
  REQUIRE(doc.Regenerate());
  doc.AddFeature(
      std::make_unique<CopeFeature>("Cope", brace, std::vector<uint64_t>{run}));
  REQUIRE(doc.Regenerate());

  // Nothing of the brace may remain within the run's OUTER radius. The run's
  // axis is Y at x=0, so the brace (running along +X to the origin) must stop
  // at x >= the outer radius somewhere on its own axis.
  const Body* braceBody = nullptr;
  for (const Body& b : doc.GetBodies()) {
    if (b.createdBy == brace)
      braceBody = &b;
  }
  REQUIRE(braceBody != nullptr);
  glm::vec3 lo{0.0f}, hi{0.0f};
  braceBody->shape.GetBoundingBox(lo, hi, /*tight=*/true);
  // The deepest the brace reaches toward the run's axis: with equal-radius
  // tubes this would be 0, and with a smaller brace into a bigger run it
  // stops short of the axis. Either way it may not pass THROUGH.
  CHECK(lo.x > -1e-3f);
}

TEST_CASE("A cope that meets nothing says so", "[tube][cope]") {
  Document doc;
  const uint64_t run =
      AddTube(doc, "Run", {0, -300, 0}, {0, 300, 0}, 25.4, 2.4);
  // A brace 2 metres away, which the run cannot possibly touch.
  const uint64_t brace =
      AddTube(doc, "Brace", {2400, 0, 0}, {2000, 0, 0}, 19.05, 1.65);
  REQUIRE(doc.Regenerate());
  doc.AddFeature(
      std::make_unique<CopeFeature>("Cope", brace, std::vector<uint64_t>{run}));
  doc.Regenerate();
  // The feature carries the reason. A cope that removed nothing is a joint
  // that does not exist, and saying "done" would put an uncoped tube in the
  // order and a gap in the frame — both found at the welding table.
  const Feature* cope = doc.GetFeature(doc.FeatureCount() - 1);
  REQUIRE(cope != nullptr);
  CHECK(cope->GetError().find("removed nothing") != std::string::npos);
  CHECK(VolumeOf(doc, brace) > 0.0);
}

TEST_CASE("A gap is a tube's, and is refused on anything else",
          "[tube][cope]") {
  Document doc;
  const uint64_t run =
      AddTube(doc, "Run", {0, -300, 0}, {0, 300, 0}, 25.4, 2.4);
  const uint64_t brace =
      AddTube(doc, "Brace", {400, 0, 0}, {0, 0, 0}, 19.05, 1.65);
  REQUIRE(doc.Regenerate());
  auto cope =
      std::make_unique<CopeFeature>("Cope", brace, std::vector<uint64_t>{run});
  cope->gap = 0.8;
  doc.AddFeature(std::move(cope));
  REQUIRE(doc.Regenerate());
  const double withGap = VolumeOf(doc, brace);

  Document tight;
  const uint64_t run2 =
      AddTube(tight, "Run", {0, -300, 0}, {0, 300, 0}, 25.4, 2.4);
  const uint64_t brace2 =
      AddTube(tight, "Brace", {400, 0, 0}, {0, 0, 0}, 19.05, 1.65);
  tight.AddFeature(std::make_unique<CopeFeature>("Cope", brace2,
                                                 std::vector<uint64_t>{run2}));
  REQUIRE(tight.Regenerate());
  // A gap opens the cut, so it takes MORE metal away.
  CHECK(withGap < VolumeOf(tight, brace2));
}

TEST_CASE("A cope onto a BEND says so, not 'tangent'", "[tube][cope]") {
  // The chassis put a nose ring exactly where the lower rail kinks. The cope
  // refused, which was right, and blamed tangency, which was not: the reader
  // then opens a `gap` that cannot help. A joint does not belong on a bend —
  // the tube is a torus there, thinned by the bending, with no straight to
  // fit a cope against. This is that rail and that ring.
  const auto frame = [](double footX, double footY) {
    auto doc = std::make_unique<Document>();
    auto rail =
        std::make_unique<CurveFeature>("Rail CL", Curve3DKind::BentPath);
    rail->points = {{0, 150, 60},
                    {380, 195, 60},
                    {950, 230, 60},
                    {1720, 260, 60},
                    {2620, 230, 60}};
    rail->radius = 101.6;
    doc->AddFeature(std::move(rail));
    const uint64_t path = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
    doc->AddFeature(
        std::make_unique<PipeFeature>("Lower rail", path, 25.4, 1.65));
    const uint64_t run = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
    const uint64_t post =
        AddTube(*doc, "Post",
                {static_cast<float>(footX), static_cast<float>(footY), 60.0f},
                {static_cast<float>(footX), static_cast<float>(footY), 420.0f},
                19.05, 1.65);
    REQUIRE(doc->Regenerate());
    doc->AddFeature(std::make_unique<CopeFeature>("Cope", post,
                                                  std::vector<uint64_t>{run}));
    doc->Regenerate();
    return doc;
  };

  SECTION("on the kink") {
    auto doc = frame(380.0, 195.0);
    const std::string why =
        doc->GetFeature(doc->FeatureCount() - 1)->GetError();
    INFO(why);
    CHECK(why.find("lands on a BEND") != std::string::npos);
    // And it says how far clear is clear, which is the number to act on.
    CHECK(why.find("either side of that corner") != std::string::npos);
    CHECK(why.find("TANGENT") == std::string::npos);
  }

  SECTION("clear of it, the same joint cuts") {
    auto doc = frame(410.0, 197.0);
    CHECK(doc->GetFeature(doc->FeatureCount() - 1)->GetError().empty());
  }
}
