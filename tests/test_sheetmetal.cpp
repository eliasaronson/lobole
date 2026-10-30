// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/io/Serialization.h"

#include <cmath>
#include <glm/glm.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
sheet::SheetMetalRules Rules(const std::string& material,
                             const std::string& gauge) {
  sheet::SheetMetalRules r;
  r.material = material;
  r.gauge = gauge;
  r.thickness = *sheet::LookupGauge(material, gauge, nullptr);
  return r;
}
} // namespace

TEST_CASE("one gauge number is three thicknesses", "[sheetmetal]") {
  // The trap the whole table exists for: 16ga is not a thickness, it is a
  // thickness PER MATERIAL, and using one column for all of them is a 15%
  // error in every blank.
  CHECK_THAT(*sheet::LookupGauge("steel", "16ga", nullptr),
             WithinAbs(1.519, 1e-9));
  CHECK_THAT(*sheet::LookupGauge("stainless_steel", "16ga", nullptr),
             WithinAbs(1.587, 1e-9));
  CHECK_THAT(*sheet::LookupGauge("aluminum_6061_t6", "16ga", nullptr),
             WithinAbs(1.291, 1e-9));
  // Metric stock is the same designation shape, so a shop that orders by
  // thickness never touches the gauge column.
  CHECK_THAT(*sheet::LookupGauge("steel", "1.5mm", nullptr),
             WithinAbs(1.5, 1e-9));
}

TEST_CASE("an unstocked gauge is refused naming what is stocked",
          "[sheetmetal]") {
  std::string why;
  CHECK_FALSE(sheet::LookupGauge("steel", "13ga", &why).has_value());
  // Refuse, don't substitute: 13ga must not quietly become 12ga or 14ga.
  CHECK(why.find("13ga") != std::string::npos);
  CHECK(why.find("12ga") != std::string::npos);
  CHECK(why.find("14ga") != std::string::npos);

  why.clear();
  CHECK_FALSE(sheet::LookupGauge("titanium", "16ga", &why).has_value());
  CHECK(why.find("titanium") != std::string::npos);
  CHECK(why.find("aluminum_6061_t6") != std::string::npos);
}

TEST_CASE("every stocked designation resolves", "[sheetmetal]") {
  // The list IS the scope: a picker built from GaugesFor cannot offer a row
  // LookupGauge refuses.
  REQUIRE_FALSE(sheet::GaugeTable().empty());
  for (const sheet::Gauge& g : sheet::GaugeTable()) {
    INFO(g.material << " " << g.designation);
    const auto t = sheet::LookupGauge(g.material, g.designation, nullptr);
    REQUIRE(t.has_value());
    CHECK_THAT(*t, WithinAbs(g.thickness, 1e-12));
    CHECK(g.thickness > 0.0);
  }
}

TEST_CASE("K moves with R/t, not with the material alone", "[sheetmetal]") {
  // The three-band chart. A single global K is the thing every shop
  // overrides, and it is wrong by ~10% at the ends of the range.
  const double t = 1.5;
  CHECK_THAT(sheet::KFactor("steel", 1.5, t), WithinAbs(0.41, 1e-12)); // R/t 1
  CHECK_THAT(sheet::KFactor("steel", 4.5, t), WithinAbs(0.46, 1e-12)); // R/t 3
  CHECK_THAT(sheet::KFactor("steel", 9.0, t), WithinAbs(0.50, 1e-12)); // R/t 6
  // Soft and hard classes sit either side of it at the same ratio.
  CHECK(sheet::KFactor("copper", 1.5, t) < sheet::KFactor("steel", 1.5, t));
  CHECK(sheet::KFactor("stainless_steel", 1.5, t) >
        sheet::KFactor("steel", 1.5, t));
  // And the band that answered is reported, so a bend can say where its
  // number came from.
  CHECK(sheet::KFactorSource("steel", 1.5, t) == "table:steel R/t<2");
  CHECK(sheet::KFactorSource("steel", 9.0, t) == "table:steel R/t>=4");
}

