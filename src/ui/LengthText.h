// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A small distance as a person reads it in a tool window: the document's
// length unit, three significant figures, and a floor -- "under 0.001 mm"
// -- under which a number is the arithmetic's, not the part's ("2.31e-13
// mm" is a fit that landed exactly, and says so in a way nobody reads).

#include "core/Units.h"

#include <cmath>
#include <string>

namespace ui {

// The floor, in mm: a micrometre.
inline constexpr double kReadableFloorMm = 1e-3;

// "0.0458 mm", "12.3 mm", "under 0.001 mm"; `sign` puts a + on a positive
// one (a deviation's side).
inline std::string ReadableLength(double mm, const units::System& sys,
                                  bool sign = false) {
  const std::string suffix = std::string(" ") + units::ToString(sys.length);
  const double per = units::MmPerLength(sys.length);
  if (!std::isfinite(mm))
    return "-";
  if (std::abs(mm) < kReadableFloorMm)
    return "under " + units::FormatNumber(kReadableFloorMm / per, 3) + suffix;
  std::string n = units::FormatNumber(mm / per, 3);
  if (sign && mm > 0.0)
    n = "+" + n;
  return n + suffix;
}

} // namespace ui
