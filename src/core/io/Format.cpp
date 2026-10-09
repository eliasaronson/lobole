// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/Format.h"

#include "core/Angles.h"

#include <cmath>
#include <cstdio>

namespace core {

namespace {

int LengthDecimals(units::Length l) {
  switch (l) {
  case units::Length::Millimetre:
    return 2;
  case units::Length::Centimetre:
    return 3;
  case units::Length::Metre:
    return 5;
  case units::Length::Inch:
    return 3;
  }
  return 2;
}

std::string Fixed(double mm, const units::System& sys, int decimals) {
  double v = mm / units::MmPerLength(sys.length);
  const double quantum = 0.5 * std::pow(10.0, -decimals);
  if (std::abs(v) < quantum)
    v = 0.0; // clamp -0 / tiny noise to a clean 0
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0')
      s.pop_back();
    if (s.back() == '.')
      s.pop_back();
  }
  return s == "-0" ? "0" : s;
}

} // namespace

std::string FormatLength(double mm, const units::System& sys) {
  return Fixed(mm, sys, LengthDecimals(sys.length));
}

std::string FormatDeviation(double mm, const units::System& sys) {
  return Fixed(mm, sys, LengthDecimals(sys.length) + 1);
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

std::string UnitsLabel(const units::System& sys) {
  std::string out = units::ToString(sys.length);
  if (sys.angle == units::Angle::Radian)
    out += " / rad";
  return out;
}

} // namespace core
