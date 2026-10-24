// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the bodies refuse before a feature is committed
// (Feature::WhyRefusedOn), and the one body a reference lands on when
// several share a createdBy (topo::FindOwningBody): a pattern's copies and a
// split's pieces.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Document.h"
#include "core/ElementHistory.h"
#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/FilletFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SplitFeature.h"

#include <cmath>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

Feature* Last(Document& doc) { return doc.GetFeature(doc.FeatureCount() - 1); }

uint64_t AddBox(Document& doc, glm::dvec3 at, double x, double y, double z) {
  doc.AddFeature(std::make_unique<BoxFeature>("Box", at, x, y, z));
  return Last(doc)->GetId();
}

// A sharp edge of `body` all of whose points satisfy `inside`.
std::string
SharpEdgeWhere(const Body& body,
               const std::function<bool(const glm::vec3&)>& inside) {
  const auto smooth = Measure::SmoothEdges(body);
  const auto polys = body.shape.GenerateEdgePolylines();
  for (const auto& [idx, name] : body.elementMap.edgeNames) {
    if (idx < 0 || static_cast<size_t>(idx) >= polys.size() ||
        smooth[static_cast<size_t>(idx)])
      continue;
    bool all = !polys[idx].empty();
    for (const glm::vec3& p : polys[idx])
      all = all && inside(p);
    if (all)
      return name;
  }
  return {};
}

} // namespace

TEST_CASE("A fillet on the second piece of a split builds", "[refuse]") {
  // Both pieces keep the split's id, and the fillet looked for the edge on
  // the first piece only: "edge ... no longer exists"
  // (reference/41-split-then-fillet).
  Document doc;
  const uint64_t box = AddBox(doc, glm::dvec3(0.0), 20, 10, 10);
  auto sp = std::make_unique<SplitFeature>("Split", box);
  sp->knife = SplitKnifeKind::Plane;
  sp->basePlane = PlaneType::YZ;
  sp->offset = 10.0;
  doc.AddFeature(std::move(sp));
  const uint64_t split = Last(doc)->GetId();
  REQUIRE(doc.GetBodies().size() == 2);
  // The piece past x = 10, whichever of the two it is.
  const Body* far = nullptr;
  for (const Body& b : doc.GetBodies()) {
    glm::dvec3 lo, hi;
    b.shape.GetBoundingBox(lo, hi);
    if (lo.x > 9.0)
      far = &b;
  }
  REQUIRE(far != nullptr);
  const std::string edge = SharpEdgeWhere(*far, [](const glm::vec3& p) {
    return p.x > 19.9f && p.z > 9.9f; // its far top edge
  });
  REQUIRE_FALSE(edge.empty());
  const double before = far->shape.Volume();
  doc.AddFeature(std::make_unique<FilletFeature>(
      "Round", std::vector<ElementRef>{{split, edge}}, 1.0));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  double after = 0.0;
  for (const Body& b : doc.GetBodies()) {
    glm::dvec3 lo, hi;
    b.shape.GetBoundingBox(lo, hi);
    if (lo.x > 9.0)
      after = b.shape.Volume();
  }
  // A quarter round of r1 along the 10 mm edge: (1 - pi/4) * 10 removed.
  CHECK_THAT(before - after, WithinAbs((1.0 - M_PI / 4.0) * 10.0, 1e-6));
}

TEST_CASE("A fillet on a pattern copy builds", "[refuse]") {
  Document doc;
  const uint64_t box = AddBox(doc, glm::dvec3(0.0), 10, 10, 10);
  auto pat = std::make_unique<PatternFeature>("Row", PatternKind::Linear, box);
  pat->count = 2;
  pat->spacing = 20.0;
  doc.AddFeature(std::move(pat));
  INFO(Last(doc)->GetError());
  REQUIRE(Last(doc)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 2);
  const Body* copy = nullptr;
  for (const Body& b : doc.GetBodies()) {
    glm::dvec3 lo, hi;
    b.shape.GetBoundingBox(lo, hi);
    if (lo.x > 15.0)
      copy = &b;
  }
  REQUIRE(copy != nullptr);
  const std::string edge = SharpEdgeWhere(
      *copy, [](const glm::vec3& p) { return p.x > 29.9f && p.z > 9.9f; });
  REQUIRE_FALSE(edge.empty());
  doc.AddFeature(std::make_unique<FilletFeature>(
      "Round", std::vector<ElementRef>{{copy->createdBy, edge}}, 1.0));
  INFO(Last(doc)->GetError());
  CHECK(Last(doc)->GetError().empty());
}

