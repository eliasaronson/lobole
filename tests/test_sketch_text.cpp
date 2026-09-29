// The text sketch entity (docs/sketching/README.md#text): layout against the
// bundled fonts' own numbers, persistence, refusals, profiles, naming and the
// solver's view of the label's box.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSolver.h"
#include "core/sketch/SketchText.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

namespace {

const SketchPlane kXY(PlaneType::XY);

// Liberation Sans Regular 2.1.5, read from the TTF with fontTools: units per
// em 2048; 'H' advance 1479, 'A' and 'V' 1366, the kern pair A-V -152; the
// cap height ('H' yMax) 1409; hhea ascender 1854, descender -434.
constexpr double kCap = 1409.0;

sketch::Text Label(const std::string& s, double h,
                   const std::string& font = "sans") {
  sketch::Text t;
  t.text = s;
  t.font = font;
  t.height = h;
  t.id = 7;
  return t;
}

// The extent of every point a label's segments carry (poles included: a
// glyph's ink never leaves its control hull, and the stems are lines).
struct Extent {
  glm::vec2 lo{1e9f}, hi{-1e9f};
};
Extent InkExtent(const sketch::TextGeometry& g) {
  Extent e;
  for (const sketch::TextContour& c : g.contours)
    for (const sketch::Entity& seg : c.segments) {
      std::vector<glm::vec2> pts;
      if (const auto* l = std::get_if<sketch::Line>(&seg))
        pts = {l->a, l->b};
      else if (const auto* b = std::get_if<sketch::BSpline>(&seg))
        pts = b->poles;
      for (const glm::vec2& p : pts) {
        e.lo = glm::min(e.lo, p);
        e.hi = glm::max(e.hi, p);
      }
    }
  return e;
}

} // namespace

TEST_CASE("A label's cap height and advance match the font", "[sketch][text]") {
  const double h = 10.0;
  auto g = sketch::BuildTextGeometry(Label("H", h));
  REQUIRE(g.has_value());
  const Extent e = InkExtent(*g);
  CHECK_THAT(e.hi.y, WithinAbs(h, 2e-3));   // the CAP height, from 'H'
  CHECK_THAT(e.lo.y, WithinAbs(0.0, 2e-3)); // on the baseline
  CHECK_THAT(g->width, WithinRel(1479.0 / kCap * h, 2e-3));

  // Kerning from the font's kern table: A-V is tighter than A + V.
  auto av = sketch::BuildTextGeometry(Label("AV", h));
  REQUIRE(av.has_value());
  CHECK_THAT(av->width, WithinRel((1366.0 - 152.0 + 1366.0) / kCap * h, 2e-3));

  // Spacing adds between characters, not after the last.
  sketch::Text spaced = Label("HH", h);
  spaced.spacing = 1.5;
  auto hh = sketch::BuildTextGeometry(spaced);
  REQUIRE(hh.has_value());
  CHECK_THAT(hh->width, WithinRel(2 * 1479.0 / kCap * h + 1.5, 2e-3));

  // The box is the FONT's ascender and descender, not the string's ink.
  const auto m = sketch::TextFontMetricsOf("sans");
  REQUIRE(m.has_value());
  CHECK_THAT(m->ascender, WithinRel(1854.0 / kCap, 2e-3));
  CHECK_THAT(m->descender, WithinRel(-434.0 / kCap, 2e-3));
  CHECK_THAT(g->box[0].y, WithinRel(-434.0 / kCap * h, 2e-3));
  CHECK_THAT(g->box[2].y, WithinRel(1854.0 / kCap * h, 2e-3));

  // Hershey Roman Simplex: 'H' is 21 units tall and advances 22 (its
  // left/right bearings are 'G' and ']', -11 and +11).
  auto hs = sketch::BuildTextGeometry(Label("H", 21.0, "hershey"));
  REQUIRE(hs.has_value());
  CHECK(hs->stroke);
  const Extent se = InkExtent(*hs);
  CHECK_THAT(se.hi.y, WithinAbs(21.0, 1e-4));
  CHECK_THAT(se.lo.y, WithinAbs(0.0, 1e-4));
  CHECK_THAT(hs->width, WithinAbs(22.0, 1e-4));
  CHECK(hs->contours.size() == 3); // two stems and the bar
  for (const sketch::TextContour& c : hs->contours)
    CHECK_FALSE(c.closed);
}