TEST_CASE("bend allowance is the neutral arc", "[sheetmetal]") {
  const double t = 1.0, r = 1.0, k = 0.44;
  const double quarter = std::numbers::pi / 2.0;
  // BA = angle * (R + K*t). At 90 degrees with R = t = 1 and K = 0.44 the
  // neutral radius is 1.44, so the arc is 1.44 * pi/2.
  CHECK_THAT(sheet::BendAllowance(quarter, r, k, t),
             WithinRel(1.44 * quarter, 1e-12));
  // A bend eats less flat stock than the two outside legs claim, which is
  // the whole reason a blank is shorter than the sum of its sides.
  CHECK(sheet::BendDeduction(quarter, r, k, t) > 0.0);
  CHECK_THAT(sheet::BendDeduction(quarter, r, k, t),
             WithinRel(2.0 * (r + t) * std::tan(quarter / 2.0) -
                           sheet::BendAllowance(quarter, r, k, t),
                       1e-12));
  // Sign of the angle is the bend's direction, never its size.
  CHECK_THAT(sheet::BendAllowance(-quarter, r, k, t),
             WithinRel(sheet::BendAllowance(quarter, r, k, t), 1e-12));
}

TEST_CASE("unset rule fields derive from the stock", "[sheetmetal]") {
  sheet::SheetMetalRules r = Rules("steel", "16ga");
  // Radius: the material's own minimum, not a literal millimetre.
  CHECK_THAT(r.EffectiveBendRadius(), WithinAbs(1.519, 1e-9)); // 1.0 * t
  CHECK(r.KSource(r.EffectiveBendRadius()) == "table:steel R/t<2");
  CHECK_THAT(r.EffectiveKFactor(r.EffectiveBendRadius()),
             WithinAbs(0.41, 1e-12));
  // Relief: wide enough for the die, deep enough to clear the tangent line.
  CHECK_THAT(r.EffectiveReliefWidth(), WithinAbs(1.5 * 1.519, 1e-9));
  CHECK_THAT(r.EffectiveReliefDepth(r.EffectiveBendRadius()),
             WithinAbs(1.519 + 1.519, 1e-9));
  // Thin stock still gets a relief a laser can cut.
  const sheet::SheetMetalRules thin = Rules("steel", "24ga"); // 0.607 mm
  CHECK_THAT(thin.EffectiveReliefWidth(), WithinAbs(1.0, 1e-12));

  // An override stands and stops tracking the table.
  r.bendRadius = 5.0;
  r.kFactor = 0.33;
  CHECK_THAT(r.EffectiveBendRadius(), WithinAbs(5.0, 1e-12));
  CHECK_THAT(r.EffectiveKFactor(5.0), WithinAbs(0.33, 1e-12));
  CHECK(r.KSource(5.0) == "rules");
}

TEST_CASE("a radius the material cannot take is refused", "[sheetmetal]") {
  sheet::SheetMetalRules r = Rules("stainless_steel", "16ga"); // 1.587 mm
  CHECK(sheet::Validate(r).empty());
  // Stainless wants 1.5t inside; asking for 1t cracks the outer fibre, and
  // the part looks perfect until it is bent.
  r.bendRadius = 1.587;
  const std::string why = sheet::Validate(r);
  CHECK_FALSE(why.empty());
  CHECK(why.find("tighter") != std::string::npos);
  r.bendRadius = sheet::MinBendRadius("stainless_steel", r.thickness);
  CHECK(sheet::Validate(r).empty());
  // A K outside (0,1) is not a neutral axis at all.
  r.kFactor = 1.4;
  CHECK_FALSE(sheet::Validate(r).empty());
}

TEST_CASE("a thickness that disagrees with its gauge is refused",
          "[sheetmetal]") {
  // The gauge IS the thickness. A document claiming 16ga at 1.2 mm bends
  // correctly and cuts to the wrong blank.
  sheet::SheetMetalRules r = Rules("steel", "16ga");
  r.thickness = 1.2;
  CHECK_FALSE(sheet::Validate(r).empty());
}

