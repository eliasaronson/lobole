// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/CornerReliefFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sheet/Check.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <set>
#include <vector>

using Catch::Matchers::WithinAbs;

// Corners (docs/sheetmetal/README.md#features): closing a corner, relieving
// one, and refusing the corners neither is shaped for.
namespace {

constexpr double kT = 1.5;
constexpr double kR = 2.25;

uint64_t Profile(Document& doc, const std::vector<glm::vec2>& pts) {
  sheet::SheetMetalRules r;
  r.material = "aluminum";
  r.gauge = "1.5mm";
  r.thickness = kT;
  doc.SetSheetMetal(r);
  auto sk = std::make_unique<SketchFeature>(
      "Blank",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  for (size_t i = 0; i < pts.size(); ++i)
    sk->AddEntity(sketch::Line{pts[i], pts[(i + 1) % pts.size()],
                               static_cast<uint32_t>(i + 1)});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<BaseFlangeFeature>(
      "Base", doc.GetFeature(doc.FeatureCount() - 1)->GetId()));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

uint64_t Rect(Document& doc) {
  return Profile(doc, {{0, 0}, {100, 0}, {100, 60}, {0, 60}});
}

ElementRef Edge(const Document& doc, uint64_t base, const glm::dvec3& a,
                const glm::dvec3& b) {
  TopTools_IndexedMapOfShape edges;
  const Body& body = doc.GetBodies()[0];
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  const auto near_ = [](const gp_Pnt& p, const glm::dvec3& q) {
    return p.Distance(gp_Pnt(q.x, q.y, q.z)) < 1e-4;
  };
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    if (c.GetType() != GeomAbs_Line)
      continue;
    const gp_Pnt p0 = c.Value(c.FirstParameter());
    const gp_Pnt p1 = c.Value(c.LastParameter());
    if ((near_(p0, a) && near_(p1, b)) || (near_(p0, b) && near_(p1, a)))
      if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
        return ElementRef{base, *n};
  }
  FAIL("no such edge");
  return {};
}

// The straight edge whose MIDPOINT is nearest `at` — for picking an edge
// whose ends have moved (a flange's setback trims the panel back from them).
ElementRef EdgeNear(const Document& doc, uint64_t base, const glm::dvec3& at) {
  TopTools_IndexedMapOfShape edges;
  const Body& body = doc.GetBodies()[0];
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  double best = 1e30;
  ElementRef found;
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    if (c.GetType() != GeomAbs_Line)
      continue;
    const gp_Pnt p0 = c.Value(c.FirstParameter());
    const gp_Pnt p1 = c.Value(c.LastParameter());
    const gp_Pnt mid((p0.X() + p1.X()) / 2.0, (p0.Y() + p1.Y()) / 2.0,
                     (p0.Z() + p1.Z()) / 2.0);
    const double dist = mid.Distance(gp_Pnt(at.x, at.y, at.z));
    if (dist < best)
      if (const std::string* n = body.elementMap.EdgeNameOf(i - 1)) {
        best = dist;
        found = ElementRef{base, *n};
      }
  }
  REQUIRE(best < 1e30);
  return found;
}

void CornerFlanges(Document& doc, uint64_t base, FlangePosition pos,
                   bool closed) {
  auto f = std::make_unique<EdgeFlangeFeature>(
      "Walls",
      std::vector<ElementRef>{Edge(doc, base, {0, 0, kT}, {100, 0, kT}),
                              Edge(doc, base, {0, 0, kT}, {0, 60, kT})},
      20.0, 90.0);
  f->position = pos;
  f->closedCorners = closed;
  doc.AddFeature(std::move(f));
}

const Feature& Last(Document& doc) {
  return *doc.GetFeature(doc.FeatureCount() - 1);
}

void Ok(Document& doc) {
  INFO(Last(doc).GetError());
  REQUIRE(Last(doc).GetError().empty());
}

bool Sound(const Document& doc) {
  return BRepCheck_Analyzer(doc.GetBodies()[0].shape.GetShape()).IsValid() &&
         doc.GetBodies()[0].shape.SolidCount() == 1;
}

bool Inside(const Document& doc, const gp_Pnt& p) {
  BRepClass3d_SolidClassifier c(doc.GetBodies()[0].shape.GetShape(), p, 1e-6);
  return c.State() == TopAbs_IN;
}

double Volume(const Document& doc) { return doc.GetBodies()[0].shape.Volume(); }

void LaysFlat(Document& doc, uint64_t base) {
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  Ok(doc);
  CHECK(Sound(doc));
  glm::dvec3 lo, hi;
  doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
  CHECK_THAT(static_cast<double>(hi.z - lo.z), WithinAbs(kT, 1e-5));
}

// Halfway up the first wall, just past the corner: in the gap an open corner
// leaves, and inside the wall a closed one carries across it.
const gp_Pnt kPastCorner(-1.0, -(kR + kT / 2.0), kT + kR + 10.0);

} // namespace

