#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/HemFeature.h"
#include "core/features/JogFeature.h"
#include "core/features/RefoldFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/UnfoldFeature.h"
#include "core/sheet/Check.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Hem and jog (docs/sheetmetal/README.md#features): the hem, the mitred corner
// and the jog. Each is checked three ways — against a hand-computed volume or
// outline, laid flat one gauge thick, and folded back up to exactly where it
// was.
namespace {

constexpr double kT = 1.5;
constexpr double kR = 2.25;
constexpr double kK = 0.41;
constexpr double kW = 100.0;
constexpr double kD = 60.0;
const double kPi = std::numbers::pi;

uint64_t Blank(Document& doc) {
  sheet::SheetMetalRules r;
  r.material = "aluminum";
  r.gauge = "1.5mm";
  r.thickness = kT;
  doc.SetSheetMetal(r);
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

ElementRef Top(const Document& doc, uint64_t base, const glm::dvec3& a,
               const glm::dvec3& b) {
  const std::string name = EdgeNamed(doc.GetBodies()[0], a, b);
  REQUIRE_FALSE(name.empty());
  return ElementRef{base, name};
}

const Feature& Last(Document& doc) {
  return *doc.GetFeature(doc.FeatureCount() - 1);
}

void Ok(Document& doc) {
  INFO(Last(doc).GetError());
  REQUIRE(Last(doc).GetError().empty());
}

glm::vec3 Lo(const Document& doc) {
  glm::vec3 lo, hi;
  doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
  return lo;
}
glm::vec3 Hi(const Document& doc) {
  glm::vec3 lo, hi;
  doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
  return hi;
}

bool Sound(const Document& doc) {
  return BRepCheck_Analyzer(doc.GetBodies()[0].shape.GetShape()).IsValid() &&
         doc.GetBodies()[0].shape.SolidCount() == 1;
}

double Sector(double angle, double r, double span) {
  return 0.5 * angle * ((r + kT) * (r + kT) - r * r) * span;
}

double Allowance(double angle, double r) {
  return sheet::BendAllowance(angle, r, sheet::KFactor("aluminum", r, kT), kT);
}

// Unfold every bend, check the blank is one gauge thick, fold it back, and
// check nothing moved.
void RoundTrip(Document& doc, uint64_t base) {
  const double volume = doc.GetBodies()[0].shape.Volume();
  const glm::vec3 lo = Lo(doc), hi = Hi(doc);
  doc.AddFeature(std::make_unique<UnfoldFeature>("Unfold", base));
  Ok(doc);
  CHECK_THAT(static_cast<double>(Hi(doc).z - Lo(doc).z), WithinAbs(kT, 1e-5));
  const uint64_t unfold = Last(doc).GetId();
  doc.AddFeature(std::make_unique<RefoldFeature>("Refold", unfold));
  Ok(doc);
  CHECK(Sound(doc));
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(volume, 1e-6));
  for (int i = 0; i < 3; ++i) {
    CHECK_THAT(static_cast<double>(Lo(doc)[i]),
               WithinAbs(static_cast<double>(lo[i]), 1e-4));
    CHECK_THAT(static_cast<double>(Hi(doc)[i]),
               WithinAbs(static_cast<double>(hi[i]), 1e-4));
  }
}

uint64_t JogSketch(Document& doc, double y) {
  auto sk = std::make_unique<SketchFeature>(
      "Jog line",
      SketchPlane(glm::vec3(0, 0, kT), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Line{{0, y}, {kW, y}, 1});
  doc.AddFeature(std::move(sk));
  return Last(doc).GetId();
}

} // namespace

// ---------------------------------------------------------------- the hem

TEST_CASE("a closed hem folds the edge back on itself", "[sheetmetal][hem]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const double ret = 6.0;
  doc.AddFeature(std::make_unique<HemFeature>(
      "Hem", std::vector<ElementRef>{Top(doc, base, {0, 0, kT}, {kW, 0, kT})},
      ret));
  Ok(doc);
  CHECK(Sound(doc));

  // Closed: the return lies a tenth of the sheet off the panel — the least a
  // model can keep between two walls without fusing them into one.
  const double r = kT / 20.0;
  const double setback = r + kT; // inside: the fold's outside on the edge
  const double expected =
      kW * kD * kT - kW * setback * kT + Sector(kPi, r, kW) + ret * kT * kW;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(expected, 1e-5));
  CHECK_THAT(static_cast<double>(Hi(doc).z), WithinAbs(kT + 2 * r + kT, 1e-5));
  // Inside means the drawn outline does not grow.
  CHECK_THAT(static_cast<double>(Lo(doc).y), WithinAbs(0.0, 1e-5));

