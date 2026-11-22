// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/FoldFeature.h"
#include "core/features/RefoldFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/UnfoldFeature.h"
#include "core/io/ExportDxf.h"
#include "core/io/Serialization.h"
#include "core/sheet/Check.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>
#include <sstream>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Fold (docs/sheetmetal/README.md#fold): a bend made in sheet that is already
// there. Every check here is on the part the fold builds -- its outside
// dimensions against the allowance, its blank against the sheet it was folded
// from, the names on it -- not on the planner's arithmetic.
namespace {

constexpr double kT = 1.5;  // aluminum_6061_t6 "1.5mm"
constexpr double kR = 2.25; // 1.5 t, its minimum
const double kPi = std::numbers::pi;

sheet::SheetMetalRules Alu() {
  sheet::SheetMetalRules r;
  r.material = "aluminum_6061_t6";
  r.gauge = "1.5mm";
  r.thickness = kT;
  return r;
}

using Loop = std::vector<glm::dvec2>;

// A base flange on XY, thickened up to z = t: `outer` with `holes` (closed
// polygons) and `circles` (centre, radius) cut through it.
uint64_t Plate(Document& doc, const Loop& outer, const std::vector<Loop>& holes,
               const std::vector<std::pair<glm::dvec2, double>>& circles = {},
               const sheet::SheetMetalRules& rules = Alu()) {
  doc.SetSheetMetal(rules);
  auto sk = std::make_unique<SketchFeature>(
      "Blank",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  uint32_t id = 1;
  const auto loop = [&](const Loop& pts) {
    for (size_t i = 0; i < pts.size(); ++i)
      sk->AddEntity(sketch::Line{pts[i], pts[(i + 1) % pts.size()], id++});
  };
  loop(outer);
  for (const Loop& h : holes)
    loop(h);
  for (const auto& [c, r] : circles)
    sk->AddEntity(sketch::Circle{c, r, id++});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<BaseFlangeFeature>(
      "Base", doc.GetFeature(doc.FeatureCount() - 1)->GetId()));
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

Loop Rect(double x0, double y0, double x1, double y1) {
  return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

// A U-shaped slot round a tab x in [x0, x1], y in [y0, y1], `gap` wide, open
// at y = y0 where the tab joins the plate.
Loop USlot(double x0, double x1, double y0, double y1, double gap) {
  return {{x0 - gap, y0},
          {x0, y0},
          {x0, y1},
          {x1, y1},
          {x1, y0},
          {x1 + gap, y0},
          {x1 + gap, y1 + gap},
          {x0 - gap, y1 + gap}};
}

const Feature& Last(const Document& doc) {
  return *doc.GetFeature(doc.FeatureCount() - 1);
}

// A sketch on the plate's top face holding one line, and a Fold of it.
FoldFeature& AddFold(Document& doc, glm::dvec2 a, glm::dvec2 b,
                     double angleDeg = 90.0, FoldSide side = FoldSide::Unset) {
  const double z = doc.SheetMetal() ? doc.SheetMetal()->thickness : kT;
  auto sk = std::make_unique<SketchFeature>(
      "Fold line", SketchPlane(glm::dvec3(0, 0, z), glm::dvec3(0, 0, 1),
                               glm::dvec3(1, 0, 0)));
  sk->AddEntity(sketch::Line{a, b, 1});
  doc.AddFeature(std::move(sk));
  auto f = std::make_unique<FoldFeature>(
      "Fold", doc.GetFeature(doc.FeatureCount() - 1)->GetId());
  f->angleDeg = angleDeg;
  f->side = side;
  doc.AddFeature(std::move(f));
  return static_cast<FoldFeature&>(*doc.GetFeature(doc.FeatureCount() - 1));
}

void Built(const Document& doc) {
  INFO(Last(doc).GetError());
  REQUIRE(Last(doc).GetError().empty());
}

bool Sound(const Document& doc) {
  return BRepCheck_Analyzer(doc.GetBodies()[0].shape.GetShape()).IsValid() &&
         doc.GetBodies()[0].shape.SolidCount() == 1;
}

bool Inside(const Document& doc, double x, double y, double z) {
  BRepClass3d_SolidClassifier c(doc.GetBodies()[0].shape.GetShape(),
                                gp_Pnt(x, y, z), 1e-6);
  return c.State() == TopAbs_IN;
}

struct Box {
  glm::dvec3 lo{0.0}, hi{0.0};
};
Box BoxOf(const Document& doc) {
  Box b;
  doc.GetBodies()[0].shape.GetBoundingBox(b.lo, b.hi);
  return b;
}

double SectorVolume(double theta, double width) {
  return 0.5 * theta * ((kR + kT) * (kR + kT) - kR * kR) * width;
}

// The two outside dimensions of a strip folded about x = const with its +x
// end turned up: from x = 0 to the virtual sharp along the outside of the
// fixed leg, and from the virtual sharp to the free end's outside corner.
std::pair<double, double> OutsideLegs(const Document& doc, double theta) {
  const glm::dvec3 cd(std::cos(theta), 0.0, std::sin(theta));
  const glm::dvec3 out(std::sin(theta), 0.0, -std::cos(theta));
  TopTools_IndexedMapOfShape vs;
  TopExp::MapShapes(doc.GetBodies()[0].shape.GetShape(), TopAbs_VERTEX, vs);
  glm::dvec3 e(0.0);
  double bestAlong = -1e300, bestOut = -1e300;
  for (int i = 1; i <= vs.Extent(); ++i) {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vs(i)));
    const glm::dvec3 q(p.X(), p.Y(), p.Z());
    const double along = glm::dot(q, cd), o = glm::dot(q, out);
    if (along > bestAlong + 1e-6 ||
        (std::abs(along - bestAlong) <= 1e-6 && o > bestOut)) {
      bestAlong = along;
      bestOut = o;
      e = q;
    }
  }
  // The outer face of the wall is the line through e along cd; it meets the
  // outside of the fixed leg (z = 0) at the virtual sharp.
  const double s = e.z / std::sin(theta);
  const double vx = e.x - std::cos(theta) * s;
  return {vx, s};
}

std::string FaceName(const Body& body, int index) {
  const std::string* n = body.elementMap.NameOf(index);
  return n ? *n : std::string{};
}

// The explorer index of the first face `pred` accepts.
template <class Pred> int FaceWhere(const Body& body, Pred pred) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), ++i)
    if (pred(BRepAdaptor_Surface(TopoDS::Face(ex.Current())),
             TopoDS::Face(ex.Current())))
      return i;
  return -1;
}

