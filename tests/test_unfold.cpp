// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/ExportDxf.h"
#include "core/io/ImportDxf.h"
#include "core/io/SketchImport.h"
#include "core/sheet/Unfold.h"

#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kT = 1.5;   // aluminum "1.5mm"
constexpr double kR = 2.25;  // its minimum bend radius, 1.5 * t
constexpr double kK = 0.41;  // its K at R/t = 1.5
constexpr double kW = 100.0; // the blank, along X
constexpr double kD = 60.0;  // along Y
const double kQuarter = std::numbers::pi / 2.0;

sheet::SheetMetalRules AluRules() {
  sheet::SheetMetalRules r;
  r.material = "aluminum_6061_t6";
  r.gauge = "1.5mm";
  r.thickness = kT;
  return r;
}

uint64_t Blank(Document& doc) {
  doc.SetSheetMetal(AluRules());
  auto sk = std::make_unique<SketchFeature>(
      "Blank",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Line{{0, 0}, {kW, 0}, 1});
  sk->AddEntity(sketch::Line{{kW, 0}, {kW, kD}, 2});
  sk->AddEntity(sketch::Line{{kW, kD}, {0, kD}, 3});
  sk->AddEntity(sketch::Line{{0, kD}, {0, 0}, 4});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<BaseFlangeFeature>(
      "Base", doc.GetFeature(doc.FeatureCount() - 1)->GetId()));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

std::string EdgeNamed(const Body& body, const glm::dvec3& a,
                      const glm::dvec3& b) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  const auto near = [](const gp_Pnt& p, const glm::dvec3& q) {
    return std::abs(p.X() - q.x) < 1e-6 && std::abs(p.Y() - q.y) < 1e-6 &&
           std::abs(p.Z() - q.z) < 1e-6;
  };
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    if (c.GetType() != GeomAbs_Line)
      continue;
    const gp_Pnt p0 = c.Value(c.FirstParameter());
    const gp_Pnt p1 = c.Value(c.LastParameter());
    if ((near(p0, a) && near(p1, b)) || (near(p0, b) && near(p1, a)))
      if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
        return *n;
  }
  return {};
}

// Adds a 90-degree flange of `wall` mm along the top edge running in X at
// y = `y`, with the bend starting right at the picked edge.
void Flange(Document& doc, uint64_t base, double y, double wall) {
  const std::string edge =
      EdgeNamed(doc.GetBodies()[0], {0, y, kT}, {kW, y, kT});
  REQUIRE_FALSE(edge.empty());
  auto f = std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, wall, 90.0);
  f->position = FlangePosition::BendOutside;
  doc.AddFeature(std::move(f));
}

double Allowance() { return sheet::BendAllowance(kQuarter, kR, kK, kT); }

// The extent of the body along Z — a flat pattern's is EXACTLY one thickness,
// which is the single strongest check there is on the whole walk: any bend
// left unrotated, or rotated by the wrong angle or about the wrong sense,
// shows up here immediately.
double ThicknessOf(const Body& body) {
  glm::dvec3 lo, hi;
  body.shape.GetBoundingBox(lo, hi);
  return hi.z - lo.z;
}

} // namespace

TEST_CASE("a blank flattens to itself", "[sheetmetal][unfold]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const double before = doc.GetBodies()[0].shape.Volume();
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  CHECK(doc.GetFeature(2)->GetError().empty());
  // Worth SAYING rather than looking as though something happened.
  CHECK_FALSE(doc.GetFeature(2)->GetWarning().empty());
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(before, 1e-9));
}

TEST_CASE("one bend flattens to the blank it was cut from",
          "[sheetmetal][unfold]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const double wall = 20.0;
  Flange(doc, base, 0.0, wall);
  REQUIRE(doc.GetFeature(2)->GetError().empty());
  const double folded = doc.GetBodies()[0].shape.Volume();

  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  const Body& flat = doc.GetBodies()[0];

  // Exactly one gauge thick, everywhere.
  CHECK_THAT(ThicknessOf(flat), WithinAbs(kT, 1e-6));

  // The blank is the panel, plus the bend allowance, plus the wall — the
  // whole of the arithmetic, checked against a hand computation.
  const double area = kW * kD + Allowance() * kW + wall * kW;
  CHECK_THAT(flat.shape.Volume(), WithinRel(area * kT, 1e-4));

  // And it is SHORTER than the folded part's own material: the K-factor
  // model holds the neutral axis fixed, so a bend gives back less flat stock
  // than it occupies. That difference is the whole reason a blank cannot be
  // measured off the folded outside dimensions.
  CHECK(flat.shape.Volume() < folded);

  // The OUTLINE, not just the thickness: a wall folded the wrong way round
  // still lands flat and still has the right volume — it lands on the wrong
  // side of the bend line. Only the blank's extent catches that.
  glm::dvec3 lo, hi;
  flat.shape.GetBoundingBox(lo, hi);
  CHECK_THAT(static_cast<double>(hi.x - lo.x), WithinAbs(kW, 1e-6));
  CHECK_THAT(static_cast<double>(hi.y - lo.y),
             WithinAbs(kD + Allowance() + wall, 1e-4));
  CHECK_THAT(static_cast<double>(hi.y), WithinAbs(kD, 1e-6));

  // The crease: one per bend, carrying its allowance and a line the width of
  // the bend.
  REQUIRE(flat.bends.size() == 1);
  const BendRegion& crease = flat.bends[0];
  CHECK(crease.flat);
  CHECK_THAT(crease.allowance, WithinRel(Allowance(), 1e-9));
  CHECK_THAT(glm::length(crease.flatB - crease.flatA), WithinRel(kW, 1e-6));
}