TEST_CASE("Alignment and angle place the box on the anchor", "[sketch][text]") {
  sketch::Text t = Label("H", 10.0);
  t.anchor = {5.0f, 5.0f};
  t.halign = sketch::TextHAlign::Center;
  t.valign = sketch::TextVAlign::Top;
  auto g = sketch::BuildTextGeometry(t);
  REQUIRE(g.has_value());
  // Centered: the box's left and right straddle the anchor; top: its top
  // edge passes through it.
  CHECK_THAT(g->box[0].x + g->box[1].x, WithinAbs(10.0, 1e-3));
  CHECK_THAT(g->box[2].y, WithinAbs(5.0, 1e-3));
  // A quarter turn stands the baseline up along +V.
  t.halign = sketch::TextHAlign::Left;
  t.valign = sketch::TextVAlign::Baseline;
  t.angle = M_PI / 2;
  auto r = sketch::BuildTextGeometry(t);
  REQUIRE(r.has_value());
  CHECK_THAT(r->box[1].x, WithinAbs(5.0 + 434.0 / kCap * 10.0, 2e-3));
  CHECK_THAT(r->box[1].y, WithinAbs(5.0 + r->width, 2e-3));
  // EntityPointUV answers the same corners (which 1..4).
  CHECK_THAT(sketch::EntityPointUV(t, 2).y, WithinAbs(r->box[1].y, 1e-4));
}

TEST_CASE("A label survives a save and reload", "[sketch][text]") {
  const fs::path path = fs::temp_directory_path() / "designer_text_test.json";
  uint32_t id = 0;
  {
    Document doc;
    auto sk = std::make_unique<SketchFeature>("Sk", kXY);
    sketch::Text t = Label("Rook 8", 6.5, "hershey");
    t.anchor = {1.0f, 2.0f};
    t.angle = 0.25;
    t.halign = sketch::TextHAlign::Right;
    t.valign = sketch::TextVAlign::Middle;
    t.spacing = 0.75;
    id = sk->AddEntity(t);
    sk->entityExprs[id]["height"] = "6.5";
    doc.AddFeature(std::move(sk));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* sk = dynamic_cast<SketchFeature*>(loaded->GetFeature(0));
  REQUIRE(sk != nullptr);
  const auto* t = std::get_if<sketch::Text>(&sk->entities.at(0));
  REQUIRE(t != nullptr);
  CHECK(t->id == id);
  CHECK(t->text == "Rook 8");
  CHECK(t->font == "hershey");
  CHECK_THAT(t->height, WithinRel(6.5, 1e-12));
  CHECK_THAT(t->angle, WithinRel(0.25, 1e-12));
  CHECK_THAT(t->spacing, WithinRel(0.75, 1e-12));
  CHECK(t->anchor == glm::vec2(1.0f, 2.0f));
  CHECK(t->halign == sketch::TextHAlign::Right);
  CHECK(t->valign == sketch::TextVAlign::Middle);
  CHECK(sk->entityExprs.at(id).at("height") == "6.5");
  // The glyphs are never saved: only the fields are.
  const nlohmann::json j = DocumentIO::FeatureToJson(*sk);
  const std::string dumped = j.dump();
  CHECK(dumped.find("poles") == std::string::npos);
  fs::remove(path);
}

TEST_CASE("An unknown font and a missing glyph are refused by name",
          "[sketch][text]") {
  auto bad = sketch::BuildTextGeometry(Label("A", 5.0, "comic"));
  REQUIRE_FALSE(bad.has_value());
  CHECK_THAT(bad.error(), ContainsSubstring("comic"));
  CHECK_THAT(bad.error(), ContainsSubstring("sans"));
  CHECK_THAT(bad.error(), ContainsSubstring("hershey"));

  auto han = sketch::BuildTextGeometry(Label("A\xe4\xb8\xad", 5.0)); // A中
  REQUIRE_FALSE(han.has_value());
  CHECK_THAT(han.error(), ContainsSubstring("U+4E2D"));

  auto accent = sketch::BuildTextGeometry(Label("caf\xc3\xa9", 5.0, "hershey"));
  REQUIRE_FALSE(accent.has_value());
  CHECK_THAT(accent.error(), ContainsSubstring("U+00E9"));

  auto twoLines = sketch::BuildTextGeometry(Label("a\nb", 5.0));
  REQUIRE_FALSE(twoLines.has_value());
  CHECK_THAT(twoLines.error(), ContainsSubstring("newline"));

  // On load: a JSON label naming an unknown font or alignment is refused,
  // never loaded as the default.
  SketchFeature proto("S", kXY);
  proto.AddEntity(Label("a", 5.0));
  nlohmann::json sk = DocumentIO::FeatureToJson(proto);
  REQUIRE_NOTHROW(DocumentIO::FeatureFromJson(sk));
  sk["entities"][0]["font"] = "helvetica";
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(sk),
                    ContainsSubstring("helvetica") &&
                        ContainsSubstring("sans"));
  sk["entities"][0]["font"] = "sans";
  sk["entities"][0]["halign"] = "centre";
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(sk),
                    ContainsSubstring("centre") && ContainsSubstring("center"));

  // A missing glyph fails the SKETCH, named, rather than dropping a letter.
  Document doc;
  auto s = std::make_unique<SketchFeature>("S", kXY);
  s->AddEntity(Label("\xe4\xb8\xad", 5.0));
  doc.AddFeature(std::move(s));
  doc.Regenerate();
  CHECK_THAT(doc.GetFeature(0)->GetError(), ContainsSubstring("U+4E2D"));
}