TopoDS_Face FaceAt(const Body& body, int index) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), ++i)
    if (i == index)
      return TopoDS::Face(ex.Current());
  return {};
}

size_t DxfBendLines(const Body& flat, std::vector<double>* lengths = nullptr) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "designer_fold.dxf";
  REQUIRE(
      dxf::WriteFlatPattern({flat.shape, flat.bends}, path.string()).empty());
  std::stringstream text;
  text << std::ifstream(path).rdbuf();
  std::filesystem::remove(path);
  size_t n = 0;
  std::istringstream in(text.str());
  std::vector<std::string> lines;
  for (std::string l; std::getline(in, l);)
    lines.push_back(l);
  for (size_t i = 0; i + 1 < lines.size(); ++i) {
    if (lines[i] != "LINE" || lines[i + 1] != "8" || i + 2 >= lines.size() ||
        lines[i + 2].rfind("BEND_", 0) != 0)
      continue;
    ++n;
    if (lengths == nullptr)
      continue;
    double c[6] = {};
    for (size_t k = i + 3; k + 1 < lines.size() && k < i + 40; k += 2) {
      const int code = std::stoi(lines[k]);
      if (code == 0)
        break;
      const double v = std::stod(lines[k + 1]);
      if (code == 10)
        c[0] = v;
      if (code == 20)
        c[1] = v;
      if (code == 11)
        c[3] = v;
      if (code == 21)
        c[4] = v;
    }
    lengths->push_back(std::hypot(c[3] - c[0], c[4] - c[1]));
  }
  return n;
}

// A plate with a U-slot round a 20 x 25 tab joined at y = 15; a line across
// the tab's root at y = 20.
constexpr double kTabX0 = 40, kTabX1 = 60, kTabY0 = 15, kTabY1 = 40, kGap = 2;
uint64_t
TabPlate(Document& doc,
         const std::vector<std::pair<glm::dvec2, double>>& circles = {}) {
  return Plate(doc, Rect(0, 0, 100, 60),
               {USlot(kTabX0, kTabX1, kTabY0, kTabY1, kGap)}, circles);
}

} // namespace