TEST_CASE("two flanges off one panel both lie down", "[sheetmetal][unfold]") {
  // A tree, not a chain: each wall swings about its own hinge and neither
  // disturbs the other.
  Document doc;
  const uint64_t base = Blank(doc);
  Flange(doc, base, 0.0, 20.0);
  Flange(doc, base, kD, 15.0);
  REQUIRE(doc.GetFeature(2)->GetError().empty());
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  REQUIRE(doc.GetBodies()[0].bends.size() == 2);

  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  REQUIRE(doc.GetFeature(4)->GetError().empty());
  const Body& flat = doc.GetBodies()[0];
  CHECK_THAT(ThicknessOf(flat), WithinAbs(kT, 1e-6));

  const double area = kW * kD + 2.0 * Allowance() * kW + (20.0 + 15.0) * kW;
  CHECK_THAT(flat.shape.Volume(), WithinRel(area * kT, 1e-4));
  CHECK(flat.bends.size() == 2);

  // The blank grew in Y by both allowances and both walls, and not at all
  // in X — which says the walls came down the right way round.
  glm::dvec3 lo, hi;
  flat.shape.GetBoundingBox(lo, hi);
  CHECK_THAT(static_cast<double>(hi.x - lo.x), WithinAbs(kW, 1e-6));
  CHECK_THAT(static_cast<double>(hi.y - lo.y),
             WithinAbs(kD + 2.0 * Allowance() + 35.0, 1e-4));
}

TEST_CASE("a flange on a flange unfolds through the chain",
          "[sheetmetal][unfold]") {
  Document doc;
  const uint64_t base = Blank(doc);
  Flange(doc, base, 0.0, 20.0);
  REQUIRE(doc.GetFeature(2)->GetError().empty());

  // The far edge of the first wall: it now stands at y = 0, running in X,
  // 20 mm below the blank (the picked edge was the underside at z = 0... the
  // wall folded up from the TOP face, so its tip is at z = kT + kR + 20).
  const Body& folded = doc.GetBodies()[0];
  glm::dvec3 lo, hi;
  folded.shape.GetBoundingBox(lo, hi);
  const double tip = hi.z;
  const std::string edge = EdgeNamed(folded, {0, lo.y, tip}, {kW, lo.y, tip});
  REQUIRE_FALSE(edge.empty());
  auto second = std::make_unique<EdgeFlangeFeature>(
      "Flange2", std::vector<ElementRef>{ElementRef{base, edge}}, 10.0, 90.0);
  second->position = FlangePosition::BendOutside;
  doc.AddFeature(std::move(second));
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  REQUIRE(doc.GetBodies()[0].bends.size() == 2);

  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  REQUIRE(doc.GetFeature(4)->GetError().empty());
  const Body& flat = doc.GetBodies()[0];
  CHECK_THAT(ThicknessOf(flat), WithinAbs(kT, 1e-6));
  const double area = kW * kD + 2.0 * Allowance() * kW + (20.0 + 10.0) * kW;
  CHECK_THAT(flat.shape.Volume(), WithinRel(area * kT, 1e-4));
}

