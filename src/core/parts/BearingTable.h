// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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
// Abutment (shoulder) limits are not ISO 15's either — they are a
// supplier's, and suppliers differ — so they are carried apart, from ONE
// cited catalogue (BearingAbutment below), and `check_seat` judges a
// shoulder against them without an import. Load ratings and limiting speeds
// are not here: they come from an imported engineering catalog
// (docs/parts/catalogs.md).
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

// A supplier's abutment and fillet dimensions for one designation
// (docs/parts/README.md#abutment-limits): how far a shaft shoulder must and
// may reach (d_a min, d_a max), how far a housing shoulder may reach inward
// before it stops supporting the ring (D_a max), and the largest fillet into
// either (r_a max). From the catalogue AbutmentSource() names. A limit it
// does not publish is NaN (`Has`), never invented: SKF prints d_a max only
// for a capped bearing, and no D_a min for a deep-groove ball bearing.
struct BearingAbutment {
  const char* designation;
  double daMin; // shaft shoulder: below it the ring's side is unsupported
  double daMax; // shaft shoulder: above it the shoulder reaches the shield
  // The capped variant every limit is read from ("2Z", "2RS1") — the body is
  // drawn shielded — or null when SKF lists none and the open row (which
  // has no d_a max) is used.
  const char* variant;
  double DaMax; // housing shoulder's bore: above it the ring is unsupported
  double raMax; // the largest fillet radius into either shoulder
  [[nodiscard]] static bool Has(double v) { return v == v; } // not NaN
};

// The row for a designation, or null when the catalogue lists no such
// bearing (618/3, 619/3, 605 and 606 are ISO 15 sizes SKF does not make).
[[nodiscard]] const BearingAbutment* AbutmentOf(const std::string& designation);
// The catalogue and edition the abutment rows were typed from — what a check
// names as the source of a limit.
[[nodiscard]] const char* AbutmentSource();

} // namespace parts
