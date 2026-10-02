#pragma once

#include <string>
#include <vector>

// Retaining rings (circlips): DIN 471 for shafts and DIN 472 for bores, the
// ring and the groove it sits in (docs/parts/README.md#retaining-rings).
//
// Static data and a FIXED SCOPE, like the bearing table: a size the table
// does not carry is refused naming the nearest and the full list, never
// interpolated — a ring for a Ø43 shaft is plausible and cannot be bought.
namespace parts {

enum class RetainingRingStandard {
  Din471, // external: sprung open over a shaft, sits in a groove in it
  Din472, // internal: squeezed into a bore, sits in a groove in its wall
};

// One size, in mm. The ring columns are the unstressed ring; the groove
// columns are what the shaft or bore is cut to.
struct RetainingRingRow {
  double d1; // nominal: the shaft (471) or bore (472) diameter
  double s;  // ring thickness
  double d3; // free diameter: the ring's inside (471) or outside (472)
  double a;  // lug height, max
  double b;  // radial section, approximate (the ring tapers to the lugs)
  double d5; // lug hole, min
  double d2; // groove diameter
  double m;  // groove width, min
  double t;  // groove depth, (d1 - d2) / 2 for 471, (d2 - d1) / 2 for 472
  double n;  // edge distance, min: the land past the groove carrying thrust
};

// "din471" / "din472" — the spelling the RPC and the file use.
[[nodiscard]] const char* RetainingRingStandardName(RetainingRingStandard s);
// Parses that spelling; false (and `out` untouched) for anything else.
[[nodiscard]] bool RetainingRingStandardFromString(const std::string& s,
                                                   RetainingRingStandard& out);
// Both spellings with what each is, for a refusal.
[[nodiscard]] std::string KnownRetainingRingStandards();

// Every size of one standard, smallest first.
[[nodiscard]] const std::vector<RetainingRingRow>&
RetainingRings(RetainingRingStandard standard);

// The row for a nominal size (to 0.001 mm), or null with `why` naming the
// nearest sizes carried and then every one.
[[nodiscard]] const RetainingRingRow*
FindRetainingRing(RetainingRingStandard standard, double size,
                  std::string* why = nullptr);

// "3, 4, 5, ... 56" — every size of one standard.
[[nodiscard]] std::string KnownRetainingRingSizes(RetainingRingStandard s);

// "DIN 471 25x1.2" — what a supplier calls it and what the BOM carries
// (the part's name; the BOM's designation is the "25x1.2" after the
// standard).
[[nodiscard]] std::string RetainingRingDesignation(RetainingRingStandard s,
                                                   const RetainingRingRow& r);
// "25x1.2".
[[nodiscard]] std::string RetainingRingSize(const RetainingRingRow& r);

// The data chart a standard's rows were typed from.
[[nodiscard]] const char* RetainingRingSource(RetainingRingStandard s);

} // namespace parts