TEST_CASE("a fold's outside dimensions are what the allowance says",
          "[sheetmetal][fold]") {
  // A 100 x 30 strip folded about x = 40, its 60 mm end turned up.
  const double L = 100.0, W = 30.0, d = 40.0;
  for (const bool rulesK : {false, true}) {
    for (const double deg : {45.0, 90.0, 135.0}) {
      INFO((rulesK ? "rules K " : "table K ") << deg << " degrees");
      sheet::SheetMetalRules rules = Alu();
      if (rulesK)
        rules.kFactor = 0.3;
      Document doc;
      Plate(doc, Rect(0, 0, L, W), {}, {}, rules);
      const double flat = doc.GetBodies()[0].shape.Volume();
      AddFold(doc, {d, 0}, {d, W}, deg, FoldSide::Right);
      Built(doc);
      CHECK(Sound(doc));
      const double theta = deg * kPi / 180.0;
      const double K = rulesK ? 0.3 : 0.41;
      const BendRegion& b = doc.GetBodies()[0].bends.at(0);
      CHECK_THAT(b.kFactor, WithinAbs(K, 1e-12));
      CHECK(b.kSource == (rulesK ? "rules" : "table:aluminum_6061_t6 R/t<2"));
      CHECK_THAT(b.radius, WithinAbs(kR, 1e-12));
      CHECK_THAT(b.width, WithinAbs(W, 1e-9));
      const double BA = sheet::BendAllowance(theta, kR, K, kT);
      const double BD = sheet::BendDeduction(theta, kR, K, kT);
      const auto [a, s] = OutsideLegs(doc, theta);
      CHECK_THAT(a + s - BD, WithinAbs(L, 1e-6));
      // The fixed leg's outside runs to the virtual sharp: the line, less
      // half the allowance, plus the outside setback.
      CHECK_THAT(
          a, WithinAbs(d - BA / 2.0 + (kR + kT) * std::tan(theta / 2.0), 1e-6));
      // The strip's metal, less only what the bend changed.
      CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
                 WithinRel(flat - BA * kT * W + SectorVolume(theta, W), 1e-7));
    }
  }

  SECTION("reverse folds away from the sketched face, the same metal") {
    const auto fold = [&](bool reverse) {
      Document doc;
      Plate(doc, Rect(0, 0, L, W), {});
      FoldFeature& f = AddFold(doc, {d, 0}, {d, W}, 90.0, FoldSide::Right);
      f.reverse = reverse;
      doc.Regenerate();
      Built(doc);
      return std::make_pair(doc.GetBodies()[0].shape.Volume(), BoxOf(doc));
    };
    const auto up = fold(false), down = fold(true);
    CHECK_THAT(down.first, WithinRel(up.first, 1e-9));
    CHECK_THAT(up.second.lo.z, WithinAbs(0.0, 1e-6));
    CHECK_THAT(down.second.hi.z, WithinAbs(kT, 1e-6));
    // Mirrored about the sheet's mid-plane: the wall hangs as far below as
    // it stood above.
    CHECK_THAT(-down.second.lo.z + kT, WithinAbs(up.second.hi.z, 1e-6));
    CHECK_THAT(down.second.hi.x, WithinAbs(up.second.hi.x, 1e-6));
  }

  SECTION("position moves the outside dimensions by its part of the "
          "allowance") {
    const auto legs = [&](sheet::FoldPosition pos) {
      Document doc;
      Plate(doc, Rect(0, 0, L, W), {});
      FoldFeature& f = AddFold(doc, {d, 0}, {d, W}, 90.0, FoldSide::Right);
      f.position = pos;
      doc.Regenerate();
      Built(doc);
      return OutsideLegs(doc, kPi / 2.0);
    };
    const double BA = sheet::BendAllowance(kPi / 2.0, kR, 0.41, kT);
    const auto mid = legs(sheet::FoldPosition::Centerline);
    const auto start = legs(sheet::FoldPosition::BendStart);
    const auto end = legs(sheet::FoldPosition::BendEnd);
    CHECK_THAT(start.first - mid.first, WithinAbs(BA / 2.0, 1e-6));
    CHECK_THAT(mid.second - start.second, WithinAbs(BA / 2.0, 1e-6));
    CHECK_THAT(mid.first - end.first, WithinAbs(BA / 2.0, 1e-6));
    CHECK_THAT(end.second - mid.second, WithinAbs(BA / 2.0, 1e-6));
  }
}

TEST_CASE("the blank of a folded sheet is the sheet it was folded from",
          "[sheetmetal][fold]") {
  const auto roundTrip = [](double holeR) {
    Document doc;
    const uint64_t base = Plate(
        doc, Rect(0, 0, 100, 30), {},
        holeR > 0.0
            ? std::vector<std::pair<glm::dvec2, double>>{{{40, 15}, holeR}}
            : std::vector<std::pair<glm::dvec2, double>>{});
    const double before = doc.GetBodies()[0].shape.Volume();
    const Box blank = BoxOf(doc);
    AddFold(doc, {40, 0}, {40, 30}, 90.0, FoldSide::Right);
    Built(doc);
    CHECK(Sound(doc));
    const double folded = doc.GetBodies()[0].shape.Volume();
    const Box foldedBox = BoxOf(doc);
    if (holeR > 0.0 && holeR < 2.0) {
      // A hole inside the zone is wrapped onto the bend: the plug the
      // neutral map removes, larger than the flat hole by the outside's
      // stretch.
      const double K = 0.41;
      const double BA = sheet::BendAllowance(kPi / 2.0, kR, K, kT);
      const double plug =
          kPi * holeR * holeR * kT * (2.0 * kR + kT) / 2.0 / (kR + K * kT);
      const double plain =
          100 * 30 * kT - BA * kT * 30 + SectorVolume(kPi / 2.0, 30);
      CHECK_THAT(plain - folded, WithinAbs(plug, 0.01));
    }

    // Saved and opened again first: the stored side is what folds.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "designer_fold_trip.json";
    REQUIRE(DocumentIO::Save(doc, path.string()));
    auto loaded = DocumentIO::Load(path.string());
    std::filesystem::remove(path);
    REQUIRE(loaded);
    Document& flatDoc = **loaded;
    CHECK_THAT(flatDoc.GetBodies()[0].shape.Volume(), WithinRel(folded, 1e-9));
    flatDoc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
    Built(flatDoc);
    CHECK(Sound(flatDoc));
    CHECK_THAT(flatDoc.GetBodies()[0].shape.Volume(),
               WithinRel(before, holeR > 0.0 ? 2e-3 : 1e-7));
    const Box flatBox = BoxOf(flatDoc);
    for (int i = 0; i < 3; ++i) {
      CHECK_THAT(flatBox.lo[i], WithinAbs(blank.lo[i], 1e-5));
      CHECK_THAT(flatBox.hi[i], WithinAbs(blank.hi[i], 1e-5));
    }

    // And unfolded and refolded about it, the folded body again.
    doc.AddFeature(std::make_unique<UnfoldFeature>("Unfold", base));
    Built(doc);
    const uint64_t unfold = Last(doc).GetId();
    doc.AddFeature(std::make_unique<RefoldFeature>("Refold", unfold));
    Built(doc);
    CHECK(Sound(doc));
    // A hole comes back a polygon of the unfolder's samples, which is all
    // the difference.
    CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
               WithinRel(folded, holeR > 0.0 ? 1e-4 : 1e-7));
    const Box again = BoxOf(doc);
    for (int i = 0; i < 3; ++i) {
      CHECK_THAT(again.lo[i], WithinAbs(foldedBox.lo[i], 1e-5));
      CHECK_THAT(again.hi[i], WithinAbs(foldedBox.hi[i], 1e-5));
    }
  };
  SECTION("a plain strip") { roundTrip(0.0); }
  SECTION("a hole inside the zone") { roundTrip(1.5); }
  SECTION("a hole wider than the zone") { roundTrip(4.0); }
}