  // A hem is crushed, not bent round a die, so it is exempt from the
  // material's minimum radius — which it is far under.
  REQUIRE(doc.GetBodies()[0].bends.size() == 1);
  CHECK(doc.GetBodies()[0].bends[0].radius <
        sheet::MinBendRadius("aluminum", kT));
}

TEST_CASE("a hem stops short of the wall beside it", "[sheetmetal][hem]") {
  // The shape every hemmed tray has: walls up, and a hem along the top of
  // one of them. Run the hem the full length and it meets the wall beside it
  // at the corner and fuses to it — a corner of continuous metal, which
  // folds from no blank. The inset is what a real tray has cut into it, and
  // a hem had no way to say it until v116.
  Document doc;
  const uint64_t base = Blank(doc);
  const double ret = 6.0, inset = kR + kT;
  const double r = kT / 20.0; // a closed hem's own radius

  auto hem = std::make_unique<HemFeature>(
      "Hem", std::vector<ElementRef>{Top(doc, base, {0, 0, kT}, {kW, 0, kT})},
      ret);
  hem->offsetStart = inset;
  hem->offsetEnd = inset;
  doc.AddFeature(std::move(hem));
  Ok(doc);
  CHECK(Sound(doc));

  // Exactly the full-length hem, less the two ends that were not folded:
  // the return, its sector, and the setback that is no longer cut away —
  // and less the relief notched at each end, because a fold stopping inside
  // a panel tears from its end whether it was bent or crushed.
  const double setback = r + kT;
  const double span = kW - 2.0 * inset;
  const double reliefW = std::max(1.5 * kT, 1.0);
  const double reliefD = r + kT; // EffectiveReliefDepth of the hem's radius
  const double notch = reliefW * (reliefD + setback) * kT;
  const double expected = kW * kD * kT - span * setback * kT +
                          Sector(kPi, r, span) + ret * kT * span - 2.0 * notch;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(expected, 1e-5));

  // ...and with the relief cut, nothing in the part is a violation: not the
  // hem's own radius, which is a crushed fold and not a die bend, and not
  // the ends, which are relieved.
  const sheet::CheckResult chk =
      sheet::Check(doc.GetBodies()[0], *doc.SheetMetal());
  for (const sheet::Violation& v : chk.violations)
    UNSCOPED_INFO(v.message);
  CHECK(chk.violations.empty());

  // And the hem is a bend the width of what was left, not of the whole edge.
  REQUIRE(doc.GetBodies()[0].bends.size() == 1);
  CHECK_THAT(doc.GetBodies()[0].bends[0].width, WithinAbs(span, 1e-6));

  // A negative inset is refused rather than quietly clamped.
  auto* edited = dynamic_cast<HemFeature*>(doc.GetFeature(2));
  REQUIRE(edited != nullptr);
  edited->offsetStart = -1.0;
  doc.ClearRegenCache();
  doc.Regenerate();
  CHECK_FALSE(doc.GetFeature(2)->GetError().empty());
  edited->offsetStart = inset;
  doc.ClearRegenCache();
  REQUIRE(doc.Regenerate());

  // It survives the round trip through the file, and lays flat.
  RoundTrip(doc, base);
}

TEST_CASE("an open hem stands off by its gap", "[sheetmetal][hem]") {
  Document doc;
  const uint64_t base = Blank(doc);
  auto hem = std::make_unique<HemFeature>(
      "Hem", std::vector<ElementRef>{Top(doc, base, {0, 0, kT}, {kW, 0, kT})},
      6.0);
  hem->kind = HemKind::Open;
  hem->gap = 1.0;
  doc.AddFeature(std::move(hem));
  Ok(doc);
  CHECK_THAT(static_cast<double>(Hi(doc).z), WithinAbs(kT + 1.0 + kT, 1e-5));
}