TEST_CASE("every face of the blank is named", "[sheetmetal][unfold]") {
  // The flat pattern is a body people pick on, dimension and cut, so every
  // face of it has to carry a stable name.
  //
  // Those names are the FLAT's own, not the folded body's: the blank is built
  // by cutting the part apart and fusing it back together, and a boolean
  // renames what it touches (the ";M<tag>" rule). So a dimension taken on the
  // folded part does not follow through to the blank — it is taken on the
  // configuration it belongs to. Carrying a name across the fold is a real
  // thing to want and it is not what this does.
  Document doc;
  const uint64_t base = Blank(doc);
  Flange(doc, base, 0.0, 20.0);
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  REQUIRE(doc.GetFeature(3)->GetError().empty());

  const Body& flat = doc.GetBodies()[0];
  int faces = 0;
  for (TopExp_Explorer ex(flat.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next())
    ++faces;
  CHECK(faces > 0);
  CHECK(static_cast<int>(flat.elementMap.names.size()) == faces);
  for (const auto& [idx, name] : flat.elementMap.names)
    CHECK_FALSE(name.empty());
}

TEST_CASE("flattening a blank twice is refused", "[sheetmetal][unfold]") {
  Document doc;
  const uint64_t base = Blank(doc);
  Flange(doc, base, 0.0, 20.0);
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat again", base));
  const std::string why = doc.GetFeature(4)->GetError();
  CHECK_FALSE(why.empty());
  CHECK(why.find("already flat") != std::string::npos);
}

TEST_CASE("the flat pattern is deterministic", "[sheetmetal][unfold]") {
  Document doc;
  const uint64_t base = Blank(doc);
  Flange(doc, base, 0.0, 20.0);
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  const double v1 = doc.GetBodies()[0].shape.Volume();
  doc.ClearRegenCache();
  REQUIRE(doc.Regenerate());
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(v1, 1e-9));
}

TEST_CASE("a configuration switches folded and flat", "[sheetmetal][unfold]") {
  // The flat pattern is authored OFF and a configuration turns it on: one
  // document, one undo stack, and the two states cannot both be in the
  // viewport occupying the same space.
  Document doc;
  const uint64_t base = Blank(doc);
  Flange(doc, base, 0.0, 20.0);
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  const uint64_t flatId = doc.GetFeature(3)->GetId();
  doc.GetFeature(3)->SetSuppressed(true);
  doc.Regenerate();
  const double folded = doc.GetBodies()[0].shape.Volume();
  CHECK(ThicknessOf(doc.GetBodies()[0]) > kT + 1.0); // still bent

  Configuration& flat = doc.AddConfiguration("Flat");
  flat.unsuppressedFeatures = {flatId};
  doc.SetActiveConfiguration(flat.id);
  CHECK_THAT(ThicknessOf(doc.GetBodies()[0]), WithinAbs(kT, 1e-6));

  doc.SetActiveConfiguration(0);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(folded, 1e-9));
}

TEST_CASE("up and down are seen from a side of the blank",
          "[sheetmetal][unfold][dxf]") {
  // Two flanges picked on the TOP face, on opposite edges, and one picked on
  // the BOTTOM face. Seen from the top the first two fold up and the third
  // folds down — whichever way each picked edge happens to run, which is what
  // a sense stored against the edge's own direction depended on.
  Document doc;
  const uint64_t base = Blank(doc);
  Flange(doc, base, 0.0, 20.0);
  Flange(doc, base, kD, 20.0);
  const std::string under =
      EdgeNamed(doc.GetBodies()[0], {kW, 0, 0}, {kW, kD, 0});
  REQUIRE_FALSE(under.empty());
  auto down = std::make_unique<EdgeFlangeFeature>(
      "Down", std::vector<ElementRef>{ElementRef{base, under}}, 20.0, 90.0);
  down->position = FlangePosition::BendOutside;
  doc.AddFeature(std::move(down));
  for (size_t i = 2; i < doc.FeatureCount(); ++i)
    REQUIRE(doc.GetFeature(i)->GetError().empty());

  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  const Body& flat = doc.GetBodies()[0];
  REQUIRE(flat.bends.size() == 3);
  const glm::dvec3 top(0, 0, 1);
  CHECK(sheet::FoldsToward(flat.bends[0], top));
  CHECK(sheet::FoldsToward(flat.bends[1], top));
  CHECK_FALSE(sheet::FoldsToward(flat.bends[2], top));
  CHECK_FALSE(sheet::FoldsToward(flat.bends[0], -top));

  // The DXF reads from the first flange's side, so it says the same.
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "designer_up_down.dxf";
  REQUIRE(
      dxf::WriteFlatPattern({flat.shape, flat.bends}, path.string()).empty());
  std::stringstream text;
  text << std::ifstream(path).rdbuf();
  const auto count = [&](const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.str().find(needle); at != std::string::npos;
         at = text.str().find(needle, at + 1))
      ++n;
    return n;
  };
  CHECK(count("LINE\n8\nBEND_UP\n") == 2);
  CHECK(count("LINE\n8\nBEND_DOWN\n") == 1);

  // Read back, the CUT layer is the WHOLE blank — base, bend strips and
  // walls — closed, and its area is the flat body's volume over the gauge.
  // It used to be the base alone: the largest face's wires.
  dxfimport::Options cut;
  cut.layers = {"CUT"};
  auto back = dxfimport::Read(path.string(), cut);
  sketchimport::SnapEndpoints(back.entities, sketchimport::kChainTol);
  CHECK(sketchimport::OpenEnds(back.entities, sketchimport::kChainTol).empty());
  Document again;
  auto sk =
      std::make_unique<SketchFeature>("Blank", SketchPlane(PlaneType::XY));
  for (const auto& e : back.entities)
    sk->AddEntity(e);
  again.AddFeature(std::move(sk));
  again.AddFeature(std::make_unique<ExtrudeFeature>(
      "Cut", again.GetFeature(0)->GetId(), 1.0, ExtrudeMode::NewBody));
  REQUIRE(again.GetBodies().size() == 1);
  CHECK_THAT(again.GetBodies()[0].shape.Volume(),
             WithinRel(flat.shape.Volume() / kT, 1e-5));
  std::filesystem::remove(path);
}

