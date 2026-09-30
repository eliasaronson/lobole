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

// Why `threadClass` cannot go on this thread, or empty when it can (empty
// class included). A class of the other side (2B on a boss, 6g in a hole) is
// refused as well as one the series does not have.
[[nodiscard]] std::string
ClassError(const Standard& s, const std::string& threadClass, bool external);

} // namespace threads
