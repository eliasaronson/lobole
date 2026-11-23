// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/ElementHistory.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <numbers>
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

// A vertical edge at sketch corner (x, y): every polyline point shares that
// (x, y), only z varies.
std::function<bool(const glm::vec3&)> AtCorner(float x, float y) {
  return [x, y](const glm::vec3& p) {
    return std::abs(p.x - x) < 1e-3f && std::abs(p.y - y) < 1e-3f;
  };
}

// A concave "envelope" pentagon (a notch at the top) + a tall NewBody extrude.
// Its top-corner vertices are each shared by a vertical edge AND the two
// top/side edges meeting there — the ambiguity the blend-edge remap must break.
void BuildPentagonPrism(Document& doc, uint64_t& extrudeId) {
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  sk->AddEntity(sketch::Line{{-1.2344f, 0.0f}, {1.4691f, 0.0f}});
  sk->AddEntity(sketch::Line{{1.4691f, 0.0f}, {1.4691f, 2.1220f}});
  sk->AddEntity(sketch::Line{{1.4691f, 2.1220f}, {0.0f, 1.4385f}});
  sk->AddEntity(sketch::Line{{0.0f, 1.4385f}, {-1.2344f, 2.1220f}});
  sk->AddEntity(sketch::Line{{-1.2344f, 2.1220f}, {-1.2344f, 0.0f}});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketchId, 3.425,
                                                  ExtrudeMode::NewBody));
  extrudeId = doc.GetFeature(1)->GetId();
}

// True when `ref`'s edge on `body` is vertical (spans z, constant x/y).
bool RefIsVertical(const Body& body, const ElementRef& ref) {
  const auto idx = body.elementMap.ResolveEdge(ref.name);
  if (!idx.has_value())
    return false;
  const auto polys = body.shape.GenerateEdgePolylines();
  const auto& poly = polys.at(static_cast<size_t>(*idx));
  return poly.size() >= 2 && std::abs(poly.front().z - poly.back().z) > 1e-3f &&
         std::abs(poly.front().x - poly.back().x) < 1e-3f &&
         std::abs(poly.front().y - poly.back().y) < 1e-3f;
}

// How many of `body`'s edges pass within `tol` of `probe` — the number of
// candidates ResolveEdgeOnBody's geometric fallback can't distinguish between.
int EdgesNear(const Body& body, const glm::vec3& probe, float tol = 1e-3f) {
  const auto distToPoly = [](const glm::vec3& p,
                             const std::vector<glm::vec3>& poly) {
    float d = std::numeric_limits<float>::max();
    for (size_t i = 0; i + 1 < poly.size(); i++) {
      const glm::vec3 a = poly[i];
      const glm::vec3 ab = poly[i + 1] - a;
      const float len2 = glm::dot(ab, ab);
      const float t = len2 > 0.0f
                          ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f)
                          : 0.0f;
      d = std::min(d, glm::length(p - (a + ab * t)));
    }
    return d;
  };
  int n = 0;
  for (const auto& poly : body.shape.GenerateEdgePolylines())
    if (poly.size() >= 2 && distToPoly(probe, poly) < tol)
      n++;
  return n;
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

TEST_CASE("A second edge picked after the first blend regen still fillets",
          "[fillet]") {
  // The GUI multi-pick flow: pick edge, preview regenerates (body is now
  // filleted), pick the NEXT edge against that regenerated body. The second
  // ref must resolve against the fillet's pre-blend input on the next regen.
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext);
  const double before = BodyVolume(doc);

  const ElementRef first = PickEdge(doc, AtZ(3.0f));
  doc.AddFeature(
      std::make_unique<FilletFeature>("Fillet", std::vector{first}, 0.3));
  auto* fil = dynamic_cast<FilletFeature*>(doc.GetFeature(2));
  REQUIRE(fil != nullptr);
  REQUIRE(doc.Regenerate());

  // Second pick reads name + createdBy from the regenerated (blended) body,
  // exactly like the viewport click does — its toponame embeds the blend's
  // own tag, so it must be remapped onto the blend's input body before it is
  // stored (topo::ResolveEdgeOnBody, the fix for the multi-pick bug).
  const ElementRef picked = PickEdge(doc, AtZ(0.0f));
  CHECK(picked.featureId == first.featureId); // same-body guard must pass
  const Body& displayed = doc.GetBodies().at(0);
  const auto polys = displayed.shape.GenerateEdgePolylines();
  const auto idx = displayed.elementMap.ResolveEdge(picked.name);
  REQUIRE(idx.has_value());
  const auto& poly = polys.at(static_cast<size_t>(*idx));
  const std::vector<Body>* before2 = doc.CachedBodiesAfter(1); // pre-blend
  REQUIRE(before2 != nullptr);
  const auto second = topo::ResolveEdgeOnBody(before2->at(0), picked.name,
                                              topo::PolylineArcMidpoint(poly));
  REQUIRE(second.has_value());
  fil->edges.push_back(*second);
  REQUIRE(doc.Regenerate());
  CHECK(fil->GetError().empty());

  const double removed = (1.0 - M_PI / 4.0) * 0.3 * 0.3 * 4.0 * 2.0;
  CHECK_THAT(BodyVolume(doc), WithinRel(before - removed, 1e-2));
}

