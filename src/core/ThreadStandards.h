// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

// Thread series (docs/modeling/holes-and-threads.md#thread-standards):
// designation -> major diameter and pitch, with the derived basic diameters.
// ISO metric coarse and fine, and Unified coarse (UNC) and fine (UNF). Both
// use the same 60 degree basic profile (ISO 68-1, ASME B1.1), so everything
// derived from D and P applies to either. Pure static data, no I/O.
namespace threads {

enum class Series { Metric, UNC, UNF };

struct Standard {
  std::string designation; // "M8", "M8x1", "3/8-16 UNC", "#10-32 UNF"
  double majorDia = 0.0;   // nominal major diameter D, mm
  double pitch = 0.0;      // P, mm (25.4 / TPI for a unified thread)
  Series series = Series::Metric;
  // The tabulated tap drill, mm; 0 = the metric rule D - P. A unified tap
  // drill is a number, letter or fraction drill, which D - P only
  // approximates, so the table carries the drill a shop would reach for.
  double tapDrill = 0.0;
};

// "ISO metric", "UNC", "UNF".
[[nodiscard]] std::string SeriesName(Series s);

// The flank angle of the ISO 68-1 / ASME B1.1 basic profile: half its 60
// degree thread angle, measured from the plane normal to the axis.
inline constexpr double kIsoFlankDeg = 30.0;
// The range a custom thread's flank angle may take
// (docs/modeling/holes-and-threads.md#flank-angle): steeper than 15 degrees
// the profile is nearly square, past 60 its depth is under a third of a
// pitch.
inline constexpr double kMinFlankDeg = 15.0, kMaxFlankDeg = 60.0;

// The basic profile's thread depth, 5H/8 of its fundamental triangle
// H = P / (2 tan a), for a flank angle `a` in degrees: 0.5413 P at the ISO
// 30, half a pitch less a fifth at 45. Twice it is what a basic minor lies
// under the major.
[[nodiscard]] double BasicDepth(double pitch, double flankDeg = kIsoFlankDeg);

// Basic minor diameter: D - 1.0825·P (ISO 68-1 / ASME B1.1 internal D1).
// M8 -> 6.647.
double MinorDiameter(const Standard& s);
// Tap-drill diameter: the tabulated drill, else D - P (M8 -> 6.75).
double TapDrill(const Standard& s);

// Every series, metric coarse then fine, then UNC and UNF ascending — combo-box
// order.
const std::vector<Standard>& All();

// The tolerance classes a series takes on one side: metric external 6g, 4h,
// ..., internal 6H, 5H, ...; unified external 1A/2A/3A, internal 1B/2B/3B.
[[nodiscard]] const std::vector<std::string>& Classes(Series s, bool external);

// A designation read against the tables. Accepted spellings: the table's own
// ("M8", "3/8-16 UNC", "#10-32 UNF"); a metric coarse thread with its pitch
// written out ("M8x1.25"); a number size without its '#' ("10-32 UNF"); and
// any of those with a class suffix ("M8-6g", "3/8-16 UNC-2B").
struct Resolved {
  Standard standard;       // the table row (its canonical designation)
  std::string threadClass; // the suffix's class, empty when none was written
};

// Resolves `designation`. nullopt with `error` EMPTY: free text that names no
// series we carry — a custom thread, whose pitch is the caller's. nullopt
// with `error` SET: the designation claims a series we carry (an "M" size, a
// size-TPI pair, a UNC/UNF suffix) or a class, and the table has no such row
// or the class is not one the series takes; the error names what is
// accepted. Refused, never rounded to a neighbour.
[[nodiscard]] std::optional<Resolved> Resolve(const std::string& designation,
                                              std::string* error);

// Lookup by designation, any accepted spelling. nullopt for unknown/custom.
std::optional<Standard> Lookup(const std::string& designation);

// How close a given pitch must be to a row's to be that row's, mm.
inline constexpr double kPitchTolMm = 1e-6;

// Why `pitch` cannot go with the row `s` that `designation` resolved to, or
// empty when it is the row's (docs/modeling/holes-and-threads.md#thread).
// A table thread's pitch is its row's: another is refused, naming the row's
// and -- a fine pitch being a row of its own -- the row of the same
// diameter that has the pitch given ("M8" at 1 mm: "M8x1"), never replaced.
[[nodiscard]] std::string
PitchError(const Standard& s, const std::string& designation, double pitch);

// Why `threadClass` cannot go on this thread, or empty when it can (empty
// class included). A class of the other side (2B on a boss, 6g in a hole) is
// refused as well as one the series does not have.
[[nodiscard]] std::string
ClassError(const Standard& s, const std::string& threadClass, bool external);

// The pitch-diameter band a tolerance class allows
// (docs/modeling/holes-and-threads.md#pitch-diameter-bands), in mm: what a
// thread is MADE to, as the class says, where the basic profile is only what
// it is drawn at. Computed from the standards' own formulas rather than
// tabulated -- ISO 965-1 (the grade's Td2 / TD2 off the diameter range and
// the pitch, the position's fundamental deviation) and ASME B1.1 (the 2A PD
// tolerance 0.0015 D^1/3 + 0.0015 (9P)^1/2 + 0.015 P^2/3 inch, scaled by
// class, and the 1A/2A allowance of 0.3 of it) -- so every row and every
// class has one. The published tables round each term; these land within
// a few microns of them (3/8-16 UNC-2B: 8.494 / 8.645 against 8.494 /
// 8.639).
struct PitchBand {
  double min = 0.0, max = 0.0; // mm
  std::string threadClass;     // the class it is for
  [[nodiscard]] double Mid() const { return 0.5 * (min + max); }
};
// Basic pitch diameter, D - 0.649519 P (ISO 68-1 / ASME B1.1 D2).
[[nodiscard]] double BasicPitchDiameter(double majorDia, double pitch);
// The band of `threadClass` on a thread of this series, major diameter and
// pitch. nullopt, with `error`, for a class the series does not have on
// that side -- refused, never read as another class. An EMPTY class is not
// a class: the caller says which one it is aiming at (DefaultClass).
[[nodiscard]] std::optional<PitchBand>
PitchDiameterBand(Series series, double majorDia, double pitch,
                  const std::string& threadClass, bool external,
                  std::string* error);
// The general-purpose class of a series on one side: 6g / 6H, 2A / 2B -- the
// first each Classes list names.
[[nodiscard]] std::string DefaultClass(Series series, bool external);

} // namespace threads
