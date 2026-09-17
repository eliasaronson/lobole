#include "core/SheetMetal.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace sheet {
namespace {

// The materials this table carries bend data for. A slug outside it is refused
// rather than bent with a neighbour's numbers: titanium at aluminium's minimum
// radius cracks, and the part looks fine until it is made.
const std::vector<std::string>& SheetMaterials() {
  static const std::vector<std::string> v = {
      "aluminum", "steel", "stainless_steel", "brass", "copper"};
  return v;
}

// Metric stock, every material. A shop outside the US orders by thickness and
// the gauge column is noise to them.
const std::vector<double>& MetricThicknesses() {
  static const std::vector<double> v = {5.0, 4.0, 3.0, 2.5, 2.0,
                                        1.5, 1.2, 1.0, 0.8, 0.5};
  return v;
}

// US gauge is three different systems wearing one name, which is exactly the
// trap: 16ga steel is 1.519 mm and 16ga aluminium is 1.291 mm, a 15% error in
// every blank if one table is used for both.
//
// steel: Manufacturers' Standard Gauge for sheet steel.
// stainless_steel: US Standard Gauge for stainless sheet.
// aluminum/brass/copper: Brown & Sharpe (AWG).
struct GaugeRow {
  const char* designation;
  double mm;
};

const std::vector<GaugeRow>& SteelGauges() {
  static const std::vector<GaugeRow> v = {
      {"7ga", 4.554},  {"8ga", 4.176},  {"10ga", 3.416}, {"11ga", 3.038},
      {"12ga", 2.657}, {"14ga", 1.897}, {"16ga", 1.519}, {"18ga", 1.214},
      {"20ga", 0.912}, {"22ga", 0.759}, {"24ga", 0.607}};
  return v;
}

const std::vector<GaugeRow>& StainlessGauges() {
  static const std::vector<GaugeRow> v = {
      {"7ga", 4.763},  {"8ga", 4.365},  {"10ga", 3.571}, {"11ga", 3.175},
      {"12ga", 2.778}, {"14ga", 1.984}, {"16ga", 1.587}, {"18ga", 1.270},
      {"20ga", 0.952}, {"22ga", 0.794}, {"24ga", 0.635}};
  return v;
}

const std::vector<GaugeRow>& BrownSharpeGauges() {
  static const std::vector<GaugeRow> v = {
      {"6ga", 4.115},  {"8ga", 3.264},  {"10ga", 2.588},
      {"12ga", 2.053}, {"14ga", 1.628}, {"16ga", 1.291},
      {"18ga", 1.024}, {"20ga", 0.812}, {"22ga", 0.644}};
  return v;
}

const std::vector<GaugeRow>& GaugesForMaterial(const std::string& material) {
  if (material == "steel")
    return SteelGauges();
  if (material == "stainless_steel")
    return StainlessGauges();
  return BrownSharpeGauges(); // aluminum, brass, copper
}

std::string MetricDesignation(double mm) {
  std::ostringstream s;
  s << mm << "mm";
  return s.str();
}

// How tight this material bends, as a multiple of thickness. Conservative
// where the alloy matters: "aluminum" here is a 5052-class sheet, and a
// 6061-T6 part wants 3t — which the user sets explicitly rather than
// discovering at the brake.
double MinRadiusMultiple(const std::string& material) {
  if (material == "steel" || material == "brass" || material == "copper")
    return 1.0;
  return 1.5; // aluminum, stainless_steel
}

// The three-band K chart, by material hardness class.
struct KBands {
  double tight;  // R < 2t
  double medium; // 2t <= R < 4t
  double loose;  // R >= 4t
};

KBands BandsFor(const std::string& material) {
  if (material == "brass" || material == "copper")
    return {0.35, 0.41, 0.45}; // soft
  if (material == "stainless_steel")
    return {0.45, 0.50, 0.50}; // hard
  return {0.41, 0.46, 0.50};   // aluminum, steel
}

const char* BandName(double radius, double t) {
  if (t <= 0.0)
    return " R/t<2";
  const double ratio = radius / t;
  if (ratio < 2.0)
    return " R/t<2";
  if (ratio < 4.0)
    return " 2<=R/t<4";
  return " R/t>=4";
}

} // namespace

std::string ReliefStyleName(ReliefStyle style) {
  switch (style) {
  case ReliefStyle::None:
    return "none";
  case ReliefStyle::Rectangular:
    return "rectangular";
  case ReliefStyle::Obround:
    return "obround";
  case ReliefStyle::Tear:
    return "tear";
  }
  return "rectangular";
}

bool ReliefStyleFromString(const std::string& name, ReliefStyle& out) {
  if (name == "none") {
    out = ReliefStyle::None;
    return true;
  }
  if (name == "rectangular") {
    out = ReliefStyle::Rectangular;
    return true;
  }
  if (name == "obround") {
    out = ReliefStyle::Obround;
    return true;
  }
  if (name == "tear") {
    out = ReliefStyle::Tear;
    return true;
  }
  return false;
}

