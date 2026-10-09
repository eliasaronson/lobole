// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

// ISO 286-1 limits and fits
// (docs/tolerances/README.md#iso-286-limits-and-fits): a tolerance CLASS
// ("H7", "g6", "js6") plus a nominal size gives the band that size may vary
// in, and a hole class paired with a shaft class gives the fit between them.
//
// Pure static data — unit tested, no I/O, and it touches no geometry. That is
// the point: unlike a clearance-hole fit, which picks a DIFFERENT diameter
// (parts::HoleDiameter — "M6 normal" means 6.6), a size tolerance leaves
// nominal alone. Ø20 H7 is still nominally 20; it just may not be under 20.000
// or over 20.021. So a tolerance edit rebuilds nothing.
//
// Every number here is MILLIMETRES. The tables are in micrometres, because
// that is how the standard publishes them, and are converted once at the
// boundary.
namespace fits {

// A permitted band, as SIGNED deviations from nominal. `lower <= upper`; both
// negative for a clearance shaft, both positive for an interference one,
// straddling zero for js/JS.
struct Band {
  double lower = 0.0; // mm
  double upper = 0.0; // mm
  int grade = 0;      // the IT number the class named
  double it = 0.0;    // that grade's width at this size == upper - lower
  [[nodiscard]] double Min(double nominal) const { return nominal + lower; }
  [[nodiscard]] double Max(double nominal) const { return nominal + upper; }
};

// What a hole/shaft pair does across their whole bands. Named the way a
// machinist names them, and decided by the bounds rather than by the letter:
// H7/k6 is a transition fit because its clearance range STRADDLES zero, not
// because "k" is on a list.
enum class FitKind { Clearance, Transition, Interference };
[[nodiscard]] std::string FitKindName(FitKind kind);
[[nodiscard]] std::optional<FitKind> FitKindFromName(const std::string& name);

struct Pair {
  Band hole;
  Band shaft;
  // Signed, mm, NEGATIVE means interference. Both are the honest extremes of
  // two independent bands: min = hole_min - shaft_max, max = hole_max -
  // shaft_min. This is worst case over the limits and nothing more — no
  // distribution, no correlation, and no account of form or position error.
  double minClearance = 0.0;
  double maxClearance = 0.0;
  FitKind kind = FitKind::Clearance;
};

// The band `cls` permits at `nominalMm`. Case says which side it is: an
// UPPERCASE letter is a hole ("H7"), lowercase a shaft ("g6").
//
// Refused, never extrapolated or rounded to a neighbour — `error` names what
// IS carried. A tolerance that is plausible and wrong is worse than none: it
// is the number a part gets made to.
[[nodiscard]] std::optional<Band>
Limits(double nominalMm, const std::string& cls, std::string* error);

// The fit `holeCls` and `shaftCls` make at one common nominal size. Refuses a
// hole class in the shaft slot (and vice versa) rather than solving the
// mirror image of what was asked, because the two answers differ and both
// look right.
[[nodiscard]] std::optional<Pair> Fit(double nominalMm,
                                      const std::string& holeCls,
                                      const std::string& shaftCls,
                                      std::string* error);

// What the tables carry, for refusal messages and pickers: the list IS the
// scope, so a picker built from it cannot offer a class Limits would refuse.
[[nodiscard]] std::vector<std::string> ShaftLetters();
[[nodiscard]] std::vector<std::string> HoleLetters();
// The largest nominal size any of it is defined for (500 mm).
[[nodiscard]] double MaxNominal();

} // namespace fits