TEST_CASE("an unknown relief style is refused", "[sheetmetal]") {
  sheet::ReliefStyle style = sheet::ReliefStyle::None;
  CHECK(sheet::ReliefStyleFromString("obround", style));
  CHECK(style == sheet::ReliefStyle::Obround);
  // Not silently rectangular: a relief of the wrong shape is a corner that
  // cracks, and nothing about the document says so.
  CHECK_FALSE(sheet::ReliefStyleFromString("round", style));
  CHECK(style == sheet::ReliefStyle::Obround); // untouched on refusal
  for (const char* name : {"none", "rectangular", "obround", "tear"})
    CHECK(sheet::ReliefStyleList().find(name) != std::string::npos);
}

TEST_CASE("rules round-trip through a document", "[sheetmetal]") {
  Document doc;
  CHECK_FALSE(doc.SheetMetal().has_value());
  sheet::SheetMetalRules r = Rules("aluminum_6061_t6", "1.5mm");
  r.bendRadius = 3.0;
  r.reliefStyle = sheet::ReliefStyle::Obround;
  doc.SetSheetMetal(r);

  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "designer_sheetmetal_roundtrip.json";
  REQUIRE(DocumentIO::Save(doc, path.string()).has_value());
  auto loaded = DocumentIO::Load(path.string());
  REQUIRE(loaded.has_value());
  Document& reloaded = **loaded;
  REQUIRE(reloaded.SheetMetal().has_value());
  const sheet::SheetMetalRules& got = *reloaded.SheetMetal();
  CHECK(got.material == "aluminum_6061_t6");
  CHECK(got.gauge == "1.5mm");
  CHECK_THAT(got.thickness, WithinAbs(1.5, 1e-12));
  CHECK_THAT(got.bendRadius, WithinAbs(3.0, 1e-12));
  CHECK(got.reliefStyle == sheet::ReliefStyle::Obround);
  // The DERIVED fields were not written, so they still track the table.
  CHECK_THAT(got.kFactor, WithinAbs(0.0, 1e-12));
  CHECK_THAT(got.EffectiveKFactor(3.0), WithinAbs(0.46, 1e-12));
  std::filesystem::remove(path);
}

TEST_CASE("a part that is not sheet metal writes no sheet_metal key",
          "[sheetmetal]") {
  // Every existing document has to re-save byte-identically, which is what
  // keeps the golden corpus valid across this bump.
  Document doc;
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "designer_sheetmetal_absent.json";
  REQUIRE(DocumentIO::Save(doc, path.string()).has_value());
  std::ifstream in(path);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  CHECK(text.find("sheet_metal") == std::string::npos);
  std::filesystem::remove(path);
}

TEST_CASE("changing the gauge invalidates the regen cache", "[sheetmetal]") {
  // Unlike a tolerance, the rules ARE geometry: every flange reads the
  // thickness, so a gauge change must rebuild rather than hit the cache.
  Document doc;
  // A hash needs something to hash: with no features the tip is 0 whatever
  // the rules say, which is right — there is nothing to rebuild.
  doc.AddFeature(std::make_unique<BoxFeature>("Blank", glm::vec3(0.0f), 100.0,
                                              60.0, 1.519));
  doc.SetSheetMetal(Rules("steel", "16ga"));
  const size_t before = doc.TipChainHash();
  REQUIRE(before != 0);
  doc.SetSheetMetal(Rules("steel", "14ga"));
  CHECK(doc.TipChainHash() != before);
  // And clearing them is a third state, not the same as either.
  doc.SetSheetMetal(std::nullopt);
  CHECK(doc.TipChainHash() != before);
}

TEST_CASE("setting rules undoes as one step", "[sheetmetal]") {
  Document doc;
  CommandStack stack;
  stack.Execute(std::make_unique<SetSheetMetalRulesCommand>(
                    Rules("aluminum_6061_t6", "16ga")),
                doc);
  REQUIRE(doc.SheetMetal().has_value());
  CHECK(doc.SheetMetal()->gauge == "16ga");

  stack.Execute(std::make_unique<SetSheetMetalRulesCommand>(
                    Rules("aluminum_6061_t6", "18ga")),
                doc);
  CHECK(doc.SheetMetal()->gauge == "18ga");
  stack.Undo(doc);
  CHECK(doc.SheetMetal()->gauge == "16ga");
  stack.Undo(doc);
  CHECK_FALSE(doc.SheetMetal().has_value());
}