TEST_CASE("a tab cut in a plate folds on its own", "[sheetmetal][fold]") {
  Document doc;
  const uint64_t base = TabPlate(doc);
  const double before = doc.GetBodies()[0].shape.Volume();
  const Box plate = BoxOf(doc);
  // Across the tab's root, inside the tab. No side given: the tab, which is
  // the side that frees less, is resolved and stored.
  FoldFeature& fold = AddFold(doc, {42, 20}, {58, 20});
  Built(doc);
  CHECK(Sound(doc));
  CHECK(fold.side == FoldSide::Left); // +y, the tab, of a line running +x

  const Body& body = doc.GetBodies()[0];
  REQUIRE(body.bends.size() == 1);
  const BendRegion& bend = body.bends[0];
  CHECK_THAT(bend.width, WithinAbs(kTabX1 - kTabX0, 1e-9));
  CHECK(bend.innerFace.IsSet());
  CHECK(bend.outerFace.IsSet());
  CHECK(bend.parentFace.IsSet());
  CHECK(bend.childFace.IsSet());
  // ... and each names a face of the folded body: what check_sheet_metal
  // points a reader at.
  for (const ElementRef* r :
       {&bend.innerFace, &bend.outerFace, &bend.parentFace, &bend.childFace}) {
    INFO(r->name);
    CHECK(body.elementMap.Resolve(r->name).has_value());
  }

  // The plate round it has not moved: its outline in its own plane is the
  // same, and only the tab stands up.
  const Box now = BoxOf(doc);
  CHECK_THAT(now.lo.x, WithinAbs(plate.lo.x, 1e-6));
  CHECK_THAT(now.hi.x, WithinAbs(plate.hi.x, 1e-6));
  CHECK_THAT(now.lo.y, WithinAbs(plate.lo.y, 1e-6));
  CHECK_THAT(now.hi.y, WithinAbs(plate.hi.y, 1e-6));
  CHECK(Inside(doc, 10, 50, kT / 2));
  CHECK(Inside(doc, 50, 50, kT / 2)); // beyond the slot's bar
  // The tab's free end: its flat length past the zone, standing on the
  // bend's height, R + t above the bottom of the plate.
  const double BA = sheet::BendAllowance(kPi / 2.0, kR, 0.41, kT);
  const double past = kTabY1 - (20 + BA / 2.0);
  CHECK_THAT(now.hi.z, WithinAbs(kT + kR + past, 1e-6));
  CHECK_FALSE(Inside(doc, 50, 30, kT / 2)); // the tab left its hole
  CHECK(Inside(doc, 50, 20 - BA / 2.0 + kR + kT / 2.0, 10.0)); // standing

  // The checks see one bend, and the blank is the slotted plate.
  const sheet::CheckResult check = sheet::Check(body, *doc.SheetMetal());
  CHECK(check.bendsChecked == 1);
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  Built(doc);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(before, 1e-7));
  const Box flat = BoxOf(doc);
  for (int i = 0; i < 3; ++i) {
    CHECK_THAT(flat.lo[i], WithinAbs(plate.lo[i], 1e-5));
    CHECK_THAT(flat.hi[i], WithinAbs(plate.hi[i], 1e-5));
  }
  std::vector<double> lengths;
  CHECK(DxfBendLines(doc.GetBodies()[0], &lengths) == 1);
  REQUIRE(lengths.size() == 1);
  CHECK_THAT(lengths[0], WithinAbs(kTabX1 - kTabX0, 1e-6));
}