TEST_CASE("Every printable ASCII glyph of the outline font builds a profile",
          "[sketch][text]") {
  // The bundled font must have no overlapping contours: a profile refuses
  // partial overlap, rightly, so one bad glyph is a letter nobody can cut.
  for (char c = '!'; c <= '~'; c++) {
    INFO("glyph '" << c << "'");
    const auto loops =
        sketch::BuildLoops({sketch::Entity(Label(std::string(1, c), 8.0))});
    REQUIRE_FALSE(loops.empty());
    ElementMap map;
    std::string err;
    CADShape prism =
        sketch::BuildPrismNamed(kXY, loops, 1.0, false, 42, map, &err);
    INFO(err);
    REQUIRE(prism.IsValid());
    CHECK(prism.Volume() > 0.0);
  }
}

TEST_CASE("A cut label's faces are named and picking one letter picks all",
          "[sketch][text]") {
  Document doc;
  auto base = std::make_unique<SketchFeature>("Base", kXY);
  base->AddRectangle({-5, -5}, {45, 20});
  doc.AddFeature(std::move(base));
  const uint64_t baseId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Block", baseId, 5.0,
                                                  ExtrudeMode::NewBody));
  auto label = std::make_unique<SketchFeature>("Label", kXY);
  const uint32_t textId = label->AddEntity(Label("ab8", 10.0));
  doc.AddFeature(std::move(label));
  const uint64_t labelId = doc.GetFeature(2)->GetId();
  auto cut =
      std::make_unique<ExtrudeFeature>("Cut", labelId, 2.0, ExtrudeMode::Cut);
  cut->regions = {{textId}}; // ONE letter's region is the whole label
  doc.AddFeature(std::move(cut));
  doc.Regenerate();
  for (size_t i = 0; i < doc.FeatureCount(); i++)
    INFO(doc.GetFeature(i)->GetName() << ": " << doc.GetFeature(i)->GetError());
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  const uint64_t cutId = doc.GetFeature(3)->GetId();

  // Every letter's walls, each named down to its segment, each decoding to
  // a side face of the ONE text entity.
  std::set<std::string> glyphs;
  size_t named = 0;
  const std::string prefix =
      std::to_string(cutId) + "/g" + std::to_string(textId) + ".";
  for (const auto& [idx, name] : body.elementMap.names) {
    if (name.rfind(prefix, 0) != 0)
      continue;
    named++;
    const std::string word = name.substr(prefix.size());
    glyphs.insert(word.substr(0, word.find('.')));
    const topo::ElementOrigin o = topo::DecodeElementName(name);
    INFO(name);
    CHECK(o.ok);
    CHECK(o.role == topo::ElementRole::Side);
    CHECK(o.entity == textId);
  }
  CHECK(named > 20);
  CHECK(glyphs == std::set<std::string>{"0", "1", "2"});

  // The same cut with no regions removes exactly as much: one region row
  // picked all three letters.
  Document whole;
  auto b2 = std::make_unique<SketchFeature>("Base", kXY);
  b2->AddRectangle({-5, -5}, {45, 20});
  whole.AddFeature(std::move(b2));
  whole.AddFeature(std::make_unique<ExtrudeFeature>(
      "Block", whole.GetFeature(0)->GetId(), 5.0, ExtrudeMode::NewBody));
  auto l2 = std::make_unique<SketchFeature>("Label", kXY);
  l2->AddEntity(Label("ab8", 10.0));
  whole.AddFeature(std::move(l2));
  whole.AddFeature(std::make_unique<ExtrudeFeature>(
      "Cut", whole.GetFeature(2)->GetId(), 2.0, ExtrudeMode::Cut));
  whole.Regenerate();
  REQUIRE(whole.GetBodies().size() == 1);
  CHECK_THAT(body.shape.Volume(),
             WithinRel(whole.GetBodies()[0].shape.Volume(), 1e-9));
  CHECK(body.shape.Volume() < 50.0 * 25.0 * 5.0 - 1.0);
}