TEST_CASE("A vertical second edge remaps to the vertical edge, not a neighbour",
          "[fillet]") {
  // Regression: chamfer one vertical edge, then pick a DIFFERENT vertical edge
  // on the regenerated body. Its displayed toponame doesn't resolve on the
  // pre-blend input, so ResolveEdgeOnBody falls back to geometry. Probing with
  // poly[size/2] (an ENDPOINT for a straight edge) lands on a shared corner and
  // snaps to the horizontal edge meeting there — the "second pick selects the
  // bottom/top horizontal edge instead" bug. The arc-length midpoint lies in
  // the edge interior and resolves to the vertical edge.
  Document doc;
  uint64_t ext = 0;
  BuildPentagonPrism(doc, ext);

  const ElementRef first = PickEdge(doc, AtCorner(-1.2344f, 0.0f));
  doc.AddFeature(std::make_unique<FilletFeature>("Chamfer", std::vector{first},
                                                 0.5, EdgeBlendOp::Chamfer));
  auto* fil = dynamic_cast<FilletFeature*>(doc.GetFeature(2));
  REQUIRE(fil != nullptr);
  REQUIRE(doc.Regenerate());

  // Second pick: the peak-left vertical edge, whose top vertex is shared with
  // that face's top horizontal edge.
  const ElementRef picked = PickEdge(doc, AtCorner(-1.2344f, 2.1220f));
  const Body& displayed = doc.GetBodies().at(0);
  CHECK(RefIsVertical(displayed, picked)); // the highlight the user sees
  const auto polys = displayed.shape.GenerateEdgePolylines();
  const auto idx = displayed.elementMap.ResolveEdge(picked.name);
  REQUIRE(idx.has_value());
  const auto& poly = polys.at(static_cast<size_t>(*idx));

  const std::vector<Body>* input = doc.CachedBodiesAfter(1); // pre-blend
  REQUIRE(input != nullptr);

  // The old probe poly[size/2] is an endpoint: it sits on a shared corner, so
  // several input edges are equidistant and the fallback's winner is a tie-
  // break accident (this is the regression). The arc-length midpoint is on
  // exactly one edge, so the resolve is unambiguous.
  CHECK(EdgesNear(input->at(0), poly[poly.size() / 2]) >= 2);
  CHECK(EdgesNear(input->at(0), topo::PolylineArcMidpoint(poly)) == 1);

  // The shipped probe resolves to the vertical edge.
  const auto midpointHit = topo::ResolveEdgeOnBody(
      input->at(0), picked.name, topo::PolylineArcMidpoint(poly));
  REQUIRE(midpointHit.has_value());
  CHECK(RefIsVertical(input->at(0), *midpointHit));

  // ...and chamfering it succeeds (two vertical chamfers, no dangling ref).
  fil->edges.push_back(*midpointHit);
  REQUIRE(doc.Regenerate());
  CHECK(fil->GetError().empty());
}

TEST_CASE("Chamfer bevels a referenced edge", "[fillet]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext);
  const double before = BodyVolume(doc);
  const ElementRef edge = PickEdge(doc, AtZ(3.0f));

  doc.AddFeature(std::make_unique<FilletFeature>("Chamfer", std::vector{edge},
                                                 0.5, EdgeBlendOp::Chamfer));
  const uint64_t chamferId = doc.GetFeature(2)->GetId();
  REQUIRE(doc.Regenerate());

  // A 45-degree chamfer removes r^2/2 * length.
  CHECK_THAT(BodyVolume(doc), WithinRel(before - 0.5 * 0.5 * 0.5 * 4.0, 1e-3));
  const ElementMap& map = doc.GetBodies()[0].elementMap;
  CHECK(map.Resolve(edge.name + ";G" + std::to_string(chamferId)).has_value());
}