TEST_CASE("a line across the whole plate folds the whole far side",
          "[sheetmetal][fold]") {
  SECTION("below the tab, which rides along") {
    Document doc;
    TabPlate(doc);
    AddFold(doc, {0, 8}, {100, 8}, 90.0, FoldSide::Left);
    Built(doc);
    CHECK(Sound(doc));
    REQUIRE(doc.GetBodies()[0].bends.size() == 1);
    CHECK_THAT(doc.GetBodies()[0].bends[0].width, WithinAbs(100.0, 1e-9));
    // The tab went up with the far side, still flat in it.
    CHECK_FALSE(Inside(doc, 50, 30, kT / 2));
    CHECK(BoxOf(doc).hi.z > 40.0);
  }
  SECTION("across the tab's root: plate and tab turn as one bend") {
    Document doc;
    const uint64_t base = TabPlate(doc);
    const double before = doc.GetBodies()[0].shape.Volume();
    AddFold(doc, {0, 20}, {100, 20}, 90.0, FoldSide::Left);
    Built(doc);
    CHECK(Sound(doc));
    REQUIRE(doc.GetBodies()[0].bends.size() == 1);
    CHECK_THAT(doc.GetBodies()[0].bends[0].width, WithinAbs(100.0, 1e-9));
    doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
    Built(doc);
    CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(before, 1e-6));
  }
}

TEST_CASE("two tabs on one line fold as two bends", "[sheetmetal][fold]") {
  // One slot, E-shaped: two tabs side by side with a leg between them.
  const Loop slot{{18, 15}, {20, 15}, {20, 40}, {40, 40}, {40, 15}, {42, 15},
                  {42, 40}, {62, 40}, {62, 15}, {64, 15}, {64, 42}, {18, 42}};
  SECTION("a line across both") {
    Document doc;
    Plate(doc, Rect(0, 0, 100, 60), {slot});
    const uint64_t id = AddFold(doc, {25, 20}, {55, 20}).GetId();
    Built(doc);
    CHECK(Sound(doc));
    const Body& body = doc.GetBodies()[0];
    REQUIRE(body.bends.size() == 2);
    CHECK_THAT(body.bends[0].width, WithinAbs(20.0, 1e-9));
    CHECK_THAT(body.bends[1].width, WithinAbs(20.0, 1e-9));
    // In order along the line, named apart.
    CHECK(body.bends[0].axisOrigin.x < body.bends[1].axisOrigin.x);
    CHECK(body.bends[0].innerFace.name != body.bends[1].innerFace.name);
    const std::string id1 = std::to_string(id) + "/bend1/";
    const std::string id2 = std::to_string(id) + "/bend2/";
    CHECK(body.bends[0].innerFace.name.rfind(id1, 0) == 0);
    CHECK(body.bends[1].innerFace.name.rfind(id2, 0) == 0);
    CHECK_FALSE(Inside(doc, 30, 30, kT / 2));
    CHECK_FALSE(Inside(doc, 52, 30, kT / 2));
  }
  SECTION("a line that reaches only one") {
    Document doc;
    Plate(doc, Rect(0, 0, 100, 60), {slot});
    AddFold(doc, {25, 20}, {35, 20});
    Built(doc);
    REQUIRE(doc.GetBodies()[0].bends.size() == 1);
    CHECK_FALSE(Inside(doc, 30, 30, kT / 2));
    CHECK(Inside(doc, 52, 30, kT / 2));
  }
}

