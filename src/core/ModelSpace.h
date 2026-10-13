// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cmath>

// How far from the origin a model reaches, in mm: a kilometre.
//
// Not a taste: OpenCASCADE's confusion tolerance is 1e-7 mm, a double carries
// about 16 significant digits, and at 1e6 mm the two are 13 orders apart --
// past about 1e9 a coordinate can no longer tell points 1e-7 apart, and an
// algorithm that relies on it loops or crashes instead of failing. A Pattern
// spaced 1e308, a Hole 2^64 across and a Shell 1e308 thick each hung or
// crashed the app (tests/hostile_params_smoke.py); a feature refuses a size
// or a reach past this instead, naming it.
inline constexpr double kModelSpaceMm = 1e6;

inline bool InModelSpace(double mm) {
  return std::isfinite(mm) && std::abs(mm) <= kModelSpaceMm;
}
