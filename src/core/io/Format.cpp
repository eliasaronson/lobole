#include "core/io/Format.h"

#include "core/Angles.h"

#include <cmath>
#include <cstdio>

namespace core {

std::string FormatLength(double mm, const units::System& sys) {
  double v = mm / units::MmPerLength(sys.length);
  if (std::abs(v) < 5e-7)
    v = 0.0; // clamp -0 / tiny noise to a clean 0
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3g", v);
  return buf;
}

std::string FormatAngle(double radians, const units::System& sys) {
  char buf[32];
  if (sys.angle == units::Angle::Radian) {
    double r = radians;
    if (std::abs(r) < 5e-5)
      r = 0.0;
    // Three decimals, not one: a tenth of a radian is 5.7 degrees, so the
    // degree precision would read as a rounded-off angle.
    std::snprintf(buf, sizeof(buf), "%.3f rad", r);
    return buf;
  }
  double deg = angles::ToDegrees(radians);
  if (std::abs(deg) < 5e-3)
    deg = 0.0;
  std::snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", deg); // UTF-8 degree sign
  return buf;
}

} // namespace core