// docs/modeling/kernel-cases.md (solidcraft/ladder/08-chamfers/004 and
// fillets/every-box-corner-three-edges-chamfer-2): chamfers that meet three
// at a corner are mitred, each bevel its own plane. OpenCASCADE closes such
// a corner with a plane through the bevels' ends, a triangle more and
// material the bevels leave. At a corner of the box [0,a]x[0,b]x[0,c] the
// three bevels of distance d remove d^2/2 (a + b + c), less each pair's
// overlap d^3/3, plus the three's d^3/4 (by inclusion and exclusion).
TEST_CASE("Three chamfers meeting at a corner are mitred", "[fillet]") {
  const auto pick = [](const Document& doc, auto pred) {
    std::vector<ElementRef> out;
    const Body& body = doc.GetBodies().at(0);
    const auto polylines = body.shape.GenerateEdgePolylines();
    for (size_t e = 0; e < polylines.size(); e++) {
      bool all = polylines[e].size() >= 2;
      for (const auto& p : polylines[e])
        all = all && pred(p);
      if (all)
        out.push_back(
            {body.createdBy, *body.elementMap.EdgeNameOf(static_cast<int>(e))});
    }
    return out;
  };
  const auto faces = [](const Document& doc) {
    return doc.GetBodies().at(0).elementMap.names.size();
  };
  const auto near = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
  SECTION("one corner of a 60 x 40 x 20 block, in either order") {
    for (const bool reversed : {false, true}) {
      Document doc;
      doc.AddFeature(
          std::make_unique<BoxFeature>("Stock", glm::vec3(0), 60, 40, 20));
      std::vector<ElementRef> edges = pick(doc, [&](const glm::vec3& p) {
        const int at = near(p.x, 60) + near(p.y, 40) + near(p.z, 20);
        return at >= 2;
      });
      REQUIRE(edges.size() == 3);
      if (reversed)
        std::reverse(edges.begin(), edges.end());
      doc.AddFeature(std::make_unique<FilletFeature>("Bevel", edges, 3.0,
                                                     EdgeBlendOp::Chamfer));
      INFO(doc.GetFeature(1)->GetError());
      REQUIRE(doc.GetFeature(1)->GetError().empty());
      const double d = 3.0;
      const double removed = d * d / 2 * 120 - d * d * d + d * d * d / 4;
      CHECK_THAT(BodyVolume(doc), WithinRel(48000.0 - removed, 1e-9));
      CHECK(faces(doc) == 9); // six and the three bevels: no corner patch
    }
  }
  SECTION("every edge of a 25 mm cube") {
    Document doc;
    doc.AddFeature(
        std::make_unique<BoxFeature>("Cube", glm::vec3(0), 25, 25, 25));
    const std::vector<ElementRef> edges =
        pick(doc, [](const glm::vec3&) { return true; });
    REQUIRE(edges.size() == 12);
    doc.AddFeature(std::make_unique<FilletFeature>("Bevel", edges, 2.5,
                                                   EdgeBlendOp::Chamfer));
    INFO(doc.GetFeature(1)->GetError());
    REQUIRE(doc.GetFeature(1)->GetError().empty());
    // Per corner as above with a = b = c = h, the half side: 8 h^3 - 12 h
    // d^2 + 6 d^3 over the eight octants.
    const double h = 12.5, d = 2.5;
    CHECK_THAT(BodyVolume(doc),
               WithinRel(8 * h * h * h - 12 * h * d * d + 6 * d * d * d, 1e-9));
    CHECK(faces(doc) == 18);
  }
  SECTION("every edge of a 40 x 30 x 12.5 box, in an order that was refused") {
    Document doc;
    doc.AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0), 40, 30, 12.5));
    // solidcraft/fillets/3655: each edge by a point on it, in its order.
    const std::vector<glm::vec3> at = {
        {40, 15, 12.5}, {20, 0, 0}, {0, 30, 6},    {0, 15, 12.5},
        {40, 30, 6},    {0, 0, 6},  {40, 15, 0},   {40, 0, 6},
        {20, 30, 12.5}, {0, 15, 0}, {20, 0, 12.5}, {20, 30, 0}};
    std::vector<ElementRef> edges;
    for (const glm::vec3& q : at) {
      const auto one = pick(doc, [&](const glm::vec3& p) {
        int same = 0;
        for (int i = 0; i < 3; i++)
          same += near(p[i], q[i]);
        return same >= 2;
      });
      REQUIRE(one.size() == 1);
      edges.push_back(one[0]);
    }
    doc.AddFeature(std::make_unique<FilletFeature>("Bevel", edges, 2.5,
                                                   EdgeBlendOp::Chamfer));
    INFO(doc.GetFeature(1)->GetError());
    REQUIRE(doc.GetFeature(1)->GetError().empty());
    CHECK_THAT(BodyVolume(doc), WithinRel(14062.5, 1e-9));
    CHECK(faces(doc) == 18);
  }
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
  glm::dvec3 mn, mx;
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

