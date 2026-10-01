#pragma once

#include <optional>
#include <string>
#include <vector>

// Deep-groove ball bearings: the boundary dimensions of ISO 15 and the
// normal-class bore and outside tolerances of ISO 492
// (docs/parts/README.md#bearings).
//
// Static data, like ThreadStandards and FitStandards, and a FIXED SCOPE like
// the fastener tables: a designation the table does not carry is refused
// naming what it does carry, never interpolated. A bearing at a size nobody
// makes is plausible and cannot be bought.
//
// What is NOT here, on purpose: load ratings, limiting speeds, abutment
// (shoulder) dimensions. ISO 15 does not state them — they are each
// supplier's, and they differ — so they come from an imported engineering
// catalog (docs/parts/catalogs.md), which `check_seat` reads alongside a
// placed bearing.
namespace parts {

struct BearingRow {
  const char* designation; // "6200", "608", "618/4"
  const char* series;      // "62", "60", "618"
  double d;                // bore, mm
  double D;                // outside diameter, mm
  double B;                // width, mm
  double rMin;             // smallest single chamfer dimension r_s min, mm
};

// Every row, in series then bore order.
[[nodiscard]] const std::vector<BearingRow>& AllBearings();

// The row for a designation, exactly as a supplier writes it. A shield or
// seal suffix ("6200-2Z", "608ZZ") is refused naming the bare designation:
// the suffix does not change a boundary dimension, but taking it would put a
// shielded bearing on the BOM as an open one, or the other way round.
[[nodiscard]] const BearingRow* FindBearing(const std::string& designation,
                                            std::string* why = nullptr);

// The series this table carries ("618, 619, 60, 62, 63") and every
// designation, for a refusal.
[[nodiscard]] std::string KnownBearingSeries();
[[nodiscard]] std::string KnownBearings();

// The rows whose bore (or outside diameter) is `diameter`, to 0.001 mm:
// what fits a shaft (or a housing bore) of that size.
[[nodiscard]] std::vector<const BearingRow*> BearingsForBore(double diameter);
[[nodiscard]] std::vector<const BearingRow*>
BearingsForOutside(double diameter);

// The rows of one series (all of them when `series` is empty) whose bore is
// at least `minBore` mm, smallest bore first: what a generator sizing a
// shaft from its load asks before any seat exists — "the smallest 60-series
// bearing that takes a 13.9 mm shaft" is the first row. An unknown series is
// refused naming the carried ones (`why` set, empty result); a bore over the
// largest carried is an empty result with `why` saying how far the table
// reaches.
[[nodiscard]] std::vector<const BearingRow*>
BearingsFrom(const std::string& series, double minBore, std::string* why);

// ISO 492 normal tolerance class: the deviation of the mean bore diameter
// (t_Δdmp) and of the mean outside diameter (t_ΔDmp) from nominal, in mm.
// Upper is 0 and lower negative for both — a bearing's bore is made SMALL,
// which is why a k5 shaft that is a transition fit in an H7 hole is an
// interference fit in a bearing. nullopt outside the sizes this table
// carries.
struct BearingBand {
  double lower = 0.0; // mm
  double upper = 0.0; // mm
};
[[nodiscard]] std::optional<BearingBand> BoreTolerance(double d);
[[nodiscard]] std::optional<BearingBand> OutsideTolerance(double D);

} // namespace parts
