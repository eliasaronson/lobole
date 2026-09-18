#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Angles.h"
#include "core/tube/Check.h"
#include "core/tube/Rules.h"

#include <algorithm>
#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// 25.4 x 2.4 steel: the size a Formula Student rulebook names for a main
// hoop, and the one the chassis generator builds with.
tube::TubeRules HoopStock() {
  tube::TubeRules r;
  r.material = "steel";
  r.size = "25.4x2.4";
  const auto s = tube::LookupSize(r.material, r.size, nullptr);
  REQUIRE(s.has_value());
  r.outerDiameter = s->outerDiameter;
  r.wallThickness = s->wallThickness;
  return r;
}

bool Has(const tube::CheckResult& r, const std::string& kind) {
  return std::any_of(r.violations.begin(), r.violations.end(),
                     [&](const tube::Violation& v) { return v.kind == kind; });
}

// A member whose every straight is comfortably long, so a test can plant one
// fault at a time.
tube::Member Easy(double radius, double angle = 90.0) {
  tube::Member m;
  m.name = "Member";
  m.bends = {{angle, radius, 300.0, 0.0}, {angle, radius, 300.0, 90.0}};
  double used = 0.0;
  for (const tube::Bend& b : m.bends)
    used += b.straightBefore +
            tube::BendArcLength(angles::ToRadians(b.angleDeg), b.radius);
  m.length = used + 300.0; // a generous tail
  return m;
}

} // namespace

TEST_CASE("A tube size is looked up, never rounded to a neighbour", "[tube]") {
  const auto s = tube::LookupSize("steel", "25.4x2.4", nullptr);
  REQUIRE(s.has_value());
  CHECK_THAT(s->outerDiameter, WithinAbs(25.4, 1e-9));
  CHECK_THAT(s->wallThickness, WithinAbs(2.4, 1e-9));

  // A wall 0.05 mm off the stocked one is a DIFFERENT tube, and substituting
  // it is how a frame comes back lighter than its rulebook allows.
  std::string err;
  CHECK_FALSE(tube::LookupSize("steel", "25.4x2.45", &err).has_value());
  CHECK(err.find("not a stocked size") != std::string::npos);
  // The NEAREST sizes, not the table's first — a refusal that opens with the
  // biggest tube in the list is one nobody can act on.
  CHECK(err.find("25.4x2.4") != std::string::npos);
  CHECK(err.find("38.1") == std::string::npos);

  err.clear();
  CHECK_FALSE(tube::LookupSize("unobtainium", "25.4x2.4", &err).has_value());
  CHECK(err.find("no tube data") != std::string::npos);
  CHECK(err.find("steel") != std::string::npos); // names what IS carried
}

TEST_CASE("The minimum former is decided by D/t, not D", "[tube]") {
  // Same outside diameter, thinner wall, bigger former — which is the whole
  // point of the rule: the thin one has nothing holding the inside up.
  const double thick = tube::MinBendRadius("steel", 25.4, 2.4); // D/t 10.6
  const double thin = tube::MinBendRadius("steel", 25.4, 1.2);  // D/t 21.2
  CHECK(thin > thick);
  CHECK_THAT(thick, WithinRel(2.0 * 25.4, 1e-9));
  CHECK_THAT(thin, WithinRel(3.0 * 25.4, 1e-9));

  // Stainless work-hardens, so it wants more former than steel of the same
  // stock, and the provenance says which band answered.
  CHECK(tube::MinBendRadius("stainless_steel", 25.4, 2.4) > thick);
  CHECK(tube::MinBendRadiusSource("steel", 25.4, 2.4) == "table:steel D/t<15");
}

