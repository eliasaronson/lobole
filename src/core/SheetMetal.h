#pragma once

#include <optional>
#include <string>
#include <vector>

// Sheet-metal rules and bend arithmetic (docs/sheetmetal/plan.md, S1).
//
// A sheet part is a solid of uniform thickness whose bends are cylindrical:
// the only surface a brake press makes. Everything a flange needs to know
// beyond its own length and angle — how thick the stock is, how tight the tool
// can bend it, how far the neutral axis sits from the inside face — is one
// per-document object read by every feature, so a part cannot be half 1.5 mm
// aluminium and half 3 mm steel.
//
// Pure static data plus arithmetic, like fits:: and parts::. It touches no
// geometry and no document; the features read it and build.
//
// Every number here is MILLIMETRES and every angle RADIANS.
namespace sheet {

// The cut taken at each end of a bend so the metal can move without tearing
// the panel next to it. `None` is legal and means the user knows the hinge
// spans the whole edge.
enum class ReliefStyle { None, Rectangular, Obround, Tear };

[[nodiscard]] std::string ReliefStyleName(ReliefStyle style);
// Refuses an unrecognised name rather than defaulting — a relief that is
// silently rectangular where obround was asked for is a part that cracks at
// the corner. Named *FromString for the silent-parser scan in
// tests/test_conventions.cpp.
[[nodiscard]] bool ReliefStyleFromString(const std::string& name,
                                         ReliefStyle& out);
// The accepted names joined with ", ", for refusal messages.
[[nodiscard]] const std::string& ReliefStyleList();

// One row of stock: a material and a designation the shop orders by. The
// designation is either a US gauge ("16ga") or a plain metric thickness
// ("1.5mm"), because both are what real orders say, and which one a shop uses
// is not something this table gets to decide.
struct Gauge {
  std::string material;    // a CADMaterials slug
  std::string designation; // "16ga" | "1.5mm"
  double thickness = 0.0;  // mm
};

// Everything stocked, in material then descending-thickness order.
[[nodiscard]] const std::vector<Gauge>& GaugeTable();
// The materials sheet data is carried for, in table order. The list IS the
// scope: a picker built from it cannot offer one LookupGauge would refuse.
[[nodiscard]] const std::vector<std::string>& Materials();
// The designations carried for one material, in table order. The list IS the
// scope: a picker built from it cannot offer a row LookupGauge would refuse.
[[nodiscard]] std::vector<std::string> GaugesFor(const std::string& material);
// The thickness of `designation` in `material`. Refused, never rounded to a
// neighbour — `error` (may be null) names the nearest stocked designations,
// the way an unstocked fastener size is refused.
[[nodiscard]] std::optional<double> LookupGauge(const std::string& material,
                                                const std::string& designation,
                                                std::string* error);

// The tightest inside radius this material takes at this thickness without
// cracking the outer fibre, as a multiple of `t` per material. Conservative
// where a material has a range (304 stainless, 6061 aluminium), because the
// cost of being wrong is a scrapped part and the cost of being careful is a
// slightly larger radius.
[[nodiscard]] double MinBendRadius(const std::string& material, double t);

// K — where the neutral axis sits, as a fraction of `t` from the INSIDE face.
// It is not one number per material: it moves with the R/t ratio, which is why
// a single global K is the thing every shop overrides. Three bands per
// material class (R < 2t, 2t <= R < 4t, R >= 4t), the chart every brake
// operator has taped to the wall.
[[nodiscard]] double KFactor(const std::string& material, double radius,
                             double t);
// Which row of that chart answered, for the provenance a bend carries:
// "table:steel R/t<2". A bend whose K nobody can trace is a bend nobody can
// argue with when the part comes back the wrong length.
[[nodiscard]] std::string KFactorSource(const std::string& material,
                                        double radius, double t);

// The arc length of the neutral axis through the bend — how much flat stock
// the bend eats. This is the whole of unfolding: BA = angle * (R + K*t).
[[nodiscard]] double BendAllowance(double angleRad, double radius, double k,
                                   double t);
// How much shorter the blank is than the sum of the outside dimensions:
// BD = 2*(R + t)*tan(angle/2) - BA. What a brake operator sets up from.
[[nodiscard]] double BendDeduction(double angleRad, double radius, double k,
                                   double t);

// The per-document rules. One per part, read by every sheet feature.
//
// `bendRadius`, `kFactor`, `reliefWidth` and `reliefDepth` are unset at 0 and
// then DERIVED from the material and thickness — the Appearance "unset
// inherits" pattern. A literal millimetre default for any of them would be
// correct in the abstract and wrong against the stock actually loaded.
struct SheetMetalRules {
  std::string material;    // a CADMaterials slug; required
  std::string gauge;       // a designation from GaugesFor(material); required
  double thickness = 0.0;  // mm, resolved from the gauge at set time
  double bendRadius = 0.0; // 0 = MinBendRadius(material, thickness)
  double kFactor = 0.0;    // 0 = KFactor(material, radius, thickness)
  ReliefStyle reliefStyle = ReliefStyle::Rectangular;
  double reliefWidth = 0.0; // 0 = max(1.5*t, 1mm)
  double reliefDepth = 0.0; // 0 = radius + t

  // The resolved values, each applying the rule in its field comment above.
  [[nodiscard]] double EffectiveBendRadius() const;
  [[nodiscard]] double EffectiveKFactor(double radius) const;
  [[nodiscard]] std::string KSource(double radius) const;
  [[nodiscard]] double EffectiveReliefWidth() const;
  [[nodiscard]] double EffectiveReliefDepth(double radius) const;
};

// Why these rules cannot be stored (empty = fine): an unknown material, a
// gauge that material is not stocked in, a bend radius under what the material
// takes, a K outside (0, 1). Checked at the RPC boundary and at load, so no
// feature has to wonder.
[[nodiscard]] std::string Validate(const SheetMetalRules& rules);

} // namespace sheet
