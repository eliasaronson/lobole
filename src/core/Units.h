// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

// The units a DOCUMENT is read in.
//
// CAD convention, and the reason this is not an application preference: a
// part authored in inches has to display in inches for whoever opens it. The
// system travels with the document (Document::Units, serialized), not with
// the reader. Commercial CAD additionally lets length, mass and angle be chosen
// independently, and so does this — but each is then FIXED. Nothing here
// picks a unit per value.
//
// STORAGE IS ALWAYS MILLIMETRES, GRAMS AND DEGREES. This header is a
// presentation layer and nothing else; a unit change must never move a
// vertex. Every Format* takes its input in storage units and converts on the
// way out.
//
// Two rules do most of the work whichever units are chosen, and both are in
// FormatNumber:
//
//   - Thousands separators. `10080000` is not a number a person can read;
//     `10 080 000` is.
//   - SIGNIFICANT FIGURES, not fixed decimals. The tree printed body volumes
//     with "%.2f", so a tessellated solid claimed `1305294.05 mm³` — ten
//     nanolitres of precision it does not have, in a string too long for the
//     column, which then clipped it to `1305294.05 m`.
namespace units {

enum class Length { Millimetre, Centimetre, Metre, Inch };
enum class Mass { Gram, Kilogram, Pound };
enum class Angle { Degree, Radian };

struct System {
  Length length = Length::Millimetre;
  Mass mass = Mass::Kilogram;
  Angle angle = Angle::Degree;

  bool operator==(const System& o) const {
    return length == o.length && mass == o.mass && angle == o.angle;
  }
  bool operator!=(const System& o) const { return !(*this == o); }
  // The default is what a document that never said otherwise gets, and what
  // is omitted from the file so such documents re-save byte-identically.
  bool IsDefault() const { return *this == System{}; }
};

// Serialization / RPC strings. The From* parsers REFUSE an unknown string
// rather than substituting a default: a document that said "inch" and loaded
// as millimetres would be wrong everywhere and look fine (CLAUDE.md, "Refuse,
// don't substitute"). [[nodiscard]] so a caller cannot drop the answer.
const char* ToString(Length u);
const char* ToString(Mass u);
const char* ToString(Angle u);
[[nodiscard]] bool LengthFromString(const std::string& s, Length& out);
[[nodiscard]] bool MassFromString(const std::string& s, Mass& out);
[[nodiscard]] bool AngleFromString(const std::string& s, Angle& out);

// Every accepted string, for `describe` and for error messages that name
// what IS accepted. Generated beside the parsers so introspection cannot
// drift from them.
std::vector<const char*> LengthNames();
std::vector<const char*> MassNames();
std::vector<const char*> AngleNames();

// Millimetres per one of `u`. The conversion table itself, for the rare
// caller that needs a bare NUMBER in display units rather than a formatted
// string — copying a measurement to the clipboard, where the field being
// pasted into already knows its own unit.
double MmPerLength(Length u);

// A number a person can read: `sigFigs` significant figures, trailing zeros
// dropped, integer part grouped in threes with a space (the SI separator —
// a comma means the decimal point to half the world).
std::string FormatNumber(double value, int sigFigs = 6);

// Storage in, display out. The suffix is included ("124.5 mm", "10 080 000
// mm³", "385.17 kg", "45°").
std::string FormatLength(double mm, const System& sys);
std::string FormatArea(double mm2, const System& sys);
std::string FormatVolume(double mm3, const System& sys);
std::string FormatMass(double grams, const System& sys);
std::string FormatAngle(double degrees, const System& sys);

// The unit suffix alone, for a table column header that should not repeat it
// on every row.
std::string LengthSuffix(const System& sys);
std::string AreaSuffix(const System& sys);
std::string VolumeSuffix(const System& sys);
std::string MassSuffix(const System& sys);

} // namespace units