TEST_CASE("An element named by the feature that made it is refused, naming "
          "its body's id",
          "[refuse]") {
  Document doc;
  const uint64_t box = AddBox(doc, glm::dvec3(0.0), 20, 10, 10);
  const std::string rim =
      SharpEdgeWhere(doc.GetBodies().front(), [](const glm::vec3& p) {
        return p.y < 0.1f && p.z > 9.9f;
      });
  REQUIRE_FALSE(rim.empty());
  doc.AddFeature(std::make_unique<FilletFeature>(
      "Round", std::vector<ElementRef>{{box, rim}}, 2.0));
  const uint64_t round = Last(doc)->GetId();
  REQUIRE(Last(doc)->GetError().empty());
  // A sharp edge the fillet made: where its face meets an end of the box.
  const Body& body = doc.GetBodies().front();
  const auto smooth = Measure::SmoothEdges(body);
  std::string made;
  for (const auto& [idx, name] : body.elementMap.edgeNames)
    if (!smooth[static_cast<size_t>(idx)] &&
        topo::DecodeElementName(name).madeBy == round)
      made = name;
  REQUIRE_FALSE(made.empty());
  const FilletFeature again("Again", {{round, made}}, 0.5);
  CHECK_THAT(again.WhyRefusedOn(doc.GetBodies(), doc),
             ContainsSubstring(
                 "reference it as {feature_id: " + std::to_string(box) + "}"));
  const FilletFeature right("Again", {{box, made}}, 0.5);
  CHECK(right.WhyRefusedOn(doc.GetBodies(), doc).empty());

  // An edge between the fillet's face and the face it rounds into is
  // tangent: nothing to round, and the builder said "no suitable edges".
  std::string tangent;
  for (const auto& [idx, name] : body.elementMap.edgeNames)
    if (smooth[static_cast<size_t>(idx)])
      tangent = name;
  REQUIRE_FALSE(tangent.empty());
  const FilletFeature onTangent("Tangent", {{box, tangent}}, 0.5);
  CHECK_THAT(onTangent.WhyRefusedOn(doc.GetBodies(), doc),
             ContainsSubstring("meet tangentially"));

  // A pattern of the fillet, which made no body: the box's is named.
  const PatternFeature row("Row", PatternKind::Linear, round);
  CHECK_THAT(row.WhyRefusedOn(doc.GetBodies(), doc),
             ContainsSubstring("{source_id: " + std::to_string(box) + "}"));
}

TEST_CASE("A shell wall at or past the body's wall is refused before the "
          "kernel, and the default is under it",
          "[refuse][shell]") {
  // A 40 x 30 x 20 box shelled 1 mm through its top: a body whose bounding
  // box says 20 mm and whose walls are 1.
  Document doc;
  const uint64_t box = AddBox(doc, glm::dvec3(0.0), 40, 30, 20);
  const auto faceAt = [&](const glm::dvec3& p) {
    const Body& b = doc.GetBodies().front();
    for (const auto& [idx, name] : b.elementMap.names)
      if (const auto d = Measure::DistanceToElementAt(b, true, idx, p, 1e-3);
          d && *d < 1e-6)
        return name;
    return std::string();
  };
  const std::string top = faceAt({20, 15, 20});
  REQUIRE_FALSE(top.empty());
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Hollow", box, std::vector<ElementRef>{{box, top}}, 1.0));
  REQUIRE(Last(doc)->GetError().empty());
  const auto wall = ThinnestWall(doc.GetBodies().front());
  REQUIRE(wall.has_value());
  CHECK_THAT(*wall, WithinAbs(1.0, 1e-6));

  // Its outside long wall opened: the 1 mm under it is all there is.
  const std::string side = faceAt({20, 0, 10});
  REQUIRE_FALSE(side.empty());
  const ShellFeature thick("Again", box, {{box, side}}, 1.5);
  const std::string why = thick.WhyRefusedOn(doc.GetBodies(), doc);
  CHECK_THAT(why, ContainsSubstring("1 mm thick under it"));
  // A wall a tenth of the thinnest is not refused here.
  const ShellFeature thin("Again", box, {{box, side}}, *wall / 10.0);
  CHECK(thin.WhyRefusedOn(doc.GetBodies(), doc).empty());
}
