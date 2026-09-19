#pragma once

#include <optional>
#include <string>
#include <vector>

// Tube-bending rules and bend arithmetic (docs/tube/plan.md, T1).
//
// A bent tube is a length of round stock pulled round a former of some radius
// through some angle, rotated about its own axis between bends. Everything a
// bend needs to know beyond its own angle and former — what the stock is, how
// tight this material and wall can be pulled, which formers the bender owns,
// how much straight it needs to clamp — is one per-document object, so a part
// cannot be half 25.4 x 2.4 steel and half 19 x 1.6 aluminium.
//
// Pure static data plus arithmetic, like sheet:: and fits::. It touches no
// geometry and no document; the features read it and build.
//
// Every number here is MILLIMETRES and every angle RADIANS.
namespace tube {

// One row of stock: a material and the designation a shop orders by, OD x
// wall in millimetres, which is how round tube is sold everywhere that is not
// a pipe schedule.
struct Size {
  std::string material;    // a CADMaterials slug
  std::string designation; // "25.4x2.4"
  double outerDiameter = 0.0;
  double wallThickness = 0.0;
};

// The designation a shop orders this stock by: "25.4x2.4", trailing zeros
// off, which is how round tube is quoted everywhere that is not a pipe
// schedule.
[[nodiscard]] std::string Designation(double outerDiameter,
                                      double wallThickness);

// Everything stocked, in material then descending-OD order.
[[nodiscard]] const std::vector<Size>& SizeTable();
// The materials tube data is carried for, in table order. The list IS the
// scope: a picker built from it cannot offer one LookupSize would refuse.
[[nodiscard]] const std::vector<std::string>& Materials();
// The designations carried for one material, in table order.
[[nodiscard]] std::vector<std::string> SizesFor(const std::string& material);
// The OD and wall of `designation` in `material`. Refused, never rounded to a
// neighbour — `error` (may be null) names the nearest stocked designations,
// the way an unstocked fastener size is refused.
[[nodiscard]] std::optional<Size> LookupSize(const std::string& material,
                                             const std::string& designation,
                                             std::string* error);

// The tightest CENTRELINE radius this stock takes without collapsing, as a
// multiple of the outside diameter.
//
// The ratio that decides it is D/t, not D: a thin wall has nothing to stop it
// folding on the inside of the bend, so a 25.4 x 1.2 tube needs a bigger
// former than a 25.4 x 2.4 one of the same material. Three bands (D/t < 15,
// 15 <= D/t < 30, D/t >= 30) times a per-material factor — the rule of thumb
// a bender quotes from, NOT a tooling vendor's chart, and deliberately on the
// conservative side of one: the cost of being wrong is a scrapped bend and
// the cost of being careful is a slightly larger former.
[[nodiscard]] double MinBendRadius(const std::string& material, double od,
                                   double wall);
// Which band answered, for the provenance a bend carries: "table:steel
// D/t<15". A limit nobody can trace is a limit nobody can argue with when the
// tube comes back kinked.
[[nodiscard]] std::string MinBendRadiusSource(const std::string& material,
                                              double od, double wall);

// What the OUTSIDE of the bend is left with. The outer fibre travels further
// than the centreline — (R + D/2) against R — and the wall gives up the
// difference: t_out = t * R / (R + D/2). Geometry, the same neutral-axis
// argument sheet::BendAllowance rests on, and the number a rules committee
// asks a team to show for a roll hoop.
[[nodiscard]] double WallAfterBend(double od, double wall, double radius);
// That loss as a fraction of the original wall (0.12 = 12% thinner).
[[nodiscard]] double WallThinning(double od, double wall, double radius);

// The developed length of one bend's arc along the centreline: angle * R.
// A tube is CUT to the developed length of its whole path, which is what
// tube_schedule reports; this is the per-bend term of it.
[[nodiscard]] double BendArcLength(double angleRad, double radius);

// The per-document rules. One per part, read by every check and every
// schedule.
//
// `formerRadii`, `minStraight` and `tailLength` are unset at empty/0 and then
// DERIVED from the stock — the "unset inherits" pattern sheet::SheetMetalRules
// uses. A literal millimetre default for any of them would be correct in the
// abstract and wrong against the tube actually loaded.
struct TubeRules {
  std::string material; // a CADMaterials slug; required
  // There is deliberately NO size here, and that is the one place this
  // parts company with sheet::SheetMetalRules. A sheet part is cut from ONE
  // blank, so a per-feature gauge would let a body be two thicknesses at
  // once. A tube FRAME is not: a Formula Student chassis is 25.4 x 2.4 in
  // its hoops, 25.4 x 1.65 in its side impact structure and 19.05 in its
  // tertiary members, and a rulebook is what says so. Each member carries
  // its own diameter and wall on its Pipe; what is shared is the MATERIAL
  // and the BENDER, which is what this holds.
  //
  // The die set: the centreline radii this bender owns. EMPTY means any
  // radius is available, which is the honest reading of "no bender declared"
  // — not a guess at a common set.
  std::vector<double> formerRadii;
  double minStraight = 0.0; // between bends; 0 = 2 x the member's OD
  double tailLength = 0.0;  // past the last bend; 0 = 1.5 x it, to grip
  // How much of the wall the outside of a bend may give up, as a fraction.
  // 0 = 0.25, roughly what draw bending is accepted at — which is deliberately
  // slack, because at the tightest former this stock legally takes it thins
  // 20% and a default that fires on every legal bend is noise. The field
  // exists for the case that is not slack: a rulebook that demands the wall
  // stay above a stated figure, which is what a Formula Student team has to
  // show for a roll hoop.
  double maxThinning = 0.0;

  // The resolved values, each applying the rule in its field comment above.
  // The straight a clamp needs and the tail a bender grips are multiples of
  // the MEMBER's diameter, so they are asked per member.
  [[nodiscard]] double EffectiveMinStraight(double outerDiameter) const;
  [[nodiscard]] double EffectiveTailLength(double outerDiameter) const;
  [[nodiscard]] double EffectiveMaxThinning() const;
  // The former to use for `wanted`: itself when the die set is empty, else
  // the smallest owned radius that is not under it — nullopt when every die
  // is smaller, which is a refusal and not a substitution.
  [[nodiscard]] std::optional<double> FormerFor(double wanted) const;
};

// Why these rules cannot be stored (empty = fine): an unknown material, a
// size that material is not stocked in, a former under what the stock takes,
// a negative straight or tail, a thinning limit outside (0, 1). Checked at
// the RPC boundary and at load, so no feature has to wonder.
[[nodiscard]] std::string Validate(const TubeRules& rules);

} // namespace tube
