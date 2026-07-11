#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/ExtrudeFeature.h"
#include "core/FilletFeature.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"

#include <array>
#include <filesystem>
#include <functional>
#include <string>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

static const SketchPlane kXY(PlaneType::XY);

namespace {

// A 4x4 rectangle sketch + a NewBody extrude of height 3.
void BuildRectPrism(Document& doc, uint64_t& extrudeId) {
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  sk->AddRectangle({0, 0}, {4, 4});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketchId, 3.0,
                                                  ExtrudeMode::NewBody));
  extrudeId = doc.GetFeature(1)->GetId();
}

// Picks an edge of the first body by geometry (every polyline point satisfies
// `pred`) and returns its stable ElementRef — the pick->store path a user
// click takes. Fails the test if no such named edge exists.
ElementRef PickEdge(const Document& doc,
                    const std::function<bool(const glm::vec3&)>& pred) {
  const Body& body = doc.GetBodies().at(0);
  const auto polylines = body.shape.GenerateEdgePolylines();
  for (size_t e = 0; e < polylines.size(); e++) {
    const auto& poly = polylines[e];
    if (poly.size() < 2) {
      continue;
    }
    bool all = true;
    for (const auto& p : poly) {
      if (!pred(p)) {
        all = false;
        break;
      }
    }
    if (!all) {
      continue;
    }
    const std::string* name = body.elementMap.EdgeNameOf(static_cast<int>(e));
    REQUIRE(name != nullptr);
    return ElementRef{body.createdBy, *name};
  }
  FAIL("no edge matches the predicate");
  return {};
}

double BodyVolume(const Document& doc) {
  return doc.GetBodies().at(0).shape.Volume();
}

// A top edge of the prism: both endpoints at z == height.
std::function<bool(const glm::vec3&)> AtZ(float z) {
  return [z](const glm::vec3& p) { return std::abs(p.z - z) < 1e-3f; };
}

} // namespace

TEST_CASE("Fillet rounds a referenced edge and names the blend face",
          "[fillet]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext);
  const double before = BodyVolume(doc);
  const ElementRef edge = PickEdge(doc, AtZ(3.0f));
  CHECK(edge.featureId == ext);

  doc.AddFeature(
      std::make_unique<FilletFeature>("Fillet", std::vector{edge}, 0.5));
  const uint64_t filletId = doc.GetFeature(2)->GetId();
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);

  // A fillet removes (1 - pi/4) r^2 * length of material along the edge.
  const double removed = (1.0 - M_PI / 4.0) * 0.5 * 0.5 * 4.0;
  CHECK_THAT(BodyVolume(doc), WithinRel(before - removed, 1e-3));

  // The blend face is named from its spine edge; the spine edge itself is
  // consumed, so its name no longer resolves.
  const ElementMap& map = doc.GetBodies()[0].elementMap;
  CHECK(map.Resolve(edge.name + ";G" + std::to_string(filletId)).has_value());
  CHECK_FALSE(map.ResolveEdge(edge.name).has_value());
  // The body still belongs to the extrude (fillet modifies in place).
  CHECK(doc.GetBodies()[0].createdBy == ext);
}

TEST_CASE("Chamfer bevels a referenced edge", "[fillet]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext);
  const double before = BodyVolume(doc);
  const ElementRef edge = PickEdge(doc, AtZ(3.0f));

  doc.AddFeature(std::make_unique<FilletFeature>(
      "Chamfer", std::vector{edge}, 0.5, EdgeBlendOp::Chamfer));
  const uint64_t chamferId = doc.GetFeature(2)->GetId();
  REQUIRE(doc.Regenerate());

  // A 45-degree chamfer removes r^2/2 * length.
  CHECK_THAT(BodyVolume(doc),
             WithinRel(before - 0.5 * 0.5 * 0.5 * 4.0, 1e-3));
  const ElementMap& map = doc.GetBodies()[0].elementMap;
  CHECK(map.Resolve(edge.name + ";G" + std::to_string(chamferId)).has_value());
}

TEST_CASE("Fillet follows an upstream height edit (the payoff)", "[fillet]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext);
  const ElementRef edge = PickEdge(doc, AtZ(3.0f));
  doc.AddFeature(
      std::make_unique<FilletFeature>("Fillet", std::vector{edge}, 0.5));
  const uint64_t filletId = doc.GetFeature(2)->GetId();
  REQUIRE(doc.Regenerate());

  // Raise the pad: the same top edge (its name is height-independent) gets
  // filleted at the new height.
  auto* pad = dynamic_cast<ExtrudeFeature*>(doc.FindFeatureById(ext));
  REQUIRE(pad != nullptr);
  pad->distance = 5.0;
  REQUIRE(doc.Regenerate());

  const double removed = (1.0 - M_PI / 4.0) * 0.5 * 0.5 * 4.0;
  CHECK_THAT(BodyVolume(doc), WithinRel(4.0 * 4.0 * 5.0 - removed, 1e-3));
  CHECK(doc.GetBodies()[0]
            .elementMap.Resolve(edge.name + ";G" + std::to_string(filletId))
            .has_value());
  // The blend sits at the new top.
  glm::vec3 mn, mx;
  doc.GetBodies()[0].shape.GetBoundingBox(mn, mx);
  CHECK_THAT(mx.z, WithinAbs(5.0f, 1e-3f));
}