TEST_CASE("A failed fillet says WHERE, not just how many", "[fillet]") {
  // The count alone is honest and unactionable. Asked which three contours
  // of eight had failed, a caller could only reason about the model and
  // guess; both times it came up in the smart-speaker port the answer was
  // worth having (four d5.5 holes asked for an R8 rim, then three studs
  // standing across the rims being rounded). OCC knows which contours
  // failed and which edges lie on them.
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext);
  const ElementRef edge = PickEdge(doc, AtZ(3.0f));
  doc.AddFeature(
      std::make_unique<FilletFeature>("Fillet", std::vector{edge}, 50.0));
  auto* fillet = dynamic_cast<FilletFeature*>(doc.GetFeature(2));
  CHECK_FALSE(doc.Regenerate());
  const std::string err = fillet->GetError();
  REQUIRE_FALSE(err.empty());
  // A coordinate, so the offending edge can be found in the model.
  CHECK(err.find("near ") != std::string::npos);
  CHECK(err.find('(') != std::string::npos);
  CHECK(err.find(',') != std::string::npos);
}

TEST_CASE("Fillet radius can be driven by a document parameter", "[fillet]") {
  Document doc;
  uint64_t ext = 0;
  BuildRectPrism(doc, ext);
  REQUIRE(doc.SetParameter("r", "0.5"));
  const ElementRef edge = PickEdge(doc, AtZ(3.0f));
  auto fil = std::make_unique<FilletFeature>("Fillet", std::vector{edge}, 0.1);
  fil->radius.expr = "r";
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
    auto fil =
        std::make_unique<FilletFeature>("Fillet", std::vector{edge}, 0.5);
    fil->radius.expr = "0.25 + 0.25";
    doc.AddFeature(std::move(fil));
    doc.AddFeature(std::make_unique<FilletFeature>(
        "Chamfer", std::vector{PickEdge(doc, AtZ(0.0f))}, 0.3,
        EdgeBlendOp::Chamfer));
    REQUIRE(doc.Regenerate());
    volume = BodyVolume(doc);
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* fil = dynamic_cast<FilletFeature*>(loaded->GetFeature(2));
  REQUIRE(fil != nullptr);
  CHECK(fil->op == EdgeBlendOp::Fillet);
  CHECK(fil->radius.expr == "0.25 + 0.25");
  REQUIRE(fil->edges.size() == 1);
  CHECK(fil->edges[0].name == edgeName);
  auto* cha = dynamic_cast<FilletFeature*>(loaded->GetFeature(3));
  REQUIRE(cha != nullptr);
  CHECK(cha->op == EdgeBlendOp::Chamfer);
  REQUIRE(loaded->Regenerate());
  CHECK_THAT(loaded->GetBodies().at(0).shape.Volume(), WithinRel(volume, 1e-6));
  fs::remove(path);
}

