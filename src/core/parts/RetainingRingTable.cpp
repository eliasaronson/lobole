#include "core/parts/RetainingRingTable.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace parts {

namespace {

// DIN 471 rings for shafts, A3 to A56: Seeger-Orbis data chart "Seeger-Rings
// for shafts A3 - A56 / DIN 471" (Seeger catalogue section 10, pp. 22-23,
// datasheet_seeger-ringe_471.pdf from seeger-orbis.com). Ring: s, d3
// (unstressed), a max, b ≈, d5 min. Groove: d2, m min, t, n min.
//
// Every size of that page is carried, so a shaft between the bearings' bores
// (3 to 30 mm) and a little past them has its ring.
const std::vector<RetainingRingRow> kDin471 = {
    // d1, s, d3, a, b, d5, d2, m, t, n
    {3, 0.4, 2.7, 1.9, 0.8, 1, 2.8, 0.5, 0.1, 0.3},
    {4, 0.4, 3.7, 2.2, 0.9, 1, 3.8, 0.5, 0.1, 0.3},
    {5, 0.6, 4.7, 2.5, 1.1, 1, 4.8, 0.7, 0.1, 0.3},
    {6, 0.7, 5.6, 2.7, 1.3, 1.2, 5.7, 0.8, 0.15, 0.5},
    {7, 0.8, 6.5, 3.1, 1.4, 1.2, 6.7, 0.9, 0.15, 0.5},
    {8, 0.8, 7.4, 3.2, 1.5, 1.2, 7.6, 0.9, 0.2, 0.6},
    {9, 1, 8.4, 3.3, 1.7, 1.2, 8.6, 1.1, 0.2, 0.6},
    {10, 1, 9.3, 3.3, 1.8, 1.5, 9.6, 1.1, 0.2, 0.6},
    {11, 1, 10.2, 3.3, 1.8, 1.5, 10.5, 1.1, 0.25, 0.8},
    {12, 1, 11, 3.3, 1.8, 1.7, 11.5, 1.1, 0.25, 0.8},
    {13, 1, 11.9, 3.4, 2, 1.7, 12.4, 1.1, 0.3, 0.9},
    {14, 1, 12.9, 3.5, 2.1, 1.7, 13.4, 1.1, 0.3, 0.9},
    {15, 1, 13.8, 3.6, 2.2, 1.7, 14.3, 1.1, 0.35, 1.1},
    {16, 1, 14.7, 3.7, 2.2, 1.7, 15.2, 1.1, 0.4, 1.2},
    {17, 1, 15.7, 3.8, 2.3, 1.7, 16.2, 1.1, 0.4, 1.2},
    {18, 1.2, 16.5, 3.9, 2.4, 2, 17, 1.3, 0.5, 1.5},
    {19, 1.2, 17.5, 3.9, 2.5, 2, 18, 1.3, 0.5, 1.5},
    {20, 1.2, 18.5, 4, 2.6, 2, 19, 1.3, 0.5, 1.5},
    {21, 1.2, 19.5, 4.1, 2.7, 2, 20, 1.3, 0.5, 1.5},
    {22, 1.2, 20.5, 4.2, 2.8, 2, 21, 1.3, 0.5, 1.5},
    {23, 1.2, 21.5, 4.3, 2.9, 2, 22, 1.3, 0.5, 1.5},
    {24, 1.2, 22.2, 4.4, 3, 2, 22.9, 1.3, 0.55, 1.7},
    {25, 1.2, 23.2, 4.4, 3, 2, 23.9, 1.3, 0.55, 1.7},
    {26, 1.2, 24.2, 4.5, 3.1, 2, 24.9, 1.3, 0.55, 1.7},
    {27, 1.2, 24.9, 4.6, 3.1, 2, 25.6, 1.3, 0.7, 2.1},
    {28, 1.5, 25.9, 4.7, 3.2, 2, 26.6, 1.6, 0.7, 2.1},
    {29, 1.5, 26.9, 4.8, 3.4, 2, 27.6, 1.6, 0.7, 2.1},
    {30, 1.5, 27.9, 5, 3.5, 2, 28.6, 1.6, 0.7, 2.1},
    {31, 1.5, 28.6, 5.1, 3.5, 2.5, 29.3, 1.6, 0.85, 2.6},
    {32, 1.5, 29.6, 5.2, 3.6, 2.5, 30.3, 1.6, 0.85, 2.6},
    {33, 1.5, 30.5, 5.2, 3.7, 2.5, 31.3, 1.6, 0.85, 2.6},
    {34, 1.5, 31.5, 5.4, 3.8, 2.5, 32.3, 1.6, 0.85, 2.6},
    {35, 1.5, 32.2, 5.6, 3.9, 2.5, 33, 1.6, 1, 3},
    {36, 1.75, 33.2, 5.6, 4, 2.5, 34, 1.85, 1, 3},
    {37, 1.75, 34.2, 5.7, 4.1, 2.5, 35, 1.85, 1, 3},
    {38, 1.75, 35.2, 5.8, 4.2, 2.5, 36, 1.85, 1, 3},
    {39, 1.75, 36, 5.9, 4.3, 2.5, 37, 1.85, 1, 3},
    {40, 1.75, 36.5, 6, 4.4, 2.5, 37.5, 1.85, 1.25, 3.8},
    {41, 1.75, 37.5, 6.2, 4.5, 2.5, 38.5, 1.85, 1.25, 3.8},
    {42, 1.75, 38.5, 6.5, 4.5, 2.5, 39.5, 1.85, 1.25, 3.8},
    {44, 1.75, 40.5, 6.6, 4.6, 2.5, 41.5, 1.85, 1.25, 3.8},
    {45, 1.75, 41.5, 6.7, 4.7, 2.5, 42.5, 1.85, 1.25, 3.8},
    {46, 1.75, 42.5, 6.7, 4.8, 2.5, 43.5, 1.85, 1.25, 3.8},
    {47, 1.75, 43.5, 6.8, 4.9, 2.5, 44.5, 1.85, 1.25, 3.8},
    {48, 1.75, 44.5, 6.9, 5, 2.5, 45.5, 1.85, 1.25, 3.8},
    {50, 2, 45.8, 6.9, 5.1, 2.5, 47, 2.15, 1.5, 4.5},
    {52, 2, 47.8, 7, 5.2, 2.5, 49, 2.15, 1.5, 4.5},
    {54, 2, 49.8, 7.1, 5.3, 2.5, 51, 2.15, 1.5, 4.5},
    {55, 2, 50.8, 7.2, 5.4, 2.5, 52, 2.15, 1.5, 4.5},
    {56, 2, 51.8, 7.3, 5.5, 2.5, 53, 2.15, 1.5, 4.5},
};

// DIN 472 rings for bores, J8 to J72: Seeger-Orbis data charts
// "Seeger-Rings for bores J8 - J58 / DIN 472" and "J60 - J150 / DIN 472"
// (Seeger catalogue section 11, pp. 32-34, datasheet_seeger-ringe_472.pdf
// from seeger-orbis.com), the second page to J72 so the largest outside
// diameter the bearing table carries (a 6306's 72) has its ring. Same
// columns; d3 is the ring's unstressed OUTSIDE.
const std::vector<RetainingRingRow> kDin472 = {
    // d1, s, d3, a, b, d5, d2, m, t, n
    {8, 0.8, 8.7, 2.4, 1.1, 1, 8.4, 0.9, 0.2, 0.6},
    {9, 0.8, 9.8, 2.5, 1.3, 1, 9.4, 0.9, 0.2, 0.6},
    {10, 1, 10.8, 3.2, 1.4, 1.2, 10.4, 1.1, 0.2, 0.6},
    {11, 1, 11.8, 3.3, 1.5, 1.2, 11.4, 1.1, 0.2, 0.6},
    {12, 1, 13, 3.4, 1.7, 1.5, 12.5, 1.1, 0.25, 0.8},
    {13, 1, 14.1, 3.6, 1.8, 1.5, 13.6, 1.1, 0.3, 0.9},
    {14, 1, 15.1, 3.7, 1.8, 1.7, 14.6, 1.1, 0.3, 0.9},
    {15, 1, 16.2, 3.7, 2, 1.7, 15.7, 1.1, 0.35, 1.1},
    {16, 1, 17.3, 3.8, 2, 1.7, 16.8, 1.1, 0.4, 1.2},
    {17, 1, 18.3, 3.9, 2.1, 1.7, 17.8, 1.1, 0.4, 1.2},
    {18, 1, 19.5, 4.1, 2.2, 2, 19, 1.1, 0.5, 1.5},
    {19, 1, 20.5, 4.1, 2.2, 2, 20, 1.1, 0.5, 1.5},
    {20, 1, 21.5, 4.1, 2.3, 2, 21, 1.1, 0.5, 1.5},
    {21, 1, 22.5, 4.2, 2.4, 2, 22, 1.1, 0.5, 1.5},
    {22, 1, 23.5, 4.2, 2.5, 2, 23, 1.1, 0.5, 1.5},
    {23, 1.2, 24.6, 4.2, 2.5, 2, 24.1, 1.3, 0.55, 1.7},
    {24, 1.2, 25.9, 4.3, 2.6, 2, 25.2, 1.3, 0.6, 1.8},
    {25, 1.2, 26.9, 4.5, 2.7, 2, 26.2, 1.3, 0.6, 1.8},
    {26, 1.2, 27.9, 4.7, 2.8, 2, 27.2, 1.3, 0.6, 1.8},
    {27, 1.2, 29.1, 4.7, 2.9, 2, 28.4, 1.3, 0.7, 2.1},
    {28, 1.2, 30.1, 4.8, 2.9, 2, 29.4, 1.3, 0.7, 2.1},
    {29, 1.2, 31.1, 4.8, 3, 2, 30.4, 1.3, 0.7, 2.1},
    {30, 1.2, 32.1, 4.8, 3, 2, 31.4, 1.3, 0.7, 2.1},
    {31, 1.2, 33.4, 5.2, 3.1, 2.5, 32.7, 1.3, 0.85, 2.6},
    {32, 1.2, 34.4, 5.4, 3.2, 2.5, 33.7, 1.3, 0.85, 2.6},
    {33, 1.2, 35.5, 5.4, 3.3, 2.5, 34.7, 1.3, 0.85, 2.6},
    {34, 1.5, 36.5, 5.4, 3.3, 2.5, 35.7, 1.6, 0.85, 2.6},
    {35, 1.5, 37.8, 5.4, 3.4, 2.5, 37, 1.6, 1, 3},
    {36, 1.5, 38.8, 5.4, 3.5, 2.5, 38, 1.6, 1, 3},
    {37, 1.5, 39.8, 5.5, 3.6, 2.5, 39, 1.6, 1, 3},
    {38, 1.5, 40.8, 5.5, 3.7, 2.5, 40, 1.6, 1, 3},
    {39, 1.5, 42, 5.6, 3.8, 2.5, 41, 1.6, 1, 3},
    {40, 1.75, 43.5, 5.8, 3.9, 2.5, 42.5, 1.85, 1.25, 3.8},
    {41, 1.75, 44.5, 5.9, 4, 2.5, 43.5, 1.85, 1.25, 3.8},
    {42, 1.75, 45.5, 5.9, 4.1, 2.5, 44.5, 1.85, 1.25, 3.8},
    {43, 1.75, 46.5, 5.9, 4.2, 2.5, 45.5, 1.85, 1.25, 3.8},
    {44, 1.75, 47.5, 6, 4.2, 2.5, 46.5, 1.85, 1.25, 3.8},
    {45, 1.75, 48.5, 6.2, 4.3, 2.5, 47.5, 1.85, 1.25, 3.8},
    {46, 1.75, 49.5, 6.3, 4.4, 2.5, 48.5, 1.85, 1.25, 3.8},
    {47, 1.75, 50.5, 6.4, 4.4, 2.5, 49.5, 1.85, 1.25, 3.8},
    {48, 1.75, 51.5, 6.4, 4.5, 2.5, 50.5, 1.85, 1.25, 3.8},
    {50, 2, 54.2, 6.5, 4.6, 2.5, 53, 2.15, 1.5, 4.5},
    {51, 2, 55.2, 6.5, 4.7, 2.5, 54, 2.15, 1.5, 4.5},
    {52, 2, 56.2, 6.7, 4.7, 2.5, 55, 2.15, 1.5, 4.5},
    {53, 2, 57.2, 6.7, 4.9, 2.5, 56, 2.15, 1.5, 4.5},
    {54, 2, 58.2, 6.7, 5, 2.5, 57, 2.15, 1.5, 4.5},
    {55, 2, 59.2, 6.8, 5, 2.5, 58, 2.15, 1.5, 4.5},
    {56, 2, 60.2, 6.8, 5.1, 2.5, 59, 2.15, 1.5, 4.5},
    {57, 2, 61.2, 6.8, 5.1, 2.5, 60, 2.15, 1.5, 4.5},
    {58, 2, 62.2, 6.9, 5.2, 2.5, 61, 2.15, 1.5, 4.5},
    {60, 2, 64.2, 7.3, 5.4, 2.5, 63, 2.15, 1.5, 4.5},
    {62, 2, 66.2, 7.3, 5.5, 2.5, 65, 2.15, 1.5, 4.5},
    {63, 2, 67.2, 7.3, 5.6, 2.5, 66, 2.15, 1.5, 4.5},
    {64, 2, 68.2, 7.4, 5.7, 2.5, 67, 2.15, 1.5, 4.5},
    {65, 2.5, 69.2, 7.6, 5.8, 3, 68, 2.65, 1.5, 4.5},
    {67, 2.5, 71.5, 7.7, 6, 3, 70, 2.65, 1.5, 4.5},
    {68, 2.5, 72.5, 7.8, 6.1, 3, 71, 2.65, 1.5, 4.5},
    {70, 2.5, 74.5, 7.8, 6.2, 3, 73, 2.65, 1.5, 4.5},
    {72, 2.5, 76.5, 7.8, 6.4, 3, 75, 2.65, 1.5, 4.5},
};

std::string Num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

} // namespace

