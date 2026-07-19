#include "core/ThreadStandards.h"

namespace threads {

double MinorDiameter(const Standard& s) {
  return s.majorDia - 1.0825 * s.pitch;
}

double TapDrill(const Standard& s) { return s.majorDia - s.pitch; }

const std::vector<Standard>& All() {
  // ISO 262 preferred sizes: coarse series, then the common fine pitches.
  static const std::vector<Standard> kSeries = {
      {"M3", 3.0, 0.5},      {"M4", 4.0, 0.7},      {"M5", 5.0, 0.8},
      {"M6", 6.0, 1.0},      {"M8", 8.0, 1.25},     {"M10", 10.0, 1.5},
      {"M12", 12.0, 1.75},   {"M16", 16.0, 2.0},    {"M20", 20.0, 2.5},
      {"M24", 24.0, 3.0},    {"M30", 30.0, 3.5},
      {"M8x1", 8.0, 1.0},    {"M10x1.25", 10.0, 1.25}, {"M10x1", 10.0, 1.0},
      {"M12x1.5", 12.0, 1.5}, {"M12x1.25", 12.0, 1.25},
      {"M16x1.5", 16.0, 1.5}, {"M20x1.5", 20.0, 1.5},
  };
  return kSeries;
}

std::optional<Standard> Lookup(const std::string& designation) {
  for (const Standard& s : All())
    if (s.designation == designation)
      return s;
  return std::nullopt;
}

} // namespace threads