TEST_CASE("a fold keeps the names of what it does not cut",
          "[sheetmetal][fold]") {
  Document doc;
  TabPlate(doc, {{{50, 32}, 3.0}});
  const Body& before = doc.GetBodies()[0];
  const int bore =
      FaceWhere(before, [](const BRepAdaptor_Surface& s, const TopoDS_Face&) {
        return s.GetType() == GeomAbs_Cylinder &&
               std::abs(s.Cylinder().Radius() - 3.0) < 1e-9;
      });
  const int top =
      FaceWhere(before, [](const BRepAdaptor_Surface& s, const TopoDS_Face& f) {
        if (s.GetType() != GeomAbs_Plane)
          return false;
        gp_Dir n = s.Plane().Axis().Direction();
        if (f.Orientation() == TopAbs_REVERSED)
          n.Reverse();
        return n.Z() > 0.5 && std::abs(s.Plane().Location().Z() - kT) < 1e-9;
      });
  REQUIRE(bore >= 0);
  REQUIRE(top >= 0);
  const std::string boreName = FaceName(before, bore);
  const std::string topName = FaceName(before, top);
  REQUIRE_FALSE(boreName.empty());
  REQUIRE_FALSE(topName.empty());

  const uint64_t id = AddFold(doc, {42, 20}, {58, 20}).GetId();
  Built(doc);
  const Body& after = doc.GetBodies()[0];
  // The hole in the tab is the same hole, standing up with it.
  const auto boreNow = after.elementMap.Resolve(boreName);
  REQUIRE(boreNow);
  BRepAdaptor_Surface boreSurf(FaceAt(after, *boreNow));
  REQUIRE(boreSurf.GetType() == GeomAbs_Cylinder);
  CHECK_THAT(boreSurf.Cylinder().Radius(), WithinAbs(3.0, 1e-9));
  CHECK_THAT(std::abs(boreSurf.Cylinder().Axis().Direction().Y()),
             WithinAbs(1.0, 1e-9));
  // The plate's top face is on the plate; its half on the tab says so.
  const auto topNow = after.elementMap.Resolve(topName);
  REQUIRE(topNow);
  BRepAdaptor_Surface topSurf(FaceAt(after, *topNow));
  CHECK(std::abs(topSurf.Plane().Axis().Direction().Z()) > 0.999);
  const auto tabTop =
      after.elementMap.Resolve(topName + ";F" + std::to_string(id) + ".1");
  REQUIRE(tabTop);
  BRepAdaptor_Surface tabSurf(FaceAt(after, *tabTop));
  REQUIRE(tabSurf.GetType() == GeomAbs_Plane);
  CHECK(std::abs(tabSurf.Plane().Axis().Direction().Y()) > 0.999);
  // Every name once.
  std::set<std::string> faces, edges;
  for (const auto& [i, n] : after.elementMap.names)
    CHECK(faces.insert(n).second);
  for (const auto& [i, n] : after.elementMap.edgeNames)
    CHECK(edges.insert(n).second);
}

