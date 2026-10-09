// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/parts/BearingTable.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>

namespace parts {

namespace {

// ISO 15:2017, "Rolling bearings — Radial bearings — Boundary dimensions,
// general plan", the tables for diameter series 8 (618), 9 (619), 0 (60),
// 2 (62) and 3 (63) at width series 1 / dimension series 18, 19, 10, 02 and
// 03 — the columns d, D, B and r_s min. Bores 3 to 30 mm. The small-bore
// designations follow the usual numbering: a bore under 10 mm is written
// out (608, 618/4), 00 is 10 mm, 01 is 12, 02 is 15, 03 is 17, and from 04
// the bore is five times the code.
//
// Cross-checked where a second source exists: bd_warehouse's
// single_row_deep_groove_ball_bearing_parameters.csv (the SKF catalogue,
// which follows ISO 15, for bores to 10 mm) and BOSL2's ball_bearing_info
// (d, D, B for 608 and the 6000/6200/6300 rows).
//
// A size is present only where the series is made in it: there is no 603 or
// 633 here, and asking for one is refused naming what is.
const std::vector<BearingRow> kRows = {
    // Dimension series 18
    {"618/3", "618", 3, 7, 2, 0.1},
    {"618/4", "618", 4, 9, 2.5, 0.1},
    {"618/5", "618", 5, 11, 3, 0.15},
    {"618/6", "618", 6, 13, 3.5, 0.15},
    {"618/7", "618", 7, 14, 3.5, 0.15},
    {"618/8", "618", 8, 16, 4, 0.2},
    {"618/9", "618", 9, 17, 4, 0.2},
    {"61800", "618", 10, 19, 5, 0.3},
    {"61801", "618", 12, 21, 5, 0.3},
    {"61802", "618", 15, 24, 5, 0.3},
    {"61803", "618", 17, 26, 5, 0.3},
    {"61804", "618", 20, 32, 7, 0.3},
    {"61805", "618", 25, 37, 7, 0.3},
    {"61806", "618", 30, 42, 7, 0.3},
    // Dimension series 19
    {"619/3", "619", 3, 8, 3, 0.15},
    {"619/4", "619", 4, 11, 4, 0.15},
    {"619/5", "619", 5, 13, 4, 0.2},
    {"619/6", "619", 6, 15, 5, 0.2},
    {"619/7", "619", 7, 17, 5, 0.3},
    {"619/8", "619", 8, 19, 6, 0.3},
    {"619/9", "619", 9, 20, 6, 0.3},
    {"61900", "619", 10, 22, 6, 0.3},
    {"61901", "619", 12, 24, 6, 0.3},
    {"61902", "619", 15, 28, 7, 0.3},
    {"61903", "619", 17, 30, 7, 0.3},
    {"61904", "619", 20, 37, 9, 0.3},
    {"61905", "619", 25, 42, 9, 0.3},
    {"61906", "619", 30, 47, 9, 0.3},
    // Dimension series 10
    {"604", "60", 4, 12, 4, 0.2},
    {"605", "60", 5, 14, 5, 0.2},
    {"606", "60", 6, 17, 6, 0.3},
    {"607", "60", 7, 19, 6, 0.3},
    {"608", "60", 8, 22, 7, 0.3},
    {"609", "60", 9, 24, 7, 0.3},
    {"6000", "60", 10, 26, 8, 0.3},
    {"6001", "60", 12, 28, 8, 0.3},
    {"6002", "60", 15, 32, 9, 0.3},
    {"6003", "60", 17, 35, 10, 0.3},
    {"6004", "60", 20, 42, 12, 0.6},
    {"6005", "60", 25, 47, 12, 0.6},
    {"6006", "60", 30, 55, 13, 1.0},
    // Dimension series 02
    {"623", "62", 3, 10, 4, 0.15},
    {"624", "62", 4, 13, 5, 0.2},
    {"625", "62", 5, 16, 5, 0.3},
    {"626", "62", 6, 19, 6, 0.3},
    {"627", "62", 7, 22, 7, 0.3},
    {"628", "62", 8, 24, 8, 0.3},
    {"629", "62", 9, 26, 8, 0.3},
    {"6200", "62", 10, 30, 9, 0.6},
    {"6201", "62", 12, 32, 10, 0.6},
    {"6202", "62", 15, 35, 11, 0.6},
    {"6203", "62", 17, 40, 12, 0.6},
    {"6204", "62", 20, 47, 14, 1.0},
    {"6205", "62", 25, 52, 15, 1.0},
    {"6206", "62", 30, 62, 16, 1.0},
    // Dimension series 03
    {"634", "63", 4, 16, 5, 0.3},
    {"635", "63", 5, 19, 6, 0.3},
    {"6300", "63", 10, 35, 11, 0.6},
    {"6301", "63", 12, 37, 12, 1.0},
    {"6302", "63", 15, 42, 13, 1.0},
    {"6303", "63", 17, 47, 14, 1.0},
    {"6304", "63", 20, 52, 15, 1.1},
    {"6305", "63", 25, 62, 17, 1.1},
    {"6306", "63", 30, 72, 19, 1.1},
};

// ISO 492:2014, radial bearings, normal tolerance class: t_Δdmp for the
// inner ring and t_ΔDmp for the outer, in micrometres, by nominal size
// "over .. up to and including". Only the steps the rows above reach.
struct Step {
  double over, upTo, lowerUm;
};
constexpr Step kBoreSteps[] = {
    {0.6, 2.5, -8}, {2.5, 10, -8}, {10, 18, -8}, {18, 30, -10}, {30, 50, -12}};
constexpr Step kOutsideSteps[] = {
    {2.5, 6, -8}, {6, 18, -8}, {18, 30, -9}, {30, 50, -11}, {50, 80, -13}};

std::string Upper(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::vector<const BearingRow*> Matching(double diameter, bool bore) {
  std::vector<const BearingRow*> out;
  for (const BearingRow& r : kRows)
    if (std::abs((bore ? r.d : r.D) - diameter) < 1e-3)
      out.push_back(&r);
  return out;
}

} // namespace

const std::vector<BearingRow>& AllBearings() { return kRows; }

std::string KnownBearingSeries() { return "618, 619, 60, 62, 63"; }

std::string KnownBearings() {
  std::string out;
  std::string series;
  for (const BearingRow& r : kRows) {
    if (series != r.series) {
      if (!out.empty())
        out += "; ";
      series = r.series;
      out += series + ": ";
    } else {
      out += ", ";
    }
    out += r.designation;
  }
  return out;
}

const BearingRow* FindBearing(const std::string& designation,
                              std::string* why) {
  for (const BearingRow& r : kRows)
    if (designation == r.designation)
      return &r;
  if (why == nullptr)
    return nullptr;
  // A shield or seal suffix on a designation the table has: say which.
  const std::string upper = Upper(designation);
  for (const BearingRow& r : kRows) {
    const std::string bare = r.designation;
    // Only a suffix that starts a new word: "62001" is not 6200 with a
    // suffix, it is a designation nobody makes.
    if (upper.size() > bare.size() &&
        upper.compare(0, bare.size(), bare) == 0 &&
        (upper[bare.size()] == '-' || upper[bare.size()] == ' ' ||
         std::isalpha(static_cast<unsigned char>(upper[bare.size()])))) {
      *why = "\"" + designation + "\" is " + bare +
             " with a suffix — a shield or seal (2Z, ZZ, 2RS) does not change "
             "an ISO 15 dimension, and this build does not model one, so the "
             "BOM would call a sealed bearing open. Ask for \"" +
             bare + "\" and note the variant on the order.";
      return nullptr;
    }
  }
  *why = "\"" + designation +
         "\" is not a deep-groove ball bearing this build carries (ISO 15, "
         "series " +
         KnownBearingSeries() + ", bores 3 to 30 mm) — it carries " +
         KnownBearings();
  return nullptr;
}

std::vector<const BearingRow*> BearingsForBore(double diameter) {
  return Matching(diameter, true);
}

std::vector<const BearingRow*> BearingsForOutside(double diameter) {
  return Matching(diameter, false);
}

std::vector<const BearingRow*> BearingsFrom(const std::string& series,
                                            double minBore, std::string* why) {
  std::vector<const BearingRow*> out;
  bool knownSeries = series.empty();
  double largest = 0.0;
  for (const BearingRow& r : kRows) {
    if (!series.empty() && series != r.series)
      continue;
    knownSeries = true;
    largest = std::max(largest, r.d);
    if (r.d >= minBore - 1e-9)
      out.push_back(&r);
  }
  if (!knownSeries) {
    if (why != nullptr)
      *why = "no bearing series \"" + series + "\" — this build carries " +
             KnownBearingSeries() + " (ISO 15 deep-groove ball bearings)";
    return {};
  }
  std::stable_sort(
      out.begin(), out.end(),
      [](const BearingRow* a, const BearingRow* b) { return a->d < b->d; });
  if (out.empty() && why != nullptr) {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "no bearing%s%s takes a %.3g mm bore: the largest bore "
                  "carried is %.3g mm",
                  series.empty() ? "" : " of series ", series.c_str(), minBore,
                  largest);
    *why = buf;
  }
  return out;
}

namespace {
std::optional<BearingBand> Band(const Step* begin, const Step* end,
                                double size) {
  for (const Step* s = begin; s != end; ++s)
    if (size > s->over && size <= s->upTo)
      return BearingBand{s->lowerUm / 1000.0, 0.0};
  return std::nullopt;
}
} // namespace

std::optional<BearingBand> BoreTolerance(double d) {
  return Band(std::begin(kBoreSteps), std::end(kBoreSteps), d);
}

std::optional<BearingBand> OutsideTolerance(double D) {
  return Band(std::begin(kOutsideSteps), std::end(kOutsideSteps), D);
}

namespace {

constexpr double kNone = std::numeric_limits<double>::quiet_NaN();

// SKF, "Rolling bearings", PUB BU/P1 17000 EN (2018), product table 1.1
// "Single row deep groove ball bearings", pp. 260-273: the columns
// "Abutment and fillet dimensions" — d_a min, d_a max, D_a max, r_a max, mm.
// bd_warehouse's SKF-derived open-bearing table (d_a, D_a, r_a; bores to
// 10 mm) agrees with every row it has, 619/8's open-row d_a min of 10 among
// them.
//
// SKF prints d_a max only for a CAPPED bearing: a shoulder above it reaches
// the shield or seal. The open row's column is a dash. The body is drawn
// shielded, so each row below is read ENTIRE from the 2Z row where SKF lists
// one, else from the capped row it lists (2RS1 for the 618/619 sizes from
// 20 mm), else from the open row with no d_a max. Only 619/8 differs between
// its open and 2Z rows (d_a min 10 against 9.5).
//
// Not here because SKF lists no such bearing: 618/3, 619/3, 605, 606. Those
// stay unverified rather than borrowing a neighbour's numbers.
const BearingAbutment kAbutments[] = {
    // designation, d_a min, d_a max, variant, D_a max, r_a max
    {"618/4", 4.6, kNone, nullptr, 8.4, 0.1},
    {"618/5", 5.8, kNone, nullptr, 10.2, 0.1},
    {"618/6", 6.8, kNone, nullptr, 12.2, 0.1},
    {"618/7", 7.8, kNone, nullptr, 13.2, 0.1},
    {"618/8", 9.4, kNone, nullptr, 14.6, 0.2},
    {"618/9", 10.4, kNone, nullptr, 15.6, 0.2},
    {"61800", 12, 12.5, "2Z", 17, 0.3},
    {"61801", 14, 14.7, "2Z", 19, 0.3},
    {"61802", 17, 17.8, "2Z", 22, 0.3},
    {"61803", 19, 19.6, "2Z", 24, 0.3},
    {"61804", 22, 23.6, "2RS1", 30, 0.3},
    {"61805", 27, 27.3, "2RS1", 35, 0.3},
    {"61806", 32, 32.5, "2RS1", 40, 0.3},
    {"619/4", 4.8, 5.8, "2Z", 10.2, 0.1},
    {"619/5", 6.4, 7.5, "2Z", 11.6, 0.2},
    {"619/6", 7.4, 8, "2Z", 13.6, 0.2},
    {"619/7", 9, 9.7, "2Z", 15, 0.3},
    {"619/8", 9.5, 9.8, "2Z", 17, 0.3},
    {"619/9", 11, 11.5, "2Z", 18, 0.3},
    {"61900", 12, 12.9, "2Z", 20, 0.3},
    {"61901", 14, 15.8, "2Z", 22, 0.3},
    {"61902", 17, 18.3, "2Z", 26, 0.3},
    {"61903", 19, 20.3, "2Z", 28, 0.3},
    {"61904", 22, 23, "2RS1", 35, 0.3},
    {"61905", 27, 29, "2RS1", 40, 0.3},
    {"61906", 32, 34, "2RS1", 45, 0.3},
    {"604", 5.4, 6, "2Z", 10.6, 0.2},
    {"607", 9, 11, "2Z", 17, 0.3},
    {"608", 10, 12, "2Z", 20, 0.3},
    {"609", 11, 14.3, "2Z", 22, 0.3},
    {"6000", 12, 14.7, "2Z", 24, 0.3},
    {"6001", 14, 16.9, "2Z", 26, 0.3},
    {"6002", 17, 20.4, "2Z", 30, 0.3},
    {"6003", 19, 22.9, "2Z", 33, 0.3},
    {"6004", 23.2, 27.1, "2Z", 38.8, 0.6},
    {"6005", 28.2, 31.9, "2Z", 43.8, 0.6},
    {"6006", 34.6, 38.1, "2Z", 50, 1},
    {"623", 4.2, 5.1, "2Z", 8.8, 0.1},
    {"624", 5.8, 6.6, "2Z", 11.2, 0.2},
    {"625", 7.4, 8.3, "2Z", 13.6, 0.3},
    {"626", 8.4, 11, "2Z", 16.6, 0.3},
    {"627", 9.4, 12.1, "2Z", 19.6, 0.3},
    {"628", 10.4, 14.4, "2Z", 21.6, 0.3},
    {"629", 11.4, 14.7, "2Z", 23.6, 0.3},
    {"6200", 14.2, 16.9, "2Z", 25.8, 0.6},
    {"6201", 16.2, 18.4, "2Z", 27.8, 0.6},
    {"6202", 19.2, 21.6, "2Z", 30.8, 0.6},
    {"6203", 21.2, 24.4, "2Z", 35.8, 0.6},
    {"6204", 25.6, 28.7, "2Z", 41.4, 1},
    {"6205", 30.6, 34.3, "2Z", 46.4, 1},
    {"6206", 35.6, 40.3, "2Z", 56, 1},
    {"634", 6.4, 8.3, "2Z", 13.6, 0.3},
    {"635", 7.4, 10.6, "2Z", 16.6, 0.3},
    {"6300", 14.2, 17.4, "2Z", 30.8, 0.6},
    {"6301", 17.6, 19.4, "2Z", 31.4, 1},
    {"6302", 20.6, 23.6, "2Z", 36.4, 1},
    {"6303", 22.6, 26.4, "2Z", 41.4, 1},
    {"6304", 27, 30.3, "2Z", 45, 1},
    {"6305", 32, 36.5, "2Z", 55, 1},
    {"6306", 37, 44.5, "2Z", 65, 1},
};

} // namespace

const BearingAbutment* AbutmentOf(const std::string& designation) {
  for (const BearingAbutment& a : kAbutments)
    if (designation == a.designation)
      return &a;
  return nullptr;
}

const char* AbutmentSource() {
  return "SKF Rolling bearings, PUB BU/P1 17000 EN (2018), table 1.1";
}

} // namespace parts