const char* RetainingRingStandardName(RetainingRingStandard s) {
  return s == RetainingRingStandard::Din471 ? "din471" : "din472";
}

bool RetainingRingStandardFromString(const std::string& s,
                                     RetainingRingStandard& out) {
  if (s == "din471") {
    out = RetainingRingStandard::Din471;
    return true;
  }
  if (s == "din472") {
    out = RetainingRingStandard::Din472;
    return true;
  }
  return false;
}

std::string KnownRetainingRingStandards() {
  return "din471 (external, for shafts), din472 (internal, for bores)";
}

const std::vector<RetainingRingRow>&
RetainingRings(RetainingRingStandard standard) {
  return standard == RetainingRingStandard::Din471 ? kDin471 : kDin472;
}

std::string KnownRetainingRingSizes(RetainingRingStandard s) {
  std::string out;
  for (const RetainingRingRow& r : RetainingRings(s))
    out += (out.empty() ? "" : ", ") + Num(r.d1);
  return out;
}

const RetainingRingRow* FindRetainingRing(RetainingRingStandard standard,
                                          double size, std::string* why) {
  const std::vector<RetainingRingRow>& rows = RetainingRings(standard);
  for (const RetainingRingRow& r : rows)
    if (std::abs(r.d1 - size) < 1e-3)
      return &r;
  if (why != nullptr) {
    // The nearest carried below and above, then everything.
    const RetainingRingRow* below = nullptr;
    const RetainingRingRow* above = nullptr;
    for (const RetainingRingRow& r : rows) {
      if (r.d1 < size)
        below = &r;
      else if (above == nullptr)
        above = &r;
    }
    std::string nearest;
    if (below != nullptr)
      nearest = Num(below->d1);
    if (above != nullptr)
      nearest += (nearest.empty() ? "" : " or ") + Num(above->d1);
    const bool shaft = standard == RetainingRingStandard::Din471;
    *why = std::string("no ") +
           (shaft ? "DIN 471 ring for a Ø" : "DIN 472 ring for a Ø") +
           Num(size) + (shaft ? " shaft" : " bore") +
           " — the nearest carried are " + nearest + "; " +
           RetainingRingStandardName(standard) + " carries " +
           KnownRetainingRingSizes(standard) + " (mm, " +
           RetainingRingSource(standard) + ")";
  }
  return nullptr;
}

std::string RetainingRingSize(const RetainingRingRow& r) {
  return Num(r.d1) + "x" + Num(r.s);
}

std::string RetainingRingDesignation(RetainingRingStandard s,
                                     const RetainingRingRow& r) {
  return std::string(s == RetainingRingStandard::Din471 ? "DIN 471 "
                                                        : "DIN 472 ") +
         RetainingRingSize(r);
}

const char* RetainingRingSource(RetainingRingStandard s) {
  return s == RetainingRingStandard::Din471
             ? "Seeger-Orbis data chart A3-A56 / DIN 471"
             : "Seeger-Orbis data charts J8-J58 and J60-J150 / DIN 472";
}

} // namespace parts
