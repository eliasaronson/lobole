#include "core/tube/Rules.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace tube {
namespace {

// The stocked sizes, OD x wall in mm. Round structural tube, the sizes a
// frame is actually built from: the 25.4 x 2.4 and 25.4 x 1.65 a Formula
// Student rulebook names for a main hoop and its bracing, the 19.05 and 22
// ranges a smaller member takes, and the larger rounds a chassis or a roll
// cage uses. Metric and imperial-derived both, because both are what real
// orders say, and which a shop stocks is not this table's decision.
struct SizeRow {
  double od;
  double wall;
};

const std::vector<SizeRow>& StructuralRounds() {
  static const std::vector<SizeRow> rows = {
      {38.10, 2.60}, {38.10, 1.65}, {31.75, 2.40}, {31.75, 1.65}, {30.00, 2.00},
      {28.00, 2.00}, {25.40, 2.60}, {25.40, 2.40}, {25.40, 1.65}, {25.40, 1.20},
      {25.00, 2.50}, {25.00, 1.50}, {22.00, 2.00}, {22.00, 1.50}, {20.00, 2.00},
      {20.00, 1.50}, {19.05, 1.65}, {19.05, 1.20}, {16.00, 1.50}, {16.00, 1.00},
      {12.00, 1.50}, {12.00, 1.00}};
  return rows;
}

const std::vector<std::string>& TubeMaterials() {
  static const std::vector<std::string> m = {"steel", "stainless_steel",
                                             "aluminum", "titanium"};
  return m;
}

// Trailing zeros off a millimetre number, so 25.4 x 2.4 is "25.4x2.4" and not
// "25.400000x2.400000". The designation is what a shop is quoted, and it has
// to read like one.
std::string Trim(double mm) {
  std::ostringstream os;
  os.precision(3);
  os << std::fixed << mm;
  std::string s = os.str();
  while (!s.empty() && s.back() == '0')
    s.pop_back();
  if (!s.empty() && s.back() == '.')
    s.pop_back();
  return s;
}

std::string Designation(double od, double wall) {
  return Trim(od) + "x" + Trim(wall);
}

// The inverse, for refusals only: "25.4x2.45" -> 25.4, 2.45. False when it is
// not of that shape at all, and then the refusal cannot say which sizes are
// near and lists the table's own order instead.
bool ParseDesignation(const std::string& s, double& od, double& wall) {
  const size_t x = s.find('x');
  if (x == std::string::npos || x == 0 || x + 1 >= s.size())
    return false;
  try {
    size_t used = 0;
    od = std::stod(s.substr(0, x), &used);
    if (used != x)
      return false;
    const std::string rest = s.substr(x + 1);
    wall = std::stod(rest, &used);
    return used == rest.size();
  } catch (const std::exception&) {
    return false;
  }
}

// How much more former each material wants than mild steel, for the same
// D/t. Stainless work-hardens and springs back; titanium more so again;
// aluminium tears its outer fibre sooner than steel of the same wall.
double MaterialFactor(const std::string& material) {
  if (material == "stainless_steel")
    return 1.2;
  if (material == "aluminum")
    return 1.15;
  if (material == "titanium")
    return 1.5;
  return 1.0; // steel, and anything unrecognised: the plain rule
}

// The D/t band, as a multiple of OD. A thin wall has nothing holding the
// inside of the bend up.
double DiameterBand(double od, double wall, std::string* label) {
  const double dOverT = wall > 0.0 ? od / wall : 0.0;
  if (dOverT < 15.0) {
    if (label != nullptr)
      *label = "D/t<15";
    return 2.0;
  }
  if (dOverT < 30.0) {
    if (label != nullptr)
      *label = "15<=D/t<30";
    return 3.0;
  }
  if (label != nullptr)
    *label = "D/t>=30";
  return 4.0;
}

} // namespace

const std::vector<Size>& SizeTable() {
  static const std::vector<Size> table = [] {
    std::vector<Size> v;
    for (const std::string& material : TubeMaterials()) {
      for (const SizeRow& row : StructuralRounds()) {
        v.push_back(
            {material, Designation(row.od, row.wall), row.od, row.wall});
      }
    }
    return v;
  }();
  return table;
}

const std::vector<std::string>& Materials() { return TubeMaterials(); }

std::vector<std::string> SizesFor(const std::string& material) {
  std::vector<std::string> v;
  for (const Size& s : SizeTable()) {
    if (s.material == material)
      v.push_back(s.designation);
  }
  return v;
}