TEST_CASE("a hem lays flat and folds back", "[sheetmetal][hem]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const double ret = 6.0;
  doc.AddFeature(std::make_unique<HemFeature>(
      "Hem", std::vector<ElementRef>{Top(doc, base, {0, 0, kT}, {kW, 0, kT})},
      ret));
  Ok(doc);
  RoundTrip(doc, base);

  // Laid flat: the panel short of its setback, the half-turn's allowance and
  // the return, all in a row.
  Document flat;
  const uint64_t b2 = Blank(flat);
  flat.AddFeature(std::make_unique<HemFeature>(
      "Hem", std::vector<ElementRef>{Top(flat, b2, {0, 0, kT}, {kW, 0, kT})},
      ret));
  flat.AddFeature(std::make_unique<FlatPatternFeature>("Flat", b2));
  Ok(flat);
  const double r = kT / 20.0;
  const double area =
      kW * kD - kW * (r + kT) + kW * Allowance(kPi, r) + kW * ret;
  CHECK_THAT(flat.GetBodies()[0].shape.Volume(), WithinRel(area * kT, 1e-5));
}

// ------------------------------------------------------------- the mitre

TEST_CASE("flanges that meet at a corner are mitred, not fused",
          "[sheetmetal][miter]") {
  // Set back into the panel, two flanges both fill the corner square. Left
  // alone they fuse into a block no flat pattern can assign to either bend.
  Document doc;
  const uint64_t base = Blank(doc);
  doc.AddFeature(std::make_unique<EdgeFlangeFeature>(
      "Flanges",
      std::vector<ElementRef>{Top(doc, base, {0, 0, kT}, {kW, 0, kT}),
                              Top(doc, base, {0, 0, kT}, {0, kD, kT})},
      20.0, 90.0));
  Ok(doc);
  CHECK(Sound(doc));
  const std::vector<BendRegion>& bends = doc.GetBodies()[0].bends;
  REQUIRE(bends.size() == 2);
  CHECK(bends[0].trims.size() == 1);
  CHECK(bends[1].trims.size() == 1);
  // The two faces of the flanges carry different names: one prefix per
  // flange, or the second edge's faces would shadow the first's.
  std::set<std::string> names;
  for (const auto& [i, n] : doc.GetBodies()[0].elementMap.names)
    names.insert(n);
  CHECK(names.size() == doc.GetBodies()[0].elementMap.names.size());

  RoundTrip(doc, base);
}

TEST_CASE("a mitred pair lays flat", "[sheetmetal][miter]") {
  Document doc;
  const uint64_t base = Blank(doc);
  doc.AddFeature(std::make_unique<EdgeFlangeFeature>(
      "Flanges",
      std::vector<ElementRef>{Top(doc, base, {0, 0, kT}, {kW, 0, kT}),
                              Top(doc, base, {0, 0, kT}, {0, kD, kT})},
      20.0, 90.0));
  Ok(doc);
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  Ok(doc);
  CHECK(Sound(doc));
  CHECK_THAT(static_cast<double>(Hi(doc).z - Lo(doc).z), WithinAbs(kT, 1e-5));
  // Both walls lie out along their own sides: the blank reaches past the
  // panel by the setback-shortened bend plus the wall, on both.
  const double reach = -(kR + kT) + Allowance(kPi / 2.0, kR) + 20.0;
  CHECK_THAT(static_cast<double>(Lo(doc).x), WithinAbs(-reach, 1e-4));
  CHECK_THAT(static_cast<double>(Lo(doc).y), WithinAbs(-reach, 1e-4));
}

TEST_CASE("flanges bent outside the panel leave an open corner",
          "[sheetmetal][miter]") {
  // They only touch at a point, so there is nothing to mitre and nothing is
  // trimmed off either.
  Document doc;
  const uint64_t base = Blank(doc);
  auto f = std::make_unique<EdgeFlangeFeature>(
      "Flanges",
      std::vector<ElementRef>{Top(doc, base, {0, 0, kT}, {kW, 0, kT}),
                              Top(doc, base, {0, 0, kT}, {0, kD, kT})},
      20.0, 90.0);
  f->position = FlangePosition::BendOutside;
  doc.AddFeature(std::move(f));
  Ok(doc);
  CHECK(doc.GetBodies()[0].bends[0].trims.empty());
  const double expected = kW * kD * kT + Sector(kPi / 2, kR, kW) +
                          Sector(kPi / 2, kR, kD) + 20.0 * kT * (kW + kD);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(expected, 1e-6));
}