TEST_CASE("A filleted spline head fused to a turned base names every edge",
          "[fillet][toponaming]") {
  // The chess knight: a closed-spline side outline extruded symmetric and
  // ADDED to a revolved base, then its two side outlines rounded. Every edge
  // must come out named — an unnamed one is invisible to drawings
  // (list_drawing_geometry skips it) and cannot be dimensioned or picked.
  // Much of it hangs off the fallback passes: the closed spline's side face
  // is L-named from the caps' U edges, and the fillet's outline edges are U
  // edges of that face and of the blends.
  Document doc;
  auto base = std::make_unique<SketchFeature>("Base", kXY);
  base->AddRectangle({0, 0}, {19.4f, 14});
  doc.AddFeature(std::move(base));
  auto rev = std::make_unique<RevolveFeature>(
      "Turned", doc.GetFeature(0)->GetId(), 360.0, ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::X;
  doc.AddFeature(std::move(rev));

  auto head = std::make_unique<SketchFeature>("Head", kXY);
  sketch::BSpline outline;
  // The knight's side outline (generators/chess/build_piece.py KNIGHT_HEAD),
  // closed by repeating the first pole.
  const std::vector<glm::dvec2> poles = {
      {17, -9},       {26, -10.5f},  {35, -10},   {45, -7.5f}, {51, -5.5f},
      {54.4f, -4.5f}, {54.8f, 2.5f}, {52, 3.2f},  {49, 4},     {44, 9},
      {39, 13.5f},    {35, 14.8f},   {31.5f, 13}, {31, 9},     {33, 5},
      {29, 3},        {23, 5},       {17, 7.5f},  {17, -9}};
  outline.poles = poles;
  head->AddEntity(outline);
  doc.AddFeature(std::move(head));
  auto pad = std::make_unique<ExtrudeFeature>(
      "Head pad", doc.GetFeature(2)->GetId(), 11.0, ExtrudeMode::Add);
  pad->endMode = ExtrudeEnd::Symmetric;
  doc.AddFeature(std::move(pad));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);

  // The two side outlines: curved edges lying in z = +-5.5, each of two
  // edges (a lone closed spline is built split at its middle parameter).
  std::vector<ElementRef> outlines;
  {
    const Body& body = doc.GetBodies().at(0);
    const auto polys = body.shape.GenerateEdgePolylines();
    for (size_t e = 0; e < polys.size(); e++) {
      const auto& poly = polys[e];
      if (poly.size() < 8 || std::abs(std::abs(poly.front().z) - 5.5f) > 1e-3f)
        continue;
      bool flat = true;
      for (const auto& p : poly)
        flat = flat && std::abs(p.z - poly.front().z) < 1e-3f;
      const std::string* name = body.elementMap.EdgeNameOf(int(e));
      if (flat && name)
        outlines.push_back({body.createdBy, *name});
    }
  }
  REQUIRE(outlines.size() == 4);
  doc.AddFeature(std::make_unique<FilletFeature>("Round", outlines, 1.5));
  REQUIRE(doc.Regenerate());
  const auto* fil = doc.GetFeature(4);
  REQUIRE(fil->GetError().empty());

  const auto names = [](const Document& d) {
    const Body& body = d.GetBodies().at(0);
    const auto polys = body.shape.GenerateEdgePolylines();
    std::vector<std::string> out;
    for (size_t e = 0; e < polys.size(); e++) {
      const std::string* n = body.elementMap.EdgeNameOf(int(e));
      INFO("edge " << e << " from (" << polys[e].front().x << ", "
                   << polys[e].front().y << ", " << polys[e].front().z
                   << ") to (" << polys[e].back().x << ", " << polys[e].back().y
                   << ", " << polys[e].back().z << ")");
      CHECK(n != nullptr);
      out.push_back(n ? *n : "");
    }
    // Names are unique: two edges answering to one name would make a pick
    // resolve to whichever came first.
    std::vector<std::string> sorted = out;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    return out;
  };
  const auto first = names(doc);
  // Stable: a from-scratch regeneration names the same edges the same way.
  const Document again(doc);
  CHECK(names(again) == first);
}

namespace {

// A block x 0..6, z from its bottom to 8, y 0..-12: the bottom flat to
// x = 0.8 and rising at `creaseDeg` from there, so its side edge at y = 0 is
// two pieces meeting at a crease of that angle (kernel case
// solidcore/fillets/15437, without the drafted side).
void CreasedBlock(Document& doc, double creaseDeg) {
  const double rise = 5.2 * std::tan(creaseDeg * std::numbers::pi / 180.0);
  auto sk = std::make_unique<SketchFeature>(
      "Section", SketchPlane({0, 0, 0}, {0, -1, 0}, {1, 0, 0}));
  const std::array<glm::dvec2, 5> p = {
      glm::dvec2{0, 0}, {0.8, 0}, {6, rise}, {6, 8}, {0, 8}};
  for (size_t i = 0; i < p.size(); i++)
    sk->AddEntity(sketch::Line{p[i], p[(i + 1) % p.size()]});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Block", doc.GetFeature(0)->GetId(), 12.0, ExtrudeMode::NewBody));
  REQUIRE(doc.Regenerate());
}

// The side edge's two pieces at y = 0: the long one past the crease, and the
// short one before it.
bool OnSide(const glm::vec3& q) { return std::abs(q.y) < 1e-3f && q.z < 0.6f; }
ElementRef LongPiece(const Document& doc) {
  return PickEdge(
      doc, [](const glm::vec3& q) { return OnSide(q) && q.x > 0.8f - 1e-3f; });
}
ElementRef ShortPiece(const Document& doc) {
  return PickEdge(
      doc, [](const glm::vec3& q) { return OnSide(q) && q.x < 0.8f + 1e-3f; });
}

} // namespace

