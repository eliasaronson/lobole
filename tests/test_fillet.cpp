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
#include <filesystem>
#include <functional>
#include <limits>
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
    auto fil =
        std::make_unique<FilletFeature>("Fillet", std::vector{edge}, 0.5);
    fil->radiusExpr = "0.25 + 0.25";
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
  CHECK(fil->radiusExpr == "0.25 + 0.25");
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

  // The two side outlines: curved edges lying in z = +-5.5.
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
  REQUIRE(outlines.size() == 2);
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
