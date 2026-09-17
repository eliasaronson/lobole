#include "core/features/EdgeFlangeFeature.h"

#include "core/Angles.h"
#include "core/sheet/Flange.h"

#include <cmath>

std::string FlangePositionName(FlangePosition p) {
  switch (p) {
  case FlangePosition::MaterialInside:
    return "material_inside";
  case FlangePosition::MaterialOutside:
    return "material_outside";
  case FlangePosition::BendOutside:
    return "bend_outside";
  }
  return "material_inside";
}

bool FlangePositionFromString(const std::string& name, FlangePosition& out) {
  if (name == "material_inside") {
    out = FlangePosition::MaterialInside;
    return true;
  }
  if (name == "material_outside") {
    out = FlangePosition::MaterialOutside;
    return true;
  }
  if (name == "bend_outside") {
    out = FlangePosition::BendOutside;
    return true;
  }
  return false;
}

const std::string& FlangePositionList() {
  static const std::string list =
      "material_inside, material_outside, bend_outside";
  return list;
}

std::string FlangeLengthModeName(FlangeLengthMode m) {
  switch (m) {
  case FlangeLengthMode::Flat:
    return "flat";
  case FlangeLengthMode::Outside:
    return "outside";
  case FlangeLengthMode::Inside:
    return "inside";
  }
  return "flat";
}

bool FlangeLengthModeFromString(const std::string& name,
                                FlangeLengthMode& out) {
  if (name == "flat") {
    out = FlangeLengthMode::Flat;
    return true;
  }
  if (name == "outside") {
    out = FlangeLengthMode::Outside;
    return true;
  }
  if (name == "inside") {
    out = FlangeLengthMode::Inside;
    return true;
  }
  return false;
}

const std::string& FlangeLengthModeList() {
  static const std::string list = "flat, outside, inside";
  return list;
}

EdgeFlangeFeature::EdgeFlangeFeature(const std::string& name,
                                     std::vector<ElementRef> edges,
                                     double length, double angleDeg)
    : edges(std::move(edges)), length(length), angleDeg(angleDeg) {
  m_name = name;
}

bool EdgeFlangeFeature::Execute(RegenContext& ctx) {
  if (ctx.sheetMetal == nullptr) {
    SetError("this part has no sheet-metal rules - set the material and gauge "
             "first (set_sheet_metal_rules)");
    return false;
  }
  if (edges.empty()) {
    SetError("no edges picked to flange");
    return false;
  }
  const sheet::SheetMetalRules& rules = *ctx.sheetMetal;
  const double t = rules.thickness;
  const double theta = angles::ToRadians(angleDeg);
  if (theta <= 1e-6 || theta > angles::kPi + 1e-9) {
    SetError("bend angle must be above 0 and at most 180 degrees (got " +
             std::to_string(angleDeg) + ")");
    return false;
  }
  const double R = radius > 0.0 ? radius : rules.EffectiveBendRadius();
  const double minR = sheet::MinBendRadius(rules.material, t);
  if (R < minR - 1e-9) {
    SetError("bend radius " + std::to_string(R) + " mm is tighter than " +
             rules.material + " takes at " + rules.gauge + " (minimum " +
             std::to_string(minR) + " mm)");
    return false;
  }

  // The setback from the picked edge to the bend's tangent line — the whole
  // of what `position` means.
  const double halfTan = std::tan(theta / 2.0);
  double setback = 0.0;
  switch (position) {
  case FlangePosition::MaterialInside:
    setback = (R + t) * halfTan;
    break;
  case FlangePosition::MaterialOutside:
    setback = R * halfTan;
    break;
  case FlangePosition::BendOutside:
    setback = 0.0;
    break;
  }

  // `length` may be stated to either mold line; the wall itself is what gets
  // built, so convert once here.
  double wall = length;
  if (lengthMode == FlangeLengthMode::Outside)
    wall = length - (R + t) * halfTan;
  else if (lengthMode == FlangeLengthMode::Inside)
    wall = length - R * halfTan;
  if (wall <= 1e-9) {
    SetError("flange length " + std::to_string(length) + " mm (" +
             FlangeLengthModeName(lengthMode) +
             ") leaves no wall past the bend - at radius " + std::to_string(R) +
             " mm the bend alone needs more than that");
    return false;
  }

  const sheet::ReliefStyle relief = reliefStyle.value_or(rules.reliefStyle);
  const double reliefW = rules.EffectiveReliefWidth();
  const double reliefD = rules.EffectiveReliefDepth(R);

  sheet::FlangeSpec spec;
  spec.radius = R;
  spec.angle = theta;
  spec.wall = wall;
  spec.setback = setback;
  spec.offsetStart = offsetStart;
  spec.offsetEnd = offsetEnd;
  spec.relief = relief;
  spec.reliefWidth = reliefW;
  spec.reliefDepth = reliefD;
  spec.closedCorners = closedCorners;
  // A mitred corner leaves a gap a tenth of the sheet wide: enough that the
  // two walls are two walls — touching ones fuse into one solid, and the flat
  // pattern cannot say which bend the corner belongs to.
  const std::string why =
      sheet::ApplyFlanges(ctx.bodies, edges, spec, rules, GetId(), t / 10.0);
  if (!why.empty()) {
    SetError(why);
    return false;
  }
  return true;
}