TEST_CASE("an open corner is left open", "[sheetmetal][corner]") {
  Document doc;
  const uint64_t base = Rect(doc);
  CornerFlanges(doc, base, FlangePosition::BendOutside, false);
  Ok(doc);
  CHECK(Sound(doc));
  CHECK_FALSE(Inside(doc, kPastCorner));
  CHECK(doc.GetBodies()[0].bends[0].trims.empty());
}

TEST_CASE("a closed corner carries both walls round and mitres them",
          "[sheetmetal][corner]") {
  Document open;
  CornerFlanges(open, Rect(open), FlangePosition::BendOutside, false);
  Ok(open);

  Document doc;
  const uint64_t base = Rect(doc);
  CornerFlanges(doc, base, FlangePosition::BendOutside, true);
  Ok(doc);
  CHECK(Sound(doc));
  CHECK(Inside(doc, kPastCorner));
  CHECK(Volume(doc) > Volume(open));
  const std::vector<BendRegion>& bends = doc.GetBodies()[0].bends;
  REQUIRE(bends.size() == 2);
  CHECK(bends[0].trims.size() == 1);
  CHECK(bends[1].trims.size() == 1);
  // Carried past the corner by as far as the other wall sticks out there.
  CHECK_THAT(bends[0].width, WithinAbs(100.0 + kR + kT, 1e-6));
  // The mitre keeps the two walls apart, a tenth of the sheet.
  CHECK_FALSE(Inside(doc, gp_Pnt(-kR - kT / 2.0, -kR - kT / 2.0, 10.0)));
  LaysFlat(doc, base);
}

TEST_CASE("a square corner gets its relief", "[sheetmetal][corner]") {
  SECTION("where two outside bends meet") {
    Document doc;
    const uint64_t base = Rect(doc);
    CornerFlanges(doc, base, FlangePosition::BendOutside, false);
    Ok(doc);
    const double before = Volume(doc);
    doc.AddFeature(std::make_unique<CornerReliefFeature>("Relief", base));
    Ok(doc);
    CHECK(Sound(doc));
    CHECK(Volume(doc) < before);
    // Centred where the bend lines cross — here the panel's own corner — and
    // one bend radius plus a thickness across.
    CHECK_FALSE(Inside(doc, gp_Pnt(1.0, 1.0, kT / 2.0)));
    CHECK(Inside(doc, gp_Pnt(kR + kT + 0.5, 1.0, kT / 2.0)));
    LaysFlat(doc, base);
  }
  SECTION("in a mitred corner, square") {
    Document doc;
    const uint64_t base = Rect(doc);
    CornerFlanges(doc, base, FlangePosition::MaterialInside, false);
    Ok(doc);
    const double before = Volume(doc);
    auto relief = std::make_unique<CornerReliefFeature>("Relief", base);
    relief->style = CornerReliefStyle::Rectangular;
    doc.AddFeature(std::move(relief));
    Ok(doc);
    CHECK(Sound(doc));
    CHECK(Volume(doc) < before);
    LaysFlat(doc, base);
  }
}

TEST_CASE("a corner counts even when the two walls fold opposite ways",
          "[sheetmetal][corner]") {
  // A heat tent: a ridge folded UP along one edge and an end tab folded DOWN
  // at the other. The two bend lines still cross, and the corner between
  // them still tears — check_sheet_metal says so — but the pairing asked for
  // both walls to leave the same FACE, so this part had a corner the check
  // called out and the relief refused to cut.
  Document doc;
  const uint64_t base = Rect(doc);
  auto ridge = std::make_unique<EdgeFlangeFeature>(
      "Ridge",
      std::vector<ElementRef>{Edge(doc, base, {0, 0, kT}, {100, 0, kT})}, 30.0,
      60.0);
  doc.AddFeature(std::move(ridge));
  Ok(doc);
  // The tab hangs off the far side of the same panel: picked on the
  // UNDERSIDE, so it folds the other way.
  // The ridge's own setback trimmed the panel back from y = 0, so this edge
  // no longer runs corner to corner: picked by where it LIES.
  auto tab = std::make_unique<EdgeFlangeFeature>(
      "Tab", std::vector<ElementRef>{EdgeNear(doc, base, {0, 31, 0})}, 15.0,
      90.0);
  doc.AddFeature(std::move(tab));
  Ok(doc);

  const sheet::CheckResult before =
      sheet::Check(doc.GetBodies()[0], *doc.SheetMetal());
  const bool tears =
      std::any_of(before.violations.begin(), before.violations.end(),
                  [](const sheet::Violation& v) {
                    return v.kind == "missing_bend_relief";
                  });
  CHECK(tears); // the check knows the corner is there

  const double volume = Volume(doc);
  doc.AddFeature(std::make_unique<CornerReliefFeature>("Relief", base));
  Ok(doc);
  CHECK(Sound(doc));
  CHECK(Volume(doc) < volume); // ...and now so does the relief
  const sheet::CheckResult after =
      sheet::Check(doc.GetBodies()[0], *doc.SheetMetal());
  for (const sheet::Violation& v : after.violations)
    UNSCOPED_INFO(v.message);
  CHECK(std::none_of(after.violations.begin(), after.violations.end(),
                     [](const sheet::Violation& v) {
                       return v.kind == "missing_bend_relief";
                     }));
}