std::optional<Size> LookupSize(const std::string& material,
                               const std::string& designation,
                               std::string* error) {
  const auto& materials = TubeMaterials();
  if (std::find(materials.begin(), materials.end(), material) ==
      materials.end()) {
    if (error != nullptr) {
      std::string names;
      for (const std::string& m : materials)
        names += names.empty() ? m : ", " + m;
      *error =
          "no tube data for material '" + material + "'; carried: " + names;
    }
    return std::nullopt;
  }
  for (const Size& s : SizeTable()) {
    if (s.material == material && s.designation == designation)
      return s;
  }
  if (error != nullptr) {
    // Name the NEAREST stocked sizes rather than rounding to one: a wall
    // 0.2 mm off the one asked for is a different tube, and substituting it
    // is how a frame comes back lighter than its rulebook allows. Nearest,
    // not first — a list that opens with the biggest tube in the table is a
    // refusal nobody can act on.
    double od = 0.0, wall = 0.0;
    const bool parsed = ParseDesignation(designation, od, wall);
    std::vector<Size> candidates;
    for (const Size& s : SizeTable()) {
      if (s.material == material)
        candidates.push_back(s);
    }
    if (parsed) {
      std::sort(candidates.begin(), candidates.end(),
                [&](const Size& a, const Size& b) {
                  const auto d = [&](const Size& s) {
                    const double dd = s.outerDiameter - od;
                    const double dw = s.wallThickness - wall;
                    return dd * dd + dw * dw;
                  };
                  return d(a) < d(b);
                });
    }
    std::string names;
    for (size_t i = 0; i < candidates.size() && i < 4; i++) {
      names += names.empty() ? candidates[i].designation
                             : ", " + candidates[i].designation;
    }
    *error = "'" + designation + "' is not a stocked size in " + material +
             " (OD x wall in mm); nearest stocked: " + names +
             (candidates.size() > 4 ? ", ..." : "");
  }
  return std::nullopt;
}

double MinBendRadius(const std::string& material, double od, double wall) {
  return DiameterBand(od, wall, nullptr) * MaterialFactor(material) * od;
}

std::string MinBendRadiusSource(const std::string& material, double od,
                                double wall) {
  std::string band;
  DiameterBand(od, wall, &band);
  return "table:" + material + " " + band;
}

double WallAfterBend(double od, double wall, double radius) {
  const double outer = radius + od / 2.0;
  if (outer <= 0.0)
    return wall;
  return wall * radius / outer;
}

double WallThinning(double od, double wall, double radius) {
  if (wall <= 0.0)
    return 0.0;
  return 1.0 - WallAfterBend(od, wall, radius) / wall;
}

double BendArcLength(double angleRad, double radius) {
  return std::abs(angleRad) * radius;
}

double TubeRules::EffectiveMinStraight() const {
  return minStraight > 0.0 ? minStraight : 2.0 * outerDiameter;
}

double TubeRules::EffectiveTailLength() const {
  return tailLength > 0.0 ? tailLength : 1.5 * outerDiameter;
}

double TubeRules::EffectiveMaxThinning() const {
  return maxThinning > 0.0 ? maxThinning : 0.25;
}

std::optional<double> TubeRules::FormerFor(double wanted) const {
  if (formerRadii.empty())
    return wanted;
  std::optional<double> best;
  for (double r : formerRadii) {
    if (r + 1e-9 >= wanted && (!best.has_value() || r < *best))
      best = r;
  }
  return best;
}

std::string Validate(const TubeRules& rules) {
  if (rules.material.empty() && rules.size.empty())
    return {};
  std::string err;
  const auto found = LookupSize(rules.material, rules.size, &err);
  if (!found.has_value())
    return err;
  if (std::abs(found->outerDiameter - rules.outerDiameter) > 1e-6 ||
      std::abs(found->wallThickness - rules.wallThickness) > 1e-6) {
    return "size '" + rules.size + "' in " + rules.material + " is " +
           Designation(found->outerDiameter, found->wallThickness) +
           ", but the rules carry " +
           Designation(rules.outerDiameter, rules.wallThickness);
  }
  const double minR =
      MinBendRadius(rules.material, rules.outerDiameter, rules.wallThickness);
  for (double r : rules.formerRadii) {
    if (r <= 0.0)
      return "a former radius must be positive";
    if (r + 1e-9 < minR) {
      return "former radius " + Trim(r) + " mm is under what " + rules.size +
             " " + rules.material + " takes (" + Trim(minR) + " mm, " +
             MinBendRadiusSource(rules.material, rules.outerDiameter,
                                 rules.wallThickness) +
             ")";
    }
  }
  if (rules.minStraight < 0.0 || rules.tailLength < 0.0)
    return "a minimum straight and a tail length cannot be negative";
  if (rules.maxThinning < 0.0 || rules.maxThinning >= 1.0)
    return "max thinning is a FRACTION of the wall and must be under 1";
  return {};
}

} // namespace tube
