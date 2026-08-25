#include "core/Units.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace units {

namespace {

// Storage (mm) per display unit.
double MmPer(Length u) {
  switch (u) {
  case Length::Millimetre:
    return 1.0;
  case Length::Centimetre:
    return 10.0;
  case Length::Metre:
    return 1000.0;
  case Length::Inch:
    return 25.4;
  }
  return 1.0;
}

// Storage (grams) per display unit.
double GramsPer(Mass u) {
  switch (u) {
  case Mass::Gram:
    return 1.0;
  case Mass::Kilogram:
    return 1000.0;
  case Mass::Pound:
    return 453.59237;
  }
  return 1.0;
}

std::string Lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// Group the integer part in threes, from the right.
std::string GroupThousands(const std::string& digits) {
  std::string out;
  const size_t n = digits.size();
  for (size_t i = 0; i < n; i++) {
    if (i > 0 && (n - i) % 3 == 0)
      out += ' ';
    out += digits[i];
  }
  return out;
}

} // namespace

const char* ToString(Length u) {
  switch (u) {
  case Length::Millimetre:
    return "mm";
  case Length::Centimetre:
    return "cm";
  case Length::Metre:
    return "m";
  case Length::Inch:
    return "in";
  }
  return "mm";
}

const char* ToString(Mass u) {
  switch (u) {
  case Mass::Gram:
    return "g";
  case Mass::Kilogram:
    return "kg";
  case Mass::Pound:
    return "lb";
  }
  return "kg";
}

const char* ToString(Angle u) {
  switch (u) {
  case Angle::Degree:
    return "deg";
  case Angle::Radian:
    return "rad";
  }
  return "deg";
}

bool LengthFromString(const std::string& s, Length& out) {
  const std::string k = Lower(s);
  if (k == "mm" || k == "millimetre" || k == "millimeter")
    out = Length::Millimetre;
  else if (k == "cm" || k == "centimetre" || k == "centimeter")
    out = Length::Centimetre;
  else if (k == "m" || k == "metre" || k == "meter")
    out = Length::Metre;
  else if (k == "in" || k == "inch")
    out = Length::Inch;
  else
    return false;
  return true;
}

bool MassFromString(const std::string& s, Mass& out) {
  const std::string k = Lower(s);
  if (k == "g" || k == "gram")
    out = Mass::Gram;
  else if (k == "kg" || k == "kilogram")
    out = Mass::Kilogram;
  else if (k == "lb" || k == "pound")
    out = Mass::Pound;
  else
    return false;
  return true;
}

bool AngleFromString(const std::string& s, Angle& out) {
  const std::string k = Lower(s);
  if (k == "deg" || k == "degree" || k == "degrees")
    out = Angle::Degree;
  else if (k == "rad" || k == "radian" || k == "radians")
    out = Angle::Radian;
  else
    return false;
  return true;
}

std::vector<const char*> LengthNames() { return {"mm", "cm", "m", "in"}; }
std::vector<const char*> MassNames() { return {"g", "kg", "lb"}; }
std::vector<const char*> AngleNames() { return {"deg", "rad"}; }

std::string FormatNumber(double value, int sigFigs) {
  if (!std::isfinite(value))
    return "-";
  if (sigFigs < 1)
    sigFigs = 1;
  if (value == 0.0)
    return "0";

  // Round to `sigFigs` significant figures, then print with as many decimals
  // as those figures actually need. %g would do the rounding but switches to
  // exponent form without asking, and "1.30529e+06 mm³" helps nobody.
  const double mag = std::floor(std::log10(std::fabs(value)));
  const int decimals = sigFigs - 1 - static_cast<int>(mag);
  double rounded = value;
  // A value with MORE integer digits than we want figures has to be rounded
  // before printing: "%.*f" cannot take a negative precision, so `%.0f` on
  // 1305294.05 rounds to the unit and keeps seven figures, not six.
  if (decimals < 0) {
    const double step = std::pow(10.0, -decimals);
    rounded = std::round(value / step) * step;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", std::max(0, decimals), rounded);
  std::string s = buf;

  // Trailing zeros carry no information and cost column width.
  if (s.find('.') != std::string::npos) {
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.')
      s.pop_back();
  }

  const bool negative = !s.empty() && s.front() == '-';
  if (negative)
    s.erase(0, 1);
  const size_t dot = s.find('.');
  const std::string intPart = s.substr(0, dot);
  const std::string frac = dot == std::string::npos ? "" : s.substr(dot);
  // Only the integer side is grouped; a grouped fraction is not a convention
  // anyone reads.
  std::string out = GroupThousands(intPart) + frac;
  return negative ? "-" + out : out;
}

std::string FormatLength(double mm, const System& sys) {
  return FormatNumber(mm / MmPer(sys.length)) + " " + ToString(sys.length);
}

std::string FormatArea(double mm2, const System& sys) {
  // The length unit SQUARED, for the same reason FormatVolume cubes it: a
  // face area belongs in the document's units, and switching between mm² and
  // cm² per value would make two areas incomparable.
  const double per = MmPer(sys.length);
  return FormatNumber(mm2 / (per * per)) + " " + AreaSuffix(sys);
}

std::string FormatVolume(double mm3, const System& sys) {
  // The length unit CUBED, fixed — not whichever of mm³/cm³/L happens to make
  // this one value short. A column of volumes has to be comparable down the
  // column, which is exactly what a per-value unit destroys.
  const double per = MmPer(sys.length);
  return FormatNumber(mm3 / (per * per * per)) + " " + VolumeSuffix(sys);
}

std::string FormatMass(double grams, const System& sys) {
  return FormatNumber(grams / GramsPer(sys.mass)) + " " + ToString(sys.mass);
}

std::string FormatAngle(double degrees, const System& sys) {
  if (sys.angle == Angle::Radian)
    return FormatNumber(degrees * 3.14159265358979323846 / 180.0) + " rad";
  return FormatNumber(degrees) + "\xc2\xb0"; // U+00B0, in the UI font's range
}

std::string LengthSuffix(const System& sys) { return ToString(sys.length); }

std::string AreaSuffix(const System& sys) {
  return std::string(ToString(sys.length)) + "\xc2\xb2"; // U+00B2
}

std::string VolumeSuffix(const System& sys) {
  return std::string(ToString(sys.length)) + "\xc2\xb3"; // U+00B3
}

std::string MassSuffix(const System& sys) { return ToString(sys.mass); }

} // namespace units
