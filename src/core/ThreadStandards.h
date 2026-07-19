#pragma once

#include <optional>
#include <string>
#include <vector>

// ISO metric thread series (docs/modeling/thread-rib.md Part A): designation
// -> pitch, with the derived basic diameters. Pure static data — unit tested,
// no I/O. UNC/UNF later; the table is just rows.
namespace threads {

struct Standard {
  std::string designation; // "M8" (coarse) / "M8x1" (fine)
  double majorDia = 0.0;   // nominal major diameter D
  double pitch = 0.0;      // P
};

// Basic minor diameter for the designation: D - 1.0825·P (ISO 68-1 internal
// thread D1). M8 -> 6.647.
double MinorDiameter(const Standard& s);
// Conventional tap-drill diameter: D - P.
double TapDrill(const Standard& s);

// The full series, coarse then fine, ascending diameter — combo-box order.
const std::vector<Standard>& All();

// Lookup by exact designation ("M8", "M8x1"). nullopt for unknown/custom.
std::optional<Standard> Lookup(const std::string& designation);

} // namespace threads