TEST_CASE("a fold refuses what it cannot make, naming the numbers",
          "[sheetmetal][fold]") {
  const double BA = sheet::BendAllowance(kPi / 2.0, kR, 0.41, kT);

  SECTION("a line beside the piece") {
    // On the plate's plane, past its end: the line carried on meets the
    // plate 10 mm before its first point.
    Document doc;
    Plate(doc, Rect(0, 0, 100, 30), {});
    AddFold(doc, {110, 10}, {120, 10});
    CHECK_THAT(Last(doc).GetError(),
               ContainsSubstring("does not cross the sheet: the nearest "
                                 "material along it is 10 mm past its first "
                                 "point"));
  }
  SECTION("a line whose chord does not part the panel") {
    // Two holes on the line; the line spans only the plate between them.
    Document doc;
    Plate(doc, Rect(0, 0, 100, 60), {}, {{{25, 30}, 5.0}, {{75, 30}, 5.0}});
    AddFold(doc, {35, 30}, {65, 30});
    const std::string why = Last(doc).GetError();
    CHECK_THAT(why, ContainsSubstring("does not part the panel"));
    CHECK_THAT(why, ContainsSubstring("past its first point and"));
    // Extended as it says, it builds.
    Document again;
    Plate(again, Rect(0, 0, 100, 60), {}, {{{25, 30}, 5.0}, {{75, 30}, 5.0}});
    AddFold(again, {35 - 15.01, 30}, {65 + 15.01, 30});
    Built(again);
  }
  SECTION("a slot ending inside the zone, and moved as the message says") {
    // The slot's legs stop at y = 19, inside the zone round y = 20.
    const Loop slot = USlot(kTabX0, kTabX1, 19, kTabY1, kGap);
    Document doc;
    Plate(doc, Rect(0, 0, 100, 60), {slot});
    AddFold(doc, {42, 20}, {58, 20}, 90.0, FoldSide::Left);
    const std::string why = Last(doc).GetError();
    CHECK_THAT(why, ContainsSubstring("a cut ends inside the bend zone"));
    // y = 19 is 1 mm behind the line; the zone starts BA/2 behind it.
    const double toFold = (-1.0) - (-BA / 2.0) + 0.01;
    char want[64];
    std::snprintf(want, sizeof want, "Move the line %.2f mm toward",
                  std::ceil(toFold * 100.0 - 1e-6) / 100.0);
    CHECK_THAT(why, ContainsSubstring(want));
    const double moved = std::ceil(toFold * 100.0 - 1e-6) / 100.0;
    Document again;
    Plate(again, Rect(0, 0, 100, 60), {slot});
    AddFold(again, {42, 20 + moved}, {58, 20 + moved}, 90.0, FoldSide::Left);
    Built(again);
    CHECK(again.GetBodies()[0].bends.size() == 1);
  }
  SECTION("too close to an edge, and at the stated distance") {
    // A 100 x 30 strip, folded near its x = 100 end.
    const double need = BA / 2.0 + kT;
    Document doc;
    Plate(doc, Rect(0, 0, 100, 30), {});
    AddFold(doc, {100 - need + 0.3, 0}, {100 - need + 0.3, 30}, 90.0,
            FoldSide::Right);
    const std::string why = Last(doc).GetError();
    CHECK_THAT(why, ContainsSubstring("from the edge of the sheet on the side "
                                      "that folds"));
    char want[64];
    std::snprintf(want, sizeof want, "needs %.2f mm",
                  std::ceil(need * 100.0 - 1e-6) / 100.0);
    CHECK_THAT(why, ContainsSubstring(want));
    const double at = std::ceil(need * 100.0 - 1e-6) / 100.0;
    Document again;
    Plate(again, Rect(0, 0, 100, 30), {});
    AddFold(again, {100 - at, 0}, {100 - at, 30}, 90.0, FoldSide::Right);
    Built(again);
  }
  SECTION("a tab folded into a wall beside it") {
    // A wall stands up off the y = 60 edge; a tab joined at y = 45 and
    // pointing away from it, folded right over, lands in it.
    Document doc;
    const uint64_t base = Plate(doc, Rect(0, 0, 100, 60),
                                {{{38, 45},
                                  {40, 45},
                                  {40, 5},
                                  {60, 5},
                                  {60, 45},
                                  {62, 45},
                                  {62, 3},
                                  {38, 3}}});
    std::string edge;
    {
      const Body& body = doc.GetBodies()[0];
      TopTools_IndexedMapOfShape es;
      TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, es);
      for (int i = 1; i <= es.Extent() && edge.empty(); ++i) {
        BRepAdaptor_Curve c(TopoDS::Edge(es(i)));
        const gp_Pnt p0 = c.Value(c.FirstParameter());
        const gp_Pnt p1 = c.Value(c.LastParameter());
        if (c.GetType() == GeomAbs_Line && std::abs(p0.Y() - 60) < 1e-6 &&
            std::abs(p1.Y() - 60) < 1e-6 && std::abs(p0.Z() - kT) < 1e-6 &&
            std::abs(p1.Z() - kT) < 1e-6)
          if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
            edge = *n;
      }
    }
    REQUIRE_FALSE(edge.empty());
    auto wall = std::make_unique<EdgeFlangeFeature>(
        "Wall", std::vector<ElementRef>{ElementRef{base, edge}}, 30.0, 90.0);
    wall->position = FlangePosition::BendOutside;
    doc.AddFeature(std::move(wall));
    Built(doc);
    FoldFeature& fold = AddFold(doc, {42, 40}, {58, 40}, 90.0);
    Built(doc);
    CHECK(fold.side == FoldSide::Right); // -y, the tab
    fold.angleDeg = 180.0;
    doc.Regenerate();
    CHECK_THAT(Last(doc).GetError(),
               ContainsSubstring("runs into the rest of the sheet"));
  }
  SECTION("a radius under the material's minimum") {
    Document doc;
    Plate(doc, Rect(0, 0, 100, 30), {});
    FoldFeature& f = AddFold(doc, {40, 0}, {40, 30});
    f.radius = 1.0;
    doc.Regenerate();
    CHECK_THAT(Last(doc).GetError(), ContainsSubstring("tighter than"));
  }
  SECTION("an angle out of range") {
    Document doc;
    Plate(doc, Rect(0, 0, 100, 30), {});
    AddFold(doc, {40, 0}, {40, 30}, 190.0);
    CHECK_THAT(Last(doc).GetError(), ContainsSubstring("at most 180"));
  }
  SECTION("a body with bends laid flat") {
    Document doc;
    const uint64_t base = Plate(doc, Rect(0, 0, 100, 30), {});
    AddFold(doc, {40, 0}, {40, 30}, 90.0, FoldSide::Right);
    Built(doc);
    doc.AddFeature(std::make_unique<UnfoldFeature>("Unfold", base));
    Built(doc);
    AddFold(doc, {20, 0}, {20, 30});
    CHECK_THAT(Last(doc).GetError(), ContainsSubstring("bends laid flat"));
  }
  SECTION("no sheet-metal rules") {
    Document doc;
    Plate(doc, Rect(0, 0, 100, 30), {});
    AddFold(doc, {40, 0}, {40, 30});
    doc.SetSheetMetal(std::nullopt);
    doc.Regenerate();
    CHECK_THAT(Last(doc).GetError(), ContainsSubstring("no sheet-metal rules"));
  }
}

TEST_CASE("a fold's defaults follow the stock", "[sheetmetal][fold]") {
  // The same fold on 16 and 10 gauge steel: each takes its own gauge's
  // radius and allowance, and the feature holds no number of either.
  const auto fold = [](const std::string& gauge) {
    sheet::SheetMetalRules r;
    r.material = "steel";
    r.gauge = gauge;
    r.thickness = *sheet::LookupGauge("steel", gauge, nullptr);
    Document doc;
    Plate(doc, Rect(0, 0, 100, 30), {}, {}, r);
    FoldFeature& f = AddFold(doc, {40, 0}, {40, 30}, 90.0, FoldSide::Right);
    Built(doc);
    CHECK(f.radius.value == 0.0);
    const BendRegion b = doc.GetBodies()[0].bends.at(0);
    CHECK_THAT(b.radius,
               WithinAbs(sheet::MinBendRadius("steel", r.thickness), 1e-12));
    CHECK_THAT(f.allowance,
               WithinAbs(sheet::BendAllowance(kPi / 2.0, b.radius, b.kFactor,
                                              r.thickness),
                         1e-12));
    return std::make_pair(b.radius, f.allowance);
  };
  const auto thin = fold("16ga");
  const auto thick = fold("10ga");
  CHECK(thick.first > thin.first);
  CHECK(thick.second > thin.second);
}

