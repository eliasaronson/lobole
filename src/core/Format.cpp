#include "core/Format.h"

#include <cmath>
#include <cstdio>

namespace core {

std::string FormatLength(double mm) {
  if (std::abs(mm) < 5e-7) mm = 0.0;  // clamp -0 / tiny noise to a clean 0
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3g", mm);
  return buf;
}

std::string FormatAngle(double radians) {
  double deg = radians * 180.0 / M_PI;
  if (std::abs(deg) < 5e-3) deg = 0.0;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", deg);  // UTF-8 degree sign
  return buf;
}

}  // namespace core
