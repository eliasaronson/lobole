// Display units, and the two rules that make a number readable whichever
// units are chosen (core/Units.h).
//
// These are worth their own file because the failure mode is quiet. The
// feature tree printed body volumes with "%.2f", so a tessellated solid
// claimed `1305294.05 mm³` — ten nanolitres of precision it does not have,
// in a string too long for its column, which then clipped it to
// `1305294.05 m`. A truncated number still looks like a number.

#include <catch2/catch_test_macros.hpp>

#include "core/Units.h"

TEST_CASE("a number is grouped and cut to significant figures", "[units]") {
  // The case that started this: seven digits, unreadable and over-precise.
  CHECK(units::FormatNumber(10080000.0) == "10 080 000");
  CHECK(units::FormatNumber(1305294.05) == "1 305 290");

  // Small values keep the precision they actually have; trailing zeros go,
  // because they cost column width and say nothing.
  CHECK(units::FormatNumber(8.0) == "8");
  CHECK(units::FormatNumber(0.5) == "0.5");
  CHECK(units::FormatNumber(12.3400) == "12.34");

  // Grouping starts above three digits and never touches the fraction.
  CHECK(units::FormatNumber(999.0) == "999");
  CHECK(units::FormatNumber(1000.0) == "1 000");
  CHECK(units::FormatNumber(1234.5) == "1 234.5");

  CHECK(units::FormatNumber(-4321.0) == "-4 321");
  CHECK(units::FormatNumber(0.0) == "0");

  // Never exponent form: "1.30529e+06 mm³" helps nobody.
  CHECK(units::FormatNumber(1.0e9).find('e') == std::string::npos);
}

TEST_CASE("storage is mm/g/deg; the units are display only", "[units]") {
  units::System mm; // the default
  CHECK(units::FormatLength(25.4, mm) == "25.4 mm");
  CHECK(units::FormatVolume(10080000.0, mm) == "10 080 000 mm\xc2\xb3");

  units::System in;
  in.length = units::Length::Inch;
  in.mass = units::Mass::Pound;
  // The SAME stored value, read in another unit. Nothing was converted on
  // the way in — that is the invariant this asserts.
  CHECK(units::FormatLength(25.4, in) == "1 in");
  CHECK(units::FormatVolume(16387.064, in) == "1 in\xc2\xb3");
  CHECK(units::FormatMass(453.59237, in) == "1 lb");
  CHECK(units::FormatArea(645.16, in) == "1 in\xc2\xb2");
}

TEST_CASE("an area is the length unit SQUARED", "[units]") {
  // Added for the measure readout, which printed "mm²" as a literal — a face
  // area on an inch part read in millimetres. Same fixed-per-document rule
  // as FormatVolume, one power down.
  units::System sys;
  CHECK(units::FormatArea(9.0, sys) == "9 mm\xc2\xb2");
  CHECK(units::AreaSuffix(sys) == "mm\xc2\xb2");
  sys.length = units::Length::Centimetre;
  CHECK(units::FormatArea(100.0, sys) == "1 cm\xc2\xb2");
  CHECK(units::AreaSuffix(sys) == "cm\xc2\xb2");
}

TEST_CASE("a volume is the length unit CUBED, not whichever fits", "[units]") {
  // Fixed per document, never per value: a column of volumes has to be
  // comparable down the column, which a per-value unit destroys.
  units::System sys;
  CHECK(units::FormatVolume(8.0, sys) == "8 mm\xc2\xb3");
  CHECK(units::FormatVolume(1.0e9, sys) == "1 000 000 000 mm\xc2\xb3");
  sys.length = units::Length::Metre;
  CHECK(units::VolumeSuffix(sys) == "m\xc2\xb3");
}

TEST_CASE("mass keeps its own unit, and does not switch per value", "[units]") {
  units::System kg; // the default mass unit
  CHECK(units::FormatMass(900.0, kg) == "0.9 kg");
  CHECK(units::FormatMass(385169.06, kg) == "385.169 kg");

  units::System g;
  g.mass = units::Mass::Gram;
  CHECK(units::FormatMass(900.0, g) == "900 g");
}

TEST_CASE("an unknown unit string is refused, not substituted", "[units]") {
  // The whole reason these are [[nodiscard]]: a document that says "inch"
  // and loads as millimetres is wrong in every dimension it shows, and looks
  // entirely reasonable (CLAUDE.md, "Refuse, don't substitute").
  units::Length len = units::Length::Metre;
  CHECK(units::LengthFromString("in", len));
  CHECK(len == units::Length::Inch);
  CHECK(units::LengthFromString("INCH", len)); // case-insensitive, like sheets
  CHECK(len == units::Length::Inch);

  units::Length untouched = units::Length::Centimetre;
  CHECK_FALSE(units::LengthFromString("inches", untouched));
  CHECK(untouched == units::Length::Centimetre); // and left alone

  units::Mass mass = units::Mass::Gram;
  CHECK_FALSE(units::MassFromString("stone", mass));
  units::Angle angle = units::Angle::Degree;
  CHECK_FALSE(units::AngleFromString("grad", angle));
}

TEST_CASE("every accepted name parses", "[units]") {
  // The name lists feed `describe` and every "expected one of" message, so a
  // name that does not parse would be advertised and then refused.
  for (const char* n : units::LengthNames()) {
    units::Length out{};
    INFO(n);
    CHECK(units::LengthFromString(n, out));
  }
  for (const char* n : units::MassNames()) {
    units::Mass out{};
    INFO(n);
    CHECK(units::MassFromString(n, out));
  }
  for (const char* n : units::AngleNames()) {
    units::Angle out{};
    INFO(n);
    CHECK(units::AngleFromString(n, out));
  }
}