// ---------------------------------------------------------------- the jog

TEST_CASE("a jog steps the far side up", "[sheetmetal][jog]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const uint64_t line = JogSketch(doc, 30.0);
  const double h = 10.0;
  doc.AddFeature(std::make_unique<JogFeature>("Jog", line, h));
  Ok(doc);
  CHECK(Sound(doc));

  // Nothing removed: both halves of the panel, two quarter bends, and the
  // wall that makes up the rest of the height.
  const double wall = h - (2 * kR + kT);
  const double expected =
      kW * kD * kT + 2 * Sector(kPi / 2, kR, kW) + wall * kT * kW;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(expected, 1e-6));
  // The far face is exactly `height` up; the far side moved out by the
  // step's own run and not at all sideways.
  CHECK_THAT(static_cast<double>(Hi(doc).z), WithinAbs(kT + h, 1e-5));
  CHECK_THAT(static_cast<double>(Hi(doc).y), WithinAbs(kD + 2 * kR + kT, 1e-5));
  CHECK_THAT(static_cast<double>(Hi(doc).x - Lo(doc).x), WithinAbs(kW, 1e-5));
  CHECK(doc.GetBodies()[0].bends.size() == 2);
}

TEST_CASE("a jog lower than its own bends is refused", "[sheetmetal][jog]") {
  Document doc;
  Blank(doc);
  const uint64_t line = JogSketch(doc, 30.0);
  doc.AddFeature(std::make_unique<JogFeature>("Jog", line, 3.0));
  const std::string why = Last(doc).GetError();
  CHECK(why.find("lower than its") != std::string::npos);
}

TEST_CASE("a jog lays flat and folds back", "[sheetmetal][jog]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const uint64_t line = JogSketch(doc, 30.0);
  const double h = 10.0;
  doc.AddFeature(std::make_unique<JogFeature>("Jog", line, h));
  Ok(doc);
  RoundTrip(doc, base);

  Document flat;
  const uint64_t b2 = Blank(flat);
  flat.AddFeature(
      std::make_unique<JogFeature>("Jog", JogSketch(flat, 30.0), h));
  flat.AddFeature(std::make_unique<FlatPatternFeature>("Flat", b2));
  Ok(flat);
  const double wall = h - (2 * kR + kT);
  const double area = kW * kD + 2 * kW * Allowance(kPi / 2, kR) + kW * wall;
  CHECK_THAT(flat.GetBodies()[0].shape.Volume(), WithinRel(area * kT, 1e-5));
}

TEST_CASE("a flange older than a jog rides up with it and still lays flat",
          "[sheetmetal][jog]") {
  // The jog puts two NEW bends between the base and a flange that was made
  // first — so which panel an old bend folds from has to be worked out from
  // where its material is, not from the order it was cut in.
  Document doc;
  const uint64_t base = Blank(doc);
  auto wallFlange = std::make_unique<EdgeFlangeFeature>(
      "Far wall",
      std::vector<ElementRef>{Top(doc, base, {0, kD, kT}, {kW, kD, kT})}, 20.0,
      90.0);
  wallFlange->position = FlangePosition::BendOutside;
  doc.AddFeature(std::move(wallFlange));
  Ok(doc);
  const double h = 10.0;
  doc.AddFeature(std::make_unique<JogFeature>("Jog", JogSketch(doc, 30.0), h));
  Ok(doc);
  CHECK(Sound(doc));
  // The old wall went up with its panel.
  CHECK_THAT(static_cast<double>(Hi(doc).z),
             WithinAbs(kT + h + kR + 20.0, 1e-4));

  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  Ok(doc);
  CHECK_THAT(static_cast<double>(Hi(doc).z - Lo(doc).z), WithinAbs(kT, 1e-5));
  const double wall = h - (2 * kR + kT);
  const double area =
      kW * kD + 3 * kW * Allowance(kPi / 2, kR) + kW * wall + kW * 20.0;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(area * kT, 1e-5));
}