const std::string& ReliefStyleList() {
  static const std::string list = "none, rectangular, obround, tear";
  return list;
}

const std::vector<Gauge>& GaugeTable() {
  static const std::vector<Gauge> table = [] {
    std::vector<Gauge> v;
    for (const std::string& material : SheetMaterials()) {
      for (const GaugeRow& row : GaugesForMaterial(material))
        v.push_back({material, row.designation, row.mm});
      for (double mm : MetricThicknesses())
        v.push_back({material, MetricDesignation(mm), mm});
    }
    return v;
  }();
  return table;
}

std::vector<std::string> GaugesFor(const std::string& material) {
  std::vector<std::string> v;
  for (const Gauge& g : GaugeTable()) {
    if (g.material == material)
      v.push_back(g.designation);
  }
  return v;
}

std::optional<double> LookupGauge(const std::string& material,
                                  const std::string& designation,
                                  std::string* error) {
  const auto& materials = SheetMaterials();
  if (std::find(materials.begin(), materials.end(), material) ==
      materials.end()) {
    if (error) {
      std::string list;
      for (const std::string& m : materials)
        list += (list.empty() ? "" : ", ") + m;
      *error = "no sheet stock for material '" + material +
               "' - sheet data is carried for: " + list;
    }
    return std::nullopt;
  }
  for (const Gauge& g : GaugeTable()) {
    if (g.material == material && g.designation == designation)
      return g.thickness;
  }
  if (error) {
    std::string list;
    for (const std::string& d : GaugesFor(material))
      list += (list.empty() ? "" : ", ") + d;
    *error = "'" + designation + "' is not stocked in " + material +
             " - stocked: " + list;
  }
  return std::nullopt;
}

double MinBendRadius(const std::string& material, double t) {
  return MinRadiusMultiple(material) * t;
}

double KFactor(const std::string& material, double radius, double t) {
  const KBands bands = BandsFor(material);
  if (t <= 0.0)
    return bands.tight;
  const double ratio = radius / t;
  if (ratio < 2.0)
    return bands.tight;
  if (ratio < 4.0)
    return bands.medium;
  return bands.loose;
}

std::string KFactorSource(const std::string& material, double radius,
                          double t) {
  return "table:" + material + BandName(radius, t);
}

double BendAllowance(double angleRad, double radius, double k, double t) {
  return std::abs(angleRad) * (radius + k * t);
}

double BendDeduction(double angleRad, double radius, double k, double t) {
  const double a = std::abs(angleRad);
  // The outside setback of both legs, less what the bend actually eats. At
  // 180 degrees tan(a/2) runs away, which is real: a hem is measured from its
  // allowance, never from its deduction.
  return 2.0 * (radius + t) * std::tan(a / 2.0) -
         BendAllowance(a, radius, k, t);
}

double SheetMetalRules::EffectiveBendRadius() const {
  if (bendRadius > 0.0)
    return bendRadius;
  return MinBendRadius(material, thickness);
}

double SheetMetalRules::EffectiveKFactor(double radius) const {
  if (kFactor > 0.0)
    return kFactor;
  return KFactor(material, radius, thickness);
}

std::string SheetMetalRules::KSource(double radius) const {
  if (kFactor > 0.0)
    return "rules";
  return KFactorSource(material, radius, thickness);
}

double SheetMetalRules::EffectiveReliefWidth() const {
  if (reliefWidth > 0.0)
    return reliefWidth;
  // Wide enough that the die does not drag the panel edge into the relief,
  // and never under a millimetre, which is about what a laser leaves clean.
  return std::max(1.5 * thickness, 1.0);
}

double SheetMetalRules::EffectiveReliefDepth(double radius) const {
  if (reliefDepth > 0.0)
    return reliefDepth;
  // Past the tangent line, or the bend tears from the corner of the cut.
  return radius + thickness;
}

std::string Validate(const SheetMetalRules& rules) {
  std::string error;
  const std::optional<double> t =
      LookupGauge(rules.material, rules.gauge, &error);
  if (!t)
    return error;
  if (std::abs(*t - rules.thickness) > 1e-6) {
    return "thickness " + std::to_string(rules.thickness) +
           " mm does not match " + rules.gauge + " " + rules.material + " (" +
           std::to_string(*t) + " mm)";
  }
  if (rules.bendRadius < 0.0)
    return "bend radius cannot be negative";
  if (rules.bendRadius > 0.0) {
    const double minR = MinBendRadius(rules.material, rules.thickness);
    if (rules.bendRadius < minR - 1e-9) {
      return "bend radius " + std::to_string(rules.bendRadius) +
             " mm is tighter than " + rules.material + " takes at " +
             rules.gauge + " (minimum " + std::to_string(minR) + " mm)";
    }
  }
  if (rules.kFactor < 0.0 || rules.kFactor >= 1.0) {
    return "K-factor must be inside (0, 1) - it is where the neutral axis "
           "sits as a fraction of thickness";
  }
  if (rules.reliefWidth < 0.0 || rules.reliefDepth < 0.0)
    return "relief width and depth cannot be negative";
  return {};
}

} // namespace sheet