TEST_CASE("A round that runs on past a crease nobody picked says so",
          "[fillet]") {
  // docs/modeling/README.md#a-blend-that-runs-on-past-a-crease-says-so: the
  // blend builder's tangent walk crosses a crease of up to 5.7 degrees and
  // rounds the edge past it, which nobody picked, and cannot be stopped
  // there. Under the builder's own angular tolerance (0.57 degrees) the two
  // pieces are one smooth run; past it, up to where the builder stops on
  // its own, the round builds as the builder walks it and the feature warns,
  // naming the crease and the edge; past that it ends at the crease.
  const auto round = [](double deg, bool both) {
    Document doc;
    CreasedBlock(doc, deg);
    const double before = doc.GetBodies().at(0).shape.Volume();
    std::vector<ElementRef> edges = {LongPiece(doc)};
    if (both)
      edges.push_back(ShortPiece(doc));
    doc.AddFeature(std::make_unique<FilletFeature>("Round", edges, 0.4));
    const bool built = doc.Regenerate();
    const auto* f = doc.GetFeature(doc.FeatureCount() - 1);
    struct Out {
      bool built;
      std::string error, warning;
      double removed;
      int shortEdges;
    };
    // The short piece, sharp, is still an edge of the body.
    int sharp = 0;
    for (const auto& poly :
         doc.GetBodies().at(0).shape.GenerateEdgePolylines()) {
      bool all = poly.size() >= 2;
      for (const auto& q : poly)
        all = all && std::abs(q.y) < 1e-3f && std::abs(q.z) < 1e-3f &&
              q.x < 0.8f + 1e-3f;
      sharp += all ? 1 : 0;
    }
    return Out{built, f->GetError(), f->GetWarning(),
               before - doc.GetBodies().at(0).shape.Volume(), sharp};
  };
  // The round's spandrel (1 - pi/4) r^2 along the long piece's 5.2 mm
  // (and its run past the end at the crease aside): what one piece removes.
  const double one = (1.0 - std::numbers::pi / 4.0) * 0.16 * 5.2;

  SECTION("0.5 degrees: one smooth run, both pieces rounded, no warning") {
    const auto out = round(0.5, false);
    INFO(out.error << "; removed " << out.removed << " mm3, one piece's "
                   << one);
    REQUIRE(out.built);
    CHECK(out.warning.empty());
    CHECK(out.shortEdges == 0);
    CHECK(out.removed > one * 1.1);
  }
  for (const double deg : {1.0, 3.0}) {
    DYNAMIC_SECTION(deg << " degrees: built past the crease, and said") {
      const auto out = round(deg, false);
      INFO(out.error);
      REQUIRE(out.built);
      CHECK(out.error.empty());
      // The short piece rounded too: more than the long piece's spandrel.
      CHECK(out.shortEdges == 0);
      CHECK(out.removed > one);
      CHECK(out.warning.find("runs on across a crease of " +
                             std::format("{:.3g}", deg) +
                             " degrees at (0.800, "
                             "0.000, 0.000) to edge '") != std::string::npos);
      CHECK(out.warning.find("which is not picked") != std::string::npos);
      // Both picked: both rounded, as asked, and nothing to say.
      const auto both = round(deg, true);
      INFO(both.error);
      CHECK(both.built);
      CHECK(both.warning.empty());
      CHECK(both.shortEdges == 0);
    }
  }
  SECTION("6 degrees: the builder's walk stops there") {
    // Past 0.1 rad the walk does not cross: the long piece is the contour,
    // built as picked, and its end at the crease is the builder's (it eats
    // into the short piece's corner, short of rounding it).
    const auto out = round(6.0, false);
    INFO(out.error << "; removed " << out.removed << " mm3, one piece's "
                   << one);
    REQUIRE(out.built);
    CHECK(out.warning.empty());
    CHECK(out.removed > one);
    CHECK(out.removed < one * 6.0 / 5.2);
  }
}
