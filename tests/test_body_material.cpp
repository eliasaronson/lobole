// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A body's material after a feature RE-OWNS it: a Boolean, a body pattern or
// mirror, and a split each erase the body they were given and make a new one
// owned by themselves
// (docs/rendering/materials.md#the-feature-that-holds-a-material). The
// material stamp keys on the creating feature, so without a rule for these
// the result took the re-owning feature's own material -- empty unless
// someone set it -- and a steel part came out of a union weighing nothing.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cad/CADMaterials.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/features/BooleanFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/SplitFeature.h"

using Catch::Matchers::WithinRel;

namespace {

uint64_t AddBox(Document& doc, const char* name, glm::vec3 pos, double s) {
  doc.AddFeature(std::make_unique<BoxFeature>(name, pos, s, s, s));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

void SetMaterialOf(Document& doc, uint64_t featureId, const char* slug) {
  Feature* f = doc.FindFeatureById(featureId);
  REQUIRE(f != nullptr);
  f->SetMaterial(slug);
  doc.Regenerate();
}

// Every body is `slug`, and mass properties weigh it at that density.
void CheckAllBodiesAre(const Document& doc, const std::string& slug) {
  REQUIRE_FALSE(doc.GetBodies().empty());
  for (const Body& b : doc.GetBodies()) {
    INFO("body '" << b.name << "'");
    CHECK(b.material == slug);
  }
  std::string err;
  const auto mass = Measure::MassOfBodies(doc.GetBodies(), &err);
  REQUIRE(mass.has_value());
  CHECK(mass->massKnown);
  const double density =
      CADMaterials::MaterialLibrary::GetMaterialOrDefault(slug).density;
  REQUIRE(density > 0.0);
  for (const Measure::BodyMass& bm : mass->bodies)
    CHECK_THAT(bm.density, WithinRel(density, 1e-9));
}

} // namespace

TEST_CASE("A body keeps its material through a union and a subtract",
          "[materials][boolean]") {
  Document doc;
  const uint64_t a = AddBox(doc, "A", {0, 0, 0}, 2.0);
  const uint64_t b = AddBox(doc, "B", {1, 0, 0}, 2.0);
  SetMaterialOf(doc, a, "steel");
  doc.AddFeature(std::make_unique<BooleanFeature>("U", a, b, BooleanOp::Union));
  const uint64_t u = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  REQUIRE(doc.GetBodies().size() == 1);
  CheckAllBodiesAre(doc, "steel");

  const uint64_t c = AddBox(doc, "C", {0, 0, 1.5f}, 1.0);
  doc.AddFeature(
      std::make_unique<BooleanFeature>("S", u, c, BooleanOp::Subtract));
  REQUIRE(doc.GetBodies().size() == 1);
  CheckAllBodiesAre(doc, "steel");

  SECTION("a material set on the target AFTER the booleans reaches them") {
    // The edit is a cache hit (material is out of the chain hash), so no
    // Boolean executes: only the stamp can carry it through.
    SetMaterialOf(doc, a, "aluminum_6061_t6");
    CheckAllBodiesAre(doc, "aluminum_6061_t6");
  }
  SECTION("a Boolean's own material wins over its target's") {
    SetMaterialOf(doc, u, "brass");
    CheckAllBodiesAre(doc, "brass");
  }
  SECTION("the tool's material is not the result's") {
    SetMaterialOf(doc, a, "");
    SetMaterialOf(doc, b, "brass");
    for (const Body& body : doc.GetBodies())
      CHECK(body.material.empty());
  }
}

TEST_CASE("A body pattern and a mirror keep the source's material",
          "[materials][pattern]") {
  SECTION("linear, fused into one body") {
    Document doc;
    const uint64_t box = AddBox(doc, "A", {0, 0, 0}, 2.0);
    SetMaterialOf(doc, box, "steel");
    auto pat =
        std::make_unique<PatternFeature>("Arr", PatternKind::Linear, box);
    pat->axis.kind = AxisRef::Kind::X;
    pat->count = 3;
    pat->spacing = 1.0;
    doc.AddFeature(std::move(pat));
    REQUIRE(doc.GetBodies().size() == 1);
    CheckAllBodiesAre(doc, "steel");
  }
  SECTION("mirror, two bodies") {
    Document doc;
    const uint64_t box = AddBox(doc, "A", {1, 0, 0}, 2.0);
    auto pat =
        std::make_unique<PatternFeature>("Mir", PatternKind::Mirror, box);
    pat->basePlane = PlaneType::YZ;
    doc.AddFeature(std::move(pat));
    REQUIRE(doc.GetBodies().size() == 2);
    // Set after the mirror exists: the cache-hit path.
    SetMaterialOf(doc, box, "steel");
    CheckAllBodiesAre(doc, "steel");
  }
  SECTION("a pattern of a union follows the chain back to the target") {
    Document doc;
    const uint64_t a = AddBox(doc, "A", {0, 0, 0}, 2.0);
    const uint64_t b = AddBox(doc, "B", {1, 0, 0}, 2.0);
    doc.AddFeature(
        std::make_unique<BooleanFeature>("U", a, b, BooleanOp::Union));
    const uint64_t u = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
    auto pat = std::make_unique<PatternFeature>("Arr", PatternKind::Linear, u);
    pat->axis.kind = AxisRef::Kind::Y;
    pat->count = 2;
    pat->spacing = 5.0;
    doc.AddFeature(std::move(pat));
    REQUIRE(doc.GetBodies().size() == 2);
    SetMaterialOf(doc, a, "steel");
    CheckAllBodiesAre(doc, "steel");
  }
}

TEST_CASE("The pieces of a split keep the body's material",
          "[materials][split]") {
  Document doc;
  const uint64_t box = AddBox(doc, "B", {0, 0, 0}, 2.0);
  SetMaterialOf(doc, box, "steel");
  auto sp = std::make_unique<SplitFeature>("Split", box);
  sp->knife = SplitKnifeKind::Plane;
  sp->basePlane = PlaneType::XY;
  sp->offset = 1.0;
  doc.AddFeature(std::move(sp));
  REQUIRE(doc.GetBodies().size() == 2);
  CheckAllBodiesAre(doc, "steel");
  SetMaterialOf(doc, box, "brass");
  CheckAllBodiesAre(doc, "brass");
}