TEST_CASE("a fold and a flange build on each other and unfold",
          "[sheetmetal][fold]") {
  SECTION("a tab folded out of a flange's wall") {
    Document doc;
    const uint64_t base = Plate(doc, Rect(0, 0, 100, 60), {});
    std::string edge;
    {
      const Body& body = doc.GetBodies()[0];
      TopTools_IndexedMapOfShape es;
      TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, es);
      for (int i = 1; i <= es.Extent() && edge.empty(); ++i) {
        BRepAdaptor_Curve c(TopoDS::Edge(es(i)));
        const gp_Pnt p0 = c.Value(c.FirstParameter());
        const gp_Pnt p1 = c.Value(c.LastParameter());
        if (c.GetType() == GeomAbs_Line && std::abs(p0.Y()) < 1e-6 &&
            std::abs(p1.Y()) < 1e-6 && std::abs(p0.Z() - kT) < 1e-6 &&
            std::abs(p1.Z() - kT) < 1e-6)
          if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
            edge = *n;
      }
    }
    REQUIRE_FALSE(edge.empty());
    auto wall = std::make_unique<EdgeFlangeFeature>(
        "Wall", std::vector<ElementRef>{ElementRef{base, edge}}, 40.0, 90.0);
    wall->position = FlangePosition::BendOutside;
    doc.AddFeature(std::move(wall));
    Built(doc);
    // The wall's outside face, y = -(R + t), facing -y; a line across it
    // at z = 30 folds the wall's top over, outward.
    const double y = -(kR + kT);
    auto sk = std::make_unique<SketchFeature>(
        "Line", SketchPlane(glm::vec3(0, y, 0), glm::vec3(0, -1, 0),
                            glm::vec3(1, 0, 0)));
    const SketchPlane& plane = sk->plane;
    sk->AddEntity(sketch::Line{plane.WorldToPlane({0, y, 30}),
                               plane.WorldToPlane({100, y, 30}), 1});
    doc.AddFeature(std::move(sk));
    auto f = std::make_unique<FoldFeature>(
        "Fold", doc.GetFeature(doc.FeatureCount() - 1)->GetId());
    doc.AddFeature(std::move(f));
    Built(doc);
    CHECK(Sound(doc));
    REQUIRE(doc.GetBodies()[0].bends.size() == 2);
    doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
    Built(doc);
    CHECK(Sound(doc));
    glm::dvec3 lo, hi;
    doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
    CHECK_THAT(hi.z - lo.z, WithinAbs(kT, 1e-6));
  }
  SECTION("a flange on a folded tab's edge") {
    Document doc;
    const uint64_t base = TabPlate(doc);
    AddFold(doc, {42, 20}, {58, 20});
    Built(doc);
    // The tab's free end, now at the top: its edge on the tab's inner face.
    const Body& body = doc.GetBodies()[0];
    glm::dvec3 lo, hi;
    body.shape.GetBoundingBox(lo, hi);
    std::string edge;
    {
      TopTools_IndexedMapOfShape es;
      TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, es);
      double bestY = 1e300;
      for (int i = 1; i <= es.Extent(); ++i) {
        BRepAdaptor_Curve c(TopoDS::Edge(es(i)));
        const gp_Pnt p0 = c.Value(c.FirstParameter());
        const gp_Pnt p1 = c.Value(c.LastParameter());
        if (c.GetType() != GeomAbs_Line || std::abs(p0.Z() - hi.z) > 1e-6 ||
            std::abs(p1.Z() - hi.z) > 1e-6 || std::abs(p0.Y() - p1.Y()) > 1e-6)
          continue;
        if (p0.Y() < bestY)
          if (const std::string* n = body.elementMap.EdgeNameOf(i - 1)) {
            bestY = p0.Y();
            edge = *n;
          }
      }
    }
    REQUIRE_FALSE(edge.empty());
    auto lip = std::make_unique<EdgeFlangeFeature>(
        "Lip", std::vector<ElementRef>{ElementRef{base, edge}}, 8.0, 90.0);
    lip->position = FlangePosition::BendOutside;
    doc.AddFeature(std::move(lip));
    Built(doc);
    REQUIRE(doc.GetBodies()[0].bends.size() == 2);
    doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
    Built(doc);
    CHECK(Sound(doc));
    doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
    CHECK_THAT(hi.z - lo.z, WithinAbs(kT, 1e-6));
  }
}