TEST_CASE("Wall thinning is the outer fibre's extra travel", "[tube]") {
  // t_out = t * R / (R + D/2): at R = 2D the outer fibre runs (R + D/2)/R
  // further, and the wall gives up exactly that fraction.
  const double od = 25.4, wall = 2.4, r = 2.0 * od;
  CHECK_THAT(tube::WallAfterBend(od, wall, r),
             WithinRel(wall * r / (r + od / 2.0), 1e-12));
  CHECK_THAT(tube::WallThinning(od, wall, r),
             WithinRel((od / 2.0) / (r + od / 2.0), 1e-12));
  // A tighter former thins more.
  CHECK(tube::WallThinning(od, wall, r / 2.0) >
        tube::WallThinning(od, wall, r));
  // And a straight tube loses nothing.
  CHECK_THAT(tube::WallThinning(od, wall, 1e9), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Unset rules derive from the stock, never from a literal", "[tube]") {
  const tube::TubeRules r = HoopStock();
  CHECK_THAT(r.EffectiveMinStraight(), WithinRel(2.0 * 25.4, 1e-9));
  CHECK_THAT(r.EffectiveTailLength(), WithinRel(1.5 * 25.4, 1e-9));
  CHECK_THAT(r.EffectiveMaxThinning(), WithinAbs(0.25, 1e-9));

  tube::TubeRules set = r;
  set.minStraight = 40.0;
  CHECK_THAT(set.EffectiveMinStraight(), WithinAbs(40.0, 1e-9));
}

TEST_CASE("An empty die set means any former; a real one refuses", "[tube]") {
  tube::TubeRules r = HoopStock();
  CHECK_THAT(*r.FormerFor(63.5), WithinAbs(63.5, 1e-9)); // nobody declared

  r.formerRadii = {76.2, 101.6};
  CHECK_THAT(*r.FormerFor(76.2), WithinAbs(76.2, 1e-9));
  CHECK_THAT(*r.FormerFor(80.0), WithinAbs(101.6, 1e-9)); // the next one up
  CHECK_FALSE(r.FormerFor(120.0).has_value()); // refused, not substituted
}

TEST_CASE("Rules that cannot be stored say why", "[tube]") {
  CHECK(tube::Validate(tube::TubeRules{}).empty()); // not a tube part: fine

  tube::TubeRules r = HoopStock();
  CHECK(tube::Validate(r).empty());

  // A die under what the stock takes is the one that scraps a bend.
  r.formerRadii = {20.0};
  const std::string err = tube::Validate(r);
  CHECK(err.find("under what") != std::string::npos);
  CHECK(err.find("table:steel") != std::string::npos);

  r.formerRadii.clear();
  r.maxThinning = 1.5;
  CHECK(tube::Validate(r).find("FRACTION") != std::string::npos);
}

TEST_CASE("check_tube finds each fault, and says what it did not look at",
          "[tube]") {
  const tube::TubeRules r = HoopStock();

  SECTION("a clean member is clean, and still says what was checked") {
    const auto res = tube::Check({Easy(76.2)}, r);
    CHECK(res.violations.empty());
    CHECK(res.membersChecked == 1);
    CHECK(res.bendsChecked == 2);
    // Ovality and springback need a machine, which this build has no model
    // of — "no violations" must not read as "all clear" on those.
    CHECK(res.notChecked.size() == 2);
  }

  SECTION("a former under the minimum") {
    const auto res = tube::Check({Easy(30.0)}, r);
    CHECK(Has(res, "bend_below_minimum_radius"));
    CHECK(res.violations.front().required > res.violations.front().actual);
  }

  SECTION("a former nobody owns") {
    tube::TubeRules dies = r;
    dies.formerRadii = {76.2, 101.6};
    const auto res = tube::Check({Easy(88.0)}, dies);
    CHECK(Has(res, "radius_not_in_die_set"));
    CHECK_FALSE(Has(res, "bend_below_minimum_radius")); // it is legal, just
                                                        // not owned
    CHECK(res.violations.front().message.find("101.60") != std::string::npos);
  }

  SECTION("two bends too close to clamp between") {
    tube::Member m = Easy(76.2);
    m.bends[1].straightBefore = 10.0;
    const auto res = tube::Check({m}, r);
    CHECK(Has(res, "straight_below_clamp_minimum"));
  }

  SECTION("nothing left to grip past the last bend") {
    tube::Member m = Easy(76.2);
    m.length -= 290.0; // a 10 mm tail
    const auto res = tube::Check({m}, r);
    CHECK(Has(res, "tail_below_gripper_minimum"));
  }

  SECTION("an outer wall pulled thinner than the rulebook allows") {
    // The radius rule and the wall rule are not the same rule. At the
    // tightest former this stock legally takes, the bend PASSES on radius and
    // thins 20% — fine by the slack default, and not fine for a team that has
    // to show 12%. That declared limit is what this check is for.
    tube::TubeRules strict = r;
    strict.maxThinning = 0.12;
    const double minR =
        tube::MinBendRadius(r.material, r.outerDiameter, r.wallThickness);
    const auto res = tube::Check({Easy(minR)}, strict);
    CHECK_FALSE(Has(res, "bend_below_minimum_radius"));
    CHECK(Has(res, "wall_thinning_over_limit"));
    // And the same bend is accepted when nobody demanded better.
    CHECK_FALSE(Has(tube::Check({Easy(minR)}, r), "wall_thinning_over_limit"));
  }
}