TEST_CASE("A dangling edge ref errors the fillet with reattach info",
          "[fillet]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext);
  // A hole through the pad; fillet its top rim.
  auto* sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  const uint32_t hole = sk->AddEntity(sketch::Circle{{2, 2}, 1.0});
  REQUIRE(doc.Regenerate());
  const ElementRef rim = PickEdge(doc, [](const glm::vec3& p) {
    return std::abs(p.z - 3.0f) < 1e-3f &&
           std::abs(glm::length(glm::vec2(p.x, p.y) - glm::vec2(2, 2)) - 1.0f) <
               1e-3f;
  });
  doc.AddFeature(
      std::make_unique<FilletFeature>("Fillet", std::vector{rim}, 0.2));
  auto* fillet = dynamic_cast<FilletFeature*>(doc.GetFeature(2));
  REQUIRE(doc.Regenerate());

  // Delete the hole: the rim's generating entity is gone; the fillet's ref
  // dangles and regeneration stops with a specific error, never a silent
  // renumber onto some other edge.
  sk->DeleteEntity(hole);
  CHECK_FALSE(doc.Regenerate());
  CHECK(fillet->GetError().find(rim.name) != std::string::npos);
}

TEST_CASE("An impossible radius fails with an error, not a crash", "[fillet]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext);
  const ElementRef edge = PickEdge(doc, AtZ(3.0f));
  doc.AddFeature(
      std::make_unique<FilletFeature>("Fillet", std::vector{edge}, 50.0));
  auto* fillet = dynamic_cast<FilletFeature*>(doc.GetFeature(2));
  CHECK_FALSE(doc.Regenerate());
  CHECK_FALSE(fillet->GetError().empty());
}

TEST_CASE("Fillet radius can be driven by a document parameter", "[fillet]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext);
  REQUIRE(doc.SetParameter("r", "0.5"));
  const ElementRef edge = PickEdge(doc, AtZ(3.0f));
  auto fil = std::make_unique<FilletFeature>("Fillet", std::vector{edge}, 0.1);
  fil->radiusExpr = "r";
  doc.AddFeature(std::move(fil));
  REQUIRE(doc.Regenerate());

  const double removed = (1.0 - M_PI / 4.0) * 0.5 * 0.5 * 4.0;
  CHECK_THAT(BodyVolume(doc), WithinRel(4.0 * 4.0 * 3.0 - removed, 1e-3));

  REQUIRE(doc.SetParameter("r", "1.0"));
  REQUIRE(doc.Regenerate());
  const double removed2 = (1.0 - M_PI / 4.0) * 1.0 * 1.0 * 4.0;
  CHECK_THAT(BodyVolume(doc), WithinRel(4.0 * 4.0 * 3.0 - removed2, 1e-3));
}

TEST_CASE("Fillet and chamfer round-trip through JSON (v17)", "[fillet]") {
  const fs::path path = fs::temp_directory_path() / "designer_fillet_test.json";
  double volume = 0.0;
  std::string edgeName;
  {
    Document doc;
    uint64_t ext = 0;
    BuildRectPrism(doc, ext);
    const ElementRef edge = PickEdge(doc, AtZ(3.0f));
    edgeName = edge.name;
    auto fil = std::make_unique<FilletFeature>("Fillet", std::vector{edge}, 0.5);
    fil->radiusExpr = "0.25 + 0.25";
    doc.AddFeature(std::move(fil));
    doc.AddFeature(std::make_unique<FilletFeature>(
        "Chamfer", std::vector{PickEdge(doc, AtZ(0.0f))}, 0.3,
        EdgeBlendOp::Chamfer));
    REQUIRE(doc.Regenerate());
    volume = BodyVolume(doc);
    REQUIRE(DocumentIO::Save(doc, path.string()));
  }
  const auto loaded = DocumentIO::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* fil = dynamic_cast<FilletFeature*>(loaded->GetFeature(2));
  REQUIRE(fil != nullptr);
  CHECK(fil->op == EdgeBlendOp::Fillet);
  CHECK(fil->radiusExpr == "0.25 + 0.25");
  REQUIRE(fil->edges.size() == 1);
  CHECK(fil->edges[0].name == edgeName);
  auto* cha = dynamic_cast<FilletFeature*>(loaded->GetFeature(3));
  REQUIRE(cha != nullptr);
  CHECK(cha->op == EdgeBlendOp::Chamfer);
  REQUIRE(loaded->Regenerate());
  CHECK_THAT(loaded->GetBodies().at(0).shape.Volume(),
             WithinRel(volume, 1e-6));
  fs::remove(path);
}