TEST_CASE("a body with no corner has nothing to relieve",
          "[sheetmetal][corner]") {
  Document doc;
  const uint64_t base = Rect(doc);
  auto f = std::make_unique<EdgeFlangeFeature>(
      "Wall",
      std::vector<ElementRef>{Edge(doc, base, {0, 0, kT}, {100, 0, kT})}, 20.0,
      90.0);
  doc.AddFeature(std::move(f));
  Ok(doc);
  doc.AddFeature(std::make_unique<CornerReliefFeature>("Relief", base));
  CHECK(Last(doc).GetError().find("no two bends") != std::string::npos);
}

TEST_CASE("a corner that is not square is refused, not approximated",
          "[sheetmetal][corner]") {
  // A 60 degree corner at the origin.
  const double x = 60.0 / std::tan(60.0 * 3.14159265358979323846 / 180.0);
  const auto build = [&](Document& doc, bool closed) {
    const uint64_t base = Profile(
        doc, {{0, 0}, {100, 0}, {100, 60}, {static_cast<float>(x), 60}});
    auto f = std::make_unique<EdgeFlangeFeature>(
        "Walls",
        std::vector<ElementRef>{Edge(doc, base, {0, 0, kT}, {100, 0, kT}),
                                Edge(doc, base, {0, 0, kT}, {x, 60, kT})},
        20.0, 90.0);
    f->position = FlangePosition::BendOutside;
    f->closedCorners = closed;
    doc.AddFeature(std::move(f));
    return base;
  };
  SECTION("closing it") {
    Document doc;
    build(doc, true);
    CHECK(Last(doc).GetError().find("only a square corner") !=
          std::string::npos);
  }
  SECTION("relieving it") {
    Document doc;
    const uint64_t base = build(doc, false);
    Ok(doc);
    doc.AddFeature(std::make_unique<CornerReliefFeature>("Relief", base));
    CHECK(Last(doc).GetError().find("right angle") != std::string::npos);
  }
}

#include "core/features/HemFeature.h"
#include "core/features/JogFeature.h"
#include "core/features/RefoldFeature.h"
#include "core/features/UnfoldFeature.h"
// A hem, a jog, a side wall picked on the NEAR panel after the jog, and an
// unfold/refold round trip: the wall has to land on the near panel, and its
// corner with the hem has to still be found. The jog once named both halves
// of the left edge alike, and the wall went up on the far panel instead.
TEST_CASE("a wall picked after a jog lands where it was picked",
          "[sheetmetal][corner]") {
  for (int variant = 0; variant < 3; ++variant) {
    Document doc;
    const uint64_t base = Rect(doc);
    doc.AddFeature(std::make_unique<HemFeature>(
        "Hem",
        std::vector<ElementRef>{Edge(doc, base, {0, 0, kT}, {100, 0, kT})},
        6.0));
    Ok(doc);
    if (variant >= 1) {
      auto sk = std::make_unique<SketchFeature>(
          "Step", SketchPlane(glm::vec3(0, 0, kT), glm::vec3(0, 0, 1),
                              glm::vec3(1, 0, 0)));
      sk->AddEntity(sketch::Line{{0, 30}, {100, 30}, 1});
      doc.AddFeature(std::move(sk));
      doc.AddFeature(
          std::make_unique<JogFeature>("Jog", Last(doc).GetId(), 10.0));
      Ok(doc);
    }
    const double setback = kT / 20.0 + kT;
    auto f = std::make_unique<EdgeFlangeFeature>(
        "Side",
        std::vector<ElementRef>{Edge(doc, base, {0, setback, kT},
                                     {0, variant >= 1 ? 30.0 : 60.0, kT})},
        15.0, 90.0);
    f->position = FlangePosition::BendOutside;
    doc.AddFeature(std::move(f));
    Ok(doc);
    // The side wall's bend is on the near panel, level with the base.
    const BendRegion& side = doc.GetBodies()[0].bends.back();
    CHECK_THAT(side.axisOrigin.z, WithinAbs(kT + kR, 1e-6));
    if (variant >= 1)
      CHECK(side.axisOrigin.y <= 30.0 + 1e-6);
    // And no name on the body resolves to two elements.
    std::set<std::string> edges;
    for (const auto& [idx, n] : doc.GetBodies()[0].elementMap.edgeNames)
      edges.insert(n);
    CHECK(edges.size() == doc.GetBodies()[0].elementMap.edgeNames.size());
    if (variant == 2) {
      doc.AddFeature(std::make_unique<UnfoldFeature>("Unfold", base));
      Ok(doc);
      doc.AddFeature(
          std::make_unique<RefoldFeature>("Refold", Last(doc).GetId()));
      Ok(doc);
    }
    doc.AddFeature(std::make_unique<CornerReliefFeature>("Relief", base));
    INFO("variant " << variant);
    Ok(doc);
  }
}