TEST_CASE("a flange that runs into the wall beside it says so",
          "[sheetmetal][unfold]") {
  // A pan with two walls on ADJACENT edges and a rim along the top of one of
  // them. The rim runs the full length of its wall, so at the mitred corner
  // it meets the other wall and fuses to it: the corner is then continuous
  // metal, and no blank folds into that. The refusal has to name THAT — it
  // used to say the wall past the bend did not exist, sending the reader to
  // look for a wall plainly visible in the viewport.
  Document doc;
  const uint64_t base = Blank(doc);
  const std::string front =
      EdgeNamed(doc.GetBodies()[0], {0, 0, kT}, {kW, 0, kT});
  const std::string side =
      EdgeNamed(doc.GetBodies()[0], {kW, 0, kT}, {kW, kD, kT});
  REQUIRE_FALSE(front.empty());
  REQUIRE_FALSE(side.empty());
  auto walls = std::make_unique<EdgeFlangeFeature>(
      "Walls",
      std::vector<ElementRef>{ElementRef{base, front}, ElementRef{base, side}},
      40.0, 90.0);
  walls->position = FlangePosition::MaterialInside;
  doc.AddFeature(std::move(walls));
  REQUIRE(doc.GetFeature(2)->GetError().empty());

  // The rim, along the whole top edge of the front wall.
  const Body& body = doc.GetBodies()[0];
  std::string top;
  {
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
    // The front wall's OUTER top edge: of the edges that run in X, the one
    // highest up and then furthest forward. Picked by extreme rather than by
    // a computed height — which of the two edges a hair apart is picked is
    // what decides which way the rim folds.
    double bestZ = -1e30, bestY = 1e30;
    for (int pass = 0; pass < 2; ++pass) {
      for (int i = 1; i <= edges.Extent(); ++i) {
        BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
        if (c.GetType() != GeomAbs_Line)
          continue;
        const gp_Pnt a = c.Value(c.FirstParameter());
        const gp_Pnt b = c.Value(c.LastParameter());
        if (std::abs(a.X() - b.X()) < 1e-6) // runs in Y: the side wall
          continue;
        if (std::abs(a.Z() - b.Z()) > 1e-6)
          continue;
        const double z = a.Z(), y = (a.Y() + b.Y()) * 0.5;
        if (pass == 0) {
          bestZ = std::max(bestZ, z);
        } else if (std::abs(z - bestZ) < 1e-6 && y < bestY) {
          bestY = y;
          if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
            top = *n;
        }
      }
    }
  }
  REQUIRE_FALSE(top.empty());
  auto rim = std::make_unique<EdgeFlangeFeature>(
      "Rim", std::vector<ElementRef>{ElementRef{base, top}}, 15.0, 90.0);
  doc.AddFeature(std::move(rim));
  REQUIRE(doc.GetFeature(3)->GetError().empty());

  std::string why;
  const auto flat =
      sheet::Unfold(doc.GetBodies()[0], AluRules(), {}, 99, {}, &why);
  REQUIRE_FALSE(flat.has_value());
  INFO(why);
  CHECK(why.find("joined to another panel") != std::string::npos);
  CHECK(why.find("offset_start") != std::string::npos);

  // Inset by a radius and a thickness and the same part lays flat: the rim
  // stops short of its neighbour, so the corner comes apart again.
  auto* edited = dynamic_cast<EdgeFlangeFeature*>(doc.GetFeature(3));
  REQUIRE(edited != nullptr);
  edited->offsetStart = kR + kT;
  edited->offsetEnd = kR + kT;
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  std::string why2;
  const auto flat2 =
      sheet::Unfold(doc.GetBodies()[0], AluRules(), {}, 100, {}, &why2);
  INFO(why2);
  REQUIRE(flat2.has_value());
  CHECK_THAT(ThicknessOf(Body{.shape = flat2->shape}), WithinAbs(kT, 1e-6));
}