TEST_CASE("Stroke text makes no profile and an extrude on it is refused",
          "[sketch][text]") {
  const sketch::Text t = Label("a1", 5.0, "hershey");
  CHECK(sketch::BuildLoops({sketch::Entity(t)}).empty());
  // Beside a closed profile it is skipped silently.
  CHECK(sketch::BuildLoops({sketch::Entity(t), sketch::Circle{{30, 0}, 2.0}})
            .size() == 1);

  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  sk->AddEntity(t);
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "E", doc.GetFeature(0)->GetId(), 1.0, ExtrudeMode::NewBody));
  doc.Regenerate();
  const std::string err = doc.GetFeature(1)->GetError();
  CHECK_THAT(err, ContainsSubstring("a1"));
  CHECK_THAT(err, ContainsSubstring("single-stroke"));
  CHECK_THAT(err, ContainsSubstring("hershey"));
}

TEST_CASE("A label's box corner can be dimensioned", "[sketch][text]") {
  SketchFeature sk("S", kXY);
  const uint32_t id = sk.AddEntity(Label("H", 10.0));
  // Bottom-left corner 3 mm right of the origin, top-left 20 mm above it:
  // the anchor (the baseline's start) must land where that puts it.
  sketch::Constraint dx{
      sketch::ConstraintKind::DistanceX, {sketch::kOrigin, 0}, {id, 1}};
  dx.value = 3.0;
  sketch::Constraint dy{
      sketch::ConstraintKind::DistanceY, {sketch::kOrigin, 0}, {id, 4}};
  dy.value = 20.0;
  sk.constraints = {dx, dy};
  sketch::Solver solver;
  solver.Load(sk);
  const sketch::SolveResult r = solver.Solve();
  REQUIRE(r.ok);
  CHECK(r.dof == 0);
  solver.WriteBack(sk.entities);
  const auto& t = std::get<sketch::Text>(sk.entities[0]);
  CHECK_THAT(t.anchor.x, WithinAbs(3.0, 1e-4));
  CHECK_THAT(t.anchor.y, WithinAbs(20.0 - 1854.0 / kCap * 10.0, 2e-3));
  CHECK_THAT(sketch::EntityPointUV(t, 4).y, WithinAbs(20.0, 1e-3));
}

TEST_CASE("A region filter keeps the edges of a loop already in place",
          "[sketch][text]") {
  // FilterLoopsToRegions compacts the kept loops forward; one already at its
  // slot used to be move-assigned onto itself, which empties a vector.
  SketchFeature sk("S", kXY);
  const auto sides = sk.AddRectangle({0, 0}, {4, 2});
  sk.AddRectangle({10, 0}, {12, 2});
  std::vector<sketch::Loop> loops = sketch::BuildLoops(sk.entities);
  REQUIRE(loops.size() == 2);
  std::vector<uint32_t> rect(sides.begin(), sides.end());
  std::sort(rect.begin(), rect.end());
  REQUIRE(sketch::FilterLoopsToRegions(loops, {rect}));
  REQUIRE(loops.size() == 1);
  CHECK(loops[0].edges.size() == 4);
}
